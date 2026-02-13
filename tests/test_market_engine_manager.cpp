// tests/test_market_engine_manager.cpp
//
// MarketEngineManager 통합 테스트
// - 생성/소멸 생명주기 (생성자 성공/실패, 중복 마켓 방어)
// - start()/stop() 흐름 (정상, 중복 start, 소멸자 자동 stop)
// - EventRouter 연동 (registerWith → routeMarketData / routeMyOrder)
// - 워커 이벤트 처리 (캔들 → 전략 실행 → postOrder 호출)
// - myOrder 처리 (체결 이벤트 → 전략 상태 전환 → 매도 주문 발생)
// - 예외 격리 (잘못된 이벤트 후 워커 생존 확인)
// - 멀티마켓 큐 격리

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "test_utils.h"
#include "mocks/MockOrderApi.h"
#include "app/MarketEngineManager.h"
#include "app/EventRouter.h"
#include "engine/OrderStore.h"
#include "trading/allocation/AccountManager.h"
#include "core/domain/Account.h"


using namespace app;
using namespace core;
using namespace trading::allocation;

namespace test {

// ── 헬퍼 ─────────────────────────────────────────────────────────────

// 테스트용 Account 생성
static core::Account makeTestAccount(double krw = 1'000'000.0)
{
    core::Account acc;
    acc.krw_free = krw;
    return acc;
}

// MockOrderApi 기본 설정 (유효한 계좌 + 빈 미체결 주문)
static void setupValidApi(MockOrderApi& api, double krw = 1'000'000.0)
{
    api.setGetMyAccountResult(makeTestAccount(krw));
    api.setGetOpenOrdersResult(std::vector<core::Order>{});
    api.setPostOrderResult("mock-order-uuid");
}

// 유효한 1분봉 캔들 JSON (EventRouter fast-path 통과 형식)
// - type이 "candle"로 시작해야 handleMarketData_에서 처리됨
// - 필수 필드: code, opening_price, high_price, low_price, trade_price,
//             candle_acc_trade_volume, candle_date_time_kst
static std::string candleJson(const std::string& market,
                              const std::string& kst_ts,
                              double close = 50'000'000.0)
{
    return R"({"type":"candle.1m","code":")" + market + R"(",)"
           R"("opening_price":)" + std::to_string(close) + R"(,)"
           R"("high_price":)"    + std::to_string(close * 1.01) + R"(,)"
           R"("low_price":)"     + std::to_string(close * 0.99) + R"(,)"
           R"("trade_price":)"   + std::to_string(close) + R"(,)"
           R"("candle_acc_trade_volume":10.5,)"
           R"("candle_date_time_kst":")" + kst_ts + R"("})";
}

// myOrder JSON 생성 (Upbit WS myOrder 형식)
// - trade_uuid가 비어 있으면 "상태 스냅샷"만 생성 (MyTrade 없음)
// - trade_uuid가 있으면 state="trade" 이벤트: MyTrade + Order 스냅샷 생성
// - identifier는 포함하지 않음 → 엔진이 내부 저장 identifier 폴백 사용
static std::string myOrderJson(
    const std::string& market,
    const std::string& uuid,
    const std::string& ask_bid,        // "BID" | "ASK"
    const std::string& state,          // "wait" | "trade" | "done"
    double price,
    double volume,
    double executed_volume = 0.0,
    const std::string& trade_uuid = "")
{
    const double remaining = volume - executed_volume;
    const double exec_funds = price * executed_volume;

    std::string j =
        R"({"type":"myOrder","code":")" + market + R"(",)"
        R"("uuid":")" + uuid + R"(",)"
        R"("ask_bid":")" + ask_bid + R"(",)"
        R"("order_type":"price",)"
        R"("state":")" + state + R"(",)"
        R"("price":)" + std::to_string(price) + R"(,)"
        R"("volume":)" + std::to_string(volume) + R"(,)"
        R"("remaining_volume":)" + std::to_string(remaining) + R"(,)"
        R"("executed_volume":)" + std::to_string(executed_volume) + R"(,)"
        R"("trades_count":)" + (executed_volume > 0.0 ? "1" : "0") + R"(,)"
        R"("reserved_fee":0.0,"remaining_fee":0.0,"paid_fee":0.0,)"
        R"("locked":0.0,"executed_funds":)" + std::to_string(exec_funds);

    if (!trade_uuid.empty())
    {
        const double fee = exec_funds * 0.0005;
        j += R"(,"trade_uuid":")" + trade_uuid + R"(",)"
             R"("trade_fee":)" + std::to_string(fee) + R"(,)"
             R"("is_maker":false)";
    }

    j += "}";
    return j;
}

// 신호 발생이 빠른 전략 파라미터 (rsiLength=3, oversold=80, 필터 완화)
// RSI <= 80 조건은 하락 추세 캔들 3개면 거의 항상 충족됨
static MarketEngineManager::MarketManagerConfig fastSignalConfig()
{
    MarketEngineManager::MarketManagerConfig cfg;
    cfg.strategy_params.rsiLength        = 3;
    cfg.strategy_params.oversold         = 80.0;
    cfg.strategy_params.overbought       = 70.0;
    cfg.strategy_params.maxTrendStrength = 1.0;
    cfg.strategy_params.minVolatility    = 0.0;
    return cfg;
}

// 조건이 참이 될 때까지 폴링 대기 (최대 timeout, interval 간격)
// 고정 sleep 대신 사용하여 CPU 부하/스케줄링에 따른 간헐 실패(flaky) 방지
template<typename Pred>
static bool waitFor(Pred pred,
                    std::chrono::milliseconds timeout  = std::chrono::milliseconds(2000),
                    std::chrono::milliseconds interval = std::chrono::milliseconds(20))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred()) return true;
        std::this_thread::sleep_for(interval);
    }
    return pred();  // 마지막 한 번 더 확인
}

