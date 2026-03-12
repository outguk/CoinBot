// app/MarketEngineManager.h
//
// 멀티마켓 중앙 코디네이터
// - 마켓별 독립 워커 스레드를 관리 (MarketContext)
// - EngineRunner의 단일 마켓 이벤트 루프 패턴을 마켓별로 복제
// - MarketEngine + AccountManager 기반으로 동작
//
// 생명주기: 생성자(동기화+복구) → registerWith(EventRouter) → start() → stop()
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>       // std::jthread, std::stop_token (C++20)
#include <unordered_map>
#include <vector>

#include "core/BlockingQueue.h"
#include "engine/input/EngineInput.h"
#include "engine/MarketEngine.h"
#include "engine/OrderStore.h"
#include "engine/EngineEvents.h"
#include "api/upbit/IOrderApi.h"
#include "trading/allocation/AccountManager.h"
#include "trading/strategies/RsiMeanReversionStrategy.h"
#include "trading/strategies/StrategyTypes.h"
#include "database/Database.h"

namespace app {

class EventRouter;  // 전방 선언

// GCC/Clang에서 중첩 struct의 default member initializer를
// 외부 클래스 생성자 기본 인자로 사용 불가 → 네임스페이스 레벨로 분리
struct MarketManagerConfig {
    trading::strategies::RsiMeanReversionStrategy::Params strategy_params;
    std::size_t queue_capacity = 5000;      // 마켓별 큐 최대 크기 (drop-oldest)
    int sync_retry = 3;                     // 초기 계좌 동기화 재시도 횟수
    std::chrono::seconds pending_timeout{120}; // Pending 상태 타임아웃 (2분)
};

class MarketEngineManager final {
public:
    using PrivateQueue = core::BlockingQueue<engine::input::EngineInput>;
    using MarketManagerConfig = app::MarketManagerConfig;  // 하위 호환 alias

    // 생성자: 계좌 동기화 + 마켓별 컨텍스트 생성 + 전략 복구
    // @param api: SharedOrderApi (thread-safe)
    // @param store: 공유 OrderStore
    // @param account_mgr: 공유 AccountManager
    // @param markets: 거래할 마켓 목록
    // @param cfg: 설정
    //
    // 실패 시 std::runtime_error 발생 (계좌 동기화 실패 등)
    MarketEngineManager(api::upbit::IOrderApi& api,
                        engine::OrderStore& store,
                        trading::allocation::AccountManager& account_mgr,
                        const std::vector<std::string>& markets,
                        MarketManagerConfig cfg = MarketManagerConfig{},
                        db::Database* db = nullptr);

    ~MarketEngineManager();

    // 복사/이동 금지
    MarketEngineManager(const MarketEngineManager&) = delete;
    MarketEngineManager& operator=(const MarketEngineManager&) = delete;

    // EventRouter에 마켓별 큐 등록 (start() 전에 호출)
    void registerWith(EventRouter& router);

    // 마켓별 워커 스레드 시작
    // 선행 조건: registerWith()가 먼저 호출되어야 함
    //   → 미등록 시 이벤트가 큐에 전달되지 않아 전략이 동작하지 않음
    void start();

    // 모든 워커 스레드 정지 + join
    void stop();

    // WS 재연결 시 복구 요청 (WS 스레드에서 호출 가능)
    // atomic flag로 우선 처리 — 일반 큐 drop-oldest 영향 없음
    void requestReconnectRecovery();

    // 비정상 종료된 워커 스레드가 있으면 true (HealthCheck용)
    bool hasFatalWorker() const;

private:
    // 마켓별 독립 컨텍스트 (스레드 + 엔진 + 전략 + 큐)
    struct MarketContext {
        std::string market;

        std::unique_ptr<engine::MarketEngine> engine;
        std::unique_ptr<trading::strategies::RsiMeanReversionStrategy> strategy;
        PrivateQueue event_queue;

