// core/domain/OrderTypes.h
#pragma once

// 주문 방향/타입/상태 등에 대한 Enum 정의
namespace core 
{

	// 주문 시 사용하는 매수/매도 구분 Enum
	enum class OrderPosition 
	{
		ASK,
		BID
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

	enum class PriceChangeState {
		Even,
		Rise,
		Fall
	};

	// [의도]
// - 로그/디버깅에서 enum 값을 사람이 읽을 수 있게 출력
// - 도메인 로직에는 영향 없음 (표현 계층)
	inline const char* to_string(OrderStatus s) noexcept {
		switch (s) {
		case OrderStatus::New:             return "New";
		case OrderStatus::Open:            return "Open";
		case OrderStatus::Pending:			return "Pending";
		case OrderStatus::Filled:          return "Filled";
		case OrderStatus::Canceled:        return "Canceled";
		case OrderStatus::Rejected:        return "Rejected";
		default:                           return "Unknown";
		}
	}

}
