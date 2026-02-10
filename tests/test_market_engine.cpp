// tests/test_market_engine.cpp
//
// MarketEngine 유닛 테스트
// - IOrderApi 인터페이스를 통한 의존성 주입으로 네트워크 없이 테스트
// - MockOrderApi로 API 응답 제어 및 호출 검증

#include <iostream>
#include <memory>
#include <string>
#include <variant>

#include "test_utils.h"
#include "engine/MarketEngine.h"
#include "engine/OrderStore.h"
#include "trading/allocation/AccountManager.h"
#include "core/domain/Account.h"
#include "core/domain/OrderRequest.h"
#include "core/domain/MyTrade.h"
#include "core/domain/Order.h"
#include "util/Config.h"
#include "mocks/MockOrderApi.h"

using namespace engine;
using namespace core;
using namespace trading::allocation;

namespace test {

    // ========== 테스트 헬퍼 ==========

    // BUY 요청 생성
    OrderRequest makeBuyRequest(const std::string& market, double krw_amount, std::string identifier = "") {
        OrderRequest req;
        req.market = market;
        req.position = OrderPosition::BID;
        req.type = OrderType::Market;
        req.size = AmountSize{krw_amount};
        req.identifier = identifier;
        return req;
    }

    // SELL 요청 생성 (시장가)
    OrderRequest makeSellRequest(const std::string& market, double volume, std::string identifier = "") {
        OrderRequest req;
        req.market = market;
        req.position = OrderPosition::ASK;
        req.type = OrderType::Market;
        req.size = VolumeSize{volume};
        req.identifier = identifier;
        return req;
    }

    // SELL 요청 생성 (지정가)
    OrderRequest makeSellLimitRequest(const std::string& market, double volume, double price) {
        OrderRequest req;
        req.market = market;
        req.position = OrderPosition::ASK;
        req.type = OrderType::Limit;
        req.size = VolumeSize{volume};
        req.price = price;
        return req;
    }

    // ========== 기본 기능 테스트 ==========

    // [TEST 1] 엔진 생성 검증
    // 목적: MarketEngine이 올바른 마켓 이름으로 생성되는지 확인
    void testConstruction() {
        std::cout << "\n[TEST 1] Construction\n";

        // Arrange: 계좌, AccountManager, OrderStore, MockAPI 준비
        Account account;
        account.krw_free = 1000000.0;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);

        // Assert: 엔진이 올바른 마켓 이름을 반환하는지 확인
        TEST_ASSERT(engine.market() == "KRW-BTC");

