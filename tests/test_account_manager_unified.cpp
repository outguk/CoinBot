// tests/test_account_manager_unified.cpp
//
// AccountManager 통합 테스트 (기본 + 개선)
//
// 목적:
// - 멀티마켓 전량 거래 환경에서 자산 관리의 정확성 검증
// - RAII 패턴 (ReservationToken) 동작 검증
// - Thread-safety 검증
// - 엣지 케이스 (dust, 과매도, 외부 거래) 검증

#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cassert>
#include <cmath>

#include "trading/allocation/AccountManager.h"
#include "core/domain/Account.h"
#include "core/domain/Position.h"

using namespace trading::allocation;
using namespace core;
using namespace std::chrono_literals;

namespace test {

    // 테스트 헬퍼: 부동소수점 비교 (epsilon 사용)
    bool almostEqual(double a, double b, double epsilon = 1e-7) {
        return std::abs(a - b) < epsilon;
    }

    // ============================================================
    // 테스트 1: 기본 초기화 (KRW만)
    // ============================================================
    void testInitializationKrwOnly() {
        std::cout << "\n[TEST 1] Initialization (KRW only)\n";

        // 1,000,000 KRW를 3개 마켓에 균등 배분
        Account account;
        account.krw_free = 1'000'000.0;
        account.krw_locked = 0.0;

        std::vector<std::string> markets = { "KRW-BTC", "KRW-ETH", "KRW-XRP" };

        AccountManager manager(account, markets);

        // 각 마켓은 1,000,000 / 3 = 333,333.33... KRW
        double expected_per_market = 1'000'000.0 / 3.0;

        for (const auto& market : markets) {
            auto budget = manager.getBudget(market);
            assert(budget.has_value());

            std::cout << "  " << market << ": "
                << "available_krw=" << budget->available_krw
                << ", initial_capital=" << budget->initial_capital << "\n";

            assert(almostEqual(budget->available_krw, expected_per_market));
            assert(almostEqual(budget->initial_capital, expected_per_market));
            assert(budget->coin_balance == 0.0);
            assert(budget->reserved_krw == 0.0);
        }

        std::cout << "  [PASS] KRW equally distributed\n";
    }