        std::jthread worker;    // stop_token 내장 (stop_flag 불필요)

        // 같은 분봉의 반복 업데이트를 최신값으로 유지하고,
        // 다음 분봉이 들어오면 이전 분봉(최종 close)을 확정 처리한다.
        std::optional<core::Candle> pending_candle;

        // intrabar 청산 submit 실패 시 기록되는 캔들 ts.
        // 동일 ts의 추가 업데이트에서 재시도를 막고, 다음 분봉에서만 재시도한다.
        std::optional<std::string> intrabar_fail_ts{};

        // Pending 상태 타임아웃 추적
        bool tracking_pending{false};
        std::chrono::steady_clock::time_point pending_entered_at{};
        bool pending_timeout_fired{false};

        // 복구 요청 제어 채널
        // atomic flag로 큐 drop-oldest와 무관하게 우선 처리
        std::atomic<bool> recovery_requested{false};

        // requestReconnectRecovery 필터링용
        // worker thread(checkPendingTimeout_)에서만 쓰기, WS IO thread에서 읽기
        std::atomic<bool> has_active_pending{false};

        // stop 요청 없이 workerLoop_를 탈출하면 비정상 종료로 판정
        std::atomic<bool> exited_abnormally{false};

        explicit MarketContext(std::string m, std::size_t queue_capacity)
            : market(std::move(m))
            , event_queue(queue_capacity)
        {}
    };

    // 시작 시점에만 사용: 거래소 계좌 조회 → AccountManager 전체 재구축
    // 런타임 복구 경로에서는 호출 금지
    bool rebuildAccountOnStartup_(bool throw_on_fail);

    // 생성자에서 호출: StartupRecovery로 시작 시점에 마켓 상태 복구
    void recoverMarketState_(MarketContext& ctx);

    // 워커 스레드(각 마켓 스레드) 메인 루프 
    void workerLoop_(MarketContext& ctx, std::stop_token stoken);

	// 이벤트 핸들러 (타입별로 분기)
    void handleOne_(MarketContext& ctx, const engine::input::EngineInput& in);
    void handleMyOrder_(MarketContext& ctx, const engine::input::MyOrderRaw& raw);
    void handleMarketData_(MarketContext& ctx, const engine::input::MarketDataRaw& raw);
    // 엔진 출력을 전략으로 전달
    void handleEngineEvents_(MarketContext& ctx, const std::vector<engine::EngineEvent>& evs);

    // 재연결/타임아웃 복구: 주문 단건 조회 기반
    // 런타임에서 rebuildFromAccount 호출 금지 — 타 마켓 KRW 재분배 없음
    void runRecovery_(MarketContext& ctx);

    // 복구 헬퍼: getOrder 재시도
    std::optional<core::Order> queryOrderWithRetry_(
        std::string_view order_uuid, int max_retries);

    // Pending 상태 타임아웃 감시 (workerLoop_ 내에서 매 반복마다 호출)
    void checkPendingTimeout_(MarketContext& ctx);

    // AccountManager에서 마켓별 예산 조회 → 전략용 AccountSnapshot 변환
    trading::AccountSnapshot buildAccountSnapshot_(std::string_view market) const;

    // 공유 자원 참조
	api::upbit::IOrderApi& api_;    // 외부 거래소 API
	engine::OrderStore& store_;     // 마켓들이 공유하는 주문 저장소
	trading::allocation::AccountManager& account_mgr_; // 공유 계좌 관리자
    db::Database* db_{ nullptr };   // SQLite DB (없으면 기록 생략)

    MarketManagerConfig cfg_;

    // 마켓별 컨텍스트 (생성자 이후 불변, 읽기 전용)
    std::unordered_map<std::string, std::unique_ptr<MarketContext>> contexts_;

    // 전체 시작 여부 (재진입 방지 플래그)
    bool started_{false};
};

} // namespace app

