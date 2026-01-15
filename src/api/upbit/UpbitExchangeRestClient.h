#pragma once
#include <string>
#include <optional>

#include "../src/core/domain/Account.h"
#include "../src/api/auth/UpbitJwtSigner.h"
#include "../src/api/rest/RestClient.h"
#include "../src/core/domain/Order.h"


namespace api::rest {

    class UpbitExchangeRestClient {
    public:
        UpbitExchangeRestClient(RestClient& rest, api::auth::UpbitJwtSigner signer);

        // GET /v1/accounts
        std::variant<core::Account, api::rest::RestError> getMyAccount();

        // 미체결 주문 조회 (GET /v1/orders/open?market=...)
        std::variant<std::vector<core::Order>, api::rest::RestError>
            getOpenOrders(std::string_view market);

        // 주문 취소 (DELETE /v1/order?uuid=... OR identifier=...)
        // - 성공/실패만 필요
        std::variant<bool, api::rest::RestError>
            cancelOrder(const std::optional<std::string>& uuid,
                const std::optional<std::string>& identifier);

        // 앞으로 확장:
        // Result<std::vector<core::Order>> getOrders(...);
        // Result<core::Order> postOrder(...);

    private:
        RestClient& rest_;
        api::auth::UpbitJwtSigner signer_;
    };

} // namespace api::rest
