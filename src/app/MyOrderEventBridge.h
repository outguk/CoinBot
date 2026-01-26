#pragma once

#include <string_view>
#include <atomic>
#include <json.hpp>

#include "core/BlockingQueue.h"
#include "engine/input/EngineInput.h"

namespace app {

    // WS raw(myOrder) -> 타입 검사 -> 엔진이 처리할 BlockingQueue에 push
    class MyOrderEventBridge {
    public:
        using PrivateQueue = core::BlockingQueue<engine::input::EngineInput>;

        explicit MyOrderEventBridge(PrivateQueue& q, std::atomic<bool>& needs_resync) 
            : private_q_(q) {}

        // msg가 myOrder가 아니면 false(무시).
        // myOrder 메시지이면 파싱/처리 결과를 true/false로 반환.
        [[nodiscard]] bool onWsMessage(std::string_view msg);

        //static bool isTerminalStatus(core::OrderStatus s) noexcept { return isTerminal(s); }

    private:
        PrivateQueue& private_q_;

        // JSON 파싱 없이 "type":"myOrder" 필드만 가볍게 검사
        static bool isMyOrderMessage(std::string_view msg);
        static std::string_view trimLeft(std::string_view s) noexcept;
        /*static bool isTerminal(core::OrderStatus s) noexcept;*/
    };

} // namespace app
