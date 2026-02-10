// src/api/upbit/SharedOrderApi.h
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <optional>
#include <variant>
#include <vector>

#include "IOrderApi.h"
#include "core/domain/Account.h"
#include "core/domain/Order.h"
#include "core/domain/OrderRequest.h"
#include "api/rest/RestError.h"

// Forward declaration
namespace api::rest {
    class UpbitExchangeRestClient;
}

namespace api::upbit {

    /*
     * SharedOrderApi
     *
     * [역할]
     * 멀티마켓 환경에서 여러 마켓 스레드가 동시에 주문 API를 호출할 때
     * UpbitExchangeRestClient를 thread-safe하게 공유할 수 있도록 감싸는 래퍼
     *
     * [설계]
     * - 내부에 UpbitExchangeRestClient를 보유하고 모든 호출을 mutex로 직렬화
     * - 각 마켓 스레드는 이 객체를 shared_ptr로 공유
     * - Upbit API는 초당 요청 제한(rate limit)이 있으므로, 직렬화가 필수
     * - IOrderApi 인터페이스 구현 (의존성 역전, 테스트 가능성)
     *
     * [확장 포인트]
     * 1. Rate Limiting: 추후 요청 간 최소 시간 간격 적용 가능 (예: 100ms)
     * 2. Request Queueing: 우선순위 큐로 긴급 주문(취소) 우선 처리
     * 3. Metrics: 요청 수, 실패율, 평균 응답 시간 등 수집
     * 4. Circuit Breaker: 연속 실패 시 일시 중단 및 복구
     *
     * [Thread-Safety]
     * - 모든 public 메서드는 std::lock_guard로 보호됨
     * - 내부 UpbitExchangeRestClient는 단일 스레드에서만 호출됨
     */
    class SharedOrderApi : public IOrderApi {
    public:
        // UpbitExchangeRestClient의 unique ownership을 받음
        explicit SharedOrderApi(std::unique_ptr<api::rest::UpbitExchangeRestClient> client);

        // Copy 금지 (mutex는 복사 불가)
        SharedOrderApi(const SharedOrderApi&) = delete;
        SharedOrderApi& operator=(const SharedOrderApi&) = delete;

        // Move 금지(move는 필요없음)
        SharedOrderApi(SharedOrderApi&&) noexcept = delete;
        SharedOrderApi& operator=(SharedOrderApi&&) noexcept = delete;

        ~SharedOrderApi() = default;

        // IOrderApi 구현

        // GET /v1/accounts
        // - 계좌 정보 조회 (KRW 잔고, 코인 보유량 등)
        std::variant<core::Account, api::rest::RestError>
            getMyAccount() override;

        // GET /v1/orders/open?market=...
        // - 특정 마켓의 미체결 주문 조회
        std::variant<std::vector<core::Order>, api::rest::RestError>
            getOpenOrders(std::string_view market) override;

        // DELETE /v1/order?uuid=... OR identifier=...
        // - 주문 취소
        std::variant<bool, api::rest::RestError>
            cancelOrder(const std::optional<std::string>& uuid,
                        const std::optional<std::string>& identifier) override;

        // POST /v1/orders
        // - 주문 제출 (매수/매도)
        // - 반환값: Upbit 주문 UUID (Order.id로 사용)
        std::variant<std::string, api::rest::RestError>
            postOrder(const core::OrderRequest& req) override;


        // --- Test-only / Debug instrumentation ---
        int debugMaxInFlight() const noexcept { return max_in_flight_.load(); }

        // 즉, 여기서부터 측정 시작
        void debugResetInFlight() noexcept {
            in_flight_.store(0);
            max_in_flight_.store(0);
        }
    private:
        std::unique_ptr<api::rest::UpbitExchangeRestClient> client_;

        // 멀티스레드 동시 호출 직렬화용 뮤텍스
        // 확장 포인트: 읽기 전용 API(getMyAccount, getOpenOrders)는
        //               shared_mutex로 병렬화 가능 (추후 최적화)
        mutable std::mutex mtx_;

        // 테스트 용
        // Instrumentation counters (atomic so even if lock breaks, it still records concurrency)
        std::atomic<int> in_flight_{ 0 };       // 현재 mutex 안에 들어와 실행 중인 호출 수
        std::atomic<int> max_in_flight_{ 0 };
    };

} // namespace api::upbit