// ── TEST 1: 생성자 정상 동작 ───────────────────────────────────────────

// 목적: getMyAccount 성공 시 예외 없이 생성, API 1회 이상 호출
void testConstructionSuccess()
{
    std::cout << "\n[TEST 1] Constructor success\n";

    MockOrderApi mock_api;
    setupValidApi(mock_api);

    engine::OrderStore store;
    AccountManager account_mgr(makeTestAccount(), {"KRW-BTC"});

    MarketEngineManager mgr(mock_api, store, account_mgr, {"KRW-BTC"});

    TEST_ASSERT(mock_api.getMyAccountCallCount() >= 1);
    std::cout << "  getMyAccount calls: " << mock_api.getMyAccountCallCount() << "\n";
    std::cout << "  [PASS] Manager constructed successfully\n";
}

// ── TEST 2: 생성자 실패 ────────────────────────────────────────────────

// 목적: getMyAccount 계속 실패 시 runtime_error 발생
void testConstructionFailure()
{
    std::cout << "\n[TEST 2] Constructor failure\n";

    MockOrderApi mock_api;
    mock_api.setGetMyAccountResult(
        api::rest::RestError{api::rest::RestErrorCode::BadStatus, "unauthorized", 401});
    mock_api.setGetOpenOrdersResult(std::vector<core::Order>{});

    engine::OrderStore store;
    AccountManager account_mgr(makeTestAccount(), {"KRW-BTC"});

    bool threw = false;
    try {
        MarketEngineManager mgr(mock_api, store, account_mgr, {"KRW-BTC"});
    }
    catch (const std::runtime_error&) {
        threw = true;
    }

    TEST_ASSERT(threw);
    std::cout << "  [PASS] runtime_error thrown on sync failure\n";
}

// ── TEST 3: 중복 마켓 방어 ────────────────────────────────────────────

// 목적: 같은 마켓 2회 입력 시 크래시 없이 1개만 등록
void testDuplicateMarketGuard()
{
    std::cout << "\n[TEST 3] Duplicate market guard\n";

    MockOrderApi mock_api;
    setupValidApi(mock_api);

    engine::OrderStore store;
    AccountManager account_mgr(makeTestAccount(), {"KRW-BTC"});

    bool ok = false;
    try {
        MarketEngineManager mgr(mock_api, store, account_mgr, {"KRW-BTC", "KRW-BTC"});
        ok = true;
    }
    catch (...) {}

    TEST_ASSERT(ok);
    std::cout << "  [PASS] Duplicate market skipped without crash\n";
}

// ── TEST 4: start/stop 정상 흐름 ──────────────────────────────────────

