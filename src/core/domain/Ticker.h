// core/domain/Ticker.h
#pragma once

#include <chrono>
#include "MarketInfo.h"
#include "Types.h"

namespace core {

    /*
	* 실시간 종목 정보 (Ticker)
	* 추후 추가로 정보 보완 예정
    * 
    * - 조회 요청한 시점의 정보
    * - 해당 변동 지표들은 전일 종가를 기준
    */

    struct Ticker {
        MarketInfo          market;                         // 종목 정보

        // 가격
        Price				ticker_opening_price;			// 해당 페어의 시가
        Price				ticker_high_price;				// 해당 페어의 고가
        Price				ticker_low_price;				// 해당 페어의 저가
        Price				ticker_trade_price;			    // 해당 페어의 종가(최종 체결가)

        // 거래량
        Volume				trade_volume;			// 해당 페어의 최근 거래량

        Price				acc_trade_price;		// 해당 페어의 누적 거래대금 (UTC 0시부터 누적)
        Price				acc_trade_price_24h;	// 해당 페어의 24시간 누적 거래대금
        Volume				acc_trade_volume;		// 해당 페어의 누적 거래량 (UTC 0시부터 누적)
        Volume				acc_trade_volume_24h;	// 해당 페어의 24시간 누적 거래량

        std::chrono::system_clock::time_point timestamp; // 정보가 들어온 시각
    };

} // namespace core
