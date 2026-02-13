// app/MarketEngineManager.h
//
// 멀티마켓 중앙 코디네이터
// - 마켓별 독립 워커 스레드를 관리 (MarketContext)
// - EngineRunner의 단일 마켓 이벤트 루프 패턴을 마켓별로 복제
// - MarketEngine + AccountManager 기반으로 동작
//
// 생명주기: 생성자(동기화+복구) → registerWith(EventRouter) → start() → stop()
#pragma once

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

namespace app {

class EventRouter;  // 전방 선언

class MarketEngineManager final {
public:
    using PrivateQueue = core::BlockingQueue<engine::input::EngineInput>;

    // 전략 파라미터 + 큐/폴링 설정
    struct MarketManagerConfig {
        trading::strategies::RsiMeanReversionStrategy::Params strategy_params;
        std::size_t queue_capacity = 5000;      // 마켓별 큐 최대 크기 (drop-oldest)
        int sync_retry = 3;                     // 초기 계좌 동기화 재시도 횟수
    };

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
                        MarketManagerConfig cfg = {});

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

        explicit MarketContext(std::string m, std::size_t queue_capacity)
            : market(std::move(m))
            , event_queue(queue_capacity)
        {}
    };

    // 생성자에서 호출: 거래소 계좌 조회 → AccountManager 동기화
    // throw_on_fail=true 시 실패하면 예외 발생
    void syncAccountWithExchange_(bool throw_on_fail);

    // 생성자에서 호출: StartupRecovery로 시작 시점에 마켓 상태 복구
    void recoverMarketState_(MarketContext& ctx);

    // 워커 스레드 메인 루프 (EngineRunner::run() 패턴 미러링)
    void workerLoop_(MarketContext& ctx, std::stop_token stoken);

	// 이벤트 핸들러 (타입별로 분기)
    void handleOne_(MarketContext& ctx, const engine::input::EngineInput& in);
    void handleMyOrder_(MarketContext& ctx, const engine::input::MyOrderRaw& raw);
    void handleMarketData_(MarketContext& ctx, const engine::input::MarketDataRaw& raw);
        // 엔진 출력을 전략으로 전달
    void handleEngineEvents_(MarketContext& ctx, const std::vector<engine::EngineEvent>& evs);

    // AccountManager에서 마켓별 예산 조회 → 전략용 AccountSnapshot 변환
    trading::AccountSnapshot buildAccountSnapshot_(std::string_view market) const;

    // 공유 자원 참조
	api::upbit::IOrderApi& api_;    // 외부 거래소 API
	engine::OrderStore& store_;     // 마켓들이 공유하는 주문 저장소
	trading::allocation::AccountManager& account_mgr_; // 공유 계좌 관리자

    MarketManagerConfig cfg_;

    // 마켓별 컨텍스트 (생성자 이후 불변, 읽기 전용)
    std::unordered_map<std::string, std::unique_ptr<MarketContext>> contexts_;

    // 전체 시작 여부 (재진입 방지 플래그)
    bool started_{false};
};

} // namespace app