// 목적: start() → stop() 시 예외/행 없이 정상 완료 (join 성공)
void testStartStop()
{
    std::cout << "\n[TEST 4] Start/stop lifecycle\n";

    MockOrderApi mock_api;
    setupValidApi(mock_api);

    engine::OrderStore store;
    AccountManager account_mgr(makeTestAccount(), {"KRW-BTC"});

    MarketEngineManager mgr(mock_api, store, account_mgr, {"KRW-BTC"});
    EventRouter router;
    mgr.registerWith(router);

    mgr.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mgr.stop();

    std::cout << "  [PASS] Start/stop completed without hang\n";
}

// ── TEST 5: 중복 start() 방어 ─────────────────────────────────────────

// 목적: start() 두 번 호출해도 워커가 중복 생성되지 않고 안전함
// 구현: if (started_) return → 두 번째 start()는 즉시 반환
void testDoubleStart()
{
    std::cout << "\n[TEST 5] Double start is idempotent\n";

    MockOrderApi mock_api;
    setupValidApi(mock_api);

    engine::OrderStore store;
    AccountManager account_mgr(makeTestAccount(), {"KRW-BTC"});

    MarketEngineManager mgr(mock_api, store, account_mgr, {"KRW-BTC"});
    EventRouter router;
    mgr.registerWith(router);

    mgr.start();
    mgr.start();  // 두 번째 → 무시됨
    mgr.stop();

    std::cout << "  [PASS] Double start is safe\n";
}

// ── TEST 6: 소멸자 자동 stop ──────────────────────────────────────────

// 목적: stop() 호출 없이 소멸 시 jthread RAII로 워커 자동 정리
void testDestructorAutoStop()
{
    std::cout << "\n[TEST 6] Destructor auto-stop\n";

    MockOrderApi mock_api;
    setupValidApi(mock_api);

    engine::OrderStore store;
    AccountManager account_mgr(makeTestAccount(), {"KRW-BTC"});

    {
        MarketEngineManager mgr(mock_api, store, account_mgr, {"KRW-BTC"});
        EventRouter router;
        mgr.registerWith(router);
        mgr.start();
        // stop() 없이 스코프 종료 → ~MarketEngineManager → stop() → jthread join
    }

    std::cout << "  [PASS] Destructor stopped workers without hang\n";
}

// ── TEST 7: EventRouter 연동 확인 ─────────────────────────────────────

// 목적: registerWith() 후 routeMarketData()가 해당 마켓 큐로 전달됨
void testRegisterWithRouter()
{
    std::cout << "\n[TEST 7] registerWith EventRouter\n";

    MockOrderApi mock_api;
    setupValidApi(mock_api);

    engine::OrderStore store;
    AccountManager account_mgr(makeTestAccount(), {"KRW-BTC"});

    MarketEngineManager mgr(mock_api, store, account_mgr, {"KRW-BTC"});
    EventRouter router;
    mgr.registerWith(router);

    bool routed = router.routeMarketData(candleJson("KRW-BTC", "2024-01-01T09:00:00"));

    TEST_ASSERT(routed);
    TEST_ASSERT(router.stats().total_routed.load() == 1);
    TEST_ASSERT(router.stats().unknown_market.load() == 0);

    std::cout << "  [PASS] Event successfully routed via EventRouter\n";
}

// ── TEST 8: 캔들 이벤트 → 전략 실행 → 주문 제출 ──────────────────────

// 목적: 워커가 캔들을 처리하고 전략이 매수 신호를 내면 postOrder 호출 확인
// 조건: rsiLength=3, 하락 추세 캔들 5개 → RSI <= 80 → 매수 신호 발생
void testCandleEventTriggersOrder()
{
    std::cout << "\n[TEST 8] Candle events trigger buy order\n";

    MockOrderApi mock_api;
    setupValidApi(mock_api, 1'000'000.0);

    engine::OrderStore store;
    AccountManager account_mgr(makeTestAccount(1'000'000.0), {"KRW-BTC"});

    MarketEngineManager mgr(mock_api, store, account_mgr, {"KRW-BTC"}, fastSignalConfig());
    EventRouter router;
    mgr.registerWith(router);
    mgr.start();

    // 5개 캔들: 가격 하락 추세 → RSI 낮음 → 매수 신호
    // 타임스탬프가 모두 달라야 candle 중복 제거를 통과함
    const std::vector<std::pair<std::string, double>> candles = {
        {"2024-01-01T09:00:00", 50'000'000.0},
        {"2024-01-01T09:01:00", 49'000'000.0},
        {"2024-01-01T09:02:00", 48'000'000.0},
        {"2024-01-01T09:03:00", 47'000'000.0},
        {"2024-01-01T09:04:00", 46'000'000.0},
    };

    for (const auto& [ts, price] : candles)
        router.routeMarketData(candleJson("KRW-BTC", ts, price));

    // 고정 sleep 대신 폴링: postOrder 호출될 때까지 최대 2초 대기
    bool ordered = waitFor([&]{ return mock_api.postOrderCallCount() >= 1; });
    mgr.stop();

    TEST_ASSERT(ordered);
    std::cout << "  postOrder calls: " << mock_api.postOrderCallCount() << "\n";
    std::cout << "  [PASS] Buy order submitted after candle processing\n";
}

