// engine/input/EngineInput.h
#pragma once

#include <string>
#include <variant>

namespace engine::input
{
    // WS에서 받은 원문(myOrder)
    struct MyOrderRaw
    {
        std::string json;
    };

    // WS에서 받은 원문(candle 등 마켓데이터) - 지금은 candle만 예시로 둠
    struct MarketDataRaw
    {
        std::string json;
    };

    // WS 재연결 등으로 인한 계좌 동기화 요청
    struct AccountSyncRequest {};

    using EngineInput = std::variant<MyOrderRaw, MarketDataRaw, AccountSyncRequest>;
}
