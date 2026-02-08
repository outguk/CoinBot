// core/domain/Order.h
#pragma once

#include <string>
#include "MarketInfo.h"
#include "Types.h"
#include "OrderTypes.h"

namespace core 
{

	/*
	* 지정한 페어의 최근 체결 목록을 조회
	* 종목의 최근 정보	
	* 
	* - 실시간 매수/매도 흐름 감지
	* - 체결 속도, 연속성 기반 타이밍 포착
	* - 거래량 급증, 매도세 쏠림 등 비정형 조건 감지
	*/

	struct Trade 
	{	
		std::string		market;				// 마켓 코드 (ex: KRW-BTC)
		int				timestamp;			// 체결 시각의 밀리초단위 타임스탬프

		Price			trade_price;		// 거래 가격
		Volume			trade_volume;		// 거래 수량

		OrderPosition	position;			// 매수/매도 구분 (ASK, BID)
	};
}