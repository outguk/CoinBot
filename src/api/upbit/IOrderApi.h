// src/api/upbit/IOrderApi.h
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/domain/Account.h"
#include "core/domain/Order.h"
#include "core/domain/OrderRequest.h"
#include "api/rest/RestError.h"

namespace api::upbit
{
    /*
     * IOrderApi
     *
     * [역할]
     * 주문 API 인터페이스 (의존성 역전 원칙, Dependency Inversion Principle)
     * - MarketEngine이 구체 타입(SharedOrderApi)에 의존하지 않도록 추상화
     * - 테스트 가능성 확보 (MockOrderApi 주입 가능)
     *
     * [설계]
     * - 순수 가상 함수(= 0)로 계약(contract) 정의
     * - SharedOrderApi (프로덕션), MockOrderApi (테스트)가 이를 구현
     * - 가상 함수 테이블(vtable)을 통한 런타임 다형성
     *
     * [사용]
     * 프로덕션:
     *   SharedOrderApi real_api(rest_client);
     *   MarketEngine engine(market, real_api, ...);  // IOrderApi& = SharedOrderApi&
     *
     * 테스트:
     *   MockOrderApi mock_api;
     *   MarketEngine engine(market, mock_api, ...);  // IOrderApi& = MockOrderApi&
     *
     * [Thread-Safety]
     * - 인터페이스 자체는 스레드 안전성을 보장하지 않음
     * - 구현체(SharedOrderApi 등)가 내부적으로 동기화 처리
     */
    class IOrderApi
    {
    public:
        virtual ~IOrderApi() = default;

        /*
         * GET /v1/accounts
         * 계좌 정보 조회 (KRW 잔고, 코인 보유량 등)
         */
        virtual std::variant<core::Account, api::rest::RestError>
            getMyAccount() = 0;

        /*
         * GET /v1/orders/open?market=...
         * 특정 마켓의 미체결 주문 조회
         */
        virtual std::variant<std::vector<core::Order>, api::rest::RestError>
            getOpenOrders(std::string_view market) = 0;

        /*
         * DELETE /v1/order?uuid=... OR identifier=...
         * 주문 취소
         */
        virtual std::variant<bool, api::rest::RestError>
            cancelOrder(const std::optional<std::string>& uuid,
                        const std::optional<std::string>& identifier) = 0;

        /*
         * POST /v1/orders
         * 주문 제출 (매수/매도)
         * @return Upbit 주문 UUID (Order.id로 사용)
         */
        virtual std::variant<std::string, api::rest::RestError>
            postOrder(const core::OrderRequest& req) = 0;
    };

} // namespace api::upbit