// ── TEST 9: 잘못된 이벤트 후 워커 생존 (핸들러 내부 catch) ────────────

// 목적: 필수 필드 누락 캔들 JSON → handleMarketData_ 내부 catch(std::exception&)가
//       오류를 처리하고 return → 워커 루프는 계속 실행됨을 검증
// 참고: 이 경로는 핸들러 내부 catch가 담당하며, 루프 수준의 catch(...)까지는
//       올라가지 않는다. catch(...)는 핸들러 밖에서 발생하는 비표준 예외용.
void testBadEventWorkerSurvives()
{
    std::cout << "\n[TEST 9] Worker survives bad event (handler-level catch)\n";

    MockOrderApi mock_api;
    setupValidApi(mock_api, 1'000'000.0);

    engine::OrderStore store;
    AccountManager account_mgr(makeTestAccount(1'000'000.0), {"KRW-BTC"});

    MarketEngineManager mgr(mock_api, store, account_mgr, {"KRW-BTC"}, fastSignalConfig());
    EventRouter router;
    mgr.registerWith(router);
    mgr.start();

    // 1) 필수 필드 누락 캔들: type은 "candle.1m"으로 라우팅 통과하지만
    //    j.get<CandleDto_Minute>()에서 nlohmann::json::exception(std::exception 파생) 발생
    //    → handleMarketData_ 내부 catch(std::exception&)에서 로그 후 return
    router.routeMarketData(
        R"({"type":"candle.1m","code":"KRW-BTC","trade_price":50000000})");

    // 2) 유효한 캔들 5개: 핸들러 catch 후에도 워커가 계속 동작하면 처리됨
    const std::vector<std::pair<std::string, double>> candles = {
        {"2024-02-01T09:00:00", 50'000'000.0},
        {"2024-02-01T09:01:00", 49'000'000.0},
        {"2024-02-01T09:02:00", 48'000'000.0},
        {"2024-02-01T09:03:00", 47'000'000.0},
        {"2024-02-01T09:04:00", 46'000'000.0},
    };

    for (const auto& [ts, price] : candles)
        router.routeMarketData(candleJson("KRW-BTC", ts, price));

    // 폴링: 잘못된 이벤트 이후에도 전략이 실행되어 주문 제출됨
    bool survived = waitFor([&]{ return mock_api.postOrderCallCount() >= 1; });
    mgr.stop();

    TEST_ASSERT(survived);
    std::cout << "  postOrder calls after bad event: " << mock_api.postOrderCallCount() << "\n";
    std::cout << "  [PASS] Worker survived bad event and continued processing\n";
}

// ── TEST 10: 멀티마켓 큐 격리 ─────────────────────────────────────────

// 목적: KRW-BTC / KRW-ETH 각각의 이벤트가 올바른 큐로만 전달됨
// 검증: router stats (total_routed=2, unknown_market=1)
void testMultiMarketIsolation()
{
    std::cout << "\n[TEST 10] Multi-market queue isolation\n";

    MockOrderApi mock_api;
    setupValidApi(mock_api, 2'000'000.0);

    engine::OrderStore store;
    AccountManager account_mgr(makeTestAccount(2'000'000.0), {"KRW-BTC", "KRW-ETH"});

    MarketEngineManager mgr(mock_api, store, account_mgr, {"KRW-BTC", "KRW-ETH"});
    EventRouter router;
    mgr.registerWith(router);

    // 두 마켓에 각각 캔들 전송
    bool btc_ok = router.routeMarketData(candleJson("KRW-BTC", "2024-01-01T09:00:00"));
    bool eth_ok = router.routeMarketData(candleJson("KRW-ETH", "2024-01-01T09:00:00"));

    // 미등록 마켓 라우팅 시도 → 실패
    bool unknown_ok = router.routeMarketData(candleJson("KRW-XRP", "2024-01-01T09:00:00"));

    TEST_ASSERT(btc_ok);
    TEST_ASSERT(eth_ok);
    TEST_ASSERT(!unknown_ok);

    TEST_ASSERT_EQ(router.stats().total_routed.load(),   uint64_t(2));
    TEST_ASSERT_EQ(router.stats().unknown_market.load(), uint64_t(1));

    std::cout << "  total_routed: "   << router.stats().total_routed.load() << "\n";
    std::cout << "  unknown_market: " << router.stats().unknown_market.load() << "\n";
    std::cout << "  [PASS] Multi-market routing is isolated\n";
}

