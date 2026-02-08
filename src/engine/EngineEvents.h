// engine/EngineEvents.h
#pragma once

#include <string>
#include <variant>

#include "core/domain/Types.h"
#include "core/domain/OrderTypes.h"

namespace engine
{
    // 엔진이 상위(App)로 내보내는 중립 이벤트 타입.
    // 이 이벤트를 받아 trading::FillEvent / trading::OrderStatusEvent로 변환해 전략에 전달한다.

    struct EngineFillEvent final
    {
        // 전략이 주문을 매칭할 때 사용하는 키(identifier).
        // - 업비트 uuid(order_id)와는 다르며, 프로그램이 부여한 identifier.
        std::string identifier;

        // 업비트 주문 uuid / 체결 uuid (추적/디버깅/중복 제거에 유용)
        std::string order_id;
        std::string trade_id;

        core::OrderPosition position{ core::OrderPosition::BID };
        core::Price  fill_price{ 0.0 };
        core::Volume filled_volume{ 0.0 };
    };

    // 종결 상태로 변경될 때만 이벤트 발생
    struct EngineOrderStatusEvent final
    {
        // 전략 매칭 키(client_order_id)
        std::string identifier;

        // 업비트 주문 uuid
        std::string order_id;

        core::OrderStatus   status{ core::OrderStatus::Pending };
        core::OrderPosition position{ core::OrderPosition::BID };

        // 아래 값들은 있으면 유용하지만(부분체결/잔량 확인) 없더라도 전략이 동작하도록 설계 가능
        double executed_volume{ 0.0 };  // 이 주문에서 지금까지 “실제로 체결된 누적 수량”
        double remaining_volume{ 0.0 }; // 이 주문에서 “아직 체결되지 않고 남아 있는 수량”
    };

    using EngineEvent = std::variant<EngineFillEvent, EngineOrderStatusEvent>;
}
