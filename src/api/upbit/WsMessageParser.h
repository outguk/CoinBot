#pragma once

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "core/domain/Candle.h"
#include "core/domain/MyTrade.h"
#include "core/domain/Order.h"

namespace api::upbit::ws {

    using WsOrderEvent = std::variant<core::Order, core::MyTrade>;

    struct WsCandleResult {
        core::Candle candle;
        int unit_minutes;
    };

    // 파싱 실패 시 empty 반환 (내부에서 logger.error 기록)
    std::vector<WsOrderEvent> parseMyOrder(
        std::string_view json, std::string_view market = "");

    // non-candle → silent nullopt / 파싱 실패 → logger.error + nullopt
    std::optional<WsCandleResult> parseCandle(
        std::string_view json, int configured_fallback_unit,
        std::string_view market = "");

} // namespace api::upbit::ws
