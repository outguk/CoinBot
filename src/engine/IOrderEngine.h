#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "core/domain/OrderRequest.h"
#include "core/domain/Order.h"
#include "core/domain/Ticker.h"

#include "EngineResult.h"  // Result, RejectReason
#include "EngineEvents.h"

/*
* 5단계(데모 주문 엔진) 기준 최소 엔진  인터페이스
* - 엔진이 외부(app/strategy)로 노출하는 최소 인터페이스
* - 전략/앱은 이 인터페이스만 보고 주문 엔진을 사용한다.
*/

namespace engine
{
	/*
	 * 5단계(데모 주문 엔진) 기준 최종 인터페이스
	 *
	 * 목표:
	 * - 전략/앱은 주문 "의도"(OrderRequest)만 던진다.
	 * - 엔진은 주문을 접수/저장하고, 시장 데이터(onMarket)가 들어올 때 체결을 판단한다.
	 * - 체결/취소/거절 같은 "상태 변화 이벤트"를 EngineResult로 외부에 전달한다.
	 *
	 * 설계 규칙:
	 * - submit()은 "접수" 결과를 반환한다. (New로 저장되거나 Reject)
	 * - 실제 체결(Filled)은 onMarket()에서 발생한다. (데모는 Ticker 기준으로 단순 판단)
	 * - cancel()은 Open(New) 주문만 취소 가능, 취소 이벤트를 반환한다.
	 */

	class IOrderEngine
	{
	public:
		virtual ~IOrderEngine() = default;

		// 주문 접수(저장) 또는 거절
		// - 성공 시: EngineResult.success=true, order=New 상태(또는 즉시 Filled로 만들지 않는 것을 권장)
		// - 실패 시: EngineResult.success=false, error=..., message=...
		virtual EngineResult submit(const core::OrderRequest& req) = 0;

		virtual void onMyTrade(const core::MyTrade& t) = 0;

		virtual void onOrderStatus(std::string_view order_id, core::OrderStatus s) = 0;

		// WS/REST가 준 “주문 스냅샷 전체”를 엔진이 받아 동기화할 때 사용
		// - 기본 구현은 아무것도 하지 않음(다른 엔진 호환 유지)
		virtual void onOrderSnapshot(const core::Order& snapshot) = 0;

		virtual std::vector<EngineEvent> pollEvents() = 0;

		// (디버그/스모크 테스트용) 주문 조회
		// - 외부가 엔진 내부 저장소를 직접 만지지 않게 하기 위한 안전한 읽기 API
		virtual std::optional<core::Order> get(std::string_view order_id) const = 0;
	};
}