#include "OrderStore.h"

#include <algorithm> // std::copy_if

namespace engine
{
	// "열려있는 주문" 판정 (데모 엔진용 규칙)
		// - New/Open/Pending : 진행 중
		// - Filled/Canceled/Rejected : 종료
	constexpr bool isOpenStatus(core::OrderStatus s) noexcept
	{
		using S = core::OrderStatus;
		switch (s)
		{
		case S::New:
		case S::Open:
		case S::Pending:
			return true;

		case S::Filled:
		case S::Canceled:
		case S::Rejected:
			return false;
		}
		return false; // enum 확장 대비
	}

	// 새 주문 저장
	bool OrderStore::add(const core::Order& order) 
	{
		// 쓰기 작업이니 unique lock
		std::unique_lock lock(mtx_);

		// order_id가 핵심 키이므로, 비어있으면 저장 불가 (엔진에서 생성 보장하는 게 이상적)
		// 여기서는 Store가 방어적으로 거른다.
		if (order.id.empty())
			return false;

		auto [it, inserted] = orders_.emplace(order.id, order);
		return inserted;	// 이미 존재하면 false
	}

	// order_id로 주문 조회(복사본)
	std::optional<core::Order> OrderStore::get(const std::string_view& order_id) const 
	{
		// 읽기 작업이니 shared lock
		std::shared_lock lock(mtx_);

		auto it = orders_.find(std::string(order_id));
		if (it == orders_.end())
			return std::nullopt;

		// "복사본" 반환: 외부가 내부 Order를 직접 변형할 수 없게 함
		return it->second;
	}

	// 주문 통으로 교체(update)
	bool OrderStore::update(const core::Order& order) 
	{
		// 쓰기 작업이니 unique lock
		std::unique_lock lock(mtx_);

		auto it = orders_.find(order.id);
		if (it == orders_.end())
			return false;

		// 전체 교체 - 데모 단계에서는 "부분 수정"보다 "통째 교체"가 단순하고 버그가 적다.
		it->second = order;
		return true;
	}

	// order_id로 주문 삭제
	bool OrderStore::erase(const std::string_view& order_id) 
	{
		// 쓰기 작업이니 unique lock
		std::unique_lock lock(mtx_);

		auto it = orders_.find(std::string(order_id));
		if (it == orders_.end())
			return false;

		orders_.erase(it);
		return true;
	}

	void OrderStore::upsert(const core::Order& order)
	{
		// “없으면 추가, 있으면 교체”
		// - 실거래에서 재연결/REST 초기화/WS 재수신을 안전하게 처리하기 위한 핵심
		std::unique_lock lock(mtx_);
		if (order.id.empty()) return;

		orders_[order.id] = order;
	}

	// 특정 마켓의 열려있는(New/Open) 주문들 조회
	std::vector<core::Order> OrderStore::getOpenOrdersByMarket(const std::string_view& market) const 
	{
		// 읽기 작업이니 shared lock
		std::shared_lock lock(mtx_);

		std::vector<core::Order> result;
		result.reserve(orders_.size()); // 대략적인 크기 예약

		for (const auto& [id, order] : orders_)
		{
			// 1) 마켓 필터링 : "KRW-BTC" 같은 페어 쌍 문자열 비교
			if (std::string_view(order.market) != market)
				continue;

			// 2) 아직 진행중인 주문만 반환
			if(!isOpenStatus(order.status))
				continue;

			// 3) 스냅샷(복사본) 반환
			result.push_back(order);
		}
		return result;
	}


	// 전체 주문 수
	std::size_t OrderStore::size() const 
	{
		// 읽기 작업이니 shared lock
		std::shared_lock lock(mtx_);
		return orders_.size();
	}
}