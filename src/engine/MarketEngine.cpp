// engine/MarketEngine.cpp

#include "MarketEngine.h"

#include <variant>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <sstream>
#include <iomanip>

#include "util/Config.h"
#include "util/Logger.h"

namespace engine
{
    MarketEngine::MarketEngine(std::string market,
                               api::upbit::IOrderApi& api,
                               OrderStore& store,
                               trading::allocation::AccountManager& account_mgr)
        : market_(std::move(market))
        , api_(api)
        , store_(store)
        , account_mgr_(account_mgr)
    {
    }

    void MarketEngine::bindToCurrentThread() noexcept
    {
        owner_thread_ = std::this_thread::get_id();
    }

    void MarketEngine::assertOwner_() const
    {
#ifndef NDEBUG
        assert(owner_thread_ != std::thread::id{});
        assert(std::this_thread::get_id() == owner_thread_);
#else
        if (owner_thread_ == std::thread::id{} || std::this_thread::get_id() != owner_thread_)
        {
            util::Logger::instance().error("[Fatal] MarketEngine[", market_, "] called from non-owner thread");
            std::terminate();
        }
#endif
    }

    // ========== submit ==========
    EngineResult MarketEngine::submit(const core::OrderRequest& req)
    {
        assertOwner_();

        // 1) 주문 요청 검증
        std::string reason;
        if (!validateRequest(req, reason))
            return EngineResult::Fail(EngineErrorCode::OrderRejected, reason);

        // 2) 마켓 범위 검증
        if (req.market != market_)
            return EngineResult::Fail(EngineErrorCode::MarketNotSupported,
                "market mismatch: expected " + market_ + ", got " + req.market);

        // 3) BUY: KRW 예약, SELL: 중복 체크
        if (req.position == core::OrderPosition::BID)
        {
            // 중복 매수 방지 (전량 거래 모델: 동시에 1개 매수만 허용)
            if (active_buy_token_.has_value())
                return EngineResult::Fail(EngineErrorCode::OrderRejected,
                    "already has pending buy order for " + market_);

            // 반대 포지션 방지 (전량 거래 모델: BUY/SELL 동시 불가)
            if (!active_sell_order_id_.empty())
                return EngineResult::Fail(EngineErrorCode::OrderRejected,
                    "cannot submit buy while sell order is active for " + market_);

            const core::Amount reserve_amount = computeReserveAmount(req);
            auto token = account_mgr_.reserve(market_, reserve_amount);
            if (!token.has_value())
                return EngineResult::Fail(EngineErrorCode::InsufficientFunds,
                    "reserve failed for " + market_);

            active_buy_token_ = std::move(*token);
            // order_id는 아직 모름 (주문 성공 후 저장)
        }
        else // ASK
        {
            // 중복 매도 방지 (전량 거래 모델: 동시에 1개 매도만 허용)
            if (!active_sell_order_id_.empty())
                return EngineResult::Fail(EngineErrorCode::OrderRejected,
                    "already has pending sell order for " + market_);

            // 반대 포지션 방지 (전량 거래 모델: BUY/SELL 동시 불가)
            if (active_buy_token_.has_value())
                return EngineResult::Fail(EngineErrorCode::OrderRejected,
                    "cannot submit sell while buy order is active for " + market_);
        }

        // 4) 거래소 주문 발송 (SharedOrderApi, variant 반환)
        auto result = api_.postOrder(req);

        if (std::holds_alternative<api::rest::RestError>(result))
        {
            // 주문 실패 시 BUY 토큰 자동 해제
            if (req.position == core::OrderPosition::BID && active_buy_token_.has_value())
            {
                account_mgr_.release(std::move(*active_buy_token_));
                active_buy_token_.reset();
                active_buy_order_id_.clear();
            }

            const auto& err = std::get<api::rest::RestError>(result);
            return EngineResult::Fail(EngineErrorCode::InternalError,
                "postOrder failed: " + err.message);
        }

        const std::string& uuid = std::get<std::string>(result);
        if (uuid.empty())
        {
            if (req.position == core::OrderPosition::BID && active_buy_token_.has_value())
            {
                account_mgr_.release(std::move(*active_buy_token_));
                active_buy_token_.reset();
                active_buy_order_id_.clear();
            }
            return EngineResult::Fail(EngineErrorCode::InternalError, "postOrder returned empty uuid");
        }

        // BID 주문 성공 시 order_id 저장 (토큰과 연결)
        if (req.position == core::OrderPosition::BID && active_buy_token_.has_value())
            active_buy_order_id_ = uuid;

        // ASK 주문 성공 시 order_id 저장 (중복 방지용)
        if (req.position == core::OrderPosition::ASK)
            active_sell_order_id_ = uuid;

        // 5) 로컬 주문 저장소에 저장 (Pending)
        core::Order o{};
        o.id = uuid;
        o.identifier = req.identifier.empty()
            ? std::nullopt
            : std::optional<std::string>(req.identifier);
        o.market = req.market;
        o.position = req.position;
        o.type = req.type;
        o.price = req.price;

        // volume: 지정가 매도만 미리 알 수 있음 (시장가 매수는 체결 후 확정)
        if (std::holds_alternative<core::VolumeSize>(req.size))
            o.volume = std::get<core::VolumeSize>(req.size).value;
        else
            o.volume = std::nullopt;

        o.status = core::OrderStatus::Pending;
        o.created_at = "";

        store_.upsert(o);

        return EngineResult::Success(std::move(o));
    }

