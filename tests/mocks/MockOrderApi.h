// tests/mocks/MockOrderApi.h
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "api/upbit/IOrderApi.h"
#include "api/rest/RestError.h"
#include "core/domain/Account.h"
#include "core/domain/Order.h"
#include "core/domain/OrderRequest.h"

/*
 * MockOrderApi
 *
 * [역할]
 * 테스트용 IOrderApi 구현 (네트워크 호출 없이 즉시 응답)
 *
 * [특징]
 * 1. 동작 제어: set* 메서드로 반환값 미리 설정
 * 2. 호출 검증: 호출 횟수, 마지막 요청 내용 기록
 * 3. 에러 시나리오: RestError 반환 설정 가능
 * 4. 즉시 응답: 네트워크 없이 0.001ms 완료
 *
 * [사용 예시]
 * TEST(MarketEngineTest, SubmitBuySuccess) {
 *     MockOrderApi mock_api;
 *     mock_api.setPostOrderResult("order-uuid-123");  // 성공 설정
 *
 *     MarketEngine engine("KRW-BTC", mock_api, ...);
 *     auto result = engine.submit(buy_req);
 *
 *     EXPECT_TRUE(result.isSuccess());
 *     EXPECT_EQ(mock_api.postOrderCallCount(), 1);
 *     EXPECT_EQ(mock_api.lastPostOrderRequest().market, "KRW-BTC");
 * }
 *
 * TEST(MarketEngineTest, HandlePostOrderError) {
 *     MockOrderApi mock_api;
 *     mock_api.setPostOrderResult(api::rest::RestError{400, "insufficient balance"});  // 실패 설정
 *
 *     auto result = engine.submit(buy_req);
 *     EXPECT_FALSE(result.isSuccess());
 * }
 */
class MockOrderApi : public api::upbit::IOrderApi
{
public:
    MockOrderApi() = default;
    ~MockOrderApi() override = default;

    // ========== 동작 제어 메서드 ==========

    // postOrder 반환값 설정
    void setPostOrderResult(std::variant<std::string, api::rest::RestError> result)
    {
        post_order_result_ = std::move(result);
    }

    // getMyAccount 반환값 설정
    void setGetMyAccountResult(std::variant<core::Account, api::rest::RestError> result)
    {
        get_my_account_result_ = std::move(result);
    }

    // getOpenOrders 반환값 설정
    void setGetOpenOrdersResult(std::variant<std::vector<core::Order>, api::rest::RestError> result)
    {
        get_open_orders_result_ = std::move(result);
    }

    // cancelOrder 반환값 설정
    void setCancelOrderResult(std::variant<bool, api::rest::RestError> result)
    {
        cancel_order_result_ = std::move(result);
    }

    // ========== 호출 검증 메서드 ==========

    // postOrder 호출 횟수
    int postOrderCallCount() const noexcept { return post_order_call_count_; }

    // 마지막 postOrder 요청
    const core::OrderRequest& lastPostOrderRequest() const { return last_post_order_request_; }

    // getMyAccount 호출 횟수
    int getMyAccountCallCount() const noexcept { return get_my_account_call_count_; }

    // getOpenOrders 호출 횟수
    int getOpenOrdersCallCount() const noexcept { return get_open_orders_call_count_; }

    // cancelOrder 호출 횟수
    int cancelOrderCallCount() const noexcept { return cancel_order_call_count_; }

    // 통계 초기화
    void reset()
    {
        post_order_call_count_ = 0;
        get_my_account_call_count_ = 0;
        get_open_orders_call_count_ = 0;
        cancel_order_call_count_ = 0;
        last_post_order_request_ = {};
    }

    // ========== IOrderApi 구현 ==========

    std::variant<std::string, api::rest::RestError>
    postOrder(const core::OrderRequest& req) override
    {
        ++post_order_call_count_;
        last_post_order_request_ = req;
        return post_order_result_;
    }

    std::variant<core::Account, api::rest::RestError>
    getMyAccount() override
    {
        ++get_my_account_call_count_;
        return get_my_account_result_;
    }

    std::variant<std::vector<core::Order>, api::rest::RestError>
    getOpenOrders(std::string_view market) override
    {
        ++get_open_orders_call_count_;
        last_get_open_orders_market_ = std::string(market);
        return get_open_orders_result_;
    }

    std::variant<bool, api::rest::RestError>
    cancelOrder(const std::optional<std::string>& uuid,
                const std::optional<std::string>& identifier) override
    {
        ++cancel_order_call_count_;
        last_cancel_order_uuid_ = uuid;
        last_cancel_order_identifier_ = identifier;
        return cancel_order_result_;
    }

private:
    // 반환값 저장
    std::variant<std::string, api::rest::RestError> post_order_result_{"mock-order-uuid"};
    std::variant<core::Account, api::rest::RestError> get_my_account_result_{core::Account{}};
    std::variant<std::vector<core::Order>, api::rest::RestError> get_open_orders_result_{std::vector<core::Order>{}};
    std::variant<bool, api::rest::RestError> cancel_order_result_{true};

    // 호출 기록
    int post_order_call_count_{0};
    int get_my_account_call_count_{0};
    int get_open_orders_call_count_{0};
    int cancel_order_call_count_{0};

    core::OrderRequest last_post_order_request_{};
    std::string last_get_open_orders_market_;
    std::optional<std::string> last_cancel_order_uuid_;
    std::optional<std::string> last_cancel_order_identifier_;
};
