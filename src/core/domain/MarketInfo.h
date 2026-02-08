// core/domain/Instrument.h
#pragma once

#include <string>

namespace core 
{
	
	// 종목 정보
	struct MarketInfo
	{
		std::string			 market;			// 시장 코드 (예: "KRW-BTC", "NASDAQ")

		// 거래 통화에 대한 정보
		std::string			ko_name;	// 한글명
		std::string			en_name;	// 영문명

		bool				is_warning = false;		// 유의 종목 여부
	};

}
