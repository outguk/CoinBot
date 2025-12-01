// core/domain/Position.h
#pragma once

#include <string>
#include "Types.h"
#include "Instrument.h"

namespace core 
{

	/*
	* 현재의 포지션
	* 보유 중인 코인의 수량과 평단가
	* 체결한 거래의 누적 상태
	*/

	struct Position 
	{
		Instrument	instrument;		// 거래 상품 정보
		Volume		volume;			// 보유 수량 
		Amount		averagePrice;	// 평단가 (원화 기준)
	};


}