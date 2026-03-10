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
#include "api/upbit/IOrderApi.h"
#include "trading/allocation/AccountManager.h"

// 마켓별 독립 주문 엔진
//
// - IOrderApi (인터페이스)를 통해 주문 제출 (의존성 역전, 테스트 가능성)
// - AccountManager를 통해 잔고 관리 (직접 Account 조작 대신)
// - reserve/finalize 기반 KRW 예약 (ReservationToken)
// - 마켓 스코프 검증 (자기 마켓 이벤트만 처리)
// - 중복 매수 방지 (active_buy_token_ 존재 시 거부)
//
namespace engine
{
    class MarketEngine final
    {
    public:
        MarketEngine(std::string market,
                     api::upbit::IOrderApi& api,
                     OrderStore& store,
                     trading::allocation::AccountManager& account_mgr);

        // 엔진을 현재 스레드로 바인딩 (엔진 루프 시작 시 1회 호출)
        void bindToCurrentThread() noexcept;

        // 주문 제출 (BUY의 경우: 내부에서 reserve -> postOrder)
        EngineResult submit(const core::OrderRequest& req);

        // WS 체결 이벤트 (AccountManager finalizeFill* 호출)
        void onMyTrade(const core::MyTrade& t);

        // 주문 상태만 업데이트 (REST 폴링 등)
        void onOrderStatus(std::string_view order_uuid, core::OrderStatus s);

        // WS/REST 스냅샷 동기화 (터미널 시 토큰 정리)
        void onOrderSnapshot(const core::Order& snapshot);

        // 이벤트 배출 (Manager에서 호출됨)
        std::vector<EngineEvent> pollEvents();

        // 주문 조회
        std::optional<core::Order> get(std::string_view order_uuid) const;

        // 현재 활성 pending 주문 ID 반환 (복구 시 조회 대상 결정용)
        struct PendingIds {
            std::string buy_order_uuid;
            std::string sell_order_uuid;
        };
        PendingIds activePendingIds() const noexcept;

        // REST snapshot으로 유실된 체결분(delta)만 정산
        // OrderStore 누적값과 snapshot 누적값의 차이를 계산하여 반영
        // 멱등성: 동일 snapshot 재주입 시 delta=0이므로 무동작
        // 중요: delta 정산 → onOrderSnapshot 순서 (터미널 시 토큰 보호)
        // 정산 금액이 불명확하면 false를 반환하고 스냅샷 확정을 보류한다.
        bool reconcileFromSnapshot(const core::Order& snapshot);

        const std::string& market() const noexcept { return market_; }

        // 확정 캔들 close 가격을 주입 (MarketEngineManager에서 캔들마다 호출)
        // finalizeSellOrder의 가치 기준 dust 판정에 사용된다
        void setMarkPrice(core::Price p) noexcept { last_mark_price_ = p; }

    private:
		// 내부 주문 요청 검증 헬퍼(시장 스코프, 매수 토큰 중복 방지, 지정가 가격/수량 검증 등)
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

        // trade_uuid 중복 수신 방지 (FIFO 기반)
        bool markTradeOnce(std::string_view trade_uuid);

        // 매수 토큰 정리 (터미널 상태 도달 시 미사용 KRW 복구)
        // AccountManager가 finalizeOrder로 토큰을 비활성화 하기 전, 토큰이 있는지, order_uuid가 일치하는지 등을 확인하는 일종의 게이트 역할
        // order_uuid: 토큰과 연결된 주문 ID (검증용)
        void finalizeBuyToken_(std::string_view order_uuid);

    private:
        std::string market_;
        api::upbit::IOrderApi& api_;
        OrderStore& store_;
        trading::allocation::AccountManager& account_mgr_;

        // 엔진 단일 소유권을 위한 owner thread (thread id를 저장)
        std::thread::id owner_thread_{};

        // 이미 처리한 trade_uuid 집합 (중복 방어)
        std::unordered_set<std::string> seen_trade_uuids_;
        std::deque<std::string> seen_trade_uuid_fifo_;

        // 이벤트 큐
        std::deque<EngineEvent> events_;

        // 매수 주문 KRW 예약 토큰 (전량 거래 모델: 마켓당 최대 1개)
        std::optional<trading::allocation::ReservationToken> active_buy_token_;

        // 현재 활성 매수 주문 ID (토큰과 연결, 지연/외부 이벤트 방어용)
        std::string active_buy_order_uuid_;

        // 현재 활성 매도 주문 ID (전량 거래 모델: 마켓당 최대 1개, 중복 매도 방지)
        std::string active_sell_order_uuid_;

        // 가장 최근에 확정된 캔들의 close 가격 (finalizeSellOrder dust 판정용)
        core::Price last_mark_price_{0.0};
    };
}

