#include "RealOrderEngine.h"

#include <variant>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <sstream>
#include <iomanip>

namespace engine
{
    RealOrderEngine::RealOrderEngine(PrivateOrderApi& api, OrderStore& store, core::Account& account)
        : api_(api), store_(store), account_(account)
    {
    }

    void RealOrderEngine::bindToCurrentThread()
    {
        // 엔진 루프 스레드에서 1회 호출해서 "소유 스레드"를 고정한다.
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
            std::cerr << "[Fatal] RealOrderEngine called from non-owner thread\n";
            std::terminate();
        }
#endif
    }

    EngineResult RealOrderEngine::submit(const core::OrderRequest& req)
    {
        assertOwner_();

        // 1) 주문 요청 검증(실거래 안전성)
        std::string reason;
        if (!validateRequest(req, reason))
            return EngineResult::Fail(EngineErrorCode::OrderRejected, reason);

        // 2) 실제 주문 생성 POST(/v1/orders) 수행 → uuid 획득
        const auto uuid = api_.getOrderId(req);
        if (!uuid.has_value() || uuid->empty())
            return EngineResult::Fail(EngineErrorCode::InternalError, "placeOrder failed");

        // 3) 로컬 스토어에 최소 스냅샷 저장(Pending)
        // - executed/fee/locked 등 정확한 값은 WS/REST 스냅샷이 권위가 더 높음
        core::Order o{};
        o.id = *uuid;

        // 추적/디버깅을 위해 client_order_id(프로그램 내부 식별자)를 identifier로 보관
        o.identifier = req.identifier.empty()
            ? std::nullopt
            : std::optional<std::string>(req.identifier);

        o.market = req.market;
        o.position = req.position;
        o.type = req.type;
        o.price = req.price;

        // volume은 "수량 기반" 주문일 때만 알 수 있음.
        // 시장가 매수(AmountSize)는 체결 전 수량이 확정되지 않으므로 nullopt가 자연스럽다.
        if (std::holds_alternative<core::VolumeSize>(req.size))
            o.volume = std::get<core::VolumeSize>(req.size).value;
        else
            o.volume = std::nullopt;

        o.status = core::OrderStatus::Pending;
        o.created_at = "";

        // 실거래에서 uuid 중복은 거의 없지만, 방어적으로 upsert해도 무방
        store_.upsert(o);

        return EngineResult::Success(std::move(o));
    }

    std::string RealOrderEngine::makeTradeDedupeKey_(const core::MyTrade& t)
    {
        // 24시간 운영에서 WS 재전송/리플레이/재연결로 동일 체결이 중복 유입될 수 있다.
        // 이상적인 dedupe 키는 trade_uuid(trade_id) 이지만,
        // 일부 케이스(특히 시장가/취소 경계)에서 trade_id가 비어 들어올 수 있으므로
        // "충분히 유니크한 대체 키"를 만들어 중복 반영을 방지

        if (!t.trade_id.empty())
            return t.trade_id;

        std::ostringstream oss;
        oss.imbue(std::locale::classic()); // locale 영향 제거(소수점 콤마 방지)
        oss << "FALLBACK|"
            << t.order_id << '|'
            << static_cast<int>(t.side) << '|'
            << t.market << '|'
            << std::fixed << std::setprecision(12)
            << t.price << '|'
            << t.volume << '|'
            << t.executed_funds << '|'
            << t.fee;

        // identifier가 있으면 충돌 가능성을 더 낮춘다
        if (t.identifier.has_value() && !t.identifier->empty())
            oss << '|' << *t.identifier;

        return oss.str();
    }

    bool RealOrderEngine::markTradeOnce(std::string_view trade_id)
    {
        // WS는 동일 trade_id를 중복으로 전달할 수 있다(재전송/리플레이/연결 이슈 등).
        // 중복 반영되면 계좌/포지션이 2번 업데이트되는 대형 사고가 발생할 수 있으므로 1회 처리만 허용.
        if (trade_id.empty())
            return false; // trade_id가 비어 있으면 dedupe 불가 → 정책상 일단 통과(아래 문제점 섹션에서 개선 제안)

        auto [it, inserted] = seen_trades_.emplace(trade_id);
        if (!inserted)
            return false; // 이미 처리한 trade_id -> 무시

        // 2) 새 trade_id면 FIFO에도 기록
        // (set의 문자열을 복사하기보다, emplace가 저장한 값을 사용하면 불필요한 재할당을 줄일 수 있음)
        seen_trade_fifo_.push_back(*it);

        // 3) 상한 초과 시 가장 오래된 id부터 제거
        while (seen_trade_fifo_.size() > kMaxSeenTrades)
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

        // 1) 주문 누적값 업데이트 + fill 이벤트 발행을 위한 identifier 확보
        // - WS 메시지에 identifier가 있으면 우선 사용
        // - 없으면 OrderStore에 저장된 주문(identifier)로 fallback
        std::optional<std::string> id = t.identifier;

        if (auto ordOpt = store_.get(t.order_id); ordOpt.has_value())
        {
            auto o = *ordOpt;

            o.market = t.market;

            // 체결이 발생했다면 Pending/New 상태에서 Open으로 넘어가는 것이 자연스러움(정책)
            if (o.status == core::OrderStatus::Pending || o.status == core::OrderStatus::New)
                o.status = core::OrderStatus::Open;

            // 누적 체결 반영
            o.executed_volume += t.volume;
            o.trades_count += 1;

            // 원 주문 수량(o.volume)이 있는 경우에만 remaining 계산 가능
            if (o.volume.has_value())
            {
                const double rem = std::max(0.0, o.volume.value() - o.executed_volume);
                o.remaining_volume = rem;
            }

            // 수수료 누적(정확한 fee/locked는 스냅샷이 더 권위 있음 → best-effort)
            o.paid_fee += t.fee;

            store_.update(o);

            if (!id.has_value())
                id = o.identifier;
        }

        // 2) EngineFillEvent 발행(부분/복수 체결도 매번 발행 가능)
        // - OrderStore에 주문이 없더라도(identifier만 있다면) 전략 매칭이 가능하므로 발행한다.
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

        // 3) 계좌/포지션 로컬 캐시(best-effort) 갱신
        // - 최종 진실은 WS 스냅샷(/accounts 등)로 맞춰야 함
        const std::string currency = extractCurrency(t.market);

        if (t.side == core::OrderPosition::BID)
        {
            // 매수 체결: KRW 감소(체결대금 + 수수료), 코인 증가
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
                // 간단 가중평균 매수가 갱신(초기 구현)
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
            // 매도 체결: KRW 증가(체결대금 - 수수료), 코인 감소
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

    // 나중에 REST 보정 경로가 생길 때 상태만 갱신하는 통로
    void RealOrderEngine::onOrderStatus(std::string_view order_id, core::OrderStatus s)
    {
        assertOwner_();

        // 상태 전이는 Upbit 이벤트를 기준으로 확정 처리하는 것이 안전
        auto ordOpt = store_.get(order_id);
        if (!ordOpt.has_value()) return;

        auto o = *ordOpt;
        o.status = s;

        if (s == core::OrderStatus::Filled)
            o.remaining_volume = 0.0;

        store_.update(o);
    }

    void RealOrderEngine::onOrderSnapshot(const core::Order& snapshot)
    {
        assertOwner_();

        // WS/REST 스냅샷은 "권위 있는 값"으로 보고 동기화한다.
        // - 유실/재정렬/부분 정보에도 비교적 안정적으로 복구 가능
        auto ordOpt = store_.get(snapshot.id);
        if (!ordOpt.has_value())
        {
            store_.upsert(snapshot);
            return;
        }

        auto o = *ordOpt;
        const core::OrderStatus old_status = o.status;

        // 변경이 의미 있는 필드만 업데이트(비어있는 값은 기존 유지)
        if (!snapshot.market.empty()) o.market = snapshot.market;
        o.position = snapshot.position;
        o.type = snapshot.type;

        if (snapshot.price.has_value())  o.price = snapshot.price;
        if (snapshot.volume.has_value()) o.volume = snapshot.volume;

        // 누적/자금/상태는 스냅샷이 더 정확하므로 그대로 덮어쓴다.
        o.executed_volume = snapshot.executed_volume;
        o.remaining_volume = snapshot.remaining_volume;
        o.trades_count = snapshot.trades_count;

        o.reserved_fee = snapshot.reserved_fee;
        o.remaining_fee = snapshot.remaining_fee;
        o.paid_fee = snapshot.paid_fee;
        o.locked = snapshot.locked;

        o.status = snapshot.status;

        // identifier는 이미 있으면 유지, 없으면 스냅샷 값으로 채움(추적 안정성)
        if (!o.identifier.has_value() && snapshot.identifier.has_value())
            o.identifier = snapshot.identifier;

        if (!snapshot.created_at.empty())
            o.created_at = snapshot.created_at;

        store_.update(o);

        // 스냅샷에 주문이 터미널 상태에 도달했다고 표시되면 EngineOrderStatusEvent를 한 번 방출
        // Reject/Cancel/Filled 등의 상태 이벤트 발행
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

        // 엔진 outbox(events_)를 "드레인"한다.
        // - 한 번 호출하면 지금까지 쌓인 이벤트를 모두 꺼내고 내부 큐는 비운다.
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

        // 값 범위 체크(0 이하는 거절)
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
        // 엔진 내부 이벤트(outbox)에 적재한다.
        // - 단일 소유권 구조에서는 events_ 접근도 엔진 스레드 1개로 고정되므로 락 없이 안전하다.
        // - 향후 정책(큐 길이 제한/드롭/통계)이 필요하면 이 지점에서 일괄 적용 가능하다.
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
