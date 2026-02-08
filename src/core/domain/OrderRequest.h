// core/domain/OrderRequest.h
#pragma once

#include <string>
#include <optional>
#include <variant>

#include "Types.h"
#include "OrderTypes.h"

namespace core
{
	/*
	* 전략(Strategy)이 엔진(OrderEngine)에 전달하는 "주문 의도" 객체
	* 전략은 시장 데이터를 보고 의도한 OrderRequest를 만든다.
	*/
	struct VolumeSize {
		Volume value{};
	};

	struct AmountSize {
		Amount value{};
	};

	using OrderSize = std::variant<VolumeSize, AmountSize>; // 수량(Volume) 또는 금액(Amount)

	struct OrderRequest
	{
		std::string		market;				// 마켓 코드 (ex: "KRW-BTC")
		OrderPosition	position;			// BID - 매수 / ASK - 매도
		OrderType		type;				// Market - 시장가 / Limit - 지정가

		OrderSize		size;				// 주문 크기 (수량 또는 금액)

		// 지정가 주문인 경우에만 가격 지정
		std::optional<Price>	price;		// 지정가 주문 가격

		// 동시 주문 / 디버깅을 위한 추적 정보
		std::string		strategy_id;		// 이 주문을 생성한 전략
		std::string		identifier;			// 클라이언트 주문 ID, 프로그램에서 부여 (동시 주문 구분)
		std::string		client_tag;			// 로그 / 추적용 태그

	};

	// ---- 유효성 체크 가이드 ----
	// - BID(매수)는 Amount를 기대 (Amount 기준)
	// - ASK(매도)는 Volume을 기대 (Volume 기준)
	// - 예외적으로 다른 조합을 허용할지는 엔진 정책에서 결정
}