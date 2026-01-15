#pragma once

#include <algorithm>
#include <type_traits>

// quote는 price 느낌 1코인의 가격
namespace engine
{
	// Stage 5 (DemoOrderEngine)용: "고정 비율" 수수료 정책
	//
	// - rate: 예) 0.0005 (0.05%)
	// - notional: 체결 금액(quote 기준) = price * volume
	// - fee = notional * rate
	// - BUY 총지출 = notional + fee
	// - SELL 순수입 = notional - fee

	class FeePolicy
	{
	public:
		using Rate = double; // 0.0005 = 0.05%

		// 수수료 없이 생성
		constexpr FeePolicy() noexcept = default;
		// 고정 비율 수수료 정책 생성
		constexpr explicit FeePolicy(double rate) noexcept : rate_(rate) {}

		[[nodiscard]] constexpr double rate() const noexcept { return rate_; }

		// 체결 금액(notional)에 대한 수수료 계산
		[[nodiscard]] constexpr double fee(double notional) const noexcept
		{
			return notional * rate_;
		}

		// 매수 체결 시 빠져나가는 총 금액(수수료 포함)
		[[nodiscard]] constexpr double buyTotalCost(double notional) const noexcept
		{
			return notional + fee(notional);
		}

		// 매도 체결 시 들어오는 순 금액(수수료 차감)
		[[nodiscard]] constexpr double sellNetProceeds(double notional) const noexcept
		{
			return notional - fee(notional);
		}

		// 안전장치
		// 엔진 내부에서 어떤 실수가 있더라도 SELL 결과가 음수가 되어 계좌를 망가뜨리지는 않게 하자
		[[nodiscard]] constexpr double safeSellNetProceeds(double notional) const noexcept
		{
			return std::max(0.0, sellNetProceeds(notional));
		}

	private:
		double rate_{ 0.0 };

	};
}