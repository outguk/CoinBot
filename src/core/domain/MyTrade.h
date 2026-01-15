#pragma once

#include <string>
#include <cstdint>
#include <optional>

#include "Types.h"       // Price, Volume, Amount
#include "OrderTypes.h"  // OrderPosition(BID/ASK)

namespace core
{
    /*
     * MyTrade (Execution / Fill)
     *
     * - Upbit WebSocket "myOrder" 메시지 중 state == "trade" 일 때,
     *   "체결 1건"을 도메인 이벤트로 분리한 형태.
     *
     * - 주의: executed_volume/remaining_volume/paid_fee/locked 같은 "주문 누적 스냅샷"은
     *   MyTrade가 아니라 core::Order(주문 상태 캐시) 쪽에서 관리하는 편이 의미가 명확하다.
     */
    struct MyTrade
    {
        // --- 키(중복 제거/정렬의 핵심) ---
        std::string order_id;     // myOrder.uuid (주문 UUID)
        std::string trade_id;     // myOrder.trade_uuid (체결 UUID)  ← fill의 고유키

        // --- 시장/방향 ---
        std::string market;       // myOrder.code (ex: "KRW-BTC")
        OrderPosition side{ OrderPosition::BID }; // myOrder.ask_bid

        // --- 체결 값 (state=trade에서 price/volume은 체결 값 의미) ---
        Price  price{ 0.0 };
        Volume volume{ 0.0 };

        // 체결 금액(편의 캐시): funds = price * volume
        // - 매수: KRW 지출(수수료 별도)
        // - 매도: KRW 수입(수수료 별도)
        Amount executed_funds{ 0.0 };

        // 체결 1건 수수료: myOrder.trade_fee (state=trade일 때만 유효)
        Amount fee{ 0.0 };

        // maker/taker(선택): myOrder.is_maker (state=trade일 때만 유효)
        std::optional<bool> is_maker;

        // 체결 시각(ms): myOrder.trade_timestamp
        std::int64_t trade_timestamp_ms{ 0 };

        // 주문 추적용 식별자(선택): myOrder.identifier
        std::optional<std::string> identifier;

        // 로컬 메타(선택): OrderStore의 주문 메타(전략/태그)를 join해서 넣고 싶다면 optional로
        std::optional<std::string> strategy_id;
        std::optional<std::string> client_tag;
    };

    inline MyTrade makeMyTradeFromFill(
        std::string order_id,
        std::string trade_id,
        std::string market,
        OrderPosition side,
        Price price,
        Volume volume,
        Amount fee,
        std::int64_t trade_timestamp_ms
    ) noexcept
    {
        MyTrade t;
        t.order_id = std::move(order_id);
        t.trade_id = std::move(trade_id);
        t.market = std::move(market);
        t.side = side;
        t.price = price;
        t.volume = volume;
        t.executed_funds = Amount{ price * volume };
        t.fee = fee;
        t.trade_timestamp_ms = trade_timestamp_ms;
        return t;
    }
}
