#pragma once

#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <utility>

#include "core/domain/Order.h"
#include "core/domain/MyTrade.h"
#include "api/upbit/dto/UpbitWsDtos.h" // 네가 만든 DTO 경로에 맞춰 조정

namespace api::upbit::mappers
{
    // "이벤트 스트림" 타입
    using MyOrderEvent = std::variant<core::Order, core::MyTrade>;

    // ---------- small helpers ----------
    inline core::OrderPosition toSide(const std::string& ask_bid) noexcept
    {
        // Upbit: "ASK"=매도, "BID"=매수
        return (ask_bid == "ASK") ? core::OrderPosition::ASK : core::OrderPosition::BID;
    }

    inline core::OrderType toOrderType(const std::string& order_type) noexcept
    {
        // Upbit: limit | price(시장가매수) | market(시장가매도) | best(최유리)
        // core는 Market/Limit만 있으니 단순화
        if (order_type == "limit" || order_type == "best") return core::OrderType::Limit;
        return core::OrderType::Market;
    }

    inline core::OrderStatus toOrderStatus(
        const std::string& state,
        double remaining_volume) noexcept
    {
        // Upbit state: wait/watch/trade/done/cancel/prevented
        if (state == "wait")  return core::OrderStatus::Open;
        if (state == "watch") return core::OrderStatus::Pending;

        if (state == "trade")
        {
            // trade 이벤트가 왔더라도 남은 물량이 있으면 "부분 체결"
            return (remaining_volume <= 0.0) ? core::OrderStatus::Filled : core::OrderStatus::Open;
        }

        if (state == "done")   return core::OrderStatus::Filled;
        if (state == "cancel") return core::OrderStatus::Canceled;
        if (state == "prevented") return core::OrderStatus::Canceled; // SMP 등으로 취소된 케이스

        // prevented / unknown
        return core::OrderStatus::Rejected;
    }

    // ---------- main mapper ----------
    inline std::vector<MyOrderEvent> toEvents(const api::upbit::dto::UpbitMyOrderDto& d)
    {
        std::vector<MyOrderEvent> out;
        out.reserve(2);

        // (1) 항상 Order 스냅샷 이벤트 생성
        core::Order o;
        o.market = d.code;
        o.id = d.uuid;
        o.position = toSide(d.ask_bid);
        o.type = toOrderType(d.order_type);
        o.status = toOrderStatus(d.state, d.remaining_volume);

        // identifier는 있으면 붙여두면 “재시작 복원/디버깅”에 유리
        if (d.identifier.has_value()) o.identifier = *d.identifier;

        // created_at: core::Order가 string이므로 WS timestamp(ms)를 string으로 저장
        if (d.order_timestamp.has_value())      o.created_at = std::to_string(*d.order_timestamp);
        else if (d.timestamp.has_value())       o.created_at = std::to_string(*d.timestamp);
        else                                    o.created_at.clear();

        // price/volume: Upbit는 state=="trade"일 때 이 값이 “체결가/체결량” 의미로 바뀔 수 있음
        // - 그래도 스냅샷 관점에선 누적(executed/remaining)이 더 중요하니,
        //   여기서는 raw를 넣되, 로직은 executed/remaining 기반으로 판단하는 정책.
        o.price = core::Price{ d.price };
        o.volume = core::Volume{ d.volume };

        // 누적 스냅샷(중요)
        o.executed_volume = core::Volume{ d.executed_volume };
        o.remaining_volume = core::Volume{ d.remaining_volume };
        o.trades_count = d.trades_count;

        o.reserved_fee = core::Amount{ d.reserved_fee };
        o.remaining_fee = core::Amount{ d.remaining_fee };
        o.paid_fee = core::Amount{ d.paid_fee };
        o.locked = core::Amount{ d.locked };
        o.executed_funds = core::Amount{ d.executed_funds };

        out.emplace_back(std::move(o));

        // (2) trade일 때만 MyTrade 이벤트 생성
        // trade_uuid가 있어야 “체결 1건 식별”이 가능하니 이를 기준으로 생성
        if (d.state == "trade" && d.trade_uuid.has_value())
        {
            core::MyTrade t;
            t.order_id = d.uuid;
            t.trade_id = *d.trade_uuid;

            t.market = d.code;
            t.side = toSide(d.ask_bid);

            // trade 상태에서 price/volume은 "체결가/체결량" 의미로 사용
            t.price = core::Price{ d.price };
            t.volume = core::Volume{ d.volume };

            // 1건 체결 금액(명확하게 price*volume)
            t.executed_funds = core::Amount{ d.price * d.volume };

            if (d.trade_fee.has_value()) t.fee = core::Amount{ *d.trade_fee };
            else                         t.fee = core::Amount{ 0.0 };

            t.is_maker = d.is_maker;
            t.identifier = d.identifier;

            if (d.trade_timestamp.has_value())
                t.trade_timestamp_ms = *d.trade_timestamp;

            out.emplace_back(std::move(t));
        }

        return out;
    }
}
