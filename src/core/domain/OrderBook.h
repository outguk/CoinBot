// core/domain/OrderBook.h
#pragma once

#include <vector>
#include "Instrument.h"
#include "Types.h"

namespace core {

    /*
    * 실시간 “호가 정보(매수/매도 대기 주문)
    */

	// 개별 호가 레벨 구조체(호가 하나의 정보)
    struct OrderBookLevel {
		Price  price;                           // 호가 가격
		Volume volume;                          // 해당 가격대의 누적(쌓인) 수량
    };

	// 여러 개의 OrderBookLevel을 포함하는 호가 정보 구조체
    struct OrderBook {
		Instrument                  instrument; // 종목 정보
        std::vector<OrderBookLevel> bids;       // 매수 호가 (가격 높은 순서대로 정렬)
		std::vector<OrderBookLevel> asks;       // 매도 호가 (가격 낮은 순서대로 정렬)
    };

} // namespace core
