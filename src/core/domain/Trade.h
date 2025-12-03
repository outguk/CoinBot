// core/domain/Order.h
#pragma once

#include <string>
#include "MarketInfo.h"
#include "Types.h"
#include "OrderEnums.h"

namespace core 
{

	/*
	* 주문 결과
	* 주문이 실제로 어느 가격·수량으로 체결됐는지
	* 트레이딩의 결과
	*/

	struct Trade 
	{	
		std::string		id;					// 거래 ID
		std::string		orderId;			// 부분 체결될 때 어떤 주문에 속하는지 알기 위한 주문 ID
		Instrument		instrument;			// 거래 상품 정보
		OrderPosition	position;			// 매수/매도 구분
		Price			price;				// 거래 가격
		Volume			volume;				// 거래 수량
		Amount			amount;				// 거래 금액 (price * quantity)
	};
}