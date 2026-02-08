#pragma once

#include <cstddef>

#include "IndicatorTypes.h"   // trading::Value<T>
#include "RingBuffer.h"       // trading::indicators::RingBuffer

namespace core { struct Candle; }

namespace trading::indicators {

     /*
     * SMA(Simple Moving Average)
     * - 최근 length_개의 값에 대한 단순 이동평균
     * - 매 update마다 전체 합을 다시 계산하지 않고(sum_ 유지) O(1)로 갱신
     *
     * 핵심 아이디어:
     * - RingBuffer에 새 값 push
     * - 윈도우가 가득 찬 상태에서 push 시, 가장 오래된 값이 overwritten으로 나오므로
     *   sum_에서 빼고 새 값을 더해 sum_을 항상 "최근 N개 합"으로 유지
     */

    class Sma final {
    public:
        // 기본 생성자: 아직 length_가 0이면 비활성 상태로 동작(ready=false)
        Sma() = default;

        // length를 받아 바로 사용할 수 있도록 reset까지 수행하는 생성자
        explicit Sma(std::size_t length) { reset(length); }

        void reset(std::size_t length); // 내부 버퍼를 length 크기로 초기화하고(sum_ 포함) 상태를 깨끗하게 리셋
        void clear() noexcept;          // 내부에 쌓인 값들만 비우고 sum_도 0으로 초기화

        // 현재 설정된 윈도우 길이(N)
        [[nodiscard]] std::size_t length() const noexcept { return length_; }
        // 현재까지 쌓인 샘플 개수(0..N)
        [[nodiscard]] std::size_t count()  const noexcept { return window_.size(); }

        // 숫자 스트림 업데이트(새 샘플 x를 SMA에 입력, 갱신, 윈도우가 length_만큼 차야 ready=true)
        trading::Value<double> update(double x);

        // Candle 기반(종가 사용)으로 update(double)로 위임
        trading::Value<double> update(const core::Candle& c);

        // 현재 SMA 값(준비 안되면 ready=false)
        [[nodiscard]] trading::Value<double> value() const noexcept;

    private:
        std::size_t length_{ 0 };       // 윈도우 길이(N). 0이면 비활성.
        RingBuffer<double> window_{};   // 최근 N개 값을 저장하는 고정 크기 버퍼
        double sum_{ 0.0 };             // 최근 N개 값의 합(ready일 때는 정확히 N개의 합)
    };

} // namespace trading::indicators