    // ========== onMyTrade ==========
    void MarketEngine::onMyTrade(const core::MyTrade& t)
    {
        assertOwner_();

        // 마켓 범위 검증 (다른 마켓 이벤트는 무시)
        if (t.market != market_)
            return;

        // 중복 방지
        const std::string dedupeKey = makeTradeDedupeKey_(t);
        if (!markTradeOnce(dedupeKey))
            return;

        // 1) OrderStore에서 주문 조회 (외부 주문 거부 정책)
        auto ordOpt = store_.get(t.order_id);
        if (!ordOpt.has_value())
        {
            // 이 엔진이 제출하지 않은 주문 (외부 주문)
            util::Logger::instance().warn(
                "[MarketEngine][", market_, "] Ignoring external trade: order_id=",
                t.order_id, ", side=", (t.side == core::OrderPosition::BID ? "BID" : "ASK"));
            return;
        }

        // identifier 확인 (EngineFillEvent 발행용)
        std::optional<std::string> id = t.identifier;
        if (!id.has_value() && ordOpt.has_value())
            id = ordOpt->identifier;

        // 2) EngineFillEvent 발행
        if (id.has_value() && !id->empty())
        {
            EngineFillEvent ev;
            ev.identifier = *id;
            ev.order_id = t.order_id;
            ev.trade_id = t.trade_id.empty() ? dedupeKey : t.trade_id;
            ev.position = t.side;
            ev.fill_price = t.price;
            ev.filled_volume = t.volume;
            pushEvent_(EngineEvent{ std::move(ev) });
        }

        // 3) AccountManager를 통한 잔고 업데이트(onOrderSnapshot에서 절대값으로 처리)
        if (t.side == core::OrderPosition::BID)
        {
            // 매수 체결: 현재 활성 토큰이 이 주문과 연결되었는지 검증
            // 리스크 1 해결: MyTrade를 먼저 처리하므로 토큰이 살아있음
            if (active_buy_token_.has_value() && active_buy_order_id_ == t.order_id)
            {
                const core::Amount executed_krw = t.executed_funds + t.fee;
                account_mgr_.finalizeFillBuy(*active_buy_token_,
                    executed_krw, t.volume, t.price);
            }
            else
            {
                // 토큰 없거나 다른 주문의 체결 (지연/외부 이벤트)
                util::Logger::instance().warn(
                    "[MarketEngine][", market_, "] BID fill ignored - "
                    "order_id=", t.order_id, ", active_order=", active_buy_order_id_,
                    ", has_token=", active_buy_token_.has_value());
            }
        }
        else
        {
            // 매도 체결: 마켓 기준으로 정산
            const core::Amount received_krw = std::max<core::Amount>(0.0, t.executed_funds - t.fee);
            account_mgr_.finalizeFillSell(market_, t.volume, received_krw);
        }
    }

