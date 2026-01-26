#pragma once
#include <optional>
#include <string>
#include "core/domain/OrderRequest.h"
#include "core/domain/Order.h"

namespace engine
{
    struct PrivateOrderApi
    {
        virtual ~PrivateOrderApi() = default;
        virtual std::optional<std::string> getOrderId(const core::OrderRequest& req) = 0;
    };
}
