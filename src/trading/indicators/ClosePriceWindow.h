#pragma once

#include <cstddef>

#include "IndicatorTypes.h" // trading::Value<T>
#include "RingBuffer.h"     // trading::indicators::RingBuffer

namespace core { struct Candle; }

namespace trading::indicators {

    /*
     * ClosePriceWindow
     *
     * 목적:
     * - close[N] (N개 이전 종가)를 제공한다. (시간 기준 표현)
     * - trendStrength = abs(close - close[N]) / close[N] 계산에 필요
     *
     * 핵심:
     * - RingBuffer<double>에 종가를 계속 push
     * - close[N]는 valueFromBack(N)로 O(1)에 조회 가능
     *
     * 주의:
     * - close[N]를 얻으려면 최소 (N+1)개의 값이 필요 (시간 기준이기 때문에)
     *   따라서 내부 버퍼 capacity는 (delay_ + 1)로 설정
     */
    class ClosePriceWindow final {
    public:
        ClosePriceWindow() = default;
        explicit ClosePriceWindow(std::size_t delay) { reset(delay); }

        /*
         * reset(delay)
         * - delay=N을 설정
         * - 내부 버퍼를 (N+1) 크기로 재설정하고 상태를 초기화
         *   (close[N]를 얻기 위해 N+1개를 유지해야 함)
         */
        void reset(std::size_t delay);

        /*
         * clear()
         * - delay 설정은 유지
         * - 내부에 쌓인 데이터만 비움
         */
        void clear() noexcept;

        // N 값(몇 개 이전 종가를 볼지)
        [[nodiscard]] std::size_t delay() const noexcept { return delay_; }

        // 현재까지 쌓인 샘플 개수(0..delay_+1)
        [[nodiscard]] std::size_t count() const noexcept { return window_.size(); }

        /*
         * update(close)
         * - 새 종가를 입력
         * - 입력 후 "close[N]"를 반환 (준비되면 ready=true)
         *
         * ready 조건:
         * - delay_=N일 때, 최소 N+1개의 값이 쌓여야 close[N]가 존재
         */
        trading::Value<double> update(double close);

        /*
         * update(candle)
         * - candle.close_price를 입력으로 사용(편의 함수)
         * - Candle include는 cpp에서만 하도록 분리
         */
        trading::Value<double> update(const core::Candle& c);

        /*
         * closeN()
         * - 상태 변화 없이 현재 close[N]를 조회
         * - 준비 안됐으면 ready=false
         */
        [[nodiscard]] trading::Value<double> closeN() const noexcept;

    private:
        std::size_t delay_{ 0 };          // N
        RingBuffer<double> window_{};     // capacity = delay_ + 1
    };

} // namespace trading::indicators
