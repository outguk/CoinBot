#pragma once

#include <cstddef>
#include <optional>
#include <type_traits>

namespace trading {

    // 지표 공통 "준비 상태 + 값" 표현
    // - optional<T>만 써도 되지만, ready 상태를 명시적으로 남기면 디버깅/테스트가 편해짐
    template <typename T>
    struct Value final {
        static_assert(std::is_arithmetic_v<T> || std::is_trivially_copyable_v<T>,
            "Indicator Value<T> is intended for arithmetic/trivially copyable types.");

        bool ready{ false };
        T v{};      // SMA - 단순이동평균 결과, RSI - RSI값, 변동성수치 등등 지표 계산값

        [[nodiscard]] constexpr std::optional<T> asOptional() const noexcept {
            return ready ? std::optional<T>(v) : std::nullopt;
        }
    };

    // (선택) Rolling window 지표들이 공통으로 쓰기 좋은 유틸
    [[nodiscard]] constexpr bool isWindowReady(std::size_t count, std::size_t window) noexcept {
        return window > 0 && count >= window;
    }

} // namespace trading
