// core/domain/Account.h
#pragma once

#include <string>
#include <vector>
#include "Position.h"
#include "Types.h"

namespace core 
{
	/*
	* 계좌 정보
	* 사용자의 전체 자산 현황
	* 잔고, 예수금, 총 자산 가치 등
	*/

	struct Account 
	{
		std::string				id;					// 계좌 ID
		std::vector<Position>	positions;			// 보유 포지션(코인) 목록

		// 추후 정리
		double					balance;			// 총 잔고
		double					availableFunds;		// 사용 가능한 예수금
		Amount					totalAssetValue;	// 총 자산 가치 (예: 잔고 + 미체결 주문 금액)
	};
}
