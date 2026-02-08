#include "RealOrderEngine.h"

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
    RealOrderEngine::RealOrderEngine(PrivateOrderApi& api, OrderStore& store, core::Account& account)
        : api_(api), store_(store), account_(account)
    {
    }

    void RealOrderEngine::bindToCurrentThread()
    {
        // 엔진 시작 스레드에서 1회 호출해서 "소유 스레드"를 기록한다.
        // 이후 public API들이 다른 스레드에서 호출되면 assertOwner_()에서 즉시 탐지한다.
        owner_thread_ = std::this_thread::get_id();
    }

    void RealOrderEngine::assertOwner_() const
    {
#ifndef NDEBUG
        assert(owner_thread_ != std::thread::id{});
        assert(std::this_thread::get_id() == owner_thread_);
#else
        if (owner_thread_ == std::thread::id{} || std::this_thread::get_id() != owner_thread_)
        {
            util::Logger::instance().error("[Fatal] RealOrderEngine called from non-owner thread");
            std::terminate();
        }
#endif
    }

    EngineResult RealOrderEngine::submit(const core::OrderRequest& req)
    {
        assertOwner_();

        // 1) 주문 요청 검증(멱등성 보장됨)
        std::string reason;
        if (!validateRequest(req, reason))
            return EngineResult::Fail(EngineErrorCode::OrderRejected, reason);

        // 2) 거래소 주문 발송 POST(/v1/orders) 호출 후 uuid 획득
        const auto uuid = api_.getOrderId(req);
        if (!uuid.has_value() || uuid->empty())
            return EngineResult::Fail(EngineErrorCode::InternalError, "placeOrder failed");

        // 3) 로컬 주문 저장소에 주문 정보를 먼저 저장(Pending)
        // - executed/fee/locked 등 정확한 값은 WS/REST 스냅샷에서 나중에 동기화한다.
        core::Order o{};
        o.id = *uuid;

        // 전략/알고리즘 구분용 client_order_id(프로그램 레벨 식별자)를 identifier로 사용
        o.identifier = req.identifier.empty()
            ? std::nullopt
            : std::optional<std::string>(req.identifier);

        o.market = req.market;
        o.position = req.position;
        o.type = req.type;
        o.price = req.price;

        // volume은 "지정가 매도" 주문만 미리 알 수 있음.
        // 시장가 매수(AmountSize)는 체결 후 거래소 확정치가 내려오므로 nullopt로 초기화함.
        if (std::holds_alternative<core::VolumeSize>(req.size))
            o.volume = std::get<core::VolumeSize>(req.size).value;
        else
            o.volume = std::nullopt;

        o.status = core::OrderStatus::Pending;
        o.created_at = "";

        // 멱등성보장 - uuid 중복이 생기면 덮어쓰고, 없으면처럼 upsert해도 안전
        store_.upsert(o);

        return EngineResult::Success(std::move(o));
    }

    std::string RealOrderEngine::makeTradeDedupeKey_(const core::MyTrade& t)
    {
        // 24시간 운용 시 WS 재연결/중복 패킷/순서 꼬임 때문에 체결 이벤트가 중복 유입될 수 있다.
        // 이상적으로는 dedupe 키로 trade_uuid(trade_id)를 사용하지만,
        // 일부 케이스(특히 시장가/취소 처리 등)에서는 trade_id가 비어 올 수 있으므로
        // "체결을 대표할 수 있는 복합 키"를 만들어 중복 반영을 방지한다.

        if (!t.trade_id.empty())
            return t.trade_id;

        std::ostringstream oss;
        oss.imbue(std::locale::classic()); // locale 영향 제거(소수점 표기 고정)
        oss << "FALLBACK|"
            << t.order_id << '|'
            << static_cast<int>(t.side) << '|'
            << t.market << '|'
            << std::fixed << std::setprecision(12)
            << t.price << '|'
            << t.volume << '|'
            << t.executed_funds << '|'
            << t.fee;

        // identifier는 충돌을 피하기 위해 옵션으로 붙임 추가
        if (t.identifier.has_value() && !t.identifier->empty())
            oss << '|' << *t.identifier;

        return oss.str();
    }

    bool RealOrderEngine::markTradeOnce(std::string_view trade_id)
    {
        // WS를 통해 trade_id가 중복전송될 가능성 있음(재연결/중복패킷/순서 꼬임 등).
        // 중복 반영되면 잔고/체결량이 2배 업데이트되는 심각한 오류가 발생할 수 있으므로 1회 처리만 허용.
        if (trade_id.empty())
            return false; // trade_id가 비어있는 경우는 dedupe 불가 시 정책적으로 하되 허용(아래 상황은 앞쪽에서 대응 완료)

        auto [it, inserted] = seen_trades_.emplace(trade_id);
        if (!inserted)
            return false; // 이미 처리한 trade_id -> 무시

        // 2) 새 trade_id를 FIFO큐에 저장
        // (set의 문자열을 복사하기보다는, emplace된 반복자 자체 저장하면 불필요한 메모리 복사 줄일 수 있음)
        seen_trade_fifo_.push_back(*it);

        // 3) 개수 초과 시 가장 오래된 id부터 삭제
        while (seen_trade_fifo_.size() > util::AppConfig::instance().engine.max_seen_trades)
        {
            const std::string& oldest = seen_trade_fifo_.front();
            seen_trades_.erase(oldest);
            seen_trade_fifo_.pop_front();
        }
        return true;
    }

    void RealOrderEngine::onMyTrade(const core::MyTrade& t)
    {
        assertOwner_();

        // 0) trade_id 중복 방지
        const std::string dedupeKey = makeTradeDedupeKey_(t);
        if (!markTradeOnce(dedupeKey))
            return;

        // 1) 주문 정보를 업데이트 + fill 이벤트 발행을 위해 identifier 확인
        // - WS 메시지에 identifier가 있으면 우선 사용
        // - 없으면 OrderStore에 저장된 주문(identifier)를 fallback
        std::optional<std::string> id = t.identifier;

        if (auto ordOpt = store_.get(t.order_id); ordOpt.has_value())
        {
            auto o = *ordOpt;

            o.market = t.market;

            // 체결이 발생했다면 Pending/New 상태에서 Open으로 전환하는 정책(선제 반영)
            if (o.status == core::OrderStatus::Pending || o.status == core::OrderStatus::New)
                o.status = core::OrderStatus::Open;

            o.executed_volume += t.volume;
            o.trades_count += 1;

            // 총 주문 수량(o.volume)이 있는 경우에는 remaining_volume 계산
            if (o.volume.has_value())
            {
                const double rem = std::max(0.0, o.volume.value() - o.executed_volume);
                o.remaining_volume = rem;
            }

            // 수수료 반영(정확한 fee/locked는 스냅샷에서 갱신, 여기서는 best-effort)
            o.paid_fee += t.fee;

            o.executed_funds += t.executed_funds;

            store_.update(o);

            if (!id.has_value())
                id = o.identifier;
        }

        // 2) EngineFillEvent 발행(부분/전체 체결도 모두 발행 필요)
        // - OrderStore에 주문이 저장됨(identifier가 있다면) 해당 매칭을 보장하므로 사용한다.
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

        // 3) 잔고/포지션 로컬 캐시(best-effort) 업데이트
        // - 최종 정확값은 WS 이벤트(/accounts 등)를 사용할 것
        const std::string currency = extractCurrency(t.market);

        if (t.side == core::OrderPosition::BID)
        {
            // 매수 체결: KRW 차감(체결액 + 수수료), 코인 증가
            const core::Amount krw_out = t.executed_funds + t.fee;
            account_.krw_free = std::max<core::Amount>(0.0, account_.krw_free - krw_out);

            auto it = std::find_if(account_.positions.begin(), account_.positions.end(),
                [&](const core::Position& p) { return p.currency == currency; });

            if (it == account_.positions.end())
            {
                core::Position p{};
                p.currency = currency;
                p.free = t.volume;
                p.avg_buy_price = t.price;
                p.unit_currency = "KRW";
                account_.positions.push_back(p);
            }
            else
            {
                // 기존 포지션에 추가매수 발생(평단 재계산)
                const double old_qty = it->free;
                const double new_qty = old_qty + t.volume;
                if (new_qty > 0.0)
                {
                    const double old_cost = it->avg_buy_price * old_qty;
                    const double add_cost = t.price * t.volume;
                    it->avg_buy_price = (old_cost + add_cost) / new_qty;
                }
                it->free = new_qty;
            }
        }
        else
        {
            // 매도 체결: KRW 증가(체결액 - 수수료), 코인 차감
            const core::Amount krw_in = std::max<core::Amount>(0.0, t.executed_funds - t.fee);
            account_.krw_free += krw_in;

            auto it = std::find_if(account_.positions.begin(), account_.positions.end(),
                [&](const core::Position& p) { return p.currency == currency; });

            if (it != account_.positions.end())
            {
                it->free -= t.volume;
                if (it->free <= 0.0)
                    account_.positions.erase(it);
            }
        }
    }

    // 나중에 REST 폴링 등으로 단순 상태만 업데이트하는 경우
    void RealOrderEngine::onOrderStatus(std::string_view order_id, core::OrderStatus s)
    {
        assertOwner_();

        // 지금 구현은 Upbit 이벤트를 전달받은 후 상태 처리하는 예제 수준
        auto ordOpt = store_.get(order_id);
        if (!ordOpt.has_value()) return;

        auto o = *ordOpt;
        const core::OrderStatus old_status = o.status;
        o.status = s;

        if (s == core::OrderStatus::Filled)
            o.remaining_volume = 0.0;

        store_.update(o);

        // 완료 주문이 전환될 때마다 주기적으로 cleanup 실행
        if (old_status != s && (s == core::OrderStatus::Filled
            || s == core::OrderStatus::Canceled
            || s == core::OrderStatus::Rejected))
        {
            // 카운터 기반 실행(스레드 안전)
            static std::size_t completed_count = 0;
            ++completed_count;

            // 100개 완료 주문마다 1회 cleanup
            if (completed_count >= 100)
            {
                completed_count = 0;
                const std::size_t removed = store_.cleanup();
                if (removed > 0)
                {
                    util::Logger::instance().info("[OrderStore] Cleanup: removed ", removed, " old orders");
                }
            }
        }
    }

    void RealOrderEngine::onOrderSnapshot(const core::Order& snapshot)
    {
        assertOwner_();

        // WS/REST 이벤트로 "최신 있는 전체"에서 상태 동기화한다.
        // - 재연결/중복전송/부분 이벤트에서 안전 보존하도록 설계 의도
        auto ordOpt = store_.get(snapshot.id);
        if (!ordOpt.has_value())
        {
            store_.upsert(snapshot);
            return;
        }

        auto o = *ordOpt;
        const core::OrderStatus old_status = o.status;

        // 스냅샷에 의미 있는 필드만 업데이트(값없거나 빈값 무시 로직)
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

        o.status = snapshot.status;
        o.executed_funds = snapshot.executed_funds;

        // identifier는 이미 있으면 유지, 없으면 스냅샷 정보로 채움(양방향 가능성)
        if (!o.identifier.has_value() && snapshot.identifier.has_value())
            o.identifier = snapshot.identifier;

        if (!snapshot.created_at.empty())
            o.created_at = snapshot.created_at;

        store_.update(o);

        // 외부에서 주문이 터미널 상태에 도달했다고 표시되면 EngineOrderStatusEvent를 쏠 수 있음
        // Reject/Cancel/Filled 같은 최종 이벤트 발행
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
        }
    }

    std::vector<EngineEvent> RealOrderEngine::pollEvents()
    {
        assertOwner_();

        // 내부 outbox(events_)를 "소진시킴"한다.
        // - 한 번 호출하면 지금까지 쌓인 이벤트를 모두 가져와서 내부 큐는 비워짐.
        std::vector<EngineEvent> out;
        out.reserve(events_.size());
        while (!events_.empty())
        {
            out.emplace_back(std::move(events_.front()));
            events_.pop_front();
        }
        return out;
    }

    std::optional<core::Order> RealOrderEngine::get(std::string_view order_id) const
    {
        assertOwner_();
        return store_.get(order_id);
    }

    bool RealOrderEngine::validateRequest(const core::OrderRequest& req, std::string& reason) noexcept
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
        // - 시장가(Market):
        //   * 매수(BID): AmountSize (총 KRW)
        //   * 매도(ASK): VolumeSize (수량)
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

        // 값 범위 체크(0 이하는 거래불가)
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

    void RealOrderEngine::pushEvent_(EngineEvent ev)
    {
        // 내부 전용 이벤트(outbox)에 저장한다.
        // - 엔진 내부는 단일스레드로 events_ 접근은 항상 스레드 1개만 돌아가므로 락 불필요하다.
        // - 복잡한 정책(큐 크기 제한/압축/통합)이 필요하면 여기 추가하거나 하되 지금 간단하다.
        events_.emplace_back(std::move(ev));
    }

    std::string RealOrderEngine::extractCurrency(std::string_view market)
    {
        // "KRW-BTC" -> "BTC"
        const auto pos = market.find('-');
        if (pos == std::string_view::npos)
            return std::string(market);
        return std::string(market.substr(pos + 1));
    }
}
