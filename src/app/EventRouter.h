// app/EventRouter.h
// WS JSON 메시지에서 마켓 코드를 추출하여 해당 마켓 큐로 라우팅
#pragma once

#include <atomic>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/BlockingQueue.h"
#include "engine/input/EngineInput.h"

namespace app {

class EventRouter {
public:
    using PrivateQueue = core::BlockingQueue<engine::input::EngineInput>;

    EventRouter() = default;

    // 마켓별 큐 등록 (WS 스레드 시작 전 호출)
    // Phase 1.5에서 MarketEngineManager가 호출 예정
    //
    // 수명 계약: queue는 EventRouter보다 오래 살아있어야 함
    // - 등록 후 WS 스레드가 시작되면 routes_는 읽기 전용으로 사용
    // - 등록된 큐 객체의 이동/해제 금지 (댕글링 포인터 방지)
    void registerMarket(const std::string& market, PrivateQueue& queue);

    // 시장 데이터 라우팅 - drop-oldest는 BlockingQueue(max_size) 생성 시 자동 처리
    // 성공 시 true, 파싱 실패/미등록 마켓 시 false
    [[nodiscard]] bool routeMarketData(std::string_view json);

    // myOrder 라우팅 - 항상 push (파싱 실패/미등록 마켓 시 drop)
    // 주의: marketData와 동일한 bounded queue (max_size=5000, drop-oldest) 공유
    //       burst 시 오래된 myOrder가 밀려날 수 있음 → 실운영에서 큐 분리 검토
    // 성공 시 true, 파싱 실패/미등록 마켓 시 false
    [[nodiscard]] bool routeMyOrder(std::string_view json);

    // 라우팅 통계 (근사 카운팅, memory_order_relaxed)
    struct Stats {
        std::atomic<uint64_t> fast_path_success{0};
        std::atomic<uint64_t> fallback_used{0};
        std::atomic<uint64_t> parse_failures{0};
        std::atomic<uint64_t> conflict_detected{0};  // code/market 값 불일치
        std::atomic<uint64_t> unknown_market{0};
        std::atomic<uint64_t> total_routed{0};
    };

    const Stats& stats() const noexcept { return stats_; }

private:
    // Fast path 추출 결과
    struct FastResult {
        std::optional<std::string_view> market;
        bool had_conflict = false;  // code/market 값 불일치 감지
    };

    // Fast path: 키 기반 문자열 추출 (zero-allocation, string_view 반환)
    // had_conflict=true 시 fallback 시도 없이 즉시 실패 처리
    FastResult extractMarketFast_(std::string_view json) const;

    // Fallback(대안): nlohmann::json 정규 파싱
    std::optional<std::string> extractMarketSlow_(std::string_view json) const;

    // JSON 내 특정 키의 문자열 값을 추출하는 헬퍼
    // 이스케이프 문자 포함 시 nullopt (fallback 전환)
    static std::optional<std::string_view> extractStringValue_(
        std::string_view json, std::string_view key);

    // 등록된 마켓 → 큐 매핑 (WS 시작 전 세팅, 이후 읽기 전용)
    std::unordered_map<std::string, PrivateQueue*> routes_;

    Stats stats_;
};

} // namespace app
