// tests/test_event_router.cpp
//
// EventRouter 유닛 테스트
// - 정상 라우팅: fast path ("code" 키 / "market" 키 / 둘 다 일치)
// - 정상 라우팅: fallback path (이스케이프 포함 → nlohmann 파싱)
// - 실패 케이스: 미등록 마켓, code/market 충돌, JSON 파싱 실패
// - 백프레셔: BlockingQueue(max_size) drop-oldest 동작, routeMyOrder 유실 불가
// - 멀티마켓: 큐 격리(타입 검증 포함), 백프레셔 독립성, routeMyOrder 격리
// - 통계: 혼합 시나리오에서 카운터 정확성

#include <iostream>
#include <string>
#include <variant>

#include "test_utils.h"
#include "app/EventRouter.h"
#include "engine/input/EngineInput.h"
#include "util/Logger.h"

using namespace app;
using namespace engine::input;

namespace test {

// ── 헬퍼 ──────────────────────────────────────────────────────────────

// Upbit ticker 형식 JSON ("code" 키 사용)
static std::string tickerJson(const std::string& market)
{
    return R"({"type":"ticker","code":")" + market + R"("})";
}

// Upbit orderbook 형식 JSON ("market" 키 사용)
static std::string orderbookJson(const std::string& market)
{
    return R"({"type":"orderbook","market":")" + market + R"("})";
}

// ── 정상 흐름: Fast Path ───────────────────────────────────────────────

// [TEST 1] "code" 키로 fast path 라우팅 성공
// ticker 형식에서 마켓 코드를 추출하여 올바른 큐로 전달
void testFastPath_CodeKey()
{
    std::cout << "\n[TEST 1] Fast path - \"code\" key routing\n";

    EventRouter router;
    EventRouter::PrivateQueue q;
    router.registerMarket("KRW-BTC", q);

    bool ok = router.routeMarketData(tickerJson("KRW-BTC"));

    TEST_ASSERT(ok);
    TEST_ASSERT_EQ(router.stats().fast_path_success.load(), uint64_t(1));
    TEST_ASSERT_EQ(router.stats().total_routed.load(), uint64_t(1));
    TEST_ASSERT_EQ(router.stats().fallback_used.load(), uint64_t(0));
    TEST_ASSERT_EQ(q.size(), std::size_t(1));

    // 큐에 MarketDataRaw 타입으로 들어있는지 확인
    auto item = q.try_pop();
    TEST_ASSERT(item.has_value());
    TEST_ASSERT(std::holds_alternative<MarketDataRaw>(*item));

    std::cout << "  [PASS] \"code\" key fast path success\n";
}

// [TEST 2] "market" 키로 fast path 라우팅 성공
// orderbook 형식에서 마켓 코드를 추출하여 올바른 큐로 전달
void testFastPath_MarketKey()
{
    std::cout << "\n[TEST 2] Fast path - \"market\" key routing\n";

    EventRouter router;
    EventRouter::PrivateQueue q;
    router.registerMarket("KRW-ETH", q);

    bool ok = router.routeMarketData(orderbookJson("KRW-ETH"));

    TEST_ASSERT(ok);
    TEST_ASSERT_EQ(router.stats().fast_path_success.load(), uint64_t(1));
    TEST_ASSERT_EQ(router.stats().total_routed.load(), uint64_t(1));
    TEST_ASSERT_EQ(q.size(), std::size_t(1));

    std::cout << "  [PASS] \"market\" key fast path success\n";
}

// [TEST 3] code == market 일치 시 fast path 정상 라우팅
// 두 키 모두 있고 값이 같은 경우: Upbit API 혼용 형식에서도 허용
void testFastPath_BothKeysMatch()
{
    std::cout << "\n[TEST 3] Fast path - code == market match\n";

    EventRouter router;
    EventRouter::PrivateQueue q;
    router.registerMarket("KRW-BTC", q);

    const std::string json = R"({"code":"KRW-BTC","market":"KRW-BTC"})";
    bool ok = router.routeMarketData(json);

    TEST_ASSERT(ok);
    TEST_ASSERT_EQ(router.stats().fast_path_success.load(), uint64_t(1));
    TEST_ASSERT_EQ(router.stats().conflict_detected.load(), uint64_t(0));
    TEST_ASSERT_EQ(router.stats().total_routed.load(), uint64_t(1));

    std::cout << "  [PASS] Matching code/market allows routing\n";
}

// ── 정상 흐름: Fallback Path ───────────────────────────────────────────

// [TEST 4] fast path 실패 시 fallback(nlohmann) 파싱으로 라우팅 성공
// code 값에 unicode escape(\u002D = '-') 포함 → fast path에서 '\\' 발견 → nullopt
// nlohmann slow path: \u002D → '-' 로 정규화 → "KRW-BTC" 추출
void testFallback_UnicodeEscape()
{
    std::cout << "\n[TEST 4] Fallback path - unicode escape triggers slow parse\n";

    EventRouter router;
    EventRouter::PrivateQueue q;
    router.registerMarket("KRW-BTC", q);

    // raw string: JSON 내용 그대로 = {"code":"KRW\u002DBTC"}
    // fast path: '\' 발견 → nullopt → fallback 호출
    // nlohmann: \u002D → '-' → code = "KRW-BTC"
    const std::string json = R"({"code":"KRW\u002DBTC"})";
    bool ok = router.routeMarketData(json);

    TEST_ASSERT(ok);
    TEST_ASSERT_EQ(router.stats().fast_path_success.load(), uint64_t(0));
    TEST_ASSERT_EQ(router.stats().fallback_used.load(), uint64_t(1));
    TEST_ASSERT_EQ(router.stats().total_routed.load(), uint64_t(1));
    TEST_ASSERT_EQ(q.size(), std::size_t(1));

    std::cout << "  [PASS] Fallback parsed unicode escape, routed correctly\n";
}

// ── 실패 케이스 ────────────────────────────────────────────────────────

// [TEST 5] 미등록 마켓 → unknown_market 증가, false 반환
// 파싱은 성공했지만 routes_에 등록되지 않은 마켓 코드
void testUnknownMarket()
{
    std::cout << "\n[TEST 5] Unknown market\n";

    EventRouter router;
    // KRW-BTC 미등록 상태

    bool ok = router.routeMarketData(tickerJson("KRW-BTC"));

    TEST_ASSERT(!ok);
    TEST_ASSERT_EQ(router.stats().unknown_market.load(), uint64_t(1));
    TEST_ASSERT_EQ(router.stats().total_routed.load(), uint64_t(0));

    std::cout << "  [PASS] Unregistered market returns false\n";
}

// [TEST 6] code/market 충돌 → conflict_detected 증가, parse_failures 미증가
// 두 키가 서로 다른 값 → fallback 없이 즉시 실패
// 현재 구현: 충돌은 conflict_detected로 집계 (ROADMAP과의 차이점 명시)
void testConflictDetected()
{
    std::cout << "\n[TEST 6] code/market conflict\n";

    EventRouter router;
    EventRouter::PrivateQueue q;
    router.registerMarket("KRW-BTC", q);

    const std::string json = R"({"code":"KRW-BTC","market":"KRW-ETH"})";
    bool ok = router.routeMarketData(json);

    TEST_ASSERT(!ok);
    TEST_ASSERT_EQ(router.stats().conflict_detected.load(), uint64_t(1));
    // 현재 구현에서 충돌은 parse_failures가 아닌 conflict_detected로만 집계됨
    TEST_ASSERT_EQ(router.stats().parse_failures.load(), uint64_t(0));
    TEST_ASSERT_EQ(router.stats().total_routed.load(), uint64_t(0));
    TEST_ASSERT_EQ(q.size(), std::size_t(0));

    std::cout << "  [PASS] Conflict increments conflict_detected (not parse_failures)\n";
}

// [TEST 7] 완전히 잘못된 JSON → parse_failures 증가
// fast path와 fallback(nlohmann) 모두 실패하는 경우
void testParseFailure_InvalidJson()
{
    std::cout << "\n[TEST 7] Parse failure - invalid JSON\n";

    EventRouter router;
    EventRouter::PrivateQueue q;
    router.registerMarket("KRW-BTC", q);

    bool ok = router.routeMarketData("not-json-at-all");

    TEST_ASSERT(!ok);
    TEST_ASSERT_EQ(router.stats().parse_failures.load(), uint64_t(1));
    TEST_ASSERT_EQ(router.stats().total_routed.load(), uint64_t(0));

    std::cout << "  [PASS] Invalid JSON increments parse_failures\n";
}

// [TEST 8] code/market 키가 없는 JSON → parse_failures 증가
// fast path: 두 키 모두 nullopt → fallback 시도
// fallback: nlohmann 파싱 성공이지만 code/market 키 없음 → nullopt → parse_failures
void testParseFailure_NoMarketKey()
{
    std::cout << "\n[TEST 8] Parse failure - no code/market key\n";

    EventRouter router;
    EventRouter::PrivateQueue q;
    router.registerMarket("KRW-BTC", q);

    // code도 market도 없는 유효한 JSON
    const std::string json = R"({"type":"ticker","symbol":"KRW-BTC"})";
    bool ok = router.routeMarketData(json);

    TEST_ASSERT(!ok);
    TEST_ASSERT_EQ(router.stats().parse_failures.load(), uint64_t(1));
    TEST_ASSERT_EQ(router.stats().fallback_used.load(), uint64_t(0));
    TEST_ASSERT_EQ(router.stats().total_routed.load(), uint64_t(0));

    std::cout << "  [PASS] JSON without market key increments parse_failures\n";
}

// ── 백프레셔 ───────────────────────────────────────────────────────────

// [TEST 9] BlockingQueue(max_size=5000) 포화 시 drop-oldest 동작
// 큐가 꽉 찬 상태에서 routeMarketData 호출 → 가장 오래된 항목이 빠지고 새 항목 push
// BlockingQueue 내부에서 처리하므로 total_routed는 정상 증가
void testBackpressure_DropOldest()
{
    std::cout << "\n[TEST 9] Backpressure - drop-oldest via BlockingQueue(max_size=5000)\n";

    EventRouter router;
    EventRouter::PrivateQueue q{5000};  // max_size 설정으로 drop-oldest 활성화
    router.registerMarket("KRW-BTC", q);

    // 큐를 max_size(5000)까지 채움
    for (int i = 0; i < 5000; ++i) {
        q.push(MarketDataRaw{"dummy"});
    }
    TEST_ASSERT_EQ(q.size(), std::size_t(5000));

    bool ok = router.routeMarketData(tickerJson("KRW-BTC"));

    // 라우팅 성공 → true, total_routed 증가
    TEST_ASSERT(ok);
    TEST_ASSERT_EQ(router.stats().total_routed.load(), uint64_t(1));
    // drop-oldest: 오래된 dummy 1개가 빠지고 새 항목이 들어옴 → 크기 유지
    TEST_ASSERT_EQ(q.size(), std::size_t(5000));

    // 마지막 항목이 새로 들어온 MarketDataRaw인지 확인
    // (큐가 FIFO이므로 4999개 skip 후 마지막 pop)
    for (int i = 0; i < 4999; ++i) q.try_pop();
    auto last = q.try_pop();
    TEST_ASSERT(last.has_value() && std::holds_alternative<MarketDataRaw>(*last));
    TEST_ASSERT(std::get<MarketDataRaw>(*last).json == tickerJson("KRW-BTC"));

    std::cout << "  [PASS] drop-oldest: size stays 5000, newest item is at tail\n";
}

// [TEST 10] routeMyOrder: max_size 없는 무제한 큐에 항상 push
// myOrder는 유실 불가 → 큐를 무제한(max_size=0)으로 생성
void testNoBackpressure_MyOrderAlwaysPushed()
{
    std::cout << "\n[TEST 10] No backpressure - MyOrder always pushed (unbounded queue)\n";

    EventRouter router;
    EventRouter::PrivateQueue q;  // max_size=0 (무제한)
    router.registerMarket("KRW-BTC", q);

    // 5000개 선채움 후에도 push 가능한지 검증
    for (int i = 0; i < 5000; ++i) {
        q.push(MarketDataRaw{"dummy"});
    }
    TEST_ASSERT_EQ(q.size(), std::size_t(5000));

    bool ok = router.routeMyOrder(tickerJson("KRW-BTC"));

    // 무제한 큐이므로 5001번째도 push 성공
    TEST_ASSERT(ok);
    TEST_ASSERT_EQ(router.stats().total_routed.load(), uint64_t(1));
    TEST_ASSERT_EQ(q.size(), std::size_t(5001));

    // 마지막이 MyOrderRaw인지 확인
    for (int i = 0; i < 5000; ++i) q.try_pop();
    auto last = q.try_pop();
    TEST_ASSERT(last.has_value() && std::holds_alternative<MyOrderRaw>(*last));

    std::cout << "  [PASS] MyOrder pushed to unbounded queue (5001st item is MyOrderRaw)\n";
}

// ── 멀티마켓 ───────────────────────────────────────────────────────────

// [TEST 11] 두 마켓 메시지가 각자 올바른 큐로만 전달
// KRW-BTC 메시지 → btc_q 증가, KRW-ETH 메시지 → eth_q 증가
// 큐에서 pop해서 MarketDataRaw 타입인지까지 확인
void testMultiMarket_CorrectRouting()
{
    std::cout << "\n[TEST 11] Multi-market - correct queue routing\n";

    EventRouter router;
    EventRouter::PrivateQueue btc_q, eth_q;
    router.registerMarket("KRW-BTC", btc_q);
    router.registerMarket("KRW-ETH", eth_q);

    router.routeMarketData(tickerJson("KRW-BTC"));
    router.routeMarketData(orderbookJson("KRW-ETH"));
    router.routeMarketData(tickerJson("KRW-BTC"));  // BTC 두 번째

    TEST_ASSERT_EQ(btc_q.size(), std::size_t(2));
    TEST_ASSERT_EQ(eth_q.size(), std::size_t(1));
    TEST_ASSERT_EQ(router.stats().total_routed.load(), uint64_t(3));

    // 큐에서 꺼내서 타입 확인 (오배달 시 타입 불일치로 감지)
    auto btc1 = btc_q.try_pop();
    auto btc2 = btc_q.try_pop();
    TEST_ASSERT(btc1.has_value() && std::holds_alternative<MarketDataRaw>(*btc1));
    TEST_ASSERT(btc2.has_value() && std::holds_alternative<MarketDataRaw>(*btc2));

    auto eth1 = eth_q.try_pop();
    TEST_ASSERT(eth1.has_value() && std::holds_alternative<MarketDataRaw>(*eth1));

    std::cout << "  [PASS] Each market message reaches correct queue as MarketDataRaw\n";
}

// [TEST 12] BTC 큐 포화(drop-oldest)가 ETH 큐 라우팅에 영향 없음
// BlockingQueue(max_size=5000)로 BTC 큐 포화 → drop-oldest 발생해도 ETH는 정상
void testMultiMarket_BackpressureIsolation()
{
    std::cout << "\n[TEST 12] Multi-market - drop-oldest isolation\n";

    EventRouter router;
    EventRouter::PrivateQueue btc_q{5000}, eth_q{5000};
    router.registerMarket("KRW-BTC", btc_q);
    router.registerMarket("KRW-ETH", eth_q);

    // BTC 큐만 max_size까지 채움
    for (int i = 0; i < 5000; ++i) {
        btc_q.push(MarketDataRaw{"dummy"});
    }

    // BTC: 포화 상태에서 push → drop-oldest 후 라우팅 성공
    bool btc_ok = router.routeMarketData(tickerJson("KRW-BTC"));
    TEST_ASSERT(btc_ok);
    TEST_ASSERT_EQ(btc_q.size(), std::size_t(5000));  // drop-oldest로 크기 유지
    TEST_ASSERT_EQ(router.stats().total_routed.load(), uint64_t(1));

    // ETH: BTC 큐 상태와 무관하게 정상 push
    bool eth_ok = router.routeMarketData(orderbookJson("KRW-ETH"));
    TEST_ASSERT(eth_ok);
    TEST_ASSERT_EQ(eth_q.size(), std::size_t(1));
    TEST_ASSERT_EQ(router.stats().total_routed.load(), uint64_t(2));

    std::cout << "  [PASS] BTC drop-oldest does not affect ETH routing\n";
}

// ── 통계 ───────────────────────────────────────────────────────────────

// [TEST 13] 혼합 시나리오에서 모든 통계 카운터 정확성
// fast + fallback + unknown + conflict + parse_failure 모두 포함
void testStats_MixedScenario()
{
    std::cout << "\n[TEST 13] Stats - mixed scenario accuracy\n";

    EventRouter router;
    EventRouter::PrivateQueue q;
    router.registerMarket("KRW-BTC", q);

    // fast path 성공 2회
    router.routeMarketData(tickerJson("KRW-BTC"));
    router.routeMarketData(orderbookJson("KRW-BTC"));

    // fallback 성공 1회 (unicode escape)
    router.routeMarketData(R"({"code":"KRW\u002DBTC"})");

    // 미등록 마켓 1회 (fast-path는 성공)
    router.routeMarketData(tickerJson("KRW-XRP"));

    // code/market 충돌 1회
    router.routeMarketData(R"({"code":"KRW-BTC","market":"KRW-ETH"})");

    // 파싱 완전 실패 1회
    router.routeMarketData("invalid");

    TEST_ASSERT_EQ(router.stats().fast_path_success.load(), uint64_t(2));
    TEST_ASSERT_EQ(router.stats().fallback_used.load(),     uint64_t(1));
    TEST_ASSERT_EQ(router.stats().unknown_market.load(),    uint64_t(1));
    TEST_ASSERT_EQ(router.stats().conflict_detected.load(), uint64_t(1));
    TEST_ASSERT_EQ(router.stats().parse_failures.load(),    uint64_t(1));
    TEST_ASSERT_EQ(router.stats().total_routed.load(),      uint64_t(3));  // 성공 3회만

    std::cout << "  fast=" << router.stats().fast_path_success.load()
              << " fallback=" << router.stats().fallback_used.load()
              << " unknown=" << router.stats().unknown_market.load()
              << " conflict=" << router.stats().conflict_detected.load()
              << " failure=" << router.stats().parse_failures.load()
              << " routed=" << router.stats().total_routed.load() << "\n";
    std::cout << "  [PASS] All counters match expected values\n";
}

// [TEST 14] routeMyOrder 정상 라우팅: MyOrderRaw 타입으로 전달
// routeMarketData와 같은 파싱 경로지만 push 타입이 다름
void testMyOrder_NormalRouting()
{
    std::cout << "\n[TEST 14] routeMyOrder - normal routing\n";

    EventRouter router;
    EventRouter::PrivateQueue q;
    router.registerMarket("KRW-BTC", q);

    bool ok = router.routeMyOrder(tickerJson("KRW-BTC"));

    TEST_ASSERT(ok);
    TEST_ASSERT_EQ(router.stats().total_routed.load(), uint64_t(1));

    // MarketDataRaw가 아닌 MyOrderRaw 타입
    auto item = q.try_pop();
    TEST_ASSERT(item.has_value());
    TEST_ASSERT(std::holds_alternative<MyOrderRaw>(*item));

    std::cout << "  [PASS] routeMyOrder pushes MyOrderRaw type\n";
}

// [TEST 15] routeMyOrder 파싱 실패 → parse_failures 증가, false 반환
// myOrder도 동일한 파싱 경로 → 실패 시 parse_failures 집계
void testMyOrder_ParseFailure()
{
    std::cout << "\n[TEST 15] routeMyOrder - parse failure\n";

    EventRouter router;
    EventRouter::PrivateQueue q;
    router.registerMarket("KRW-BTC", q);

    bool ok = router.routeMyOrder("bad-json");

    TEST_ASSERT(!ok);
    TEST_ASSERT_EQ(router.stats().parse_failures.load(), uint64_t(1));
    TEST_ASSERT_EQ(router.stats().total_routed.load(), uint64_t(0));
    TEST_ASSERT_EQ(q.size(), std::size_t(0));

    std::cout << "  [PASS] routeMyOrder parse failure increments parse_failures\n";
}

// [TEST 16] routeMyOrder 멀티마켓 격리: 각 마켓 MyOrder가 올바른 큐에만 전달
// routeMyOrder가 별도 함수/push 타입(MyOrderRaw)으로 분리돼 있어
// 독립 수정 시 격리가 깨지는 회귀를 방지하기 위한 테스트
void testMultiMarket_MyOrderIsolation()
{
    std::cout << "\n[TEST 16] Multi-market - routeMyOrder isolation\n";

    EventRouter router;
    EventRouter::PrivateQueue btc_q, eth_q;
    router.registerMarket("KRW-BTC", btc_q);
    router.registerMarket("KRW-ETH", eth_q);

    router.routeMyOrder(tickerJson("KRW-BTC"));
    router.routeMyOrder(tickerJson("KRW-ETH"));

    // 각자의 큐에만 전달됐는지 크기 확인
    TEST_ASSERT_EQ(btc_q.size(), std::size_t(1));
    TEST_ASSERT_EQ(eth_q.size(), std::size_t(1));

    // 타입이 MyOrderRaw인지 확인 (MarketDataRaw와 혼용 방지)
    auto btc_item = btc_q.try_pop();
    TEST_ASSERT(btc_item.has_value() && std::holds_alternative<MyOrderRaw>(*btc_item));

    auto eth_item = eth_q.try_pop();
    TEST_ASSERT(eth_item.has_value() && std::holds_alternative<MyOrderRaw>(*eth_item));

    std::cout << "  [PASS] routeMyOrder routes each market to correct queue as MyOrderRaw\n";
}

// ── 실행 함수 ─────────────────────────────────────────────────────────

struct TestCase {
    const char* name;
    void (*func)();
};

bool runAllTests()
{
    std::cout << "\n========================================\n";
    std::cout << "  EventRouter Unit Tests\n";
    std::cout << "========================================\n";

    TestCase tests[] = {
        {"FastPath_CodeKey",                   testFastPath_CodeKey},
        {"FastPath_MarketKey",                 testFastPath_MarketKey},
        {"FastPath_BothKeysMatch",             testFastPath_BothKeysMatch},
        {"Fallback_UnicodeEscape",             testFallback_UnicodeEscape},
        {"UnknownMarket",                      testUnknownMarket},
        {"ConflictDetected",                   testConflictDetected},
        {"ParseFailure_InvalidJson",           testParseFailure_InvalidJson},
        {"ParseFailure_NoMarketKey",           testParseFailure_NoMarketKey},
        {"Backpressure_DropOldest",            testBackpressure_DropOldest},
        {"NoBackpressure_MyOrderAlwaysPushed", testNoBackpressure_MyOrderAlwaysPushed},
        {"MultiMarket_CorrectRouting",         testMultiMarket_CorrectRouting},
        {"MultiMarket_BackpressureIsolation",  testMultiMarket_BackpressureIsolation},
        {"Stats_MixedScenario",                testStats_MixedScenario},
        {"MyOrder_NormalRouting",              testMyOrder_NormalRouting},
        {"MyOrder_ParseFailure",               testMyOrder_ParseFailure},
        {"MultiMarket_MyOrderIsolation",       testMultiMarket_MyOrderIsolation},
    };

    int passed = 0;
    int failed = 0;
    const int total = static_cast<int>(sizeof(tests) / sizeof(tests[0]));

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
        std::cerr << "  TESTS FAILED: " << failed << " failed, "
                  << passed << " passed (" << total << " total)\n";
        std::cerr << "========================================\n";
        return false;
    }
}

} // namespace test

int main()
{
    // 충돌/실패 케이스에서 나오는 WARN 로그 억제
    util::log().setLevel(util::LogLevel::LV_ERROR);
    bool success = test::runAllTests();
    return success ? 0 : 1;
}
