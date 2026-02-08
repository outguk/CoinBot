// core/concurrency/BlockingQueue.h
#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace core
{
    // 스레드 안전 블로킹 큐(Blocking Queue)
    // - pop()은 데이터가 들어올 때까지 대기한다.
    // - max_size_가 설정되어 있으면, 초과 시 가장 오래된 데이터를 버린다(FIFO drop-oldest).
    template <typename T>
    class BlockingQueue final
    {
    public:
        // 최대 크기 설정:
        //  - 0  : 무제한(제한 없음)
        //  - >0 : 최대 크기(초과 시 오래된 요소부터 제거)
        explicit BlockingQueue(std::size_t max_size = 0)
            : max_size_(max_size)
        {
        }

        void push(T v)
        {
            {
                std::lock_guard<std::mutex> lk(mu_);

                // 최대 크기를 초과하면 가장 오래된 원소를 제거
                if (max_size_ > 0 && q_.size() >= max_size_)
                {
                    q_.pop_front();  // FIFO: 가장 먼저 들어온 요소를 제거
                }

                q_.push_back(std::move(v));
            }
            cv_.notify_one();
        }

        // 즉시 꺼내기(대기하지 않음)
        // - 비어 있으면 nullopt 반환
        // - 비어 있지 않으면 front를 꺼내서 반환
        std::optional<T> try_pop()
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (q_.empty()) return std::nullopt;
            T v = std::move(q_.front());
            q_.pop_front();
            return v;
        }

        // 블로킹 pop: 데이터가 들어올 때까지 대기 후 1개 반환
        T pop()
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return !q_.empty(); });
            T v = std::move(q_.front());
            q_.pop_front();
            return v;
        }

        // 제한 시간 동안만 대기하는 pop
        // - timeout 안에 데이터가 들어오면 값을 반환
        // - timeout 안에 데이터가 안 들어오면 nullopt 반환
        template <class Rep, class Period>
        std::optional<T> pop_for(const std::chrono::duration<Rep, Period>& timeout)
        {
            std::unique_lock<std::mutex> lk(mu_);

            if (!cv_.wait_for(lk, timeout, [&] { return !q_.empty(); }))
            {
                // timeout
                return std::nullopt;
            }

            T v = std::move(q_.front());
            q_.pop_front();
            return v;
        }

        std::size_t size() const
        {
            std::lock_guard<std::mutex> lk(mu_);
            return q_.size();
        }

        void clear()
        {
            std::lock_guard<std::mutex> lk(mu_);
            q_.clear();
        }

    private:
        mutable std::mutex mu_;
        std::condition_variable cv_;
        std::deque<T> q_;

        // 0 = 무제한, >0 = 최대 크기(초과 시 오래된 요소 제거)
        const std::size_t max_size_;
    };
}
