// core/domain/Instrument.h
#pragma once

#include <string>

namespace core 
{
	
	// 종목 정보
	struct Instrument 
	{
		std::string market;			// 시장 코드 (예: "KRW-BTC", "NASDAQ")

		// 거래 통화에 대한 정보
		std::string base;			// 기본 통화 (예: "BTC")
		std::string quote;			// 상대 통화 (예: "KRW")

		bool		isActive;		// 거래 가능 여부
		std::string marketwaring;	// 시장 경고 정보
	};

}
