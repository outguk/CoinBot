#pragma once

#include <string>

#include "../src/api/upbit/dto/UpbitQuotationDtos.h"
#include "../src/core/domain/Candle.h"
#include "TimeFrameMapper.h"

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

	// 캔들 + 타임프레임
	/*inline core::CandleWithTimeFrame toDomainWithTimeFrame(
		const dto::CandleDto_Minute& d,
		int unit) noexcept
	{
		core::CandleWithTimeFrame out;
		out.candle = toDomain(d);
		out.unit_minutes = unit;
		out.c_info = toTimeFrameFromMinuteUnit(unit);
		return out;
	}*/
}
