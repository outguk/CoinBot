#pragma once

#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/domain/Order.h"

namespace engine
{
	/*
	 * OrderStore
	 * - 하나의 "주문 저장소"를 구현하고 추가/조회/갱신하기 쉽게 만듦
	 *
	 * 설계 요구사항:
	 * 1) 여러 외부(app/strategy)에서 동일 컨테이너에 접근 가능해야 함
	 * 2) order_uuid 를 기준으로 찾을 수 있어야 함 (unordered_map)
	 * 3) get()은 "복사본"을 반환하여 외부가 내부 상태를 망가뜨릴 수 없게 함
	 * 4) update는 "전체 Order 객체" 교체 (단순하고 명확 의도)
	 * 5) 멀티스레드 환경 대비 shared_mutex 사용(읽기 병렬, 쓰기 배타)
	 *
	 * 생명주기 정책: orders_에 존재 = 활성(New/Open/Pending) 주문
	 * 터미널 상태 도달 시 호출자(MarketEngine::onOrderSnapshot)가 erase()로 즉시 제거
	 */
	class OrderStore
	{
	public:
		OrderStore() = default;

		// 새 주문을 추가
		// - 이미 존재 order_uuid면 실패하고 false 반환
		[[nodiscard]] bool add(const core::Order& order);

		// order_uuid로 주문 조회(복사본)
		[[nodiscard]] std::optional<core::Order> get(const std::string_view& order_uuid) const;

		// 주문의 상태를 교체(update)
		// - 성공하면 교체 후 true
		// - 없으면 false
		[[nodiscard]] bool update(const core::Order& order);

		// order_uuid로 주문 삭제
		bool erase(const std::string_view& order_uuid);

		// 이미 order 있으면 덮어쓰고, 없으면 추가(멱등성)
		void upsert(const core::Order& order);

		// 특정 마켓에 활성중인(New/Open) 주문들 조회
		[[nodiscard]] std::vector<core::Order> getOpenOrdersByMarket(const std::string_view& market) const;

		// 전체 주문 수
		[[nodiscard]] std::size_t size() const;

	private:
		// 주문 저장소 : order_uuid -> Order
		std::unordered_map<std::string, core::Order> orders_;

		// 동시성 제어용 뮤텍스
		// 읽기(get/size/list)는 shared lock, 쓰기(add/update/erase)는 unique lock
		mutable std::shared_mutex mtx_;
	};
}
