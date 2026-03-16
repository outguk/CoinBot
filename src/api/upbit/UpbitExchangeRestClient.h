#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "api/auth/UpbitJwtSigner.h"
#include "api/rest/RestClient.h"
#include "core/domain/Account.h"
#include "core/domain/Order.h"
#include "core/domain/OrderRequest.h"


namespace api::rest {

    class UpbitExchangeRestClient {
    public:
        UpbitExchangeRestClient(RestClient& rest, api::auth::UpbitJwtSigner signer);

        // GET /v1/accounts
        std::variant<core::Account, api::rest::RestError> 
            getMyAccount();

        // 미체결 주문 조회 (GET /v1/orders/open?market=...)
        std::variant<std::vector<core::Order>, api::rest::RestError>
            getOpenOrders(std::string_view market);

        // 주문 취소 (DELETE /v1/order?uuid=... OR identifier=...)
        // - 성공/실패만 필요
        std::variant<bool, api::rest::RestError>
            cancelOrder(const std::optional<std::string>& order_uuid,
                const std::optional<std::string>& identifier);

        // GET /v1/order?uuid=...
        // - 단건 주문 조회 (reconnect 복구용)
        std::variant<core::Order, api::rest::RestError>
            getOrder(std::string_view order_uuid);

        // POST /v1/orders
        // - core::OrderRequest -> Upbit order create
        // - 반환 core::Order.id 는 Upbit order_uuid
        std::variant<std::string, api::rest::RestError>
            postOrder(const core::OrderRequest& req);



        // 앞으로 확장:
        // Result<std::vector<core::Order>> getOrders(...);
        // Result<core::Order> postOrder(...);

    private:
        RestClient& rest_;
        api::auth::UpbitJwtSigner signer_;
    };

} // namespace api::rest

