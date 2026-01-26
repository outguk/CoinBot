#include <iostream>

#include "MarketDataEventBridge.h"

namespace app {

    bool MarketDataEventBridge::isCandleMessage(std::string_view msg) noexcept
    {
        // 간단 필터: "type":"candle" 또는 "type":"candle.1s" 등
        // (정확 파싱은 엔진에서 1회 수행)
        const auto p = msg.find("\"type\"");
        if (p == std::string_view::npos) return false;

        const auto c = msg.find(':', p);
        if (c == std::string_view::npos) return false;

        auto rest = msg.substr(c + 1);
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\n' || rest.front() == '\r' || rest.front() == '\t'))
            rest.remove_prefix(1);

        if (rest.empty() || rest.front() != '"') return false;
        rest.remove_prefix(1);

        // prefix "candle"
        constexpr std::string_view k = "candle";
        if (rest.size() < k.size()) return false;
        return rest.compare(0, k.size(), k) == 0;
    }

    bool MarketDataEventBridge::onWsMessage(std::string_view msg)
    {
        if (!isCandleMessage(msg))
            return false;

        // MarketData는 삭제하여 무제한 메모리 증가를 방지
        constexpr std::size_t kMaxBacklog = 5000; // 운영 환경에 맞게 조절
        if (private_q_.size() > kMaxBacklog)
        {
            static std::uint64_t dropped = 0;
            if ((++dropped % 1000) == 0)
            {
                std::cout << "[Bridge][MarketData] dropped=" << dropped
                    << " backlog=" << private_q_.size() << "\n";
            }
            return true; // candle 메시지는 맞지만 큐 적재는 생략
        }

        private_q_.push(engine::input::MarketDataRaw{ std::string(msg) });
        return true;
    }

} // namespace app
