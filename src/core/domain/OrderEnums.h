// core/domain/OrderEnums.h
#pragma once


namespace core 
{

	// 주문 시 사용하는 매수/매도 구분 Enum
	enum class OrderPosition 
	{
		BUY,
		SELL
		// 추후 추가
	};

	// 주문 타입 Enum
	enum class OrderType 
	{
		Market,		// 시장가
		Limit		// 지정가
	};

	enum class OrderStatus 
	{
		New,		// 신규 주문
		Open,		// 미체결(호가 대기) 상태
		Pending,	// 주문 접수됨
		Filled,		// 주문 체결됨
		Canceled,	// 주문 취소됨
		Rejected	// 주문 거부됨
	};


}