// ── TEST 11: myOrder 라우팅 경로 검증 ─────────────────────────────────

// 목적: routeMyOrder()가 등록 마켓 큐로 전달, 미등록 마켓은 거부
// 검증: total_routed 증가 / unknown_market 증가
void testMyOrderRouting()
{
    std::cout << "\n[TEST 11] myOrder routing path\n";

    MockOrderApi mock_api;
    setupValidApi(mock_api);

    engine::OrderStore store;
    AccountManager account_mgr(makeTestAccount(), {"KRW-BTC"});

    MarketEngineManager mgr(mock_api, store, account_mgr, {"KRW-BTC"});
    EventRouter router;
    mgr.registerWith(router);
    mgr.start();

    // 등록된 마켓 → 라우팅 성공
    const std::string wait_json = myOrderJson(
        "KRW-BTC", "some-uuid", "BID", "wait", 46'000'000.0, 0.02);
    bool routed = router.routeMyOrder(wait_json);

    TEST_ASSERT(routed);
    TEST_ASSERT(router.stats().total_routed.load() >= 1);
    TEST_ASSERT(router.stats().unknown_market.load() == 0);

    // 미등록 마켓 → 라우팅 실패, unknown_market 증가
    bool unknown_routed = router.routeMyOrder(
        myOrderJson("KRW-XRP", "other-uuid", "BID", "wait", 1000.0, 10.0));

    TEST_ASSERT(!unknown_routed);
    TEST_ASSERT(router.stats().unknown_market.load() == 1);

    mgr.stop();
    std::cout << "  total_routed: "   << router.stats().total_routed.load() << "\n";
    std::cout << "  unknown_market: " << router.stats().unknown_market.load() << "\n";
    std::cout << "  [PASS] myOrder routed correctly / unknown market rejected\n";
}

// ── TEST 12: buy → myOrder 체결 → InPosition → 매도 주문 발생 ──────────

