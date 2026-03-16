// api/upbit/WsMessageParser.cpp

#include "api/upbit/WsMessageParser.h"

#include <charconv>

#include <json.hpp>

#include "api/upbit/dto/UpbitQuotationDtos.h"
#include "api/upbit/dto/UpbitWsDtos.h"
#include "api/upbit/mappers/CandleMapper.h"
#include "api/upbit/mappers/MyOrderMapper.h"
#include "util/Logger.h"

namespace {

    std::optional<int> parseMinuteCandleUnit(std::string_view type) noexcept
    {
        constexpr std::string_view prefix = "candle.";
        if (!type.starts_with(prefix) || type.size() <= prefix.size() + 1)
            return std::nullopt;

        if (type.back() != 'm')
            return std::nullopt;

        const std::string_view number = type.substr(prefix.size(), type.size() - prefix.size() - 1);
        int unit = 0;
        const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), unit);
        if (ec != std::errc{} || ptr != number.data() + number.size() || unit <= 0)
            return std::nullopt;

        return unit;
    }

} // anonymous namespace

namespace api::upbit::ws {

std::vector<WsOrderEvent> parseMyOrder(std::string_view json, std::string_view market)
{
    auto& logger = util::Logger::instance();

    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded())
    {
        logger.error("[WsParser][", market, "] myOrder JSON parse failed");
        return {};
    }

    api::upbit::dto::UpbitMyOrderDto dto{};
    try
    {
        dto = j.get<api::upbit::dto::UpbitMyOrderDto>();
    }
    catch (const std::exception& e)
    {
        logger.error("[WsParser][", market, "] myOrder dto convert failed: ", e.what());
        return {};
    }

    // toEvents()는 항상 최소 1개의 Order를 반환하므로,
    // empty는 JSON/DTO 파싱 실패를 의미한다. mapper 계약 변경 시 같이 점검 필요.
    const auto events = api::upbit::mappers::toEvents(dto);
    return std::vector<WsOrderEvent>(events.begin(), events.end());
}

std::optional<WsCandleResult> parseCandle(
    std::string_view json, int configured_fallback_unit, std::string_view market)
{
    auto& logger = util::Logger::instance();

    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded())
    {
        logger.error("[WsParser][", market, "] MarketData JSON parse failed");
        return std::nullopt;
    }

    const std::string type = j.value("type", "");
    if (type.rfind("candle", 0) != 0)
        return std::nullopt;  // non-candle 정상 경로: silent drop

    const auto parsed_unit = parseMinuteCandleUnit(type);
    const int unit = parsed_unit.value_or(configured_fallback_unit);
    if (!parsed_unit.has_value())
    {
        logger.warn("[WsParser][", market,
            "] candle unit parse failed, type=", type, " fallback=", configured_fallback_unit);
    }

    api::upbit::dto::CandleDto_Minute candleDto{};
    try
    {
        candleDto = j.get<api::upbit::dto::CandleDto_Minute>();
    }
    catch (const std::exception& e)
    {
        logger.error("[WsParser][", market, "] candle dto convert failed: ", e.what());
        return std::nullopt;
    }

    return WsCandleResult{api::upbit::mappers::toDomain(candleDto), unit};
}

} // namespace api::upbit::ws
