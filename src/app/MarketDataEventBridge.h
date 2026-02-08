#pragma once
#include <string_view>
#include <string>
#include "core/BlockingQueue.h"
#include "engine/input/EngineInput.h"

namespace app {

    class MarketDataEventBridge {
    public:
        using PrivateQueue = core::BlockingQueue<engine::input::EngineInput>;
        explicit MarketDataEventBridge(PrivateQueue& q) : private_q_(q) {}

        [[nodiscard]] bool onWsMessage(std::string_view msg);

    private:
        PrivateQueue& private_q_;

        // WS 스레드에서 JSON parse 없이 type prefix만 가볍게 필터
        static bool isCandleMessage(std::string_view msg) noexcept;
    };

} // namespace app
