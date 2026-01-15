// engine/RealOrderEngine.h
#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <optional>
#include <unordered_set>

#include "core/domain/OrderRequest.h"
#include "core/domain/MyTrade.h"
#include "core/domain/Account.h"
#include "IOrderEngine.h"
#include "OrderStore.h"
#include "EngineResult.h"

// 실거래 주문 엔진(껍데기)
// - Step 1에서는 "스위치 교체 가능"하게만 만든다.
// - Step 2에서 REST POST(/orders) 실제 구현 연결
// - Step 3에서 WS myTrade 이벤트를 onMyTrade로 반영
namespace engine
{
    // (Step 2에서 /api 쪽 구현으로 연결될 인터페이스)
    // - 지금은 "엔진이 REST에 의존"하도록 붙이는 형태가 가장 단순하다.
    // - 나중에 Mock으로 테스트하기도 쉬워진다.
    struct IPrivateOrderApi
    {
        virtual ~IPrivateOrderApi() = default;

        // 주문을 전송 (POST /orders) 하고 결과로 거래소 주문 id(uuid)를 받는다.
        // 실패 시 nullopt.
        virtual std::optional<std::string> getOrderId(const core::OrderRequest& req) = 0;
    };

    class RealOrderEngine final : public IOrderEngine
    {
    public:
        // account: 로컬 캐시 (진실은 업비트 서버) - Step 3에서 체결 이벤트로 갱신한다.
        RealOrderEngine(IPrivateOrderApi& api, OrderStore& store, core::Account& account);

        // 전략이 만든 주문 의도를 "거래소로 전송"(POST)
        // [중요] Step1/2에서도 submit이 체결을 만들면 안 된다.
        EngineResult submit(const core::OrderRequest& req) override;

        // 실거래에선 체결은 항상 외부 이벤트로 들어오므로, 엔진은 이를 받아 반영한다.
        // Step 3에서 WS(myTrade) 연결 시 여기로 들어오게 된다.
        void onMyTrade(const core::MyTrade& t) override;

        // 주문 상태 변화(대기/취소/완료 등)를 반영할 훅.
        // Step 4(WS order 또는 REST 폴링)에서 사용한다.
        void onOrderStatus(std::string_view order_id, core::OrderStatus s) override;

        // WS 온 정보를 order로 만들어주는 헬퍼
        // “업비트가 보내준 ‘주문 전체 상태 스냅샷’을 엔진 내부 상태와 동기화하는 함수”
        void onOrderSnapshot(const core::Order& snapshot);

        std::optional<core::Order> get(std::string_view order_id) const override;

    private:
        // 요청 최소 검증(실거래에서 잘못된 주문은 곧 장애/손실로 이어지니 강하게 방어)
        static bool validateRequest(const core::OrderRequest& req, std::string& reason) noexcept;

        // "KRW-BTC" -> "BTC"
        static std::string extractCurrency(std::string_view market);

        // trade_uuid 중복 수신 방지
        bool markTradeOnce(std::string_view trade_id);
    private:
        IPrivateOrderApi& api_;
        OrderStore& store_;
        core::Account& account_;

        // WS 스레드 + 메인루프 스레드 등 동시성 대비
        std::mutex mtx_;

        // 이미 처리한 trade_id 집합(최소 방어)
        std::unordered_set<std::string> seen_trades_;
    };
}
