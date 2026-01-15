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
		std::string				id = "inguk";			// 계좌 ID

		// 원화 잔고(가용/잠김) - KRW행에서 오는 부분 따로 때냄
		Amount krw_free{ 0 };
		Amount krw_locked{ 0 };

		std::vector<Position>	positions;				// 보유 포지션(코인) 목록

		Amount					totalAssetValue = 0;	// 총 자산 가치 (예: 잔고 + 미체결 주문 금액)
	};
}
