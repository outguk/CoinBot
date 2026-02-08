#pragma once

#include <cstddef>
#include <optional>

#include "IndicatorTypes.h"     // trading::Value<T>
#include "core/domain/Candle.h" // core::Candle (close_price 사용)

namespace trading::indicators
{
	/*
		Wilder RSI

		핵심 아이디어
		1) seed(초기값) 구간(length개 변화량)을 먼저 모은다.
		   - gainSum = 양의 변화량 합
		   - lossSum = 음의 변화량(절대값) 합
		   - seed 완료 시:
			 avgGain = gainSum / length
			 avgLoss = lossSum / length

		2) seed 이후부터는 Wilder smoothing(이전 평균을 이용해 갱신) 적용
		   avgGain = (avgGain*(length-1) + gain) / length (오른 구간의 평균값)
		   avgLoss = (avgLoss*(length-1) + loss) / length (내린 구간의 평균값)

		3) RSI 계산
		   RS  = avgGain / avgLoss
		   RSI = 100 - 100/(1+RS)

		주의점(경계값)
		- avgLoss == 0 && avgGain == 0 : 변화가 전혀 없으므로 RSI=50으로 둔다(관례적으로 중립)
		- avgLoss == 0 && avgGain  > 0 : RSI=100
		- avgGain == 0 && avgLoss  > 0 : RSI=0
	*/
	class RsiWilder final
	{
	public:
		RsiWilder() = default;
		explicit RsiWilder(std::size_t length) { reset(length); }

		// length 변경 + 내부 상태 초기화
		void reset(std::size_t length);

		// length는 유지하고, 누적 상태만 초기화
		void clear() noexcept;

		// 가격(보통 close)을 한 틱/한 캔들마다 업데이트
		trading::Value<double> update(double close_price);

		// Candle 입력 버전(종가 사용)
		trading::Value<double> update(const core::Candle& c);

		// 현재 RSI 값(ready 포함)을 조회 (update를 호출하지 않아도 현재 상태 확인 가능)
		[[nodiscard]] trading::Value<double> value() const noexcept;

		[[nodiscard]] std::size_t length() const noexcept { return length_; }

	private:
		[[nodiscard]] static double computeRsi(double avg_gain, double avg_loss) noexcept;

	private:
		std::size_t length_{ 0 };	// 설정할 RSI 기간(14면 14개 기반으로 RSI 측정)

		// 직전 가격(변화량 계산용). 첫 값 들어오기 전까지는 없음.
		std::optional<double> prev_price_{};

		// seed 단계에서 length개의 변화량을 모으기 위한 카운터/누적합
		std::size_t seed_count_{ 0 }; // 지금까지 누적한 "변화량(delta)" 개수
		double seed_gain_sum_{ 0.0 }; // 변화량이 +인 크기만 length 개 만큼 모은다
		double seed_loss_sum_{ 0.0 }; // 변화량이 -인 크기만 length 개 만큼 모은다

		// Wilder smoothing에 사용되는 상태값 seed를 length로 나누어 초기 기준선을 만든다
		double avg_gain_{ 0.0 };
		double avg_loss_{ 0.0 };

		// 마지막으로 계산된 RSI(ready일 때 유효)
		trading::Value<double> last_{};
	};
} // namespace trading::indicators