    // ============================================================
    // 테스트 2: 초기화 with 코인 포지션
    // ============================================================
    void testInitializationWithPositions() {
        std::cout << "\n[TEST 2] Initialization with coin positions\n";

        Account account;
        account.krw_free = 500'000.0;  // 남은 KRW

        // BTC 포지션: 0.01 BTC @ 50,000,000 KRW (500,000 KRW 상당)
        Position btc_pos;
        btc_pos.currency = "BTC";
        btc_pos.free = 0.01;
        btc_pos.avg_buy_price = 50'000'000.0;
        btc_pos.unit_currency = "KRW";
        account.positions.push_back(btc_pos);

        std::vector<std::string> markets = { "KRW-BTC", "KRW-ETH" };

        AccountManager manager(account, markets);

        // BTC: 코인 보유 (500,000 KRW 상당)
        auto btc_budget = manager.getBudget("KRW-BTC");
        assert(btc_budget.has_value());
        assert(btc_budget->coin_balance == 0.01);
        assert(btc_budget->avg_entry_price == 50'000'000.0);
        assert(almostEqual(btc_budget->initial_capital, 500'000.0));
        assert(btc_budget->available_krw == 0.0);  // 전량 매수 상태

        // ETH: KRW만 (500,000 KRW 전액)
        auto eth_budget = manager.getBudget("KRW-ETH");
        assert(eth_budget.has_value());
        assert(eth_budget->coin_balance == 0.0);
        assert(almostEqual(eth_budget->available_krw, 500'000.0));
        assert(almostEqual(eth_budget->initial_capital, 500'000.0));

        std::cout << "  KRW-BTC: coin_balance=" << btc_budget->coin_balance
            << ", initial_capital=" << btc_budget->initial_capital << "\n";
        std::cout << "  KRW-ETH: available_krw=" << eth_budget->available_krw
            << ", initial_capital=" << eth_budget->initial_capital << "\n";

        std::cout << "  [PASS] Coin positions correctly reflected\n";
    }

    // ============================================================
    // 테스트 3: Dust 처리 (초기화 시)
    // ============================================================
    void testInitializationDustHandling() {
        std::cout << "\n[TEST 3] Initialization dust handling\n";

        Account account;
        account.krw_free = 1'000'000.0;

        // Dust 포지션: 4,000원 상당 (init_dust_threshold_krw = 5,000원 미만)
        Position dust_pos;
        dust_pos.currency = "DOGE";
        dust_pos.free = 100.0;
        dust_pos.avg_buy_price = 40.0;  // 100 * 40 = 4,000 KRW
        dust_pos.unit_currency = "KRW";
        account.positions.push_back(dust_pos);

        std::vector<std::string> markets = { "KRW-DOGE", "KRW-BTC" };

        AccountManager manager(account, markets);

        // DOGE: dust 무시 → KRW로 거래 가능 상태
        auto doge_budget = manager.getBudget("KRW-DOGE");
        assert(doge_budget.has_value());
        assert(doge_budget->coin_balance == 0.0);  // dust 제거
        assert(doge_budget->available_krw > 0.0);  // KRW 배분받음

        std::cout << "  KRW-DOGE: dust ignored, available_krw=" << doge_budget->available_krw << "\n";
        std::cout << "  [PASS] Dust positions filtered\n";
    }

    // ============================================================
    // 테스트 4: 예약/해제 (reserve/release)
    // ============================================================
    void testReserveRelease() {
        std::cout << "\n[TEST 4] Reserve and release\n";

        Account account;
        account.krw_free = 100'000.0;
        std::vector<std::string> markets = { "KRW-BTC" };

        AccountManager manager(account, markets);

        // 예약 전 상태
        auto budget_before = manager.getBudget("KRW-BTC");
        assert(almostEqual(budget_before->available_krw, 100'000.0));
        assert(budget_before->reserved_krw == 0.0);

        // 50,000 KRW 예약
        auto token = manager.reserve("KRW-BTC", 50'000.0);
        assert(token.has_value());
        assert(token->amount() == 50'000.0);
        assert(token->isActive());

        // 예약 후 상태
        auto budget_after = manager.getBudget("KRW-BTC");
        assert(almostEqual(budget_after->available_krw, 50'000.0));
        assert(almostEqual(budget_after->reserved_krw, 50'000.0));

        std::cout << "  After reserve: available=" << budget_after->available_krw
            << ", reserved=" << budget_after->reserved_krw << "\n";

        // 예약 해제
        manager.release(std::move(token.value()));

        // 해제 후 상태 (원복)
        auto budget_released = manager.getBudget("KRW-BTC");
        assert(almostEqual(budget_released->available_krw, 100'000.0));
        assert(budget_released->reserved_krw == 0.0);

        std::cout << "  After release: available=" << budget_released->available_krw
            << ", reserved=" << budget_released->reserved_krw << "\n";

        std::cout << "  [PASS] Reserve/release cycle works\n";
    }

    // ============================================================
    // 테스트 5: 예약 실패 (잔액 부족, 미등록 마켓)
    // ============================================================
    void testReserveFailures() {
        std::cout << "\n[TEST 5] Reserve failures\n";

        Account account;
        account.krw_free = 50'000.0;
        std::vector<std::string> markets = { "KRW-BTC" };

        AccountManager manager(account, markets);

        // Case 1: 잔액 부족
        auto token1 = manager.reserve("KRW-BTC", 100'000.0);
        assert(!token1.has_value());
        std::cout << "  [PASS] Reserve fails when insufficient balance\n";

        // Case 2: 미등록 마켓
        auto token2 = manager.reserve("KRW-ETH", 10'000.0);
        assert(!token2.has_value());
        std::cout << "  [PASS] Reserve fails for unregistered market\n";

        // 통계 확인
        assert(manager.stats().reserve_failures.load() == 2);
        std::cout << "  Reserve failures count: " << manager.stats().reserve_failures.load() << "\n";
    }

    // ============================================================
    // 테스트 6: ReservationToken RAII (소멸자 자동 해제)
    // ============================================================
    void testTokenRAII() {
        std::cout << "\n[TEST 6] ReservationToken RAII (destructor auto-release)\n";

        Account account;
        account.krw_free = 100'000.0;
        std::vector<std::string> markets = { "KRW-BTC" };

        AccountManager manager(account, markets);

        // 스코프 내에서 토큰 생성
        {
            auto token = manager.reserve("KRW-BTC", 30'000.0);
            assert(token.has_value());

            auto budget = manager.getBudget("KRW-BTC");
            assert(almostEqual(budget->reserved_krw, 30'000.0));

            std::cout << "  Inside scope: reserved=" << budget->reserved_krw << "\n";
            // 토큰 소멸 (스코프 종료)
        }

        // 소멸자가 자동으로 release() 호출
        auto budget_after = manager.getBudget("KRW-BTC");
        assert(almostEqual(budget_after->available_krw, 100'000.0));
        assert(budget_after->reserved_krw == 0.0);

        std::cout << "  After scope: available=" << budget_after->available_krw << "\n";
        std::cout << "  [PASS] Token destructor auto-released\n";
    }

    // ============================================================
    // 테스트 7: 매수 체결 (전체)
    // ============================================================
    void testFinalizeFillBuy() {
        std::cout << "\n[TEST 7] Finalize buy fill (full execution)\n";

        Account account;
        account.krw_free = 100'000.0;
        std::vector<std::string> markets = { "KRW-BTC" };

        AccountManager manager(account, markets);

        auto token = manager.reserve("KRW-BTC", 100'000.0);
        assert(token.has_value());

        // 전량 매수: 100,000 KRW → 0.002 BTC @ 50,000,000 KRW
        double executed_krw = 100'000.0;
        double received_coin = 0.002;
        double fill_price = 50'000'000.0;

        manager.finalizeFillBuy(token.value(), executed_krw, received_coin, fill_price);

        auto budget = manager.getBudget("KRW-BTC");
        assert(budget->reserved_krw == 0.0);  // 예약 소진
        assert(budget->coin_balance == 0.002);
        assert(almostEqual(budget->avg_entry_price, 50'000'000.0));

        std::cout << "  coin_balance=" << budget->coin_balance
            << ", avg_entry_price=" << budget->avg_entry_price << "\n";

        // 토큰 정리
        manager.finalizeOrder(std::move(token.value()));

        std::cout << "  [PASS] Buy fill correctly updated balance\n";
    }

    // ============================================================
    // 테스트 8: 부분 매수 체결 (평균 매수가 계산)
    // ============================================================
    void testPartialFillBuyWithAvgPrice() {
        std::cout << "\n[TEST 8] Partial buy fills with avg price calculation\n";

        Account account;
        account.krw_free = 100'000.0;
        std::vector<std::string> markets = { "KRW-BTC" };

        AccountManager manager(account, markets);

        auto token = manager.reserve("KRW-BTC", 100'000.0);
        assert(token.has_value());

        // 1차 체결: 50,000 KRW → 0.001 BTC @ 50,000,000 KRW
        manager.finalizeFillBuy(token.value(), 50'000.0, 0.001, 50'000'000.0);

        // 2차 체결: 30,000 KRW → 0.0006 BTC @ 50,000,000 KRW
        manager.finalizeFillBuy(token.value(), 30'000.0, 0.0006, 50'000'000.0);

        auto budget = manager.getBudget("KRW-BTC");
        double total_coin = 0.001 + 0.0006;  // 0.0016
        double avg_price = (50'000.0 + 30'000.0) / total_coin;  // 50,000,000

        assert(almostEqual(budget->coin_balance, total_coin, 1e-8));
        assert(almostEqual(budget->avg_entry_price, avg_price, 100.0));  // 부동소수점 오차 허용

        std::cout << "  After 2 fills: coin_balance=" << budget->coin_balance
            << ", avg_entry_price=" << budget->avg_entry_price << "\n";

        // 미체결 잔액 확인 (100,000 - 50,000 - 30,000 = 20,000)
        assert(almostEqual(token->remaining(), 20'000.0));

        manager.finalizeOrder(std::move(token.value()));

        // finalizeOrder 후 미체결 금액이 available로 복구
        auto budget_final = manager.getBudget("KRW-BTC");
        assert(almostEqual(budget_final->available_krw, 20'000.0));

        std::cout << "  Remaining KRW released: " << budget_final->available_krw << "\n";
        std::cout << "  [PASS] Partial fills and avg price correct\n";
    }

    // ============================================================
    // 테스트 9: 매도 체결 (전체, realized PnL)
    // ============================================================
    void testFinalizeFillSell() {
        std::cout << "\n[TEST 9] Finalize sell fill (full, realized PnL)\n";

        // 초기 상태: BTC 포지션 보유
        Account account;
        account.krw_free = 0.0;

        Position btc_pos;
        btc_pos.currency = "BTC";
        btc_pos.free = 0.002;
        btc_pos.avg_buy_price = 50'000'000.0;  // 매수가 100,000 KRW 상당
        account.positions.push_back(btc_pos);

        std::vector<std::string> markets = { "KRW-BTC" };

        AccountManager manager(account, markets);

        auto budget_before = manager.getBudget("KRW-BTC");
        assert(budget_before->coin_balance == 0.002);
        assert(almostEqual(budget_before->initial_capital, 100'000.0));

        // 전량 매도: 0.002 BTC → 110,000 KRW (10,000 KRW 수익)
        double sold_coin = 0.002;
        double received_krw = 110'000.0;

        manager.finalizeFillSell("KRW-BTC", sold_coin, received_krw);

        auto budget_after = manager.getBudget("KRW-BTC");
        assert(budget_after->coin_balance == 0.0);  // 전량 매도
        assert(budget_after->avg_entry_price == 0.0);  // 리셋
        assert(almostEqual(budget_after->available_krw, 110'000.0));

        // Realized PnL = 110,000 - 100,000 = 10,000
        double expected_pnl = 110'000.0 - 100'000.0;
        assert(almostEqual(budget_after->realized_pnl, expected_pnl));

        std::cout << "  available_krw=" << budget_after->available_krw
            << ", realized_pnl=" << budget_after->realized_pnl << "\n";

        double roi = budget_after->getRealizedROI();
        std::cout << "  Realized ROI: " << roi << "%\n";
        assert(almostEqual(roi, 10.0));  // 10% 수익

        std::cout << "  [PASS] Sell fill and realized PnL correct\n";
    }

    // ============================================================
    // 테스트 10: 부분 매도 체결
    // ============================================================
    void testPartialFillSell() {
        std::cout << "\n[TEST 10] Partial sell fills\n";

        Account account;
        account.krw_free = 0.0;

        Position btc_pos;
        btc_pos.currency = "BTC";
        btc_pos.free = 0.01;
        btc_pos.avg_buy_price = 50'000'000.0;  // 500,000 KRW 상당
        account.positions.push_back(btc_pos);

        std::vector<std::string> markets = { "KRW-BTC" };

        AccountManager manager(account, markets);

        // 1차 매도: 0.005 BTC → 250,000 KRW
        manager.finalizeFillSell("KRW-BTC", 0.005, 250'000.0);

        auto budget1 = manager.getBudget("KRW-BTC");
        assert(almostEqual(budget1->coin_balance, 0.005));
        assert(almostEqual(budget1->available_krw, 250'000.0));
        assert(budget1->avg_entry_price == 50'000'000.0);  // 아직 유지

        // 2차 매도: 나머지 0.005 BTC → 260,000 KRW (상승 매도)
        manager.finalizeFillSell("KRW-BTC", 0.005, 260'000.0);

        auto budget2 = manager.getBudget("KRW-BTC");
        assert(budget2->coin_balance == 0.0);
        assert(budget2->avg_entry_price == 0.0);
        assert(almostEqual(budget2->available_krw, 510'000.0));

        // Realized PnL = 510,000 - 500,000 = 10,000
        assert(almostEqual(budget2->realized_pnl, 10'000.0));

        std::cout << "  After 2 sells: available_krw=" << budget2->available_krw
            << ", realized_pnl=" << budget2->realized_pnl << "\n";

        std::cout << "  [PASS] Partial sells work correctly\n";
    }

    // ============================================================
    // 테스트 11: syncWithAccount - 상태 모델 검증
    // ============================================================
    void testSyncWithAccountStateModel() {
        std::cout << "\n[TEST 11] Sync with account - State model validation\n";

        // 초기 상태: 균등 배분
        Account initial;
        initial.krw_free = 1'000'000.0;
        std::vector<std::string> markets = { "KRW-BTC", "KRW-ETH" };

        AccountManager manager(initial, markets);

        // 외부 거래 시뮬레이션: KRW-BTC만 0.01 BTC 매수
        Account updated;
        updated.krw_free = 500'000.0;  // 500,000원으로 BTC 매수

        Position btc_pos;
        btc_pos.currency = "BTC";
        btc_pos.free = 0.01;
        btc_pos.avg_buy_price = 50'000'000.0;
        updated.positions.push_back(btc_pos);

        // 동기화
        manager.syncWithAccount(updated);

        auto btc_budget = manager.getBudget("KRW-BTC");
        auto eth_budget = manager.getBudget("KRW-ETH");

        std::cout << "  After sync:\n";
        std::cout << "    KRW-BTC: coin=" << btc_budget->coin_balance
                  << ", krw=" << btc_budget->available_krw << "\n";
        std::cout << "    KRW-ETH: coin=" << eth_budget->coin_balance
                  << ", krw=" << eth_budget->available_krw << "\n";

        // ⭐ 핵심: 전량 거래 모델 검증
        // 코인을 보유한 마켓은 available_krw = 0 이어야 함
        assert(btc_budget->coin_balance == 0.01);
        assert(btc_budget->avg_entry_price == 50'000'000.0);
        assert(btc_budget->available_krw == 0.0);  // 전량 거래 모델

        // 코인이 없는 마켓만 KRW 보유
        assert(eth_budget->coin_balance == 0.0);
        assert(almostEqual(eth_budget->available_krw, 500'000.0, 100.0));

        // 상태 불변 조건: coin > 0 XOR krw > 0
        for (const auto& [market, budget] : manager.snapshot()) {
            bool has_coin = budget.coin_balance > 1e-9;
            bool has_krw = budget.available_krw > 1.0;
            assert(!(has_coin && has_krw));  // 둘 다 있으면 안됨
        }

        std::cout << "  [PASS] State model validated\n";
    }

    // ============================================================
    // 테스트 12: 다중 포지션 동기화
    // ============================================================
    void testSyncWithMultiplePositions() {
        std::cout << "\n[TEST 12] Sync with multiple positions\n";

        Account initial;
        initial.krw_free = 1'500'000.0;
        std::vector<std::string> markets = { "KRW-BTC", "KRW-ETH", "KRW-XRP" };

        AccountManager manager(initial, markets);

        // 외부 거래: BTC와 ETH 매수, XRP는 Flat
        Account updated;
        updated.krw_free = 500'000.0;

        Position btc_pos;
        btc_pos.currency = "BTC";
        btc_pos.free = 0.01;
        btc_pos.avg_buy_price = 50'000'000.0;
        updated.positions.push_back(btc_pos);

        Position eth_pos;
        eth_pos.currency = "ETH";
        eth_pos.free = 0.2;
        eth_pos.avg_buy_price = 2'500'000.0;
        updated.positions.push_back(eth_pos);

        manager.syncWithAccount(updated);

        auto btc_budget = manager.getBudget("KRW-BTC");
        auto eth_budget = manager.getBudget("KRW-ETH");
        auto xrp_budget = manager.getBudget("KRW-XRP");

        std::cout << "  KRW-BTC: coin=" << btc_budget->coin_balance
                  << ", krw=" << btc_budget->available_krw << "\n";
        std::cout << "  KRW-ETH: coin=" << eth_budget->coin_balance
                  << ", krw=" << eth_budget->available_krw << "\n";
        std::cout << "  KRW-XRP: coin=" << xrp_budget->coin_balance
                  << ", krw=" << xrp_budget->available_krw << "\n";

        // BTC: 코인 보유 → krw = 0
        assert(btc_budget->coin_balance == 0.01);
        assert(btc_budget->available_krw == 0.0);

        // ETH: 코인 보유 → krw = 0
        assert(eth_budget->coin_balance == 0.2);
        assert(eth_budget->available_krw == 0.0);

        // XRP: 코인 없음 → 모든 KRW
        assert(xrp_budget->coin_balance == 0.0);
        assert(almostEqual(xrp_budget->available_krw, 500'000.0, 100.0));

        std::cout << "  [PASS] Multiple positions synced correctly\n";
    }

    // ============================================================
    // 테스트 13: getCurrentEquity/ROI 계산
    // ============================================================
    void testEquityAndROI() {
        std::cout << "\n[TEST 13] Equity and ROI calculation\n";

        Account account;
        account.krw_free = 100'000.0;
        std::vector<std::string> markets = { "KRW-BTC" };

        AccountManager manager(account, markets);

        // 매수 시뮬레이션
        auto token = manager.reserve("KRW-BTC", 100'000.0);
        assert(token.has_value());

        manager.finalizeFillBuy(token.value(), 100'000.0, 0.002, 50'000'000.0);
        manager.finalizeOrder(std::move(token.value()));

        auto budget = manager.getBudget("KRW-BTC");

        // 현재가 55M으로 상승 → 10% 수익
        Price current_price = 55'000'000.0;
        Amount equity = budget->getCurrentEquity(current_price);
        double roi = budget->getROI(current_price);

        std::cout << "  Initial: 100,000 KRW\n";
        std::cout << "  Bought: 0.002 BTC @ 50M\n";
        std::cout << "  Current price: 55M\n";
        std::cout << "  Equity: " << equity << "\n";
        std::cout << "  ROI: " << roi << "%\n";

        assert(almostEqual(equity, 110'000.0));
        assert(almostEqual(roi, 10.0));

        // 매도 후 realized PnL
        manager.finalizeFillSell("KRW-BTC", 0.002, 110'000.0);
        budget = manager.getBudget("KRW-BTC");

        double realized_roi = budget->getRealizedROI();
        std::cout << "  After sell: realized_pnl=" << budget->realized_pnl
                  << ", ROI=" << realized_roi << "%\n";

        assert(almostEqual(budget->realized_pnl, 10'000.0));
        assert(almostEqual(realized_roi, 10.0));

        std::cout << "  [PASS] Equity and ROI calculations correct\n";
    }

    // ============================================================
    // 테스트 14: syncWithAccount Dust 처리 일관성
    // ============================================================
    void testSyncWithAccountDustHandling() {
        std::cout << "\n[TEST 14] syncWithAccount dust handling (value-based)\n";

        Account initial;
        initial.krw_free = 1'000'000.0;
        std::vector<std::string> markets = { "KRW-BTC", "KRW-DOGE" };

        AccountManager manager(initial, markets);

        // 외부 거래: dust 코인 보유
        Account updated;
        updated.krw_free = 996'000.0;  // 4,000원어치 매수

        // Dust 포지션: 0.00004 BTC @ 100M = 4,000원 (init_dust_threshold 5,000원 미만)
        Position dust_pos;
        dust_pos.currency = "BTC";
        dust_pos.free = 0.00004;
        dust_pos.avg_buy_price = 100'000'000.0;
        updated.positions.push_back(dust_pos);

        // 동기화
        manager.syncWithAccount(updated);

        auto btc_budget = manager.getBudget("KRW-BTC");
        auto doge_budget = manager.getBudget("KRW-DOGE");

        std::cout << "  Dust coin (4,000원 < 5,000원):\n";
        std::cout << "    KRW-BTC: coin=" << btc_budget->coin_balance
                  << ", krw=" << btc_budget->available_krw << "\n";
        std::cout << "    KRW-DOGE: coin=" << doge_budget->coin_balance
                  << ", krw=" << doge_budget->available_krw << "\n";

        // Dust는 0으로 처리되어야 함
        assert(btc_budget->coin_balance == 0.0);
        assert(btc_budget->avg_entry_price == 0.0);
        assert(btc_budget->available_krw > 0.0);

        // 전체 KRW 균등 분배
        double total_krw = btc_budget->available_krw + doge_budget->available_krw;
        assert(almostEqual(total_krw, 996'000.0, 100.0));

        std::cout << "  [PASS] Dust handling consistent with constructor\n";
    }

    // ============================================================
    // 테스트 15: finalizeFillSell 저가 코인 dust 처리
    // ============================================================
    void testFinalizeFillSellLowPriceCoin() {
        std::cout << "\n[TEST 15] finalizeFillSell - low price coin dust handling\n";

        Account initial;
        initial.krw_free = 100'000.0;
        std::vector<std::string> markets = { "KRW-LOWCOIN" };

        AccountManager manager(initial, markets);

        // 매수: 100,000원으로 저가 코인 매수
        // 가격: 0.1원/개 → 1,000,000개 매수
        auto token = manager.reserve("KRW-LOWCOIN", 100'000.0);
        assert(token.has_value());

        // 매수 체결: 1,000,000개 @ 0.1원
        manager.finalizeFillBuy(token.value(), 100'000.0, 1'000'000.0, 0.1);
        manager.finalizeOrder(std::move(token.value()));

        auto budget_after_buy = manager.getBudget("KRW-LOWCOIN");
        std::cout << "  After buy: coin=" << budget_after_buy->coin_balance
                  << ", value=" << (budget_after_buy->coin_balance * 0.1) << " KRW\n";

        // 매도: 대부분 매도, 소량 잔량 남김
        // 999,900개 매도 → 100개 (= 10원) 잔량
        // 가치: 100 * 0.1 = 10원 < 5,000원 → dust
        Volume sold = 999'900.0;
        Amount received = sold * 0.1;  // 99,990원

        manager.finalizeFillSell("KRW-LOWCOIN", sold, received);

        auto budget_after_sell = manager.getBudget("KRW-LOWCOIN");
        Volume remaining_coin = budget_after_sell->coin_balance;
        Amount remaining_value = remaining_coin * 0.1;

        std::cout << "  After sell:\n";
        std::cout << "    coin_balance=" << remaining_coin << "\n";
        std::cout << "    remaining_value=" << remaining_value << " KRW\n";
        std::cout << "    available_krw=" << budget_after_sell->available_krw << "\n";

        // 핵심 검증: 10원 가치의 잔량은 dust로 처리되어야 함
        assert(remaining_coin == 0.0);
        assert(budget_after_sell->avg_entry_price == 0.0);
        assert(budget_after_sell->available_krw > 0.0);

        std::cout << "  [PASS] Low price coin dust handled correctly\n";
    }

    // ============================================================
    // 테스트 16: finalizeFillSell 고가 코인 정상 잔량
    // ============================================================
    void testFinalizeFillSellHighPriceCoin() {
        std::cout << "\n[TEST 16] finalizeFillSell - high price coin normal remainder\n";

        Account initial;
        initial.krw_free = 100'000.0;
        std::vector<std::string> markets = { "KRW-BTC" };

        AccountManager manager(initial, markets);

        // 매수: 100,000원으로 BTC 매수
        auto token = manager.reserve("KRW-BTC", 100'000.0);
        assert(token.has_value());

        manager.finalizeFillBuy(token.value(), 100'000.0, 0.001, 100'000'000.0);
        manager.finalizeOrder(std::move(token.value()));

        // 매도: 부분 매도, 유의미한 잔량 남김
        // 0.0009 BTC 매도 → 0.0001 BTC (= 10,000원) 잔량
        Volume sold = 0.0009;
        Amount received = sold * 100'000'000.0;  // 90,000원

        manager.finalizeFillSell("KRW-BTC", sold, received);

        auto budget = manager.getBudget("KRW-BTC");
        Volume remaining = budget->coin_balance;
        Amount remaining_value = remaining * 100'000'000.0;

        std::cout << "  After partial sell:\n";
        std::cout << "    coin_balance=" << remaining << " BTC\n";
        std::cout << "    remaining_value=" << remaining_value << " KRW\n";

        // 핵심 검증: 10,000원 가치의 잔량은 유지되어야 함
        assert(almostEqual(remaining, 0.0001));
        assert(budget->avg_entry_price > 0.0);

        std::cout << "  [PASS] High price coin remainder preserved correctly\n";
    }

    // ============================================================
    // 테스트 17: reserve 입력 검증
    // ============================================================
    void testReserveInputValidation() {
        std::cout << "\n[TEST 17] reserve input validation (zero/negative)\n";

        Account initial;
        initial.krw_free = 100'000.0;
        std::vector<std::string> markets = { "KRW-BTC" };

        AccountManager manager(initial, markets);

        // 0원 예약 시도 → 실패
        auto token_zero = manager.reserve("KRW-BTC", 0.0);
        assert(!token_zero.has_value());

        // 음수 예약 시도 → 실패
        auto token_negative = manager.reserve("KRW-BTC", -100.0);
        assert(!token_negative.has_value());

        // 정상 예약 → 성공
        auto token_valid = manager.reserve("KRW-BTC", 50'000.0);
        assert(token_valid.has_value());

        auto budget = manager.getBudget("KRW-BTC");
        assert(almostEqual(budget->available_krw, 50'000.0));
        assert(almostEqual(budget->reserved_krw, 50'000.0));

        std::cout << "  [PASS] Input validation works correctly\n";
    }

    // ============================================================
    // 테스트 18: finalizeFillBuy 입력 검증
    // ============================================================
    void testFinalizeFillBuyInputValidation() {
        std::cout << "\n[TEST 18] finalizeFillBuy input validation (zero/negative/excess)\n";

        Account initial;
        initial.krw_free = 100'000.0;
        std::vector<std::string> markets = { "KRW-BTC" };

        AccountManager manager(initial, markets);

        // 예약: 50,000 KRW
        auto token = manager.reserve("KRW-BTC", 50'000.0);
        assert(token.has_value());

        auto budget_before = manager.getBudget("KRW-BTC");
        double initial_coin = budget_before->coin_balance;
        double initial_reserved = budget_before->reserved_krw;

        std::cout << "  Before invalid fills: coin=" << initial_coin
                  << ", reserved=" << initial_reserved << "\n";

        // Case 1: 0원 체결 시도 → 무시됨
        manager.finalizeFillBuy(token.value(), 0.0, 0.001, 50'000'000.0);
        auto budget1 = manager.getBudget("KRW-BTC");
        assert(budget1->coin_balance == initial_coin);  // 변화 없음
        std::cout << "  After 0 KRW fill: coin unchanged ✓\n";

        // Case 2: 음수 체결 시도 → 무시됨
        manager.finalizeFillBuy(token.value(), -10'000.0, 0.001, 50'000'000.0);
        auto budget2 = manager.getBudget("KRW-BTC");
        assert(budget2->coin_balance == initial_coin);  // 변화 없음
        std::cout << "  After negative KRW fill: coin unchanged ✓\n";

        // Case 3: 0 코인 체결 시도 → 무시됨
        manager.finalizeFillBuy(token.value(), 10'000.0, 0.0, 50'000'000.0);
        auto budget3 = manager.getBudget("KRW-BTC");
        assert(budget3->coin_balance == initial_coin);  // 변화 없음
        std::cout << "  After 0 coin fill: coin unchanged ✓\n";

        // Case 4: 예약 초과 체결 시도 → clamp됨
        // 예약: 50,000 KRW, 시도: 60,000 KRW → 50,000 KRW만 처리
        manager.finalizeFillBuy(token.value(), 60'000.0, 0.0012, 50'000'000.0);
        auto budget4 = manager.getBudget("KRW-BTC");

        // 50,000 KRW로 clamp되어 처리됨
        assert(budget4->coin_balance > initial_coin);  // 코인 증가
        assert(almostEqual(budget4->reserved_krw, 0.0));  // 예약 전액 소진
        assert(almostEqual(token->consumed(), 50'000.0));  // 50,000원만 소비

        std::cout << "  After excess fill (60k → 50k): coin=" << budget4->coin_balance
                  << ", consumed=" << token->consumed() << " ✓\n";

        manager.finalizeOrder(std::move(token.value()));

        std::cout << "  [PASS] Input validation prevents invalid operations\n";
    }

    // ============================================================
    // 테스트 19: finalizeFillSell 과매도
    // ============================================================
    void testFinalizeFillSellOversell() {
        std::cout << "\n[TEST 18] finalizeFillSell oversell detection and KRW adjustment\n";

        Account initial;
        initial.krw_free = 100'000.0;
        std::vector<std::string> markets = { "KRW-BTC" };

        AccountManager manager(initial, markets);

        // 매수: 0.001 BTC @ 100M = 100,000원
        auto token = manager.reserve("KRW-BTC", 100'000.0);
        manager.finalizeFillBuy(token.value(), 100'000.0, 0.001, 100'000'000.0);
        manager.finalizeOrder(std::move(token.value()));

        auto budget_after_buy = manager.getBudget("KRW-BTC");
        std::cout << "  After buy: coin=" << budget_after_buy->coin_balance << "\n";

        // 과매도 시도: 보유량(0.001 BTC)보다 많이(0.002 BTC) 매도
        // 요청: 0.002 BTC → 200,000 KRW
        // 실제: 0.001 BTC만 보유 → 100,000 KRW만 받아야 함
        manager.finalizeFillSell("KRW-BTC", 0.002, 200'000.0);

        auto budget_after_sell = manager.getBudget("KRW-BTC");
        std::cout << "  After oversell attempt:\n";
        std::cout << "    coin_balance=" << budget_after_sell->coin_balance << "\n";
        std::cout << "    available_krw=" << budget_after_sell->available_krw << "\n";

        // 핵심 검증:
        // 1. coin_balance는 0으로 clamp
        assert(budget_after_sell->coin_balance == 0.0);

        // 2. KRW는 실제 보유량(0.001 BTC)에 대한 금액(100,000원)만 받아야 함
        // 과매도분(0.001 BTC, 100,000원)은 받지 않음
        assert(almostEqual(budget_after_sell->available_krw, 100'000.0, 100.0));

        std::cout << "  [PASS] Oversell detection and KRW adjustment work correctly\n";
    }

    // ============================================================
    // 테스트 20: finalizeFillSell 입력 검증
    // ============================================================
    void testFinalizeFillSellInputValidation() {
        std::cout << "\n[TEST 20] finalizeFillSell input validation (zero/negative)\n";

        Account initial;
        initial.krw_free = 0.0;

        Position btc_pos;
        btc_pos.currency = "BTC";
        btc_pos.free = 0.01;
        btc_pos.avg_buy_price = 50'000'000.0;
        initial.positions.push_back(btc_pos);

        std::vector<std::string> markets = { "KRW-BTC" };
        AccountManager manager(initial, markets);

        auto budget_before = manager.getBudget("KRW-BTC");
        double initial_coin = budget_before->coin_balance;
        double initial_krw = budget_before->available_krw;

        std::cout << "  Before invalid sells: coin=" << initial_coin
                  << ", krw=" << initial_krw << "\n";

        // Case 1: 0 코인 매도 시도 → 무시됨
        manager.finalizeFillSell("KRW-BTC", 0.0, 100'000.0);
        auto budget1 = manager.getBudget("KRW-BTC");
        assert(almostEqual(budget1->coin_balance, initial_coin));  // 변화 없음
        assert(almostEqual(budget1->available_krw, initial_krw));  // 변화 없음
        std::cout << "  After 0 coin sell: unchanged ✓\n";

        // Case 2: 음수 코인 매도 시도 → 무시됨
        manager.finalizeFillSell("KRW-BTC", -0.001, 100'000.0);
        auto budget2 = manager.getBudget("KRW-BTC");
        assert(almostEqual(budget2->coin_balance, initial_coin));  // 변화 없음
        std::cout << "  After negative coin sell: unchanged ✓\n";

        // Case 3: 0 KRW 수령 시도 → 무시됨
        manager.finalizeFillSell("KRW-BTC", 0.001, 0.0);
        auto budget3 = manager.getBudget("KRW-BTC");
        assert(almostEqual(budget3->coin_balance, initial_coin));  // 변화 없음
        std::cout << "  After 0 KRW received sell: unchanged ✓\n";

        // Case 4: 음수 KRW 수령 시도 → 무시됨
        manager.finalizeFillSell("KRW-BTC", 0.001, -50'000.0);
        auto budget4 = manager.getBudget("KRW-BTC");
        assert(almostEqual(budget4->coin_balance, initial_coin));  // 변화 없음
        std::cout << "  After negative KRW received sell: unchanged ✓\n";

        std::cout << "  [PASS] Input validation prevents invalid operations\n";
    }

    // ============================================================
    // 테스트 21: syncWithAccount 포지션 사라짐
    // ============================================================
    void testSyncWithAccountPositionDisappears() {
        std::cout << "\n[TEST 21] syncWithAccount - position disappears (external trade)\n";

        Account initial;
        initial.krw_free = 1'000'000.0;
        std::vector<std::string> markets = { "KRW-BTC", "KRW-ETH" };

        AccountManager manager(initial, markets);

        // 초기 매수: BTC 0.01개
        auto token = manager.reserve("KRW-BTC", 500'000.0);
        manager.finalizeFillBuy(token.value(), 500'000.0, 0.01, 50'000'000.0);
        manager.finalizeOrder(std::move(token.value()));

        auto btc_before = manager.getBudget("KRW-BTC");
        std::cout << "  Before external trade:\n";
        std::cout << "    KRW-BTC: coin=" << btc_before->coin_balance
                  << ", krw=" << btc_before->available_krw << "\n";

        // 외부 거래: Upbit 앱에서 BTC 전량 매도 시뮬레이션
        Account updated;
        updated.krw_free = 1'000'000.0;  // BTC 매도 후 원금 복구
        // positions = [] (BTC 없음)

        // 동기화
        manager.syncWithAccount(updated);

        auto btc_after = manager.getBudget("KRW-BTC");
        auto eth_after = manager.getBudget("KRW-ETH");

        std::cout << "  After external trade (BTC sold):\n";
        std::cout << "    KRW-BTC: coin=" << btc_after->coin_balance
                  << ", krw=" << btc_after->available_krw << "\n";
        std::cout << "    KRW-ETH: coin=" << eth_after->coin_balance
                  << ", krw=" << eth_after->available_krw << "\n";

        // 핵심 검증: BTC 포지션 사라짐 → coin_balance = 0
        assert(btc_after->coin_balance == 0.0);
        assert(btc_after->avg_entry_price == 0.0);
        assert(btc_after->available_krw > 0.0);
        assert(eth_after->available_krw > 0.0);

        // 전체 KRW 합계 검증
        double total_krw = btc_after->available_krw + eth_after->available_krw;
        assert(almostEqual(total_krw, 1'000'000.0, 100.0));

        std::cout << "  [PASS] Position disappears correctly handled\n";
    }

    // ============================================================
    // 테스트 22: Thread-safety (멀티스레드 예약/해제)
    // ============================================================
    void testThreadSafety() {
        std::cout << "\n[TEST 22] Thread-safety (multi-threaded reserve/release)\n";

        Account account;
        account.krw_free = 10'000'000.0;  // 큰 금액
        std::vector<std::string> markets = { "KRW-BTC" };

        AccountManager manager(account, markets);

        std::atomic<int> success_count{ 0 };
        std::atomic<int> failure_count{ 0 };

        const int num_threads = 10;
        const int ops_per_thread = 100;

        auto worker = [&]() {
            for (int i = 0; i < ops_per_thread; ++i) {
                // 10,000 KRW 예약 시도
                auto token = manager.reserve("KRW-BTC", 10'000.0);

                if (token.has_value()) {
                    success_count.fetch_add(1, std::memory_order_relaxed);

                    // 즉시 해제
                    manager.release(std::move(token.value()));
                }
                else {
                    failure_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
            };

        std::vector<std::thread> threads;
        threads.reserve(num_threads);

        auto start = std::chrono::steady_clock::now();

        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back(worker);
        }

        for (auto& t : threads) {
            t.join();
        }

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "  Success: " << success_count.load() << "\n";
        std::cout << "  Failure: " << failure_count.load() << "\n";
        std::cout << "  Duration: " << duration.count() << "ms\n";

        // 최종 상태: 모든 예약 해제되어 원래대로
        auto budget = manager.getBudget("KRW-BTC");
        assert(almostEqual(budget->available_krw, 10'000'000.0, 1000.0));
        assert(budget->reserved_krw == 0.0);

        std::cout << "  Final available_krw: " << budget->available_krw << "\n";
        std::cout << "  [PASS] Thread-safety verified (no corruption)\n";
    }

    // ============================================================
    // 테스트 23: 통계 추적
    // ============================================================
    void testStatistics() {
        std::cout << "\n[TEST 23] Statistics tracking\n";

        Account account;
        account.krw_free = 100'000.0;
        std::vector<std::string> markets = { "KRW-BTC" };

        AccountManager manager(account, markets);

        // 예약 2회
        auto token1 = manager.reserve("KRW-BTC", 30'000.0);
        auto token2 = manager.reserve("KRW-BTC", 20'000.0);

        assert(manager.stats().total_reserves.load() == 2);

        // 해제 1회
        manager.release(std::move(token1.value()));
        assert(manager.stats().total_releases.load() == 1);

        // 매수 체결
        manager.finalizeFillBuy(token2.value(), 20'000.0, 0.0004, 50'000'000.0);
        assert(manager.stats().total_fills_buy.load() == 1);

        manager.finalizeOrder(std::move(token2.value()));

        // 매도 체결
        manager.finalizeFillSell("KRW-BTC", 0.0004, 20'500.0);
        assert(manager.stats().total_fills_sell.load() == 1);

        // 예약 실패 (잔액 부족)
        auto token_fail = manager.reserve("KRW-BTC", 200'000.0);
        assert(!token_fail.has_value());
        assert(manager.stats().reserve_failures.load() >= 1);

        std::cout << "  total_reserves: " << manager.stats().total_reserves.load() << "\n";
        std::cout << "  total_releases: " << manager.stats().total_releases.load() << "\n";
        std::cout << "  total_fills_buy: " << manager.stats().total_fills_buy.load() << "\n";
        std::cout << "  total_fills_sell: " << manager.stats().total_fills_sell.load() << "\n";
        std::cout << "  reserve_failures: " << manager.stats().reserve_failures.load() << "\n";

        std::cout << "  [PASS] Statistics correctly tracked\n";
    }

    // ============================================================
    // 테스트 실행
    // ============================================================
    void runAllTests() {
        std::cout << "\n========================================\n";
        std::cout << "  AccountManager Unified Tests\n";
        std::cout << "========================================\n";

        testInitializationKrwOnly();
        testInitializationWithPositions();
        testInitializationDustHandling();
        testReserveRelease();
        testReserveFailures();
        testTokenRAII();
        testFinalizeFillBuy();
        testPartialFillBuyWithAvgPrice();
        testFinalizeFillSell();
        testPartialFillSell();
        testSyncWithAccountStateModel();
        testSyncWithMultiplePositions();
        testEquityAndROI();
        testSyncWithAccountDustHandling();
        testFinalizeFillSellLowPriceCoin();
        testFinalizeFillSellHighPriceCoin();
        testReserveInputValidation();
        testFinalizeFillBuyInputValidation();
        testFinalizeFillSellOversell();
        testFinalizeFillSellInputValidation();
        testSyncWithAccountPositionDisappears();
        testThreadSafety();
        testStatistics();

        std::cout << "\n========================================\n";
        std::cout << "  All tests PASSED!\n";
        std::cout << "========================================\n";
    }

} // namespace test

int main() {
    try {
        test::runAllTests();
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "\n[FATAL ERROR] " << e.what() << "\n";
        return 1;
    }
}
