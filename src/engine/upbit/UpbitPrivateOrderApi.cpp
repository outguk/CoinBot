#include <iostream>

#include "engine/upbit/UpbitPrivateOrderApi.h"

namespace engine::upbit
{
    std::optional<std::string>
        UpbitPrivateOrderApi::getOrderId(const core::OrderRequest& req)
    {
        // Upbit로 실제 주문 생성(POST /v1/orders)
        auto r = client_.postOrder(req);

        // 실패면 nullopt (엔진 submit이 Fail 처리)
        if (std::holds_alternative<api::rest::RestError>(r))
        {
            const auto& e = std::get<api::rest::RestError>(r);
            std::cout << "[UpbitPrivateOrderApi][getOrderId] FAIL restCode="
                << static_cast<int>(e.code)
                << " http=" << e.http_status
                << " msg=" << e.message
                << "\n";
            return std::nullopt;
        }
            

        // 성공이면 order_uuid만 반환
        const auto& order_uuid = std::get<std::string>(r);
        if (order_uuid.empty())
        {
            std::cout << "[UpbitPrivateOrderApi] postOrder returned empty order_uuid\n";
            return std::nullopt;
        }
            

        return order_uuid;
    }
}

