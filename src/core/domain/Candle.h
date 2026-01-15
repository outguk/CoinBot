// core/domain/Candle.h
#pragma once

#include <chrono>
#include <string>
#include "Types.h"

namespace core {

    /*
	* 들어오는 캔들(봉) 데이터 구조체
    */

	enum class TimeFrame
	{
		MIN_3,
		MIN_5,
		MIN_10,
		MIN_15,
		MIN_30,
		MIN_60,
		MIN_240
	};

    struct Candle 
	{
		std::string	market;		// 종목 정보

		Price  open_price;		// 시가
		Price  high_price;		// 고가
		Price  low_price;		// 저가
		Price  close_price;		// 종가

		Volume volume;			// 거래량

		std::string start_timestamp;			// 정보가 들어온 시각
    };

} // namespace core
