#pragma once

#include <mutex>
#include <optional>
#include <cstddef>

#include "trading/indicators/RingBuffer.h" // 경로는 네 프로젝트 include 구조에 맞춰 조정

namespace util
{
    /*
    * ThreadSafeRingBuffer<T>
    * - RingBuffer<T>는 thread-safe가 아니므로 mutex로 감싼 래퍼
    * - WS producer thread: push()
    * - main/strategy thread: (다음 단계에서) tryPopOldest() 같은 함수로 소비
    */
    template <typename T>
    class ThreadSafeRingBuffer final
    {
    public:
        explicit ThreadSafeRingBuffer(std::size_t capacity)
            : rb_(capacity)
        {
        }

        // push: 가득 차면 oldest drop (RingBuffer가 overwritten을 반환)
        // 반환값: drop된 값이 있으면 optional로 제공(로그/통계용)
        std::optional<T> pushDropOldest(const T& v)
        {
            std::scoped_lock lk(mtx_);
            return rb_.push(v);
        }

        std::optional<T> pushDropOldest(T&& v)
        {
            std::scoped_lock lk(mtx_);
            return rb_.push(std::move(v));
        }

        // size 관찰(성공 기준 확인용)
        std::size_t size() const
        {
            std::scoped_lock lk(mtx_);
            return rb_.size();
        }

        std::size_t capacity() const
        {
            std::scoped_lock lk(mtx_);
            return rb_.capacity();
        }

        // (Chat 5에서 소비자가 필요해질 함수)
        // oldest를 꺼내고 제거: RingBuffer는 pop이 없어서 “가벼운 소비용 버퍼”로 쓰려면 확장 필요.
        // 지금 단계(Chat 4)는 producer push + size 로그까지만 요구하니 여기서 멈춘다.

    private:
        mutable std::mutex mtx_;
        trading::indicators::RingBuffer<T> rb_;
    };
} // namespace util
