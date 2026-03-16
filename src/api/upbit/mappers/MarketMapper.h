#pragma once

#include "api/upbit/dto/UpbitQuotationDtos.h"
#include "core/domain/MarketInfo.h"

namespace api::upbit::mappers
{
	
	inline core::MarketInfo toDomain(const api::upbit::dto::MarketDto& dto)
	{
		core::MarketInfo m;
		m.market = dto.market;
		m.ko_name = dto.korean_name;
		m.en_name = dto.english_name;
		return m;
	}
}
