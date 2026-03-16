#pragma once

#include "api/upbit/dto/UpbitQuotationDtos.h"
#include "core/domain/Candle.h"

namespace api::upbit::mappers
{

	inline core::Candle toDomain(const api::upbit::dto::CandleDto_Minute& dto)
	{
		core::Candle c;
		c.market = dto.market;

		// 가격
		c.open_price = dto.opening_price;
		c.high_price = dto.high_price;
		c.low_price = dto.low_price;
		c.close_price = dto.trade_price;

		c.volume = dto.candle_acc_trade_volume;

		c.start_timestamp = dto.candle_date_time_kst;

		return c;
	}
}
