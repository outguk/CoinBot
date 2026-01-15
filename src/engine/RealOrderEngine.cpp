#include "RealOrderEngine.h"

#include <variant>
#include <algorithm>

namespace engine
{
    RealOrderEngine::RealOrderEngine(IPrivateOrderApi& api, OrderStore& store, core::Account& account)
        : api_(api), store_(store), account_(account)
    {
    }

    EngineResult RealOrderEngine::submit(const core::OrderRequest& req)
    {
        // 1) 실거래는 “잘못된 주문”이 곧 손실/장애로 이어질 수 있어서 강하게 검증
        std::string reason;
        if (!validateRequest(req, reason))
            return EngineResult::Fail(EngineErrorCode::OrderRejected, reason);

        // 2) REST로 실제 주문 전송(POST /orders)
        const auto uuid = api_.getOrderId(req);
        if (!uuid.has_value() || uuid->empty())
            return EngineResult::Fail(EngineErrorCode::InternalError, "placeOrder failed");

        // 3) 로컬 주문 캐시에 최소 스냅샷 저장
        // - 이 시점에는 executed/fee/locked 같은 값이 확정이 아닐 수 있음
        // - 진짜 값은 WS/REST 스냅샷에서 갱신됨
        core::Order o{};
        o.id = *uuid;

        // - 추적/디버깅을 위해 Order.identifier에 client_order_id를 매핑
        o.identifier = req.identifier.empty()
            ? std::nullopt
            : std::optional<std::string>(req.identifier);

        o.market = req.market;
        o.position = req.position;
        o.type = req.type;

        // 지정가는 price가 있고, 시장가는 보통 price가 없음(시장가 매수는 AmountSize로 처리)
        o.price = req.price;

        // volume은 “수량 기반”일 때만 채움
        // - 시장가 매수(AmountSize)는 체결 전 수량을 모를 수 있어 nullopt가 자연스러움
        if (std::holds_alternative<core::VolumeSize>(req.size))
            o.volume = std::get<core::VolumeSize>(req.size).value;
        else
            o.volume = std::nullopt;

        o.status = core::OrderStatus::Pending;
        o.created_at = "";

        // 실거래에선 “uuid 중복”은 거의 없지만, 방어적으로 upsert를 써도 됨
        store_.upsert(o);

        return EngineResult::Success(std::move(o));
    }

    bool RealOrderEngine::markTradeOnce(std::string_view trade_id)
    {
        // WS는 동일 trade가 중복으로 들어올 수 있어서 “1회만 처리”가 필수
        std::lock_guard<std::mutex> lk(mtx_);

        if (trade_id.empty()) return true; // trade_id가 비었으면 dedupe 불가 → 일단 통과(정책 선택)
        auto [it, inserted] = seen_trades_.emplace(trade_id);
        return inserted; // true면 처음 본 trade_id
    }