    // ========== onOrderStatus ==========
    void MarketEngine::onOrderStatus(std::string_view order_id, core::OrderStatus s)
    {
        assertOwner_();

        auto ordOpt = store_.get(order_id);
        if (!ordOpt.has_value()) return;

        auto o = *ordOpt;

        // 마켓 격리 검증 (OrderStore 공유 환경에서 크로스 마켓 오염 방지)
        if (!o.market.empty() && o.market != market_)
        {
            util::Logger::instance().warn(
                "[MarketEngine][", market_, "] Ignoring order status for other market: ",
                "order_market=", o.market, ", order_id=", order_id);
            return;
        }

        const core::OrderStatus old_status = o.status;
        o.status = s;

        if (s == core::OrderStatus::Filled)
            o.remaining_volume = 0.0;

        store_.update(o);

        // 터미널 상태 전환 시 cleanup + BID 토큰 정리
        const bool isTerminal = (s == core::OrderStatus::Filled
            || s == core::OrderStatus::Canceled
            || s == core::OrderStatus::Rejected);

        if (old_status != s && isTerminal)
        {
            // BID 주문 터미널 → 현재 활성 주문인 경우에만 토큰 정리
            if (o.position == core::OrderPosition::BID && o.id == active_buy_order_id_)
                finalizeBuyToken_(o.id);

            // ASK 주문 터미널 → 현재 활성 주문인 경우에만 ID 정리
            if (o.position == core::OrderPosition::ASK && o.id == active_sell_order_id_)
                active_sell_order_id_.clear();

            // 주기적 cleanup (인스턴스별 100개 완료 주문마다)
            ++completed_count_;
            if (completed_count_ >= 100)
            {
                completed_count_ = 0;
                const std::size_t removed = store_.cleanup();
                if (removed > 0)
                    util::Logger::instance().info("[MarketEngine][", market_,
                        "] OrderStore cleanup: removed ", removed, " old orders");
            }
        }
    }

    // ========== onOrderSnapshot ==========
    void MarketEngine::onOrderSnapshot(const core::Order& snapshot)
    {
        assertOwner_();

        // 마켓 범위 검증
        if (!snapshot.market.empty() && snapshot.market != market_)
            return;

        auto ordOpt = store_.get(snapshot.id);
        if (!ordOpt.has_value())
        {
            store_.upsert(snapshot);
            return;
        }

        auto o = *ordOpt;
        const core::OrderStatus old_status = o.status;

        // 스냅샷 필드 동기화
        if (!snapshot.market.empty()) o.market = snapshot.market;
        o.position = snapshot.position;
        o.type = snapshot.type;

        if (snapshot.price.has_value())  o.price = snapshot.price;
        if (snapshot.volume.has_value()) o.volume = snapshot.volume;

        o.executed_volume = snapshot.executed_volume;
        o.remaining_volume = snapshot.remaining_volume;
        o.trades_count = snapshot.trades_count;

        o.reserved_fee = snapshot.reserved_fee;
        o.remaining_fee = snapshot.remaining_fee;
        o.paid_fee = snapshot.paid_fee;
        o.locked = snapshot.locked;
        o.executed_funds = snapshot.executed_funds;  // 누적 체결 금액 동기화

        o.status = snapshot.status;

        // identifier: 기존 유지, 없으면 스냅샷에서 채움
        if (!o.identifier.has_value() && snapshot.identifier.has_value())
            o.identifier = snapshot.identifier;

        if (!snapshot.created_at.empty())
            o.created_at = snapshot.created_at;

        store_.update(o);

        // 터미널 상태 도달 시 이벤트 발행 + 토큰 정리
        const bool isTerminal = (o.status == core::OrderStatus::Filled
            || o.status == core::OrderStatus::Canceled
            || o.status == core::OrderStatus::Rejected);

        if (isTerminal && o.status != old_status)
        {
            if (o.identifier.has_value() && !o.identifier->empty())
            {
                EngineOrderStatusEvent ev;
                ev.identifier = *o.identifier;
                ev.order_id = o.id;
                ev.status = o.status;
                ev.position = o.position;
                ev.executed_volume = o.executed_volume;
                ev.remaining_volume = o.remaining_volume;
                pushEvent_(EngineEvent{ std::move(ev) });
            }

            // BID 주문 터미널 → 현재 활성 주문인 경우에만 토큰 정리
            if (o.position == core::OrderPosition::BID && o.id == active_buy_order_id_)
                finalizeBuyToken_(o.id);

            // ASK 주문 터미널 → 현재 활성 주문인 경우에만 ID 정리
            if (o.position == core::OrderPosition::ASK && o.id == active_sell_order_id_)
                active_sell_order_id_.clear();
        }
    }

