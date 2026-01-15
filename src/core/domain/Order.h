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
	* 주문 엔진	
	* 엔진이 “주문 생애주기(상태/체결/수수료/시간)”를 추적하는 관리 객체(현재 진행형으로 업데이트)
	* 주문이	생성되고, 체결되고, 취소되는 전체 흐름을 관리
	* 
	* 엔진은 전략의 OrderRequest를 받아서 진짜 주문 생애주기를 책임
	* 
	* 엔진이 submit(OrderRequest)를 받으면
	* 유효성 검사(잔고/보유수량/최소단위)
	* 필요 시 Amount→Volume 환산(매수)
	* Order를 생성하고 id/status/timestamp를 채움
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

