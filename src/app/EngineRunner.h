// app/EngineRunner.h
#pragma once

#include <atomic>

#include "core/BlockingQueue.h"
#include "engine/input/EngineInput.h"
#include "core/domain/Account.h"
#include "engine/RealOrderEngine.h"
#include "trading/strategies/StrategyTypes.h"
#include "trading/strategies/RsiMeanReversionStrategy.h"

// 여기서부터는 "엔진 단일 스레드"로만 호출될 것.
// WS는 queue push만 담당.
// “무엇을, 어떤 순서로, 어떤 스레드에서 호출할지”를 결정
namespace app
{
    struct EngineRunnerConfig
    {
        // 필요하면 tick sleep, batch size, 종료 조건 등을 확장
        int max_private_batch = 256; // myOrder는 유실 불가라 배치로 최대한 빨리 소화
    };

    class EngineRunner
    {
    public:
        using PrivateQueue = core::BlockingQueue<engine::input::EngineInput>;

        EngineRunner(engine::RealOrderEngine& engine,
            trading::strategies::RsiMeanReversionStrategy strategy,
            PrivateQueue& private_q,
            core::Account& account,
            std::string market,
            EngineRunnerConfig cfg = {})
            : engine_(engine)
            , strategy_(std::move(strategy))
            , private_q_(private_q)
            , account_(account)
            , market_(std::move(market))
            , cfg_(cfg)
        {
            // 시작 시점에 account 캐시 1회 생성
            rebuildAccountSnapshot_();
        }

        // 단일 엔진 스레드에서 호출
        void run(std::atomic<bool>& stop_flag);

    private:
        // Account/Position(core::Account)이 바뀌었을 때만 스냅샷을 재구성
        void rebuildAccountSnapshot_();

        static std::string extractCurrency_(std::string_view market);

        void handleOne_(const engine::input::EngineInput& in);

        // engine::EngineEvent -> trading 이벤트로 변환 후 RSI 전략에 직접 전달
        void handleEngineEvents_(const std::vector<engine::EngineEvent>& evs);

    private:
        engine::RealOrderEngine& engine_;
        trading::strategies::RsiMeanReversionStrategy strategy_;
        PrivateQueue& private_q_;
        core::Account& account_;
        std::string market_;
        trading::AccountSnapshot last_account_{};
        EngineRunnerConfig cfg_;
    };
}
