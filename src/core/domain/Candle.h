// core/domain/Candle.h
#pragma once

#include <chrono>
#include "Instrument.h"
#include "Types.h"

namespace core {

    /*
	* 들어오는 캔들(봉) 데이터 구조체
    */

    struct Candle {
		Instrument instrument; // 종목 정보

		Price  open;			// 시가
		Price  high;			// 고가
		Price  low;				// 저가
		Price  close;			// 종가
		Volume volume;			// 거래량

		std::chrono::system_clock::time_point timestamp; // 정보가 들어온 시각
    };

} // namespace core