    // ========== pollEvents ==========
    std::vector<EngineEvent> MarketEngine::pollEvents()
    {
        assertOwner_();

        std::vector<EngineEvent> out;
        out.reserve(events_.size());
        while (!events_.empty())
        {
            out.emplace_back(std::move(events_.front()));
            events_.pop_front();
        }
        return out;
    }

    // ========== get ==========
    std::optional<core::Order> MarketEngine::get(std::string_view order_id) const
    {
        assertOwner_();
        return store_.get(order_id);
    }

    // ========== validateRequest (RealOrderEngine과 동일) ==========
    bool MarketEngine::validateRequest(const core::OrderRequest& req, std::string& reason) noexcept
    {
        if (req.market.empty())
        {
            reason = "market is empty";
            return false;
        }

        const bool isVolume = std::holds_alternative<core::VolumeSize>(req.size);
        const bool isAmount = std::holds_alternative<core::AmountSize>(req.size);

        if (!isVolume && !isAmount)
        {
            reason = "invalid OrderSize variant";
            return false;
        }

        // Upbit 정책:
        // - 지정가(Limit): BID/ASK 모두 price + volume(VolumeSize)
        // - 시장가(Market): 매수(BID) → AmountSize, 매도(ASK) → VolumeSize
        if (req.type == core::OrderType::Limit)
        {
            if (!req.price.has_value())
            {
                reason = "limit order requires price";
                return false;
            }
            if (!isVolume)
            {
                reason = "limit order requires VolumeSize";
                return false;
            }
        }
        else // Market
        {
            if (req.price.has_value())
            {
                reason = "market order must not specify price";
                return false;
            }

            if (req.position == core::OrderPosition::BID && !isAmount)
            {
                reason = "market buy(BID) requires AmountSize";
                return false;
            }
            if (req.position == core::OrderPosition::ASK && !isVolume)
            {
                reason = "market sell(ASK) requires VolumeSize";
                return false;
            }
        }

        // 값 범위 체크 (0 이하 거래 불가)
        if (isAmount && std::get<core::AmountSize>(req.size).value <= 0.0)
        {
            reason = "amount must be > 0";
            return false;
        }
        if (isVolume && std::get<core::VolumeSize>(req.size).value <= 0.0)
        {
            reason = "volume must be > 0";
            return false;
        }
        if (req.price.has_value() && req.price.value() <= 0.0)
        {
            reason = "price must be > 0";
            return false;
        }

        return true;
    }

    // ========== computeReserveAmount ==========
    // 시장가 매수: AmountSize (총 KRW) * margin
    // 지정가 매수: price * volume * margin
    //
    // [수수료 커버용 여유분]
    // Upbit 시장가 매수는 price 파라미터가 수수료 제외 금액
    // 실제 차감: executed_funds + fee ≈ price × 1.0005
    // reserve_margin = 1.001 (0.1% 여유)로 초과 방지
    core::Amount MarketEngine::computeReserveAmount(const core::OrderRequest& req)
    {
        const auto& cfg = util::AppConfig::instance().engine;

        if (std::holds_alternative<core::AmountSize>(req.size)) {
            core::Amount base = std::get<core::AmountSize>(req.size).value;
            return base * cfg.reserve_margin;
        }

        // 지정가: price * volume * margin
        const double volume = std::get<core::VolumeSize>(req.size).value;
        const double price = req.price.value_or(0.0);
        return price * volume * cfg.reserve_margin;
    }

    // ========== finalizeBuyToken_ ==========
    // order_id: 토큰과 연결된 주문 ID (검증용)
    void MarketEngine::finalizeBuyToken_(std::string_view order_id)
    {
        // 이중 검증: 토큰 존재 + order_id 일치
        if (!active_buy_token_.has_value())
            return;

        if (active_buy_order_id_.empty() || active_buy_order_id_ != order_id)
        {
            util::Logger::instance().warn(
                "[MarketEngine][", market_, "] finalizeBuyToken_ order_id mismatch: "
                "requested=", order_id, ", active=", active_buy_order_id_);
            return;
        }

        // 미사용 잔액 available_krw로 복구
        account_mgr_.finalizeOrder(std::move(*active_buy_token_));
        active_buy_token_.reset();
        active_buy_order_id_.clear();
    }