    void RealOrderEngine::onMyTrade(const core::MyTrade& t)
    {
        // 0) trade_uuid 중복 방지(중복이면 계좌/포지션이 2번 바뀌는 대형 사고 발생)
        if (!markTradeOnce(t.trade_id))
            return;

        // 1) 주문 누적값 갱신(가능한 범위에서만)
        if (auto ordOpt = store_.get(t.order_id); ordOpt.has_value())
        {
            auto o = *ordOpt;

            o.market = t.market;

            // trade가 왔다는 건 최소 Open 상태 이상으로 보는 게 자연스러움
            if (o.status == core::OrderStatus::Pending || o.status == core::OrderStatus::New)
                o.status = core::OrderStatus::Open;

            // 체결 누적
            o.executed_volume += t.volume;
            o.trades_count += 1;

            // 남은 수량은 “원 주문 수량이 있을 때만” 계산 가능
            if (o.volume.has_value())
            {
                const double rem = std::max(0.0, o.volume.value() - o.executed_volume);
                o.remaining_volume = rem;
            }

            // 이번 체결 수수료를 누적(정확한 fee/locked는 스냅샷이 더 권위 있음)
            o.paid_fee += t.fee;

            store_.update(o);
        }

        // 2) 계좌/포지션 로컬 캐시 반영(best-effort)
        // - 최종 진실값은 /accounts나 WS 스냅샷의 locked/fee 값과 맞춰야 함
        std::lock_guard<std::mutex> lk(mtx_);

        const std::string currency = extractCurrency(t.market);

        if (t.side == core::OrderPosition::BID)
        {
            // 매수 체결: KRW 감소(체결금액 + 수수료), 코인 증가
            const core::Amount krw_out = t.executed_funds + t.fee;
            account_.krw_free = std::max<core::Amount>(0.0, account_.krw_free - krw_out);

            auto it = std::find_if(account_.positions.begin(), account_.positions.end(),
                [&](const core::Position& p) { return p.currency == currency; });

            if (it == account_.positions.end())
            {
                core::Position p{};
                p.currency = currency;
                p.balance = t.volume;
                p.avg_buy_price = t.price;
                p.unit_currency = "KRW";
                account_.positions.push_back(p);
            }
            else
            {
                // 간단 가중평균 매수가 갱신(초기 구현치)
                const double old_qty = it->balance;
                const double new_qty = old_qty + t.volume;
                if (new_qty > 0.0)
                {
                    const double old_cost = it->avg_buy_price * old_qty;
                    const double add_cost = t.price * t.volume;
                    it->avg_buy_price = (old_cost + add_cost) / new_qty;
                }
                it->balance = new_qty;
            }
        }
        else
        {
            // 매도 체결: KRW 증가(체결금액 - 수수료), 코인 감소
            const core::Amount krw_in = std::max<core::Amount>(0.0, t.executed_funds - t.fee);
            account_.krw_free += krw_in;

            auto it = std::find_if(account_.positions.begin(), account_.positions.end(),
                [&](const core::Position& p) { return p.currency == currency; });

            if (it != account_.positions.end())
            {
                it->balance -= t.volume;
                if (it->balance <= 0.0)
                    account_.positions.erase(it);
            }
        }
    }

    void RealOrderEngine::onOrderStatus(std::string_view order_id, core::OrderStatus s)
    {
        // 상태 전이는 이벤트 기반으로 “확정 처리”하는 게 안전
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
        // WS/REST가 준 스냅샷은 “권위 있는 값”으로 보고 동기화
        // - 재연결/유실/순서 뒤집힘에도 안정적으로 복구 가능
        auto ordOpt = store_.get(snapshot.id);
        if (!ordOpt.has_value())
        {
            store_.upsert(snapshot);
            return;
        }

        auto o = *ordOpt;

        // 변하지 않는 속성들(없으면 기존값 유지)
        if (!snapshot.market.empty()) o.market = snapshot.market;
        o.position = snapshot.position;
        o.type = snapshot.type;

        if (snapshot.price.has_value())  o.price = snapshot.price;
        if (snapshot.volume.has_value()) o.volume = snapshot.volume;

        // 누적/자금/상태는 스냅샷이 더 정확하므로 그대로 덮어씀
        o.executed_volume = snapshot.executed_volume;
        o.remaining_volume = snapshot.remaining_volume;
        o.trades_count = snapshot.trades_count;

        o.reserved_fee = snapshot.reserved_fee;
        o.remaining_fee = snapshot.remaining_fee;
        o.paid_fee = snapshot.paid_fee;
        o.locked = snapshot.locked;

        o.status = snapshot.status;

        // identifier는 “이미 있으면 유지, 없으면 채움”(추적 안정성)
        if (!o.identifier.has_value() && snapshot.identifier.has_value())
            o.identifier = snapshot.identifier;

        if (!snapshot.created_at.empty())
            o.created_at = snapshot.created_at;

        store_.update(o);
    }

    std::optional<core::Order> RealOrderEngine::get(std::string_view order_id) const
    {
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

        // 실거래(Upbit) 기준 정책:
        // - 지정가(Limit): BID/ASK 모두 price + volume(VolumeSize)
        // - 시장가(Market):
        //    * BID(시장가 매수): AmountSize (총 KRW)
        //    * ASK(시장가 매도): VolumeSize (수량)
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

    std::string RealOrderEngine::extractCurrency(std::string_view market)
    {
        // "KRW-BTC" -> "BTC"
        const auto pos = market.find('-');
        if (pos == std::string_view::npos)
            return std::string(market);
        return std::string(market.substr(pos + 1));
    }
}
