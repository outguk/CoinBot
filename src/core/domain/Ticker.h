// core/domain/Ticker.h
#pragma once

#include <chrono>
#include "Instrument.h"
#include "Types.h"

namespace core {

    /*
	* 실시간 종목 정보 (Ticker)
	* 추후 추가로 정보 보완 예정
    */

    struct Ticker {
		Instrument instrument;     // 종목 정보

        // 추후 추가
        Price      tradePrice;     // 최근 체결 가격
        Volume     accVolume24h;   // 24시간 누적 거래량
        Amount     accAmount24h;   // 24시간 누적 거래대금
        double     changeRate;     // 등락률 (예: 0.0123 => +1.23%)

        std::chrono::system_clock::time_point timestamp; // 정보가 들어온 시각
    };

} // namespace core
