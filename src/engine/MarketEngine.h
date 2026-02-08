// engine/MarketEngine.h
#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <unordered_set>
#include <deque>
#include <vector>
#include <thread>

#include "core/domain/OrderRequest.h"
#include "core/domain/MyTrade.h"
#include "core/domain/Order.h"
#include "OrderStore.h"
#include "EngineResult.h"
#include "EngineEvents.h"
#include "api/upbit/SharedOrderApi.h"
#include "trading/allocation/AccountManager.h"

// 마켓별 독립 주문 엔진
//
// RealOrderEngine과의 핵심 차이:
// - SharedOrderApi (thread-safe)를 통해 주문 제출
// - AccountManager를 통해 잔고 관리 (직접 Account 조작 대신)
// - reserve/finalize 기반 KRW 예약 (ReservationToken)
// - 마켓 스코프 검증 (자기 마켓 이벤트만 처리)
// - 중복 매수 방지 (active_buy_token_ 존재 시 거부)
//
// IOrderEngine을 구현하지 않음:
// - SharedOrderApi 반환 타입(variant)이 다르고 AccountManager 통합으로 인터페이스가 상이
// - Step 1.5(MarketManager)에서 사용 예정
namespace engine
{
    class MarketEngine final
    {
    public:
        MarketEngine(std::string market,
                     api::upbit::SharedOrderApi& api,
                     OrderStore& store,
                     trading::allocation::AccountManager& account_mgr);

        // 엔진을 현재 스레드로 바인딩 (엔진 루프 시작 시 1회 호출)
        void bindToCurrentThread();

        // 주문 제출 (BUY: 내부에서 reserve -> postOrder)
        EngineResult submit(const core::OrderRequest& req);

        // WS 체결 이벤트 (AccountManager finalizeFill* 호출)
        void onMyTrade(const core::MyTrade& t);

        // 주문 상태만 업데이트 (REST 폴링 등)
        void onOrderStatus(std::string_view order_id, core::OrderStatus s);

        // WS/REST 스냅샷 동기화 (터미널 시 토큰 정리)
        void onOrderSnapshot(const core::Order& snapshot);

        // 이벤트 배출
        std::vector<EngineEvent> pollEvents();

        // 주문 조회
        std::optional<core::Order> get(std::string_view order_id) const;

        const std::string& market() const noexcept { return market_; }

    private:
        // 주문 요청 최소 검증
        static bool validateRequest(const core::OrderRequest& req, std::string& reason) noexcept;

        // 시장가 매수(AmountSize) / 지정가(price*volume)에서 예약 금액 계산
        static core::Amount computeReserveAmount(const core::OrderRequest& req);

        // 단일 소유권(엔진 스레드 1개 호출) 검증
        void assertOwner_() const;

        // 이벤트 큐에 push
        void pushEvent_(EngineEvent ev);

        // "KRW-BTC" -> "BTC"
        static std::string extractCurrency(std::string_view market);

        // 중복 체결 방지용 키 생성
        static std::string makeTradeDedupeKey_(const core::MyTrade& t);

        // trade_id 중복 수신 방지 (FIFO 기반)
        bool markTradeOnce(std::string_view trade_id);

        // 매수 토큰 정리 (터미널 상태 도달 시 미사용 KRW 복구)
        // order_id: 토큰과 연결된 주문 ID (검증용)
        void finalizeBuyToken_(std::string_view order_id);

    private:
        std::string market_;
        api::upbit::SharedOrderApi& api_;
        OrderStore& store_;
        trading::allocation::AccountManager& account_mgr_;

        // 엔진 단일 소유권을 위한 owner thread
        std::thread::id owner_thread_{};

        // 이미 처리한 trade_id 집합 (중복 방어)
        std::unordered_set<std::string> seen_trades_;
        std::deque<std::string> seen_trade_fifo_;

        // 이벤트 큐
        std::deque<EngineEvent> events_;

        // 매수 주문 KRW 예약 토큰 (전량 거래 모델: 마켓당 최대 1개)
        std::optional<trading::allocation::ReservationToken> active_buy_token_;

        // 현재 활성 매수 주문 ID (토큰과 연결, 지연/외부 이벤트 방어용)
        std::string active_buy_order_id_;

        // 현재 활성 매도 주문 ID (전량 거래 모델: 마켓당 최대 1개, 중복 매도 방지)
        std::string active_sell_order_id_;

        // OrderStore cleanup 카운터 (인스턴스별 독립, 100개마다 실행)
        std::size_t completed_count_{0};
    };
}
