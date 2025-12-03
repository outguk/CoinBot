// core//domain/Order.h
#pragma once

#include <string>
#include <optional>
#include "OrderEnums.h"
#include "Instrument.h"
#include "Types.h"

namespace core 
{

	/*
	* 주문 생성
	* 사용자가 또는 전략이 “이렇게 사고/팔고 싶다”는 의사
	* 트레이딩의 시작점
	*/

	struct Order 
	{
		std::string			id;						// 내부 주문 ID (UUID 등)
		MarketInfo			market;					// 거래 상품 정보
		OrderPosition		position;				// 매수/매도 구분
		OrderType			type;					// 주문 타입 (시장가/지정가)
		Price				price;					// 주문 가격
		Volume				volume;				// 주문 수량
		OrderStatus			status;					// 주문 상태
		std::optional<Amount> fees;					// 수수료 (있을 경우)

		// 시간은 추후 방식 검토
		std::string			  timestamp;			// 주문 생성 시각 (ISO 8601 형식) 
	};

}

