// core/domain/OrderBook.h
#pragma once

#include <string>
#include <vector>
#include <optional>
#include "Types.h"

namespace core {

    /*
    * 실시간 호가 정보(매수/매도 대기 주문)
    */

	// 개별 호가 레벨 구조체(호가 하나의 정보)
    struct OrderbookLevel
	{
		// 매도
		Price	ask_price;		// 매도 호가
		Volume	ask_size;		// 매도 잔량

		// 매수
		Price	bid_price;		// 매수 호가
		Volume	bid_size;		// 매수 잔량
    };

	// 여러 개의 OrderBookLevel을 포함하는 호가 정보 구조체
    struct Orderbook {
		std::string					market;			// 종목 정보
		std::int64_t				timestamp;		// 조회 요청 시각의 타임스탬프(ms)

		// 매수 / 매도 잔량
		Volume						total_ask_size;	// 전체 매도 잔량
		Volume						total_bid_size;	// 전체 매수 잔량

        std::vector<OrderbookLevel> top_levels;     // 각 호가단 가격·잔량 (상위 5단계만)
		std::optional<double> price_unit;			// level=0이면 미지정/기본 단위로 보통 처리
    };

} // namespace core