// 목적: myOrder 처리 체인 검증
//   1) 하락 캔들 → 매수 신호 → postOrder (BID, count=1)
//   2) myOrder "trade"(uuid="mock-order-uuid", remaining=0) 전송
//      → engine.onMyTrade() → AccountManager 잔고 업데이트 (coin_available > 0)
//      → FillEvent → strategy.onFill() (체결 누적)
//      → engine.onOrderSnapshot(Filled) → EngineOrderStatusEvent
//      → strategy.onOrderUpdate(Filled) → InPosition (entry_price=46M, target=46.46M)
//   3) 상승 캔들 (47M > 46.46M 목표) → maybeExit hitTarget → postOrder (ASK, count=2)
// 검증: postOrderCallCount()==2, lastRequest.position==ASK
//
// 참고: identifier는 myOrder JSON에 포함하지 않아
//       engine이 내부 저장된 strategy client ID로 폴백함
void testMyOrderFillEnablesExit()
{
    std::cout << "\n[TEST 12] myOrder fill → InPosition → exit order\n";

    MockOrderApi mock_api;
    setupValidApi(mock_api, 1'000'000.0);

    engine::OrderStore store;
    AccountManager account_mgr(makeTestAccount(1'000'000.0), {"KRW-BTC"});

    MarketEngineManager mgr(mock_api, store, account_mgr, {"KRW-BTC"}, fastSignalConfig());
    EventRouter router;
    mgr.registerWith(router);
    mgr.start();

    // 1단계: 하락 캔들 5개 → RSI 낮음 → 매수 신호 → postOrder (BID)
    const std::vector<std::pair<std::string, double>> entry_candles = {
        {"2024-03-01T09:00:00", 50'000'000.0},
        {"2024-03-01T09:01:00", 49'000'000.0},
        {"2024-03-01T09:02:00", 48'000'000.0},
        {"2024-03-01T09:03:00", 47'000'000.0},
        {"2024-03-01T09:04:00", 46'000'000.0},
    };

    for (const auto& [ts, price] : entry_candles)
        router.routeMarketData(candleJson("KRW-BTC", ts, price));

    // 매수 주문 완료 대기
    bool bought = waitFor([&]{ return mock_api.postOrderCallCount() >= 1; });
    TEST_ASSERT(bought);

    // 2단계: myOrder "trade" 이벤트 (uuid="mock-order-uuid", 전량 체결)
    //   - remaining_volume=0 → toOrderStatus("trade",0) = Filled
    //   - trade_uuid 있음 → MyTrade 생성 → engine.onMyTrade() → AccountManager 갱신
    //   - Order 스냅샷 Filled → engine.onOrderSnapshot() → EngineOrderStatusEvent
    //   - 이후 pollEvents() → onFill() + onOrderUpdate(Filled) → InPosition
    const double fill_price  = 46'000'000.0;
    const double fill_volume = 0.02;
    router.routeMyOrder(myOrderJson(
        "KRW-BTC", "mock-order-uuid", "BID", "trade",
        fill_price, fill_volume, fill_volume, "trade-uuid-001"));

    // 체결 이벤트 처리 대기 (큐 전달 + 워커 처리 + AccountManager 갱신)
    // "부재 검증"이 아닌 후속 동작 검증이므로 처리 완료 추정 시간만큼 대기
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // 3단계: 상승 캔들 → close=47M > target(46M*1.01=46.46M) → maybeExit hitTarget → postOrder (ASK)
    router.routeMarketData(candleJson("KRW-BTC", "2024-03-01T09:10:00", 47'000'000.0));

    // 매도 주문 대기
    bool sold = waitFor([&]{ return mock_api.postOrderCallCount() >= 2; });
    mgr.stop();

    TEST_ASSERT(sold);
    TEST_ASSERT(mock_api.lastPostOrderRequest().position == core::OrderPosition::ASK);
    std::cout << "  Total postOrder calls: " << mock_api.postOrderCallCount() << "\n";
    std::cout << "  Last order side: "
              << (mock_api.lastPostOrderRequest().position == core::OrderPosition::ASK
                  ? "ASK (sell)" : "BID (buy)") << "\n";
    std::cout << "  [PASS] Fill processed → InPosition → exit order placed\n";
}

// ── 메인 실행 ────────────────────────────────────────────────────────

bool runAllTests()
{
    std::cout << "\n========================================\n";
    std::cout << "  MarketEngineManager Integration Tests\n";
    std::cout << "========================================\n";

    struct TestCase {
        const char* name;
        void (*fn)();
    };

    TestCase tests[] = {
        {"ConstructionSuccess",      testConstructionSuccess},
        {"ConstructionFailure",      testConstructionFailure},
        {"DuplicateMarketGuard",     testDuplicateMarketGuard},
        {"StartStop",                testStartStop},
        {"DoubleStart",              testDoubleStart},
        {"DestructorAutoStop",       testDestructorAutoStop},
        {"RegisterWithRouter",       testRegisterWithRouter},
        {"CandleEventTriggersOrder", testCandleEventTriggersOrder},
        {"BadEventWorkerSurvives",   testBadEventWorkerSurvives},
        {"MultiMarketIsolation",     testMultiMarketIsolation},
        {"MyOrderRouting",           testMyOrderRouting},
        {"MyOrderFillEnablesExit",   testMyOrderFillEnablesExit},
    };

    int passed = 0, failed = 0;
    const int total = static_cast<int>(sizeof(tests) / sizeof(tests[0]));

    for (int i = 0; i < total; ++i)
    {
        try {
            tests[i].fn();
            ++passed;
        }
        catch (const TestFailure& e) {
            ++failed;
            std::cerr << "\n[TEST FAILED] " << tests[i].name << "\n";
            std::cerr << "  Reason: "   << e.condition() << "\n";
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
        std::cerr << "  TESTS FAILED: " << failed << " failed, "
                  << passed << " passed (" << total << " total)\n";
        std::cerr << "========================================\n";
        return false;
    }
}

} // namespace test

int main()
{
    bool success = test::runAllTests();
    return success ? 0 : 1;
}