        std::cout << "  [PASS] Engine constructed with correct market\n";
    }

    // [TEST 2] 매수 주문 제출 성공
    // 목적: 정상적인 BUY 주문이 성공적으로 제출되는지 확인
    // 검증: API 호출, KRW 예약, 주문 정보 정확성
    void testSubmitBuySuccess() {
        std::cout << "\n[TEST 2] Submit buy success\n";

        // Arrange: 100만원 잔고로 시작
        Account account;
        account.krw_free = 1000000.0;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("mock-order-uuid");  // API 성공 설정
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        // Act: 10만원 매수 주문 제출
        auto req = makeBuyRequest("KRW-BTC", 100000);
        auto result = engine.submit(req);

        // Assert: 주문 성공 확인
        TEST_ASSERT(result.success);
        TEST_ASSERT(mock_api.postOrderCallCount() == 1);  // API 1회 호출
        TEST_ASSERT(mock_api.lastPostOrderRequest().market == "KRW-BTC");
        TEST_ASSERT(mock_api.lastPostOrderRequest().position == OrderPosition::BID);

        // Assert: KRW가 예약되었는지 확인 (전량 거래 모델)
        auto budget = account_mgr.getBudget("KRW-BTC");
        TEST_ASSERT(budget.has_value());
        TEST_ASSERT(budget->reserved_krw > 0);  // reserve_margin 적용된 금액

        std::cout << "  reserved_krw: " << budget->reserved_krw << "\n";
        std::cout << "  [PASS] Buy order submitted successfully\n";
    }

    // [TEST 3] 매도 주문 제출 성공
    // 목적: 코인 보유 시 SELL 주문이 성공적으로 제출되는지 확인
    // 전제: 0.01 BTC 보유 상태
    void testSubmitSellSuccess() {
        std::cout << "\n[TEST 3] Submit sell success\n";

        // Arrange: 0.01 BTC 보유 상태로 시작
        Account account;
        account.krw_free = 1000000;
        account.positions.push_back(Position{"BTC", 0.01, 50000000});  // 0.01 BTC @ 5천만원
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("sell-order-uuid");  // API 성공 설정
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        // Act: 전량(0.01 BTC) 매도 주문 제출
        auto req = makeSellRequest("KRW-BTC", 0.01);
        auto result = engine.submit(req);

        // Assert: 주문 성공 및 ASK 포지션 확인
        TEST_ASSERT(result.success);
        TEST_ASSERT(mock_api.postOrderCallCount() == 1);
        TEST_ASSERT(mock_api.lastPostOrderRequest().position == OrderPosition::ASK);

        std::cout << "  [PASS] Sell order submitted successfully\n";
    }

    // ========== 중복 방지 테스트 ==========

    // [TEST 4] 중복 매수 주문 거부
    // 목적: 전량 거래 모델에서 BUY 주문이 활성화된 상태에서 추가 BUY 차단 확인
    // 핵심: active_buy_token_ 존재 시 중복 매수 불가
    void testDuplicateBuyRejection() {
        std::cout << "\n[TEST 4] Duplicate buy rejection\n";

        // Arrange: 100만원 잔고로 시작
        Account account;
        account.krw_free = 1000000.0;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("mock-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        // Act: 동일한 BUY 주문 2회 제출
        auto req = makeBuyRequest("KRW-BTC", 100000);
        auto result1 = engine.submit(req);  // 첫 번째 → 성공 (active_buy_token_ 생성)
        auto result2 = engine.submit(req);  // 두 번째 → 거부 (active_buy_token_ 존재)

        // Assert: 첫 번째는 성공, 두 번째는 거부
        TEST_ASSERT(result1.success);
        TEST_ASSERT(!result2.success);
        TEST_ASSERT(result2.code == EngineErrorCode::OrderRejected);
        TEST_ASSERT(mock_api.postOrderCallCount() == 1);  // API는 1회만 호출

        std::cout << "  [PASS] Duplicate buy rejected\n";
    }

    // [TEST 5] 중복 매도 주문 거부
    // 목적: 전량 거래 모델에서 SELL 주문이 활성화된 상태에서 추가 SELL 차단 확인
    // 핵심: active_sell_order_id_ 존재 시 중복 매도 불가
    void testDuplicateSellRejection() {
        std::cout << "\n[TEST 5] Duplicate sell rejection\n";

        // Arrange: 0.01 BTC 보유 상태로 시작
        Account account;
        account.krw_free = 1000000;
        account.positions.push_back(Position{"BTC", 0.01, 50000000});
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("sell-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        // Act: 동일한 SELL 주문 2회 제출
        auto req = makeSellRequest("KRW-BTC", 0.01);
        auto result1 = engine.submit(req);  // 첫 번째 → 성공 (active_sell_order_id_ 설정)
        auto result2 = engine.submit(req);  // 두 번째 → 거부 (active_sell_order_id_ 존재)

        // Assert: 첫 번째는 성공, 두 번째는 거부
        TEST_ASSERT(result1.success);
        TEST_ASSERT(!result2.success);
        TEST_ASSERT(result2.code == EngineErrorCode::OrderRejected);
        TEST_ASSERT(mock_api.postOrderCallCount() == 1);

        std::cout << "  [PASS] Duplicate sell rejected\n";
    }

    // ========== 반대 포지션 방지 테스트 ==========

    // [TEST 6] SELL 활성 시 BUY 차단
    // 목적: 전량 거래 모델에서 SELL 주문이 활성화된 상태에서 BUY 차단 확인
    // 핵심: active_sell_order_id_ 존재 시 BUY 불가 ("sell order is active" 메시지)
    void testOppositeSellBlocksBuy() {
        std::cout << "\n[TEST 6] Opposite sell blocks buy\n";

        // Arrange: 0.01 BTC 보유 상태로 시작
        Account account;
        account.krw_free = 1000000;
        account.positions.push_back(Position{"BTC", 0.01, 50000000});
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("sell-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        // Act: SELL 먼저 제출 → active_sell_order_id_ 설정
        auto sell_req = makeSellRequest("KRW-BTC", 0.01);
        engine.submit(sell_req);

        // Act: BUY 시도 → 반대 포지션 방지 로직에 의해 차단
        auto buy_req = makeBuyRequest("KRW-BTC", 100000);
        auto result = engine.submit(buy_req);

        // Assert: BUY 거부 및 "sell order is active" 메시지 확인
        TEST_ASSERT(!result.success);
        TEST_ASSERT(result.code == EngineErrorCode::OrderRejected);
        TEST_ASSERT(result.message.find("sell order is active") != std::string::npos);

        std::cout << "  [PASS] Buy blocked when sell is active\n";
    }

    // [TEST 7] BUY 활성 시 SELL 차단
    // 목적: 전량 거래 모델에서 BUY 주문이 활성화된 상태에서 SELL 차단 확인
    // 핵심: active_buy_token_ 존재 시 SELL 불가 ("buy order is active" 메시지)
    // 참고: 반대 포지션 체크가 먼저 수행되므로 코인 없어도 차단됨
    void testOppositeBuyBlocksSell() {
        std::cout << "\n[TEST 7] Opposite buy blocks sell\n";

        // Arrange: KRW만으로 시작 (BUY 성공을 위해 코인 없이)
        Account account;
        account.krw_free = 1000000;
        // positions 제거: 코인 없이 시작
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        // Act: BUY 먼저 제출 → active_buy_token_ 생성
        auto buy_req = makeBuyRequest("KRW-BTC", 100000);
        auto buy_result = engine.submit(buy_req);
        TEST_ASSERT(buy_result.success);

        // Act: SELL 시도 → 반대 포지션 체크에서 차단 (코인 없어도 이 체크가 먼저)
        auto sell_req = makeSellRequest("KRW-BTC", 0.01);
        auto result = engine.submit(sell_req);

        // Assert: SELL 거부 및 "buy order is active" 메시지 확인
        TEST_ASSERT(!result.success);
        TEST_ASSERT(result.code == EngineErrorCode::OrderRejected);
        TEST_ASSERT(result.message.find("buy order is active") != std::string::npos);

        std::cout << "  [PASS] Sell blocked when buy is active\n";
    }

    // ========== 마켓 검증 테스트 ==========

    // [TEST 8] 다른 마켓 주문 거부
    // 목적: MarketEngine이 자신의 마켓이 아닌 주문을 거부하는지 확인
    // 핵심: 엔진은 생성 시 지정된 마켓만 처리 (KRW-BTC 엔진은 KRW-ETH 거부)
    // 참고: API 호출 전에 검증하므로 불필요한 네트워크 요청 방지
    void testRejectWrongMarket() {
        std::cout << "\n[TEST 8] Reject wrong market\n";

        // Arrange: KRW-BTC 엔진 준비
        Account account;
        account.krw_free = 1000000.0;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("mock-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        // Act: 다른 마켓(KRW-ETH) 주문 제출
        auto req = makeBuyRequest("KRW-ETH", 100000);
        auto result = engine.submit(req);

        // Assert: 마켓 불일치로 거부, API 호출 없음
        TEST_ASSERT(!result.success);
        TEST_ASSERT(result.code == EngineErrorCode::MarketNotSupported);
        TEST_ASSERT(mock_api.postOrderCallCount() == 0);  // API 호출 전에 검증됨

        std::cout << "  [PASS] Wrong market rejected\n";
    }

    // ========== 에러 처리 테스트 ==========

    // [TEST 9] 잔고 부족 감지
    // 목적: 주문 금액이 사용 가능한 KRW보다 클 때 거부되는지 확인
    // 핵심: AccountManager.reserve() 실패 시 InsufficientFunds 에러 반환
    // 참고: reserve_margin(1.01) 적용으로 요청 금액보다 더 많은 KRW 필요
    void testInsufficientBalance() {
        std::cout << "\n[TEST 9] Insufficient balance\n";

        // Arrange: 100만원 잔고로 시작
        Account account;
        account.krw_free = 1000000.0;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("mock-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        // Act: 200만원 매수 시도 (잔고 초과)
        auto req = makeBuyRequest("KRW-BTC", 2000000);
        auto result = engine.submit(req);

        // Assert: 잔고 부족으로 거부, API 호출 없음
        TEST_ASSERT(!result.success);
        TEST_ASSERT(result.code == EngineErrorCode::InsufficientFunds);
        TEST_ASSERT(mock_api.postOrderCallCount() == 0);  // reserve 실패 후 중단

        std::cout << "  [PASS] Insufficient balance detected\n";
    }

    // [TEST 10] API 실패 시 예약 해제
    // 목적: postOrder() API 호출 실패 시 예약된 KRW가 올바르게 해제되는지 확인
    // 핵심: 트랜잭션 롤백 - reserve 성공 후 API 실패 시 unreserve 호출
    // 참고: 예약된 자금이 영구 잠김 방지 (자금 누수 방지)
    void testPostOrderFailure_ShouldReleaseReservation() {
        std::cout << "\n[TEST 10] PostOrder failure should release reservation\n";

        // Arrange: 100만원 잔고, API는 400 에러 반환하도록 설정
        Account account;
        account.krw_free = 1000000.0;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult(api::rest::RestError{api::rest::RestErrorCode::BadStatus, "api error", 400 });
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        // Act: 주문 제출 (reserve 성공 → API 호출 실패 → unreserve)
        auto req = makeBuyRequest("KRW-BTC", 100000);
        auto result = engine.submit(req);

        // Assert: 주문 실패 및 예약 해제 확인
        TEST_ASSERT(!result.success);
        TEST_ASSERT(result.code == EngineErrorCode::InternalError);

        // Assert: reserved_krw = 0, available_krw 원래대로 복구
        auto budget = account_mgr.getBudget("KRW-BTC");
        TEST_ASSERT(budget.has_value());
        TEST_ASSERT(budget->reserved_krw == 0);
        TEST_ASSERT_DOUBLE_EQ(budget->available_krw, 1000000);

        std::cout << "  [PASS] Reservation released on API failure\n";
    }

    // ========== 체결 처리 테스트 ==========

    // [TEST 11] 중복 체결 방지
    // 목적: 동일한 trade_id를 가진 체결이 여러 번 들어와도 한 번만 처리되는지 확인
    // 핵심: seen_trades_ 맵으로 이미 처리된 trade_id 추적
    // 참고: WebSocket 재연결 시 중복 메시지 수신 가능성 대비
    void testOnMyTradeDuplicatePrevention() {
        std::cout << "\n[TEST 11] OnMyTrade duplicate prevention\n";

        // Arrange: BUY 주문 제출
        Account account;
        account.krw_free = 1000000.0;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("mock-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        auto req = makeBuyRequest("KRW-BTC", 100000);
        auto result = engine.submit(req);
        TEST_ASSERT(result.success);

        // Act: 동일한 체결 메시지 2회 전달
        MyTrade trade;
        trade.trade_id = "trade-123";  // 같은 trade_id
        trade.order_id = "mock-order-uuid";
        trade.market = "KRW-BTC";
        trade.side = OrderPosition::BID;
        trade.executed_funds = 100000;
        trade.volume = 0.002;
        trade.fee = 50;
        trade.price = 50000000;

        engine.onMyTrade(trade);   // 첫 번째 → 처리됨
        engine.onMyTrade(trade);   // 두 번째 → 무시됨 (seen_trades_)

        // Assert: coin_balance가 2배가 아닌 0.002 그대로
        auto budget = account_mgr.getBudget("KRW-BTC");
        TEST_ASSERT(budget.has_value());
        TEST_ASSERT_DOUBLE_EQ(budget->coin_balance, 0.002);  // 0.004가 아님!

        std::cout << "  coin_balance: " << budget->coin_balance << " (not doubled)\n";
        std::cout << "  [PASS] Duplicate trade prevented\n";
    }

    // [TEST 12] 매수 체결 처리
    // 목적: BUY 주문 체결 시 코인 잔량이 증가하는지 확인
    // 핵심: AccountManager.finalizeFillBuy(volume) 호출로 coin_balance 증가
    // 참고: reserved_krw는 아직 유지 (OrderStatus로 최종 해제)
    void testOnMyTradeBuyFill() {
        std::cout << "\n[TEST 12] OnMyTrade buy fill\n";

        // Arrange: BUY 주문 제출
        Account account;
        account.krw_free = 1000000.0;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("mock-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        auto req = makeBuyRequest("KRW-BTC", 100000);
        auto result = engine.submit(req);
        TEST_ASSERT(result.success);

        // Act: BUY 체결 메시지 전달 (0.002 BTC 취득)
        MyTrade trade;
        trade.trade_id = "trade-456";
        trade.order_id = "mock-order-uuid";
        trade.market = "KRW-BTC";
        trade.side = OrderPosition::BID;
        trade.executed_funds = 100000;
        trade.volume = 0.002;  // 취득한 코인 수량
        trade.fee = 50;
        trade.price = 50000000;

        engine.onMyTrade(trade);

        // Assert: 코인 잔량 증가, reserved_krw는 아직 유지
        auto budget = account_mgr.getBudget("KRW-BTC");
        TEST_ASSERT(budget.has_value());
        TEST_ASSERT_DOUBLE_EQ(budget->coin_balance, 0.002);
        TEST_ASSERT(budget->reserved_krw > 0);  // 아직 해제 안됨

        std::cout << "  coin_balance: " << budget->coin_balance << "\n";
        std::cout << "  [PASS] Buy fill processed\n";
    }

    // [TEST 13] 매도 체결 처리
    // 목적: SELL 주문 체결 시 코인 감소 및 KRW 증가 확인
    // 핵심: AccountManager.finalizeFillSell(executed_funds, fee, market_price)
    // 참고: 수수료는 KRW에서 차감 (available_krw = executed_funds - fee)
    void testOnMyTradeSellFill() {
        std::cout << "\n[TEST 13] OnMyTrade sell fill\n";

        // Arrange: 0.01 BTC 보유 상태에서 SELL 주문 제출
        Account account;
        account.krw_free = 0;
        account.positions.push_back(Position{"BTC", 0.01, 50000000});
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("sell-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        auto req = makeSellRequest("KRW-BTC", 0.01);
        auto result = engine.submit(req);
        TEST_ASSERT(result.success);

        // Act: SELL 체결 메시지 전달 (50만원 받음, 250원 수수료)
        MyTrade trade;
        trade.trade_id = "trade-789";
        trade.order_id = "sell-order-uuid";
        trade.market = "KRW-BTC";
        trade.side = OrderPosition::ASK;
        trade.executed_funds = 500000;  // 받은 KRW
        trade.volume = 0.01;            // 팔린 BTC
        trade.fee = 250;                // 수수료
        trade.price = 50000000;

        engine.onMyTrade(trade);

        // Assert: 코인 0으로 감소, KRW는 수수료 차감 후 증가
        auto budget = account_mgr.getBudget("KRW-BTC");
        TEST_ASSERT(budget.has_value());
        TEST_ASSERT_DOUBLE_EQ(budget->coin_balance, 0);
        TEST_ASSERT_DOUBLE_EQ(budget->available_krw, 500000 - 250);  // 499,750원

        std::cout << "  coin_balance: " << budget->coin_balance << "\n";
        std::cout << "  available_krw: " << budget->available_krw << "\n";
        std::cout << "  [PASS] Sell fill processed\n";
    }

    // ========== 상태 업데이트 테스트 ==========

    // [TEST 14] 종료 상태 시 매수 토큰 해제
    // 목적: BUY 주문이 종료 상태(Canceled/Filled)가 되면 active_buy_token_ 해제 확인
    // 핵심: 종료 상태 후 새로운 BUY 주문 가능 (중복 방지 플래그 리셋)
    // 참고: 종료 상태 = Filled, Canceled (Pending은 비종료)
    void testOnOrderStatusTerminalClearsBuyToken() {
        std::cout << "\n[TEST 14] OnOrderStatus terminal clears buy token\n";

        // Arrange: BUY 주문 제출 (active_buy_token_ 생성)
        Account account;
        account.krw_free = 1000000.0;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("mock-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        auto req = makeBuyRequest("KRW-BTC", 100000);
        auto result = engine.submit(req);
        TEST_ASSERT(result.success);

        // Act: 종료 상태(Canceled) 수신 → active_buy_token_ 해제
        engine.onOrderStatus("mock-order-uuid", OrderStatus::Canceled);

        // Assert: 새로운 BUY 주문 성공 (토큰 해제됨)
        mock_api.setPostOrderResult("second-order-uuid");
        auto req2 = makeBuyRequest("KRW-BTC", 50000);
        auto result2 = engine.submit(req2);
        TEST_ASSERT(result2.success);  // 중복 거부 안됨

        std::cout << "  [PASS] Buy token cleared on terminal status\n";
    }

    // [TEST 15] 종료 상태 시 매도 ID 해제
    // 목적: SELL 주문이 종료 상태(Filled)가 되면 active_sell_order_id_ 해제 확인
    // 핵심: 종료 상태 후 새로운 SELL 주문 가능
    // 참고: 이 테스트는 잔량 부족으로 실패할 수 있지만, "중복 매도" 에러는 아님을 검증
    void testOnOrderStatusTerminalClearsSellId() {
        std::cout << "\n[TEST 15] OnOrderStatus terminal clears sell ID\n";

        // Arrange: 0.01 BTC 보유 상태에서 SELL 주문 제출
        Account account;
        account.krw_free = 0;
        account.positions.push_back(Position{"BTC", 0.01, 50000000});
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("sell-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        auto req = makeSellRequest("KRW-BTC", 0.01);
        auto result = engine.submit(req);
        TEST_ASSERT(result.success);

        // Act: 종료 상태(Filled) 수신 → active_sell_order_id_ 해제
        engine.onOrderStatus("sell-order-uuid", OrderStatus::Filled);

        // Assert: 새로운 SELL 시도 (실패해도 "중복 매도" 에러는 아님)
        auto req2 = makeSellRequest("KRW-BTC", 0.005);
        auto result2 = engine.submit(req2);

        if (!result2.success) {
            // 코인 부족 등의 이유로 실패할 수 있지만, "중복" 에러는 아니어야 함
            TEST_ASSERT(result2.message.find("already has pending sell") == std::string::npos);
        }

        std::cout << "  [PASS] Sell ID cleared on terminal status\n";
    }

    // [TEST 16] 마켓 격리 검증
    // 목적: MarketEngine이 다른 마켓의 주문 상태 업데이트를 무시하는지 확인
    // 핵심: 같은 order_id라도 market이 다르면 처리하지 않음 (엔진별 독립성)
    // 참고: OrderStore에 여러 마켓 주문이 있어도 각 엔진은 자신의 마켓만 관리
    void testOnOrderStatusMarketIsolation() {
        std::cout << "\n[TEST 16] OnOrderStatus market isolation\n";

        // Arrange: KRW-BTC 엔진에서 BUY 주문 제출
        Account account;
        account.krw_free = 1000000.0;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("mock-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        auto req = makeBuyRequest("KRW-BTC", 100000);
        auto result = engine.submit(req);
        TEST_ASSERT(result.success);

        // Arrange: OrderStore에 같은 order_id지만 다른 마켓(KRW-ETH) 주문 추가
        Order eth_order;
        eth_order.id = "mock-order-uuid";  // 같은 ID
        eth_order.market = "KRW-ETH";      // 다른 마켓
        eth_order.position = OrderPosition::BID;
        eth_order.status = OrderStatus::Pending;
        eth_order.type = OrderType::Market;
        store.upsert(eth_order);

        // Act: 다른 마켓 주문 상태 업데이트 시도
        engine.onOrderStatus("mock-order-uuid", OrderStatus::Canceled);

        // Assert: KRW-BTC 엔진의 active_buy_token_은 여전히 유지됨 (다른 마켓 무시)
        auto req2 = makeBuyRequest("KRW-BTC", 50000);
        auto result2 = engine.submit(req2);
        TEST_ASSERT(!result2.success);  // 여전히 중복 거부
        TEST_ASSERT(result2.code == EngineErrorCode::OrderRejected);

        // Assert: OrderStore의 KRW-ETH 주문도 변경 안됨
        auto stored_order = store.get("mock-order-uuid");
        TEST_ASSERT(stored_order.has_value());
        TEST_ASSERT(stored_order->market == "KRW-ETH");
        TEST_ASSERT(stored_order->status == OrderStatus::Pending);  // Canceled 안됨

        std::cout << "  [PASS] Market isolation verified\n";
    }

    // ========== 예약 금액 계산 테스트 ==========

    // [TEST 17] 예약 마진 적용 (AmountSize)
    // 목적: 시장가 BUY 주문 시 reserve_margin이 올바르게 적용되는지 확인
    // 핵심: reserved_krw = amount * reserve_margin (기본값 1.01)
    // 참고: 시장가는 가격 변동에 대비해 여유분 확보 (1% 추가)
    void testReserveAmountWithMargin_AmountSize() {
        std::cout << "\n[TEST 17] Reserve amount with margin (AmountSize)\n";

        // Arrange: 100만원 잔고로 시작
        Account account;
        account.krw_free = 1000000.0;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("mock-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        // Act: 시장가 BUY 주문 (AmountSize)
        const auto& cfg = util::AppConfig::instance().engine;
        double amount = 100000;
        auto req = makeBuyRequest("KRW-BTC", amount);
        auto result = engine.submit(req);

        // Assert: 예약 금액 = amount * reserve_margin
        TEST_ASSERT(result.success);
        auto budget = account_mgr.getBudget("KRW-BTC");
        TEST_ASSERT(budget.has_value());

        double expected_reserve = amount * cfg.reserve_margin;  // 100000 * 1.01 = 101000
        TEST_ASSERT_DOUBLE_EQ(budget->reserved_krw, expected_reserve);

        std::cout << "  reserved_krw: " << budget->reserved_krw
                  << " (expected: " << expected_reserve << ")\n";
        std::cout << "  [PASS] Reserve margin applied correctly\n";
    }

    // [TEST 18] 예약 마진 적용 (VolumeSize)
    // 목적: 지정가 BUY 주문 시 reserve_margin이 올바르게 적용되는지 확인
    // 핵심: reserved_krw = price * volume * reserve_margin
    // 참고: 지정가도 체결 가격 변동 가능성 대비 (부분 체결 등)
    void testReserveAmountWithMargin_VolumeSize() {
        std::cout << "\n[TEST 18] Reserve amount with margin (VolumeSize)\n";

        // Arrange: 100만원 잔고로 시작
        Account account;
        account.krw_free = 1000000;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        // Act: 지정가 BUY 주문 (VolumeSize + price)
        const auto& cfg = util::AppConfig::instance().engine;
        double volume = 0.001;
        double price = 50000000;

        OrderRequest req;
        req.market = "KRW-BTC";
        req.position = OrderPosition::BID;
        req.type = OrderType::Limit;
        req.size = VolumeSize{volume};
        req.price = price;

        auto result = engine.submit(req);

        // Assert: 예약 금액 = price * volume * reserve_margin
        TEST_ASSERT(result.success);
        auto budget = account_mgr.getBudget("KRW-BTC");
        TEST_ASSERT(budget.has_value());

        double expected_reserve = price * volume * cfg.reserve_margin;  // 50M * 0.001 * 1.01
        TEST_ASSERT_DOUBLE_EQ(budget->reserved_krw, expected_reserve);

        std::cout << "  reserved_krw: " << budget->reserved_krw
                  << " (expected: " << expected_reserve << ")\n";
        std::cout << "  [PASS] Reserve margin applied correctly for VolumeSize\n";
    }

    // ========== 이벤트 생성 테스트 ==========

    // [TEST 19] 주문 제출 시 즉시 이벤트 생성 안됨
    // 목적: submit() 호출은 이벤트를 생성하지 않고, 체결/상태 변경 시에만 이벤트 발생 확인
    // 핵심: 주문 제출 자체는 동기 작업, 이벤트는 비동기 업데이트(체결, 취소 등)에만 발생
    // 참고: 전략은 submit() 결과로 성공/실패 확인, 이벤트로 후속 업데이트 수신
    void testSubmitDoesNotGenerateImmediateEvent() {
        std::cout << "\n[TEST 19] Submit does not generate immediate event\n";

        // Arrange: 엔진 준비
        Account account;
        account.krw_free = 1000000.0;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("mock-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        // Act: 주문 제출 및 이벤트 확인
        auto req = makeBuyRequest("KRW-BTC", 100000, "test-buy-1");
        auto result = engine.submit(req);
        auto events = engine.pollEvents();

        // Assert: 주문은 성공했지만 이벤트는 생성 안됨
        TEST_ASSERT(result.success);
        TEST_ASSERT(events.empty());  // 제출만으로는 이벤트 없음

        std::cout << "  [PASS] No event generated on submit\n";
    }

    // [TEST 20] 체결 시 Fill 이벤트 생성
    // 목적: onMyTrade() 호출 시 EngineFillEvent가 생성되고 identifier가 전달되는지 확인
    // 핵심: 전략이 체결 이벤트를 받아 상태 전환 (PendingEntry → InPosition 등)
    // 참고: identifier 필드가 있어야 이벤트 생성 (전략 매칭용)
    void testOnMyTradeGeneratesFillEvent() {
        std::cout << "\n[TEST 20] OnMyTrade generates fill event\n";

        // Arrange: 주문 제출 및 기존 이벤트 제거
        Account account;
        account.krw_free = 1000000.0;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("mock-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        auto req = makeBuyRequest("KRW-BTC", 100000, "test-buy-fill");
        engine.submit(req);
        engine.pollEvents();  // submit 이벤트 제거 (없지만 명시적으로)

        // Act: 체결 메시지 전달 (identifier 포함)
        MyTrade trade;
        trade.trade_id = "trade-abc";
        trade.order_id = "mock-order-uuid";
        trade.market = "KRW-BTC";
        trade.side = OrderPosition::BID;
        trade.executed_funds = 100000;
        trade.volume = 0.002;
        trade.fee = 50;
        trade.price = 50000000;
        trade.identifier = "test-buy-fill";  // 이벤트 생성 트리거

        engine.onMyTrade(trade);
        auto events = engine.pollEvents();

        // Assert: EngineFillEvent 생성 및 필드 확인
        TEST_ASSERT(!events.empty());
        TEST_ASSERT(std::holds_alternative<EngineFillEvent>(events[0]));
        auto& event = std::get<EngineFillEvent>(events[0]);
        TEST_ASSERT(event.identifier == "test-buy-fill");
        TEST_ASSERT(event.order_id == "mock-order-uuid");
        TEST_ASSERT(event.trade_id == "trade-abc");

        std::cout << "  [PASS] Fill event generated with identifier\n";
    }

    // [TEST 21] 주문 스냅샷 시 Status 이벤트 생성
    // 목적: onOrderSnapshot() 호출 시 EngineOrderStatusEvent가 생성되는지 확인
    // 핵심: WebSocket myOrder 스트림의 전체 주문 정보로 상태 업데이트
    // 참고: onOrderStatus()는 내부 호출, onOrderSnapshot()만 이벤트 생성
    void testOnOrderSnapshotGeneratesStatusEvent() {
        std::cout << "\n[TEST 21] OnOrderSnapshot generates status event\n";

        // Arrange: 주문 제출 및 기존 이벤트 제거
        Account account;
        account.krw_free = 1000000.0;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("mock-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        auto req = makeBuyRequest("KRW-BTC", 100000, "test-buy-snapshot");
        engine.submit(req);
        engine.pollEvents();

        // Act: 주문 스냅샷 전달 (WebSocket 메시지 시뮬레이션)
        Order snapshot;
        snapshot.id = "mock-order-uuid";
        snapshot.market = "KRW-BTC";
        snapshot.status = OrderStatus::Canceled;
        snapshot.position = OrderPosition::BID;
        snapshot.type = OrderType::Market;
        snapshot.identifier = "test-buy-snapshot";  // 이벤트 생성 트리거

        engine.onOrderSnapshot(snapshot);
        auto events = engine.pollEvents();

        // Assert: EngineOrderStatusEvent 생성 및 필드 확인
        TEST_ASSERT(!events.empty());
        TEST_ASSERT(std::holds_alternative<EngineOrderStatusEvent>(events[0]));
        auto& event = std::get<EngineOrderStatusEvent>(events[0]);
        TEST_ASSERT(event.identifier == "test-buy-snapshot");
        TEST_ASSERT(event.order_id == "mock-order-uuid");
        TEST_ASSERT(event.status == OrderStatus::Canceled);

        std::cout << "  [PASS] Status event generated on snapshot\n";
    }

    // [TEST 22] 다른 마켓 스냅샷 무시
    // 목적: onOrderSnapshot()이 다른 마켓의 주문을 무시하는지 확인
    // 핵심: 같은 order_id라도 market이 다르면 무시 (TEST 16과 유사, snapshot 경로)
    // 참고: WebSocket 구독이 전체 계정 주문이라면 마켓 필터링 필수
    void testOnOrderSnapshotIgnoresWrongMarket() {
        std::cout << "\n[TEST 22] OnOrderSnapshot ignores wrong market\n";

        // Arrange: KRW-BTC 엔진에서 BUY 주문 제출
        Account account;
        account.krw_free = 1000000.0;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("mock-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        auto req = makeBuyRequest("KRW-BTC", 100000);
        auto result = engine.submit(req);
        TEST_ASSERT(result.success);

        // Act: 다른 마켓(KRW-ETH) 주문 스냅샷 전달
        Order snapshot;
        snapshot.id = "mock-order-uuid";  // 같은 ID
        snapshot.market = "KRW-ETH";      // 다른 마켓
        snapshot.status = OrderStatus::Canceled;
        snapshot.position = OrderPosition::BID;

        engine.onOrderSnapshot(snapshot);

        // Assert: KRW-BTC 엔진의 active_buy_token_은 여전히 유지됨
        auto req2 = makeBuyRequest("KRW-BTC", 50000);
        auto result2 = engine.submit(req2);
        TEST_ASSERT(!result2.success);  // 여전히 중복 거부
        TEST_ASSERT(result2.code == EngineErrorCode::OrderRejected);

        std::cout << "  [PASS] Wrong market snapshot ignored\n";
    }

    // [TEST 23] 매수 Filled 시 예약 KRW 해제
    // 목적: BUY 주문이 Filled 상태가 되면 reserved_krw가 0으로 복구되는지 확인
    // 핵심: AccountManager.finalizeOrder() 호출로 예약 해제
    // 참고: 체결 완료 시 실제 사용 금액만큼 차감, 나머지는 available_krw로 복구
    void testBuyFilledStatusRestoredReservedKrw() {
        std::cout << "\n[TEST 23] Buy filled status restored reserved KRW\n";

        // Arrange: BUY 주문 제출 (reserved_krw 생성)
        Account account;
        account.krw_free = 1000000.0;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("mock-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        auto req = makeBuyRequest("KRW-BTC", 100000);
        auto result = engine.submit(req);
        TEST_ASSERT(result.success);

        // Assert: 예약 확인
        auto budget_before = account_mgr.getBudget("KRW-BTC");
        TEST_ASSERT(budget_before.has_value());
        TEST_ASSERT(budget_before->reserved_krw > 0);

        // Act: Filled 상태 수신 → finalizeOrder() → reserved_krw 해제
        engine.onOrderStatus("mock-order-uuid", OrderStatus::Filled);

        // Assert: 예약 해제 확인
        auto budget_after = account_mgr.getBudget("KRW-BTC");
        TEST_ASSERT(budget_after.has_value());
        TEST_ASSERT(budget_after->reserved_krw == 0);

        // Assert: 새로운 주문 가능 (잔액 복구됨)
        mock_api.setPostOrderResult("second-buy-uuid");
        auto req2 = makeBuyRequest("KRW-BTC", 50000);
        auto result2 = engine.submit(req2);
        TEST_ASSERT(result2.success);

        std::cout << "  [PASS] Reserved KRW restored after Filled\n";
    }

    // [TEST 24] 매수 Canceled 시 예약 KRW 해제
    // 목적: BUY 주문이 Canceled 상태가 되면 reserved_krw가 전액 복구되는지 확인
    // 핵심: AccountManager.finalizeOrder() 호출로 예약 해제
    // 참고: 취소 시 체결 없으므로 전액 available_krw로 복구
    void testBuyCanceledStatusRestoredReservedKrw() {
        std::cout << "\n[TEST 24] Buy canceled status restored reserved KRW\n";

        // Arrange: BUY 주문 제출 (reserved_krw 생성)
        Account account;
        account.krw_free = 1000000.0;
        AccountManager account_mgr(account, {"KRW-BTC"});
        OrderStore store;
        MockOrderApi mock_api;
        mock_api.setPostOrderResult("mock-order-uuid");
        MarketEngine engine("KRW-BTC", mock_api, store, account_mgr);
        engine.bindToCurrentThread();

        auto req = makeBuyRequest("KRW-BTC", 100000);
        auto result = engine.submit(req);
        TEST_ASSERT(result.success);

        // Assert: 예약 확인
        auto budget_before = account_mgr.getBudget("KRW-BTC");
        TEST_ASSERT(budget_before.has_value());
        TEST_ASSERT(budget_before->reserved_krw > 0);

        // Act: Canceled 상태 수신 → finalizeOrder() → reserved_krw 해제
        engine.onOrderStatus("mock-order-uuid", OrderStatus::Canceled);

        // Assert: 예약 전액 해제 확인
        auto budget_after = account_mgr.getBudget("KRW-BTC");
        TEST_ASSERT(budget_after.has_value());
        TEST_ASSERT(budget_after->reserved_krw == 0);

        // Assert: 새로운 주문 가능 (전액 복구됨)
        mock_api.setPostOrderResult("second-buy-uuid");
        auto req2 = makeBuyRequest("KRW-BTC", 50000);
        auto result2 = engine.submit(req2);
        TEST_ASSERT(result2.success);

        std::cout << "  [PASS] Reserved KRW restored after Canceled\n";
    }

    // ========== 메인 실행 함수 ==========

    struct TestCase {
        const char* name;
        void (*func)();
    };

    bool runAllTests() {
        std::cout << "\n========================================\n";
        std::cout << "  MarketEngine Unit Tests\n";
        std::cout << "========================================\n";

        // 테스트 케이스 목록
        TestCase tests[] = {
            {"Construction", testConstruction},
            {"SubmitBuySuccess", testSubmitBuySuccess},
            {"SubmitSellSuccess", testSubmitSellSuccess},
            {"DuplicateBuyRejection", testDuplicateBuyRejection},
            {"DuplicateSellRejection", testDuplicateSellRejection},
            {"OppositeSellBlocksBuy", testOppositeSellBlocksBuy},
            {"OppositeBuyBlocksSell", testOppositeBuyBlocksSell},
            {"RejectWrongMarket", testRejectWrongMarket},
            {"InsufficientBalance", testInsufficientBalance},
            {"PostOrderFailure_ShouldReleaseReservation", testPostOrderFailure_ShouldReleaseReservation},
            {"OnMyTradeDuplicatePrevention", testOnMyTradeDuplicatePrevention},
            {"OnMyTradeBuyFill", testOnMyTradeBuyFill},
            {"OnMyTradeSellFill", testOnMyTradeSellFill},
            {"OnOrderStatusTerminalClearsBuyToken", testOnOrderStatusTerminalClearsBuyToken},
            {"OnOrderStatusTerminalClearsSellId", testOnOrderStatusTerminalClearsSellId},
            {"OnOrderStatusMarketIsolation", testOnOrderStatusMarketIsolation},
            {"ReserveAmountWithMargin_AmountSize", testReserveAmountWithMargin_AmountSize},
            {"ReserveAmountWithMargin_VolumeSize", testReserveAmountWithMargin_VolumeSize},
            {"SubmitDoesNotGenerateImmediateEvent", testSubmitDoesNotGenerateImmediateEvent},
            {"OnMyTradeGeneratesFillEvent", testOnMyTradeGeneratesFillEvent},
            {"OnOrderSnapshotGeneratesStatusEvent", testOnOrderSnapshotGeneratesStatusEvent},
            {"OnOrderSnapshotIgnoresWrongMarket", testOnOrderSnapshotIgnoresWrongMarket},
            {"BuyFilledStatusRestoredReservedKrw", testBuyFilledStatusRestoredReservedKrw},
            {"BuyCanceledStatusRestoredReservedKrw", testBuyCanceledStatusRestoredReservedKrw},
        };

        int passed = 0;
        int failed = 0;
        const int total = sizeof(tests) / sizeof(tests[0]);

        for (int i = 0; i < total; ++i) {
            try {
                tests[i].func();
                ++passed;
            }
            catch (const TestFailure& e) {
                ++failed;
                std::cerr << "\n[TEST FAILED] " << tests[i].name << "\n";
                std::cerr << "  Reason: " << e.condition() << "\n";
                std::cerr << "  Location: " << e.file() << ":" << e.line() << "\n";
            }
            catch (const std::exception& e) {
                ++failed;
                std::cerr << "\n[TEST FAILED] " << tests[i].name << "\n";
                std::cerr << "  Exception: " << e.what() << "\n";
            }
        }

        std::cout << "\n========================================\n";
        if (failed == 0) {
            std::cout << "  ALL TESTS PASSED (" << passed << "/" << total << ")\n";
            std::cout << "========================================\n";
            return true;
        }
        else {
            std::cerr << "  TESTS FAILED: " << failed << " failed, " << passed << " passed (" << total << " total)\n";
            std::cerr << "========================================\n";
            return false;
        }
    }

} // namespace test

int main() {
    bool success = test::runAllTests();
    return success ? 0 : 1;
}
