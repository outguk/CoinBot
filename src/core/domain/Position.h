// core/domain/Position.h
#pragma once

#include <string>
#include "Types.h"
#include "MarketInfo.h"

namespace core 
{

	/*
	* 현재의 포지션
	* 보유 중인 코인의 수량과 평단가
	* 체결한 거래의 누적 상태
	*/

	struct Position 
	{
		std::string		currency;			// 거래 상품 정보
		double			free;			// 주문 가능 수량 또는 금액
		
		Price			avg_buy_price;		// 매수 평균가

		std::string		unit_currency;		// 평균가 기준 통화 (매수 평균가의 단위 KRW)
	};


}