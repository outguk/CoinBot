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
	 * -  내부에서 "주문 상태"를 안전하게 저장/조회/갱신하기 위한 저장소
	 *
	 * 설계 포인트(5단계 기준):
	 * 1) 엔진 외부(app/strategy)가 내부 컨테이너에 직접 접근하지 못하게 함
	 * 2) order_id 로 빠르게 찾을 수 있어야 함 (unordered_map)
	 * 3) get()은 "복사본"을 반환하여 외부가 내부 상태를 망가뜨리지 못하게 함
	 * 4) update는 "전체 Order 교체" 방식 (단순하고 예측 가능)
	 * 5) 멀티스레드 대비를 위해 shared_mutex 사용(읽기 다중, 쓰기 단일)
	 */
	class OrderStore
	{
	public:
		OrderStore() = default;

		// 새 주문을 저장
		// - 이미 같은 order_id가 있으면 false 반환
		[[nodiscard]] bool add(const core::Order& order);

		// order_id로 주문 조회(복사본)
		[[nodiscard]] std::optional<core::Order> get(const std::string_view& order_id) const;

		// 주문을 통으로 교체(update)
		// - 존재하면 교체 후 true
		// - 없으면 false
		[[nodiscard]] bool update(const core::Order& order);

		// order_id로 주문 삭제
		[[nodiscard]] bool erase(const std::string_view& order_id);

		// 이미 order 존재 시 갱신, 없으면 추가(실거래용)
		void upsert(const core::Order& order);

		// 특정 마켓의 열려있는(New/Open) 주문들 조회
		[[nodiscard]] std::vector<core::Order> getOpenOrdersByMarket(const std::string_view& market) const;

		// 전체 주문 수
		[[nodiscard]] std::size_t size() const;

	private:
		// 내부 저장소 : order_id -> Order
		std::unordered_map<std::string, core::Order> orders_;

		// 동시성 제어용 뮤텍스
		// 읽기(get/size/list)는 shared lock, 쓰기(add/update/erase)는 unique lock
		mutable std::shared_mutex mtx_;	// shared_mutex는 는 shared와 unique 모두 지원
	};
}