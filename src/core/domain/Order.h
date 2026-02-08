// core//domain/Order.h
#pragma once

#include <string>
#include <optional>
#include <cstdint>

#include "OrderTypes.h"
#include "Types.h"

namespace core 
{

	/*
	* Order - 주문 상태 추적 객체
	*
	* [역할]
	* OrderEngine이 주문 생애주기(상태/체결/수수료/시간)를 추적하는 도메인 모델
	* 전략의 OrderRequest를 받아 실제 거래소 주문으로 변환된 후 생성됨
	*
	* [생애주기]
	* 1. 엔진이 submit(OrderRequest)를 받음
	* 2. 유효성 검사 후 Order 생성 (id/status/timestamp 할당)
	* 3. WebSocket 이벤트로 상태 업데이트 (체결량/잔여량/수수료)
	* 4. OrderStore에서 관리 (활성 주문 조회/완료 주문 정리)
	*/

	struct Order 
	{

		// --- 식별 ---
		std::string			market;					// 거래 상품 정보
		std::optional<std::string> identifier;		// 클라이언트 지정 식별자, 프로그램에서 부여. OrderRequest에서 보낸 것의 복사본

		// --- 기본 속성(변하지 않는 성격) ---
		std::string			id;						// 내부 주문 ID, 업비트에서 부여 (UUID 등)
		OrderPosition		position;				// 매수/매도 구분
		OrderType			type;					// 주문 타입 (시장가/지정가)

		// --- 요청 값(있을 수도/없을 수도) ---
		std::optional<Price>	price;					// 주문 가격
		std::optional<Volume>	volume;					// 주문 수량

		// --- 부분체결 추적(실거래 핵심) ---
		Volume				executed_volume{ 0.0 };     // 누적 체결량
		Volume				remaining_volume{ 0.0 };	// 잔여량
		int					trades_count{ 0 };          // 체결 건수(있으면 추적/검증에 도움)

		// --- 비용/자금 묶임(실거래 핵심) ---
		Amount				reserved_fee{ 0.0 };        // 예약 수수료
		Amount				paid_fee{ 0.0 };			// 이미 지불한 수수료
		Amount				remaining_fee{ 0.0 };		// 남은 수수료
		Amount				locked{ 0.0 };				// 주문으로 인해 묶인 금액(예: KRW 또는 코인)

		Amount				executed_funds{ 0.0 };		// 누적 체결 금액(Price*Volume 누적)

		// --- 상태(시간에 따라 변함) ---
		OrderStatus				status;					// 주문 상태
		std::string				created_at;				// 우선 문자열로
		//std::int64_t			created_at{ 0 };        // 비교/정렬 가능한 UTC

		// 편의 함수(선택): 엔진/전략의 판단을 단순화
		// 편의 함수
		bool isOpen() const noexcept
		{
			return status == OrderStatus::New
				|| status == OrderStatus::Open
				|| status == OrderStatus::Pending;
		}
		bool isDone() const noexcept
		{
			return status == OrderStatus::Filled
				|| status == OrderStatus::Canceled
				|| status == OrderStatus::Rejected;
		}
	};

}

