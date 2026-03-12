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

	// 멤버 변수 owner_thread_에 현재 스레드 ID 저장 (엔진 루프 시작 시 1회 호출)
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

        // 1) 주문 요청(req) 검증
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
            // 중복 매수 방지 매수 토큰 검증 (전량 거래 모델: 동시에 1개 매수만 허용)
            if (active_buy_token_.has_value())
                return EngineResult::Fail(EngineErrorCode::OrderRejected,
                    "already has pending buy order for " + market_);

            // 반대 포지션 방지 매도 uuid 검증 (전량 거래 모델: BUY/SELL 동시 불가)
            if (!active_sell_order_uuid_.empty())
                return EngineResult::Fail(EngineErrorCode::OrderRejected,
                    "cannot submit buy while sell order is active for " + market_);

            const core::Amount reserve_amount = computeReserveAmount(req);
            auto token = account_mgr_.reserve(market_, reserve_amount);
            if (!token.has_value())
                return EngineResult::Fail(EngineErrorCode::InsufficientFunds,
                    "reserve failed for " + market_);

            active_buy_token_.emplace(std::move(*token));
            // order_uuid는 아직 모름 (주문 성공 후 저장)
        }
        else // ASK
        {
            // 중복 매도 방지 (전량 거래 모델: 동시에 1개 매도만 허용)
            if (!active_sell_order_uuid_.empty())
                return EngineResult::Fail(EngineErrorCode::OrderRejected,
                    "already has pending sell order for " + market_);

            // 반대 포지션 방지 (전량 거래 모델: BUY/SELL 동시 불가)
            if (active_buy_token_.has_value())
                return EngineResult::Fail(EngineErrorCode::OrderRejected,
                    "cannot submit sell while buy order is active for " + market_);
        }

        // 4) 거래소 주문 발송 (SharedOrderApi, variant(order_uuid or RestError) 반환)
        auto result = api_.postOrder(req);

		// 주문 실패 시 처리: BUY 토큰 해제 + 실패 반환
        if (std::holds_alternative<api::rest::RestError>(result))
        {
            // 주문 실패 시 BUY 토큰 자동 해제
            if (req.position == core::OrderPosition::BID)
            {
                account_mgr_.release(std::move(*active_buy_token_));
                active_buy_token_.reset();
                active_buy_order_uuid_.clear();
            }

            const auto& err = std::get<api::rest::RestError>(result);
            return EngineResult::Fail(EngineErrorCode::InternalError,
                "postOrder failed: " + err.message);
        }

		// uuid 처리
        const std::string& order_uuid = std::get<std::string>(result);

        // BID 주문 성공 시 order_uuid 저장 (토큰과 연결)
        if (req.position == core::OrderPosition::BID)
            active_buy_order_uuid_ = order_uuid;

        // ASK 주문 성공 시 order_uuid 저장 (중복 방지용)
        if (req.position == core::OrderPosition::ASK)
            active_sell_order_uuid_ = order_uuid;

        // 5) 로컬 주문 저장소에 저장 (Pending)
        core::Order o{};
        o.id = order_uuid;
        o.identifier = req.identifier.empty()
            ? std::nullopt
            : std::optional<std::string>(req.identifier);
        o.market = req.market;
        o.position = req.position;
        o.type = req.type;
        o.price = req.price;

        // volume: VolumeSize인 경우만 설정. AmountSize(시장가 매수)는 requested_amount에 보존
        if (std::holds_alternative<core::VolumeSize>(req.size))
            o.volume = std::get<core::VolumeSize>(req.size).value;
        else
            o.requested_amount = std::get<core::AmountSize>(req.size).value;

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
        auto ordOpt = store_.get(t.order_uuid);
        if (!ordOpt.has_value())
        {
            // 이 엔진이 제출하지 않은 주문 (외부 주문)
            util::Logger::instance().warn(
                "[MarketEngine][", market_, "] Ignoring external trade: order_uuid=",
                t.order_uuid, ", side=", (t.side == core::OrderPosition::BID ? "BID" : "ASK"));
            return;
        }

        // identifier 확인 (EngineFillEvent 발행용)
        std::optional<std::string> id = t.identifier;
        if (!id.has_value())
            id = ordOpt->identifier;

        // 2) EngineFillEvent 발행
        if (id.has_value() && !id->empty())
        {
            EngineFillEvent ev;
            ev.identifier = *id;
            ev.order_uuid = t.order_uuid;
            ev.trade_uuid = t.trade_uuid.empty() ? dedupeKey : t.trade_uuid;
            ev.position = t.side;
            ev.fill_price = t.price;
            ev.filled_volume = t.volume;
            pushEvent_(EngineEvent{ std::move(ev) });
        }

        // 3) AccountManager를 통한 잔고 업데이트(onOrderSnapshot에서 절대값으로 처리)
        if (t.side == core::OrderPosition::BID)
        {
            // 매수 체결: 현재 활성 토큰이 이 주문과 연결되었는지 검증
            if (active_buy_token_.has_value() && active_buy_order_uuid_ == t.order_uuid)
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
                    "order_uuid=", t.order_uuid, ", active_order=", active_buy_order_uuid_,
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
    //void MarketEngine::onOrderStatus(std::string_view order_uuid, core::OrderStatus s)
    //{
    //    assertOwner_();

    //    auto ordOpt = store_.get(order_uuid);
    //    if (!ordOpt.has_value()) return;

    //    auto o = *ordOpt;

    //    // 마켓 격리 검증 (OrderStore 공유 환경에서 크로스 마켓 오염 방지)
    //    if (!o.market.empty() && o.market != market_)
    //    {
    //        util::Logger::instance().warn(
    //            "[MarketEngine][", market_, "] Ignoring order status for other market: ",
    //            "order_market=", o.market, ", order_uuid=", order_uuid);
    //        return;
    //    }

    //    const core::OrderStatus old_status = o.status;
    //    o.status = s;

    //    if (s == core::OrderStatus::Filled)
    //        o.remaining_volume = 0.0;

    //    if (!store_.update(o))
    //    {
    //        util::Logger::instance().warn(
    //            "[MarketEngine][", market_, "] update miss in onOrderStatus, skip: order_uuid=", o.id);
    //        return;
    //    }

    //    // 터미널 상태 전환 시 BID 토큰 정리
    //    // ASK 주문은 snapshot 최종 정산(onOrderSnapshot)까지 erase를 미룬다
    //    const bool isTerminal = (s == core::OrderStatus::Filled
    //        || s == core::OrderStatus::Canceled
    //        || s == core::OrderStatus::Rejected);

    //    if (old_status != s && isTerminal)
    //    {
    //        // BID 주문 터미널 → 현재 활성 주문인 경우에만 토큰 정리
    //        if (o.position == core::OrderPosition::BID && o.id == active_buy_order_uuid_)
    //            finalizeBuyToken_(o.id);

    //        // ASK 주문 터미널: 상태만 확인하고 ID 정리는 snapshot 최종 정산으로 미룬다.
    //        if (o.position == core::OrderPosition::ASK && o.id == active_sell_order_uuid_)
    //        {
    //            util::Logger::instance().info(
    //                "[MarketEngine][", market_, "] onOrderStatus terminal ASK observed, "
    //                "waiting snapshot finalize, id=", o.id);
    //        }
    //    }
    //}

    // ========== onOrderSnapshot ==========
    void MarketEngine::onOrderSnapshot(const core::Order& snapshot)
    {
        assertOwner_();

        // 마켓 범위 검증
        if (!snapshot.market.empty() && snapshot.market != market_)
            return;

		// 바로 체결되어 store에 없으면 그냥 데이터 넣고 종료
        auto ordOpt = store_.get(snapshot.id);
        if (!ordOpt.has_value())
        {
			// store에 없는 주문은 이 엔진이 제출한 주문이 아닐 가능성이 높음 (외부 주문 또는 store 누락)
            return;
        }

        // 기존에 부분 체결된 주문을 스냅샷으로 업데이트
        auto o = *ordOpt;
        const core::OrderStatus old_status = o.status;

        // 스냅샷 필드 동기화
        if (!snapshot.market.empty()) o.market = snapshot.market;
        o.position = snapshot.position;
        o.type = snapshot.type;

        // price: 지정가에서만 업데이트 허용, 이미 있으면 유지 (요청 원본 불변 정책)
        // Market 주문은 wait 스냅샷에서 price=주문총액이 오더라도 차단 (requested_amount로 관리)
        if (!o.price.has_value() && snapshot.price.has_value()
            && o.type == core::OrderType::Limit)
            o.price = snapshot.price;

        // volume: 최초 설정 후 유지
        if (!o.volume.has_value() && snapshot.volume.has_value())
            o.volume = snapshot.volume;

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

        if (!store_.update(o))
        {
            util::Logger::instance().warn(
                "[MarketEngine][", market_, "] update miss in onOrderSnapshot, skip: order_uuid=", o.id);
            return;
        }

        // 터미널 상태 도달 시 이벤트 발행 + 토큰 정리 + store 제거
        const bool isTerminal = (o.status == core::OrderStatus::Filled
            || o.status == core::OrderStatus::Canceled
            || o.status == core::OrderStatus::Rejected);

        if (isTerminal)
        {
			// 동일 종결 상태 업데이트는 무시 (이벤트 중복 방지)
            if (o.status != old_status)
            {
                if (o.identifier.has_value() && !o.identifier->empty())
                {
                    EngineOrderStatusEvent ev;
                    ev.identifier = *o.identifier;
                    ev.order_uuid = o.id;
                    ev.status = o.status;
                    ev.position = o.position;
                    ev.executed_volume = o.executed_volume;
                    ev.remaining_volume = o.remaining_volume;
                    ev.executed_funds = o.executed_funds;  // WS 유실 시 전략 vwap 폴백용
                    pushEvent_(EngineEvent{ std::move(ev) });
                }

                // BID 주문 터미널 → 현재 활성 주문인 경우에만 토큰 정리
                if (o.position == core::OrderPosition::BID && o.id == active_buy_order_uuid_)
                    finalizeBuyToken_(o.id);

                // ASK 주문 터미널 → 현재 활성 주문인 경우에만 ID 정리
                if (o.position == core::OrderPosition::ASK && o.id == active_sell_order_uuid_)
                {
                    // 매도 주문 종료 시점에만 dust/실현손익을 확정한다.
                    // last_mark_price_가 0이면 nullopt로 전달 → 가치 기준 판정 생략 (수량 기준만 적용)
                    std::optional<core::Price> mark =
                        (last_mark_price_ > 0.0) ? std::optional<core::Price>(last_mark_price_) : std::nullopt;
                    account_mgr_.finalizeSellOrder(market_, mark);
                    active_sell_order_uuid_.clear();
                }
            }

            // 터미널 주문은 store에서 즉시 제거 (활성 주문만 store에 유지하는 정책)
            store_.erase(o.id);
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
    std::optional<core::Order> MarketEngine::get(std::string_view order_uuid) const
    {
        assertOwner_();
        return store_.get(order_uuid);
    }

    // ========== validateRequest ==========
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
    // order_uuid: 토큰과 연결된 주문 ID (검증용)
    void MarketEngine::finalizeBuyToken_(std::string_view order_uuid)
    {
        // 이중 검증: 토큰 존재 유뮤 + order_uuid 일치
        if (!active_buy_token_.has_value())
            return;

        if (active_buy_order_uuid_.empty() || active_buy_order_uuid_ != order_uuid)
        {
            util::Logger::instance().warn(
                "[MarketEngine][", market_, "] finalizeBuyToken_ order_uuid mismatch: "
                "requested=", order_uuid, ", active=", active_buy_order_uuid_);
            return;
        }

        // 미사용 잔액 available_krw로 복구
        account_mgr_.finalizeOrder(std::move(*active_buy_token_));
        active_buy_token_.reset();
        active_buy_order_uuid_.clear();
    }

    // 현재 활성 pending 주문 ID 반환
    MarketEngine::PendingIds MarketEngine::activePendingIds() const noexcept
    {
        return { active_buy_order_uuid_, active_sell_order_uuid_ };
    }

    // REST snapshot 기반 delta 정산
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
        {
            util::Logger::instance().warn(
                "[MarketEngine][", market_, "] reconcile: order not in store, id=", snapshot.id);
            return false;
        }

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
            // 핵심 원칙: 체결 금액을 확정할 수 없으면 0으로 정산하지 않는다.
            if (delta_funds <= 0.0)
            {
                util::Logger::instance().warn(
                    "[MarketEngine][", market_, "] reconcile unknown_funds: "
                    "delta_vol=", delta_volume,
                    " delta_funds=", delta_funds,
                    " order=", snapshot.id,
                    " status=", static_cast<int>(snapshot.status));
                return false;
            }

            if (snapshot.position == core::OrderPosition::BID)
            {
                if (!active_buy_token_.has_value() || active_buy_order_uuid_ != snapshot.id)
                {
                    util::Logger::instance().warn(
                        "[MarketEngine][", market_, "] reconcile BID: token mismatch, "
                        "order_uuid=", snapshot.id);
                    return false;
                }

                const double fill_price = delta_funds / delta_volume;
                if (fill_price <= 0.0)
                {
                    util::Logger::instance().warn(
                        "[MarketEngine][", market_, "] reconcile unknown_price: "
                        "delta_funds=", delta_funds,
                        "delta_vol=", delta_volume, " order=", snapshot.id);
                    return false;
                }

                const double delta_krw = delta_funds + delta_paid_fee;
                account_mgr_.finalizeFillBuy(
                    *active_buy_token_, delta_krw, delta_volume, fill_price);
            }
            else // ASK
            {
                if (active_sell_order_uuid_ != snapshot.id)
                {
                    util::Logger::instance().warn(
                        "[MarketEngine][", market_, "] reconcile ASK: order_id mismatch, "
                        "snapshot=", snapshot.id, " active=", active_sell_order_uuid_);
                    return false;
                }
                const double received_krw = std::max(0.0, delta_funds - delta_paid_fee);
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

    // ========== makeTradeDedupeKey_ ==========
    std::string MarketEngine::makeTradeDedupeKey_(const core::MyTrade& t)
    {
        if (!t.trade_uuid.empty())
            return t.trade_uuid;

        std::ostringstream oss;
        // oss를 항상 C 기본 방식으로 사용하라는 설정
        oss.imbue(std::locale::classic());
        oss << "FALLBACK|"
            << t.order_uuid << '|'
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

    // ========== markTradeOnce ==========
    bool MarketEngine::markTradeOnce(std::string_view trade_uuid)
    {
        if (trade_uuid.empty())
            return false;

		// unordeded_set에 trade_uuid 삽입 시도, 이미 존재하면 false 반환
        auto [it, inserted] = seen_trade_uuids_.emplace(trade_uuid);
        if (!inserted)
            return false;

        seen_trade_uuid_fifo_.push_back(*it);

        while (seen_trade_uuid_fifo_.size() > util::AppConfig::instance().engine.max_seen_trades)
        {
            const std::string& oldest = seen_trade_uuid_fifo_.front();
            seen_trade_uuids_.erase(oldest);
            seen_trade_uuid_fifo_.pop_front();
        }
        return true;
    }
}

