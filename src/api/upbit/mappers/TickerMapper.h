#pragma once

#include <string>

#include "../src/api/upbit/dto/UpbitQuotationDtos.h"
#include "../src/core/domain/Ticker.h"

namespace api::upbit::mappers
{

	inline core::Ticker toDomain(const api::upbit::dto::TickerDto& dto)
	{
		core::Ticker t;
		t.market = dto.market;

		// АЁАн
		t.ticker_opening_price = dto.opening_price;
		t.ticker_high_price = dto.high_price;
		t.ticker_low_price = dto.low_price;
		t.ticker_trade_price = dto.trade_price;

		t.prev_closing_price = dto.prev_closing_price;
		t.trade_timestamp = dto.trade_timestamp;

		t.change_price = dto.signed_change_price;
		t.change_rate = dto.signed_change_rate;

		t.trade_volume = dto.trade_volume;
		t.acc_trade_volume = dto.acc_trade_volume;
		t.acc_trade_volume_24h = dto.acc_trade_volume_24h;

		return t;
	}
}