    // [HYBRID v2 §4.5] 현재 활성 pending 주문 ID 반환
    MarketEngine::PendingIds MarketEngine::activePendingIds() const noexcept
    {
        return { active_buy_order_id_, active_sell_order_id_ };
    }

    // [HYBRID v2 §4.5] REST snapshot 기반 delta 정산
    // 정상 경로(onMyTrade)와 복구 경로(reconcile) 모두 MarketEngine을 통과하여
    // AccountManager 정산의 단일 진입점을 보장한다.
    //
    // 중요 순서: delta 정산 → onOrderSnapshot
    // 이유: 터미널 snapshot은 onOrderSnapshot 내부에서 토큰을 정리(finalizeBuyToken_)할 수 있다.
    //       snapshot 반영을 먼저 하면 매수 delta 정산 시 토큰이 사라져 누락된다.
    bool MarketEngine::reconcileFromSnapshot(const core::Order& snapshot)
    {
        assertOwner_();

        if (!snapshot.market.empty() && snapshot.market != market_)
            return false;

        // OrderStore에서 이전 누적값 조회
        auto prev = store_.get(snapshot.id);
        if (!prev.has_value())
            return false;

        // delta 계산 (음수 방어: 데이터 불일치 시 역정산 방지)
        const double delta_volume = std::max(0.0,
            snapshot.executed_volume - prev->executed_volume);
        const double delta_funds = std::max(0.0,
            snapshot.executed_funds - prev->executed_funds);
        const double delta_paid_fee = std::max(0.0,
            snapshot.paid_fee - prev->paid_fee);

        // delta > 0인 경우에만 AccountManager 정산
        if (delta_volume > 0.0)
        {
            if (snapshot.position == core::OrderPosition::BID)
            {
                if (!active_buy_token_.has_value() || active_buy_order_id_ != snapshot.id)
                {
                    util::Logger::instance().warn(
                        "[MarketEngine][", market_, "] reconcile BID: token mismatch, "
                        "order_id=", snapshot.id);
                    return false;
                }

                // 1순위: delta_funds로 정확한 단가 계산
                // 2순위: delta_funds=0 시 snapshot.price로 추정 (ASK 4-2와 대칭)
                //        거래소에서 코인이 들어온 건 확정이므로 반드시 반영해야 함
                //        미반영 시 이중 매수 유발, 단가 오차는 rebuildFromAccount로 교정 가능
                double fill_price = 0.0;
                double delta_krw = 0.0;

                if (delta_funds > 0.0)
                {
                    fill_price = delta_funds / delta_volume;
                    delta_krw = delta_funds + delta_paid_fee;
                }
                else if (snapshot.type == core::OrderType::Limit
                         && snapshot.price.has_value() && *snapshot.price > 0.0)
                {
                    // 지정가만 허용: 시장가의 price는 총액이므로 단가로 사용 불가
                    fill_price = *snapshot.price;
                    delta_krw = fill_price * delta_volume + delta_paid_fee;

                    util::Logger::instance().warn(
                        "[MarketEngine][", market_, "] reconcile BID: "
                        "delta_funds=0, using limit price as fallback, "
                        "price=", fill_price, " delta_vol=", delta_volume,
                        " order=", snapshot.id);
                }

                if (fill_price > 0.0)
                {
                    account_mgr_.finalizeFillBuy(
                        *active_buy_token_, delta_krw, delta_volume, fill_price);
                }
                else
                {
                    // 가격 추정 불가: 코인 미반영 상태로 터미널 진행
                    // rebuildFromAccount에서 교정 필요
                    util::Logger::instance().error(
                        "[MarketEngine][", market_, "] reconcile BID: "
                        "delta_funds=0 and no price available, coin not credited, "
                        "delta_vol=", delta_volume, " order=", snapshot.id);
                }
            }
            else // ASK
            {
                // delta_funds=0이어도 코인 차감은 반드시 수행 (4-2)
                // 거래소에서 체결된 코인은 이미 빠져나간 확정 사실
                // KRW 과소반영은 rebuildFromAccount로 교정 가능하지만,
                // 코인 과대보유는 즉시 insufficient_funds_ask 반복을 유발함
                const double received_krw = std::max(0.0, delta_funds - delta_paid_fee);
                if (received_krw == 0.0)
                {
                    util::Logger::instance().warn(
                        "[MarketEngine][", market_, "] reconcile ASK: "
                        "delta_funds=0, forcing coin deduction only, "
                        "delta_vol=", delta_volume, " order=", snapshot.id);
                }
                account_mgr_.finalizeFillSell(market_, delta_volume, received_krw);
            }

            util::Logger::instance().info(
                "[MarketEngine][", market_, "] reconcile: delta_vol=", delta_volume,
                " delta_funds=", delta_funds, " order=", snapshot.id);
        }

        // OrderStore + 터미널 처리는 onOrderSnapshot에 위임
        onOrderSnapshot(snapshot);
        return true;
    }

