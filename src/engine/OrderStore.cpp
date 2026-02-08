#include "OrderStore.h"

#include <algorithm> // std::copy_if

namespace engine
{
	// "활성중인 주문" 판별 (업비트 스펙에 맞춘)
		// - New/Open/Pending : 활성 중
		// - Filled/Canceled/Rejected : 완료
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

	// 새 주문 추가
	bool OrderStore::add(const core::Order& order)
	{
		// 쓰기 작업이니 unique lock
		std::unique_lock lock(mtx_);

		// order_id는 반드시 키이므로, 비어있으면 추가 불가 (비어있는 값을 저장하는 건 이상함)
		// 여기서는 Store의 책임범위로 판단함.
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
		if (it == orders_.end()) // 컨테이너에서 마지막 다음(즉 범위의 끝)
			return std::nullopt;

		// "복사본" 반환: 외부가 내부 Order를 임의로 변경할 수 없게 함
		return it->second;
	}

	// 주문 상태를 교체(update)
	bool OrderStore::update(const core::Order& order)
	{
		// 쓰기 작업이니 unique lock
		std::unique_lock lock(mtx_);

		auto it = orders_.find(order.id);
		if (it == orders_.end())
			return false;

		const core::OrderStatus old_status = it->second.status;

		// 전체 교체 - 지금 단계에서는 "부분 수정"보다 "통째 교체"가 단순하고 명확한 의미.
		it->second = order;

		// 만약 Open -> Completed 전환이면 큐에 추가
		if (isOpenStatus(old_status) && !isOpenStatus(order.status))
		{
			completed_order_ids_.push_back(order.id);
		}

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
		// 없으면 추가, 있으면 교체함
		// - 멱등성보장 - 재연결/REST 초기화/WS 순서꼬임 등을 안전하게 처리하기 위한 방식
		std::unique_lock lock(mtx_);
		if (order.id.empty()) return;

		// 이전 상태 확인 (중복 큐 추가 방지)
		auto it = orders_.find(order.id);
		core::OrderStatus old_status = core::OrderStatus::New;  // 기본값 (새 주문)
		if (it != orders_.end())
			old_status = it->second.status;

		orders_[order.id] = order;

		// 상태 전환 시에만 큐에 추가 (Open → Completed)
		// update()와 동일한 로직으로 중복 방지
		if (isOpenStatus(old_status) && !isOpenStatus(order.status))
		{
			completed_order_ids_.push_back(order.id);
		}
	}

	// 특정 마켓에 활성중인(New/Open) 주문들 조회
	std::vector<core::Order> OrderStore::getOpenOrdersByMarket(const std::string_view& market) const
	{
		// 읽기 작업이니 shared lock
		std::shared_lock lock(mtx_);

		std::vector<core::Order> result;
		result.reserve(orders_.size()); // 넉넉하게 크기 예약

		for (const auto& [id, order] : orders_)
		{
			// 1) 마켓 필터링 : "KRW-BTC" 같은 것만 뽑을 때 문자열 비교
			if (std::string_view(order.market) != market)
				continue;

			// 2) 아직 활성중인 주문만 반환
			if(!isOpenStatus(order.status))
				continue;

			// 3) 복사하여(복사본) 반환
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

	// 완료 주문 정리
	std::size_t OrderStore::cleanup()
	{
		std::unique_lock lock(mtx_);

		std::size_t removed_count = 0;

		// 완료 주문이 최대 개수 초과하면 가장 오래된 것부터 삭제
		while (completed_order_ids_.size() > max_completed_orders_)
		{
			const std::string& oldest_id = completed_order_ids_.front();

			// 실제로 저장중인 주문이면 삭제
			auto it = orders_.find(oldest_id);
			if (it != orders_.end())
			{
				// 체크: 정말 Open 상태가 아닌지 확인(혹시라도 큐에 중복 추가된 경우 대비 방어)
				if (!isOpenStatus(it->second.status))
				{
					orders_.erase(it);
					++removed_count;
				}
			}

			completed_order_ids_.pop_front();
		}

		return removed_count;
	}
}