// /api/ws/dto/UpbitMyOrderDto.h
#pragma once

#include <string>
#include <optional>
#include <cstdint>

#include <json.hpp>

namespace api::upbit::dto
{
    struct UpbitMyOrderDto
    {
        // ---- 식별자 ----
        std::string type;   // "myOrder"
        std::string code;   // market code (e.g. "KRW-BTC")
        std::string uuid;   // order uuid

        // ---- 주문 속성 정보 ----
        std::string ask_bid;     // "ASK" | "BID"
        std::string order_type;  // "limit" | "price" | "market" | "best"
        std::string state;       // "wait" | "watch" | "trade" | "done" | "cancel" | "prevented"

        // ---- price/volume (meaning changes when state == "trade") ----
        double price{ 0.0 };   // order price OR trade price when state="trade"
        double volume{ 0.0 };  // order volume OR trade volume when state="trade"

        // ---- cumulative order snapshot fields ----
        double remaining_volume{ 0.0 };
        double executed_volume{ 0.0 };
        int    trades_count{ 0 };

        double reserved_fee{ 0.0 };
        double remaining_fee{ 0.0 };
        double paid_fee{ 0.0 };
        double locked{ 0.0 };
        double executed_funds{ 0.0 }; // cumulative executed funds

        // ---- trade-only fields (present/meaningful when state == "trade") ----
        std::optional<std::string> trade_uuid;
        std::optional<double>      trade_fee;        // fee for the trade event (null if not trade)
        std::optional<bool>        is_maker;         // null if not trade
        std::optional<std::string> identifier;       // client identifier (may exist even if not trade)
        std::optional<std::int64_t> trade_timestamp; // ms

        // ---- timestamps ----
        std::optional<std::int64_t> order_timestamp; // ms
        std::optional<std::int64_t> timestamp;       // ms (message timestamp)

        // (SMP / TIF 등은 현재 도메인에서 안 쓰면 생략 가능)
        // std::optional<std::string> time_in_force;
        // std::optional<std::string> smp_type;
        // std::optional<double> prevented_volume;
        // std::optional<double> prevented_locked;
        // std::optional<std::string> stream_type;
    };

    // JSON -> DTO
    inline void from_json(const nlohmann::json& j, UpbitMyOrderDto& d)
    {
        // required-ish (Upbit spec)
        j.at("type").get_to(d.type);
        j.at("code").get_to(d.code);
        j.at("uuid").get_to(d.uuid);
        j.at("ask_bid").get_to(d.ask_bid);
        j.at("order_type").get_to(d.order_type);
        j.at("state").get_to(d.state);

        // numeric fields (Upbit sends these as number in JSON examples)
        if (j.contains("price") && !j["price"].is_null()) d.price = j["price"].get<double>();
        if (j.contains("volume") && !j["volume"].is_null()) d.volume = j["volume"].get<double>();

        if (j.contains("remaining_volume") && !j["remaining_volume"].is_null()) d.remaining_volume = j["remaining_volume"].get<double>();
        if (j.contains("executed_volume") && !j["executed_volume"].is_null())   d.executed_volume = j["executed_volume"].get<double>();
        if (j.contains("trades_count") && !j["trades_count"].is_null())         d.trades_count = j["trades_count"].get<int>();

        if (j.contains("reserved_fee") && !j["reserved_fee"].is_null())   d.reserved_fee = j["reserved_fee"].get<double>();
        if (j.contains("remaining_fee") && !j["remaining_fee"].is_null()) d.remaining_fee = j["remaining_fee"].get<double>();
        if (j.contains("paid_fee") && !j["paid_fee"].is_null())           d.paid_fee = j["paid_fee"].get<double>();
        if (j.contains("locked") && !j["locked"].is_null())               d.locked = j["locked"].get<double>();
        if (j.contains("executed_funds") && !j["executed_funds"].is_null()) d.executed_funds = j["executed_funds"].get<double>();

        // optionals
        if (j.contains("trade_uuid") && !j["trade_uuid"].is_null()) d.trade_uuid = j["trade_uuid"].get<std::string>();
        if (j.contains("trade_fee") && !j["trade_fee"].is_null())   d.trade_fee = j["trade_fee"].get<double>();
        if (j.contains("is_maker") && !j["is_maker"].is_null())     d.is_maker = j["is_maker"].get<bool>();
        if (j.contains("identifier") && !j["identifier"].is_null()) d.identifier = j["identifier"].get<std::string>();

        if (j.contains("trade_timestamp") && !j["trade_timestamp"].is_null())
            d.trade_timestamp = j["trade_timestamp"].get<std::int64_t>();
        if (j.contains("order_timestamp") && !j["order_timestamp"].is_null())
            d.order_timestamp = j["order_timestamp"].get<std::int64_t>();
        if (j.contains("timestamp") && !j["timestamp"].is_null())
            d.timestamp = j["timestamp"].get<std::int64_t>();
    }
}
