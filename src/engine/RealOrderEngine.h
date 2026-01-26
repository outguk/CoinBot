// engine/RealOrderEngine.h
#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <optional>
#include <unordered_set>
#include <deque>
#include <vector>

#include "core/domain/OrderRequest.h"
#include "core/domain/MyTrade.h"
#include "core/domain/Account.h"
#include "IOrderEngine.h"
#include "OrderStore.h"
#include "EngineResult.h"
#include "EngineEvents.h"
#include "PrivateOrderApi.h"

// 실거래 주문 엔진
// RealOrderEngine은 “상태 머신 + 영속(스토어) + 계좌/포지션 업데이트”
namespace engine
{
    class RealOrderEngine final : public IOrderEngine
    {
    public:
        // account: 로컬 캐시 (진실은 업비트 서버) - Step 3에서 체결 이벤트로 갱신한다.
        RealOrderEngine(PrivateOrderApi& api, OrderStore& store, core::Account& account);

        // 엔진을 현재 스레드(엔진 루프 스레드)로 바인딩한다.
        // 반드시 엔진 루프 시작 시점에 1회 호출해야 한다.
        void bindToCurrentThread();

        // 전략이 만든 주문 의도를 "거래소로 전송"(POST) getOrderId는 submit안에서 쓰인다
        EngineResult submit(const core::OrderRequest& req) override;

        // WS(myTrade) 연결 후 체결이 발생하면 여기로 들어오게 된다.
        // 주문 누적/상태 업데이트, 계좌/포지션 로컬 캐시(best-effort) 갱신, EngineFillEvent 발행
        void onMyTrade(const core::MyTrade& t) override;

        // 체결이 누적되다가 주문 상태 변화(대기/취소/완료 등)를 반영
        void onOrderStatus(std::string_view order_id, core::OrderStatus s) override;

        // 수정된 정보를 덮어쓴다
        // “업비트가 보내준 ‘주문 전체 상태 스냅샷’을 엔진 내부 상태와 동기화하는 함수”
        void onOrderSnapshot(const core::Order& snapshot);

        // [핵심] 엔진이 수집한 체결/상태 이벤트를 상위(App)가 가져갈 수 있도록 배출한다.
        // - WS 스레드/메인 루프 스레드가 분리되더라도 안전하게 동작하도록 내부 큐를 사용.
        // - 반환된 이벤트는 App에서 trading 이벤트로 매핑하여 strategy에 전달
        std::vector<EngineEvent> pollEvents();

        std::optional<core::Order> get(std::string_view order_id) const override;

    private:
        // 요청 최소 검증(실거래에서 잘못된 주문은 곧 장애/손실로 이어지니 강하게 방어)
        static bool validateRequest(const core::OrderRequest& req, std::string& reason) noexcept;

        // 단일 소유권(엔진 스레드 1개 호출) 검증용 가드
        void assertOwner_() const;

        // 엔진 내부 이벤트 큐에 push (락 포함)
        void pushEvent_(EngineEvent ev);

        // "KRW-BTC" -> "BTC"
        static std::string extractCurrency(std::string_view market);
        static std::string makeTradeDedupeKey_(const core::MyTrade& t);

        // trade_uuid 중복 수신 방지
        bool markTradeOnce(std::string_view trade_id);
    private:
        PrivateOrderApi& api_;
        OrderStore& store_;
        core::Account& account_;

        // 엔진 단일 소유권을 위한 "owner thread"
        std::thread::id owner_thread_{};

        // 이미 처리한 trade_id 집합(최소 방어)
        std::unordered_set<std::string> seen_trades_;
        std::deque<std::string> seen_trade_fifo_;
        // 이미 처리한 trade_id 중복 방어용(장시간 운영 시 메모리 무한증가 방지)
        static constexpr std::size_t kMaxSeenTrades = 20'000;

        // 엔진이 생성한 이벤트 큐 (체결 이벤트)
        std::deque<EngineEvent> events_;
    };
}
