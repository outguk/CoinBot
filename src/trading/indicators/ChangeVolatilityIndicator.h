#pragma once

#include <cstddef>
#include <optional>

#include "IndicatorTypes.h"
#include "RingBuffer.h"

namespace core { struct Candle; }

namespace trading::indicators {

    /*
     * ChangeVolatilityIndicator
     *
     * 목적:
     * - change(close)의 표준편차(rolling stdev)를 유지한다.
     * - recentVolatility 필터의 핵심 입력으로 사용한다.
     *
     * change 정의(권장):
     * - return r = (close - prevClose) / prevClose  (퍼센트 변화율, 단위: 비율)
     *   예: +1% -> 0.01
     *
     * 롤링 표준편차 계산:
     * - 최근 window_개 r을 유지
     * - sum, sumsq를 O(1)로 갱신하여 분산/표준편차 계산
     *
     * ready 조건:
     * - 변화율을 만들기 위해 prevClose가 필요 (최소 2개 close)
     * - 그리고 변화율 윈도우(window_)가 꽉 차야 ready=true
     */
    class ChangeVolatilityIndicator final {
    public:
        ChangeVolatilityIndicator() = default;
        explicit ChangeVolatilityIndicator(std::size_t window) { reset(window); }

        // 표준편차 윈도우 크기 설정(=volatilityWindow)
        void reset(std::size_t window);

        // 윈도우 크기는 유지하고 상태만 초기화
        void clear() noexcept;

        [[nodiscard]] std::size_t window() const noexcept { return window_; }
        [[nodiscard]] std::size_t count()  const noexcept { return returns_.size(); }

        /*
         * update(close)
         * - close로부터 변화율 r을 만들고(이전 close 필요)
         * - r의 롤링 표준편차를 반환
         */
        trading::Value<double> update(double close);

        // Candle 기반 편의 함수 (close_price 사용)
        trading::Value<double> update(const core::Candle& c);

        // 현재 표준편차 조회(상태 변화 없음)
        [[nodiscard]] trading::Value<double> value() const noexcept;

    private:
        // 표준 편차를 구할 때 몇개의 변화율을 사용할지(표본 집합 크기)
        std::size_t window_{ 0 };

        // 최근 window_개 봉의 change(close) 값(변화율 r)을 저장
        RingBuffer<double> returns_{};

        // 롤링 합/제곱합 (최근 W개 r의 합과 r^2의 합) - 표준 편차 계산용
        double sum_{ 0.0 };
        double sumsq_{ 0.0 };

        // 변화율 계산을 위한 직전 종가
        std::optional<double> prevClose_{};

    private:
        // 내부 계산: 표준편차(모집단 기준, ddof=0)
        [[nodiscard]] double stdev_() const noexcept;
    };

} // namespace trading::indicators
