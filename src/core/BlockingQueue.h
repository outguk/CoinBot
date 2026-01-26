// core/concurrency/BlockingQueue.h
#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace core
{
    // 유실 불가 큐(Blocking)
    template <typename T>
    class BlockingQueue final
    {
    public:
        void push(T v)
        {
            {
                std::lock_guard<std::mutex> lk(mu_);
                q_.push_back(std::move(v));
            }
            cv_.notify_one();
        }

        // 종료 플래그 없이도 쓸 수 있게, try_pop / pop 둘 다 제공
        std::optional<T> try_pop()
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (q_.empty()) return std::nullopt;
            T v = std::move(q_.front());
            q_.pop_front();
            return v;
        }

        T pop()
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return !q_.empty(); });
            T v = std::move(q_.front());
            q_.pop_front();
            return v;
        }

        // 일정 시간만 기다리는 pop
        // - timeout 안에 데이터가 오면 반환
        // - timeout 동안 데이터가 안 오면 nullopt 반환
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
    };
}
