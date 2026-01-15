// core/domain/Types.h
#pragma once

namespace core 
{
	// 고유 타입 정의
	using Price = double;		// 가격(코인의 가격)

	// 매도는 Volume 기준이 안정적
	using Volume = double;		// 수량(코인이 몇개냐)

	// Amount는 리스크 관리의 언어
	using Amount = double;		// Prcie * Quantity -> 거래 금액(얼마를 사고 팔았냐)
}
