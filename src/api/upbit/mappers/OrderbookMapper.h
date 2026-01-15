// api/upbit/mappers/OrderbookMapper.h
#pragma once

#include "../src/core/domain/Orderbook.h"
#include "../src/api/upbit/dto/UpbitQuotationDtos.h"

namespace api::upbit::mappers {

    inline core::Orderbook toDomain(api::upbit::dto::OrderbookDto& dto)
    {
        core::Orderbook ob;

        ob.market = dto.market;
        ob.timestamp = dto.timestamp;

        ob.total_ask_size = dto.total_ask_size;
        ob.total_bid_size = dto.total_bid_size;

        ob.top_levels.reserve(dto.orderbook_units.size());
        for (const auto& u : dto.orderbook_units)
        {
            core::OrderbookLevel lv;
            lv.ask_price = u.ask_price;
            lv.ask_size = u.ask_size;
            lv.bid_price = u.bid_price;
            lv.bid_size = u.bid_size;
            ob.top_levels.push_back(lv);
        }

        // 응답 level은 “가격 단위”로 의미 보관
        if (dto.level > 0.0)
            ob.price_unit = dto.level;
        else
            ob.price_unit.reset();

        return ob;
    }

} // namespace api::upbit::mappers
