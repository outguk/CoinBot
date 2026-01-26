#include "MyOrderEventBridge.h"

#include <iostream>

namespace app {

    // 간단한 left - trim(공백 / 탭 / 개행)
        std::string_view MyOrderEventBridge::trimLeft(std::string_view s) noexcept
    {
        while (!s.empty())
        {
            const unsigned char ch = static_cast<unsigned char>(s.front());
            if (!std::isspace(ch)) break;
            s.remove_prefix(1);
        }
        return s;
    }

    bool MyOrderEventBridge::onWsMessage(std::string_view msg)
    {
        // 여기에서는 JSON 파싱을 하지 않고 type만 빠르게 검사
        if (!isMyOrderMessage(msg))
            return false; // not myOrder => ignore

        // 유실 불가: 엔진 스레드가 pop해서 처리하도록 큐에 적재
        // (엔진 스레드에서 단 한 번만 JSON 파싱 + DTO 변환)
        private_q_.push(engine::input::MyOrderRaw{ std::string(msg) });
        return true;
    }

    bool MyOrderEventBridge::isMyOrderMessage(std::string_view msg) noexcept
    {
        // 최소 필터: "type" + "myOrder" 포함 여부
        if (msg.find("\"type\"") == std::string_view::npos) return false;
        if (msg.find("myOrder") == std::string_view::npos) return false;
        return true;
    }

    //bool MyOrderEventBridge::isTerminal(core::OrderStatus s) noexcept
    //{
    //    using S = core::OrderStatus;
    //    switch (s)
    //    {
    //    case S::Filled:
    //    case S::Canceled:
    //    case S::Rejected:
    //        return true;
    //    case S::New:
    //    case S::Open:
    //    case S::Pending:
    //        return false;
    //    }
    //    return false; // enum 확장 대비
    //}

} // namespace app