    // ========== clearPendingState ==========
    // 재연결/타임아웃 복구 시 호출: 토큰과 주문 ID를 안전하게 정리
    // reconciled=true: 정산 완료 상태 → deactivate만 (이미 finalizeBuyToken_에서 처리됨)
    // reconciled=false: 정산 실패 → release로 reserved_krw 복구 (영구 잠김 방지)
    void MarketEngine::clearPendingState(bool reconciled)
    {
        assertOwner_();

        if (active_buy_token_.has_value())
        {
            if (reconciled)
            {
                // 정상: onOrderSnapshot → finalizeBuyToken_에서 이미 정산됨
                active_buy_token_->deactivate();
                active_buy_token_.reset();
            }
            else
            {
                // 정산 실패: 토큰을 release하여 reserved_krw → available_krw 복구
                util::Logger::instance().warn(
                    "[MarketEngine][", market_,
                    "] clearPendingState: reconcile failed, releasing reserved_krw");
                account_mgr_.release(std::move(*active_buy_token_));
                active_buy_token_.reset();
            }
            active_buy_order_id_.clear();

            util::Logger::instance().info(
                "[MarketEngine][", market_, "] clearPendingState: buy token cleared"
                " (reconciled=", reconciled ? "true" : "false", ")");
        }

        if (!active_sell_order_id_.empty())
        {
            util::Logger::instance().info(
                "[MarketEngine][", market_, "] clearPendingState: sell order cleared, id=",
                active_sell_order_id_);
            active_sell_order_id_.clear();
        }
    }

    // ========== pushEvent_ ==========
    void MarketEngine::pushEvent_(EngineEvent ev)
    {
        events_.emplace_back(std::move(ev));
    }

    // ========== extractCurrency ==========
    std::string MarketEngine::extractCurrency(std::string_view market)
    {
        const auto pos = market.find('-');
        if (pos == std::string_view::npos)
            return std::string(market);
        return std::string(market.substr(pos + 1));
    }

    // ========== makeTradeDedupeKey_ (RealOrderEngine과 동일) ==========
    std::string MarketEngine::makeTradeDedupeKey_(const core::MyTrade& t)
    {
        if (!t.trade_id.empty())
            return t.trade_id;

        std::ostringstream oss;
        oss.imbue(std::locale::classic());
        oss << "FALLBACK|"
            << t.order_id << '|'
            << static_cast<int>(t.side) << '|'
            << t.market << '|'
            << std::fixed << std::setprecision(12)
            << t.price << '|'
            << t.volume << '|'
            << t.executed_funds << '|'
            << t.fee;

        if (t.identifier.has_value() && !t.identifier->empty())
            oss << '|' << *t.identifier;

        return oss.str();
    }

    // ========== markTradeOnce (RealOrderEngine과 동일) ==========
    bool MarketEngine::markTradeOnce(std::string_view trade_id)
    {
        if (trade_id.empty())
            return false;

        auto [it, inserted] = seen_trades_.emplace(trade_id);
        if (!inserted)
            return false;

        seen_trade_fifo_.push_back(*it);

        while (seen_trade_fifo_.size() > util::AppConfig::instance().engine.max_seen_trades)
        {
            const std::string& oldest = seen_trade_fifo_.front();
            seen_trades_.erase(oldest);
            seen_trade_fifo_.pop_front();
        }
        return true;
    }
}
