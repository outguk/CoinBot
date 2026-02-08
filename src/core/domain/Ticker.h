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
        std::string         market;                         // 종목 정보

        // 가격
        Price				ticker_opening_price;			// 해당 페어의 시가
        Price				ticker_high_price;				// 해당 페어의 고가
        Price				ticker_low_price;				// 해당 페어의 저가
        Price				ticker_trade_price;			    // 해당 페어의 종가(최종 체결가)

        Price				prev_closing_price;		        // 전일 종가 (UTC0시 기준)
        std::int64_t		trade_timestamp;		        // 체결 시각의 밀리초단위 타임스탬프

        double				change_price;	                // 부호 있는 가격 변화 (change_price와 동일하지만 부호 존재)
        double				change_rate;		            // 부호 있는 가격 변화율 (change_rate와 동일하지만 부호 존재)

        // 거래량
        Volume				trade_volume;			        // 해당 페어의 최근 거래량
        Volume				acc_trade_volume;		        // 해당 페어의 누적 거래량 (UTC 0시부터 누적)
        Volume				acc_trade_volume_24h;	        // 해당 페어의 24시간 누적 거래량


        std::chrono::system_clock::time_point timestamp; // 정보가 들어온 시각
    };

} // namespace core
