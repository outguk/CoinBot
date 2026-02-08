#pragma once

#include <optional>
#include <string>

#include "core/domain/Order.h"
#include "core/domain/MyTrade.h"
#include "core/domain/Account.h"

/*
* 주문 엔진 처리 결과 객체
* - 주문을 처리한 결과만 책임
* - 계좌·전략·API 상태를 직접 노출하지 않는다
* - Demo / Real 엔진 모두 공용으로 사용 가능
*/

namespace engine
{
	// 주문 엔진 처리 결과 코드
	enum class EngineErrorCode
	{
		None = 0,               // 성공

		// 입력 / 상태 오류
		InvalidOrderId,			// 잘못된 주문 요청 (ID)
		InsufficientFunds,		// 잔고 부족
		MarketNotSupported,		// 지원하지 않는 마켓
		OrderRejected,			// 엔진 내부 정책에 의한 거부

		// 엔진 오류
		InternalError,			// 내부 오류
	};

	// 주문 엔진의 실행 결과
	struct EngineResult
	{
		// -- 기본 상태 --
		bool success{ false };							// 성공 여부
		EngineErrorCode code{ EngineErrorCode::None };	// 실패 사유

		// -- 주문 결과 --
		std::optional<core::Order>		order;			// 주문 처리 후 주문 상태 (있을 경우)
		std::optional<core::MyTrade>	myTrade;		// 주문 체결 후 내 거래 상태 (있을 경우)

		// -- 계좌 스냅샷 --
		std::optional<core::Account>	account;		// 처리 후 계좌 상태 (있을 경우)

		// -- 메시지 (디버그/로그) --
		std::string message;

		// 성공과 실패를 쉽게 생성하는 헬퍼
		// - 의미적으로 올바른 상태를 강제
		// - 엔진 구현 코드가 압도적으로 읽기 쉬워짐

		// -- 성공 결과 헬퍼 --
		static EngineResult Success(
			core::Order order,
			std::optional<core::MyTrade> trade = std::nullopt,
			std::optional<core::Account> account = std::nullopt
		)
		{
			return EngineResult
			{
				true,
				EngineErrorCode::None,
				std::move(order),
				std::move(trade),
				std::move(account),
				{}
			};
		}

		// -- 실패 결과 헬퍼 --
		static EngineResult Fail(
			EngineErrorCode error_code,
			std::string msg = {}
		)
		{
			return EngineResult
			{
				false,
				error_code,
				std::nullopt,
				std::nullopt,
				std::nullopt,
				std::move(msg)
			};
		}
	};
}