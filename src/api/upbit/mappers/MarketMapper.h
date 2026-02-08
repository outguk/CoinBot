#pragma once

#include <string>

#include "../src/api/upbit/dto/UpbitQuotationDtos.h"
#include "../src/core/domain/MarketInfo.h"

namespace api::upbit::mappers
{
	
	inline core::MarketInfo toDomain(const api::upbit::dto::MarketDto& dto)
	{
		core::MarketInfo m;
		m.market = dto.market;
		m.ko_name = dto.korean_name;
		m.en_name = dto.english_name;
		
		/*m.is_warning = dto.market_event.has_value() ? dto.market_event->warning : false;*/

		return m;
	}
}
