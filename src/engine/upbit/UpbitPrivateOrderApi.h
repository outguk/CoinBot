#pragma once

#include <optional>
#include <string>
#include <variant>

#include "engine/PrivateOrderApi.h"
#include "api/upbit/UpbitExchangeRestClient.h"

namespace engine::upbit
{
    // PrivateOrderApi의 Upbit 구현체.
    // - UpbitExchangeRestClient::postOrder()를 호출해서 uuid만 뽑아 엔진에 제공한다.
    class UpbitPrivateOrderApi final : public PrivateOrderApi
    {
    public:
        explicit UpbitPrivateOrderApi(api::rest::UpbitExchangeRestClient& client)
            : client_(client) {
        }

        std::optional<std::string> getOrderId(const core::OrderRequest& req) override;

    private:
        api::rest::UpbitExchangeRestClient& client_;
    };
}
