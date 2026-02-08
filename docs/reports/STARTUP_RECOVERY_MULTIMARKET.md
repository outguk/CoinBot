# 멀티마켓 환경에서의 StartupRecovery 설계

> **상태**: 설계 제안 (ROADMAP Phase 1에 추가 필요)

## 문제 정의

현재 `StartupRecovery`는 단일 마켓/단일 전략만 처리하도록 설계되어 있습니다. ROADMAP Phase 1의 멀티마켓 환경에서는 다음 문제가 발생합니다:

1. **각 마켓별 포지션 복구 필요**: 여러 마켓에서 동시에 거래하므로 각 마켓별로 포지션 상태를 복구해야 함
2. **AccountManager 동기화 필요**: 실제 계좌 잔고와 각 마켓의 KRW 할당을 동기화해야 함
3. **미체결 주문 처리 정책**: 마켓별로 독립적으로 미체결 주문을 취소/복원해야 함

## 재시작 시나리오

### 비정상 종료 전 상태

```
계좌 상태:
  - 총 KRW 잔고: 1,000,000원
  - KRW free: 400,000원
  - KRW locked: 300,000원 (KRW-ETH 매수 주문)

마켓별 상태:
  KRW-BTC:
    - 전략 상태: InPosition
    - 포지션: 0.01 BTC (평단 100,000,000원)
    - 할당 KRW: 300,000원 (available: 300,000원)

  KRW-ETH:
    - 전략 상태: PendingEntry
    - 미체결 주문: 매수 300,000원 (locked)
    - 할당 KRW: 300,000원 (reserved: 300,000원, available: 0원)

  KRW-XRP:
    - 전략 상태: Flat
    - 할당 KRW: 300,000원 (available: 300,000원)

  예비금: 100,000원
```

### 재시작 후 복구 플로우

#### 1단계: 전체 계좌 조회

```cpp
// MarketManager 초기화 전
auto account_result = api.getMyAccount();
const core::Account& account = std::get<core::Account>(account_result);

// account 내용:
// - krw_free: 400,000
// - positions: [
//     {currency: "BTC", free: 0.01, avg_buy_price: 100,000,000},
//     {currency: "ETH", free: 0.0, avg_buy_price: 0}  // 미체결이므로 0
//   ]
```

#### 2단계: AccountManager 초기화 및 동기화

```cpp
// 잘못된 방법 (현재):
AccountManager account_mgr(1'000'000, {"KRW-BTC", "KRW-ETH", "KRW-XRP"}, 0.1);
// → 각 마켓에 300,000원씩 할당
// → 하지만 실제로는 KRW-ETH에 300,000원이 locked!

// 올바른 방법 (제안):
AccountManager account_mgr(1'000'000, {"KRW-BTC", "KRW-ETH", "KRW-XRP"}, 0.1);
account_mgr.syncWithAccount(account);  // ← 실제 잔고로 동기화

// syncWithAccount 결과:
// - KRW-BTC: coin_balance = 0.01, avg_entry_price = 100,000,000
// - KRW-ETH: coin_balance = 0.0
// - KRW-XRP: coin_balance = 0.0
// - 총 free KRW = 400,000 → 마켓별 재분배 필요
```

#### 3단계: 각 마켓별 StartupRecovery

```cpp
for (const auto& [market, context] : market_contexts_) {
    // 미체결 주문 조회 및 처리
    StartupRecovery::Options opt;
    opt.bot_identifier_prefix = "rsi_mean_reversion:" + market + ":";

    StartupRecovery::run(api, market, opt, context.strategy);

    // StartupRecovery::run() 내부:
    // 1. cancelBotOpenOrders(market)
    //    → KRW-ETH의 300,000원 주문 취소
    //    → Upbit에서 locked → free로 전환
    // 2. buildPositionSnapshot(market)
    //    → KRW-BTC: {coin: 0.01, avg: 100M}
    //    → KRW-ETH: {coin: 0.0, avg: 0}
    //    → KRW-XRP: {coin: 0.0, avg: 0}
    // 3. strategy.syncOnStart(snapshot)
    //    → KRW-BTC: Flat → InPosition 전환
    //    → KRW-ETH: PendingEntry → Flat 전환 (주문 취소됨)
    //    → KRW-XRP: Flat 유지
}
```

#### 4단계: AccountManager 최종 동기화

```cpp
// 미체결 주문 취소 후 잔고 재조회
auto account_after = api.getMyAccount();
account_mgr.syncWithAccount(account_after);

// 최종 상태:
// - 총 KRW free: 700,000원 (400,000 + 300,000 취소 복구)
// - KRW-BTC: coin_balance = 0.01
// - 각 마켓에 KRW 재분배
```

## 제안 설계

### Option 1: MarketManager에서 통합 처리

```cpp
class MarketManager {
public:
    MarketManager(
        UpbitExchangeRestClient& api,
        AccountManager& account_mgr,
        const std::vector<std::string>& markets
    ) {
        // 1. 초기 계좌 동기화
        syncAccountWithExchange(api, account_mgr);

        // 2. 각 마켓 생성 및 복구
        for (const auto& market : markets) {
            auto& context = createMarketContext(market);
            recoverMarketState(api, context);
        }

        // 3. 최종 동기화 (미체결 주문 취소 후)
        syncAccountWithExchange(api, account_mgr);
    }

private:
    void syncAccountWithExchange(
        UpbitExchangeRestClient& api,
        AccountManager& account_mgr
    ) {
        auto result = api.getMyAccount();
        if (std::holds_alternative<core::Account>(result)) {
            account_mgr.syncWithAccount(std::get<core::Account>(result));
        }
    }

    void recoverMarketState(
        UpbitExchangeRestClient& api,
        MarketContext& context
    ) {
        StartupRecovery::Options opt;
        opt.bot_identifier_prefix = buildBotPrefix(context.market);

        StartupRecovery::run(api, context.market, opt, context.strategy);
    }
};
```

### Option 2: StartupRecovery를 멀티마켓용으로 확장

```cpp
class StartupRecovery {
public:
    // 기존: 단일 마켓
    template <class StrategyT>
    static void run(
        UpbitExchangeRestClient& api,
        std::string_view market,
        const Options& opt,
        StrategyT& strategy
    );

    // 신규: 멀티마켓
    static void runMultiMarket(
        UpbitExchangeRestClient& api,
        AccountManager& account_mgr,
        const std::map<std::string, MarketContext>& contexts,
        const Options& opt
    ) {
        // 1. 초기 계좌 동기화
        auto account = api.getMyAccount();
        account_mgr.syncWithAccount(std::get<core::Account>(account));

        // 2. 각 마켓별 복구
        for (const auto& [market, context] : contexts) {
            Options market_opt = opt;
            market_opt.bot_identifier_prefix += market + ":";

            run(api, market, market_opt, context.strategy);
        }

        // 3. 최종 계좌 동기화
        auto account_after = api.getMyAccount();
        account_mgr.syncWithAccount(std::get<core::Account>(account_after));
    }
};
```

## 미체결 주문 정책별 처리

### 정책 1: Cancel (현재 구현)

```cpp
// 모든 미체결 주문 취소
cancelBotOpenOrders(api, market, opt);

// AccountManager 영향:
// - reserved_krw가 복구되지 않음 (주문이 취소되었으므로)
// - syncWithAccount()로 실제 잔고 반영
```

### 정책 2: KeepOpen (미래 확장)

```cpp
// 미체결 주문 유지
auto open_orders = api.getOpenOrders(market);

for (const auto& order : open_orders) {
    if (isBotOrder(order)) {
        // ReservationToken 재생성
        auto token = account_mgr.reserve(market, order.remaining_krw);

        // OrderStore에 복원
        order_store.restore(order);

        // 전략 상태 복원
        strategy.restoreOrder(order, std::move(token));
    }
}
```

**문제점**: ReservationToken 재생성이 복잡
- 토큰은 RAII, move-only
- 장기 보관 불가능
- 대안: OrderStore에 예약 금액만 저장, 필요 시 재생성

## ROADMAP Phase 1 수정 제안

### 1.6 MarketContext + 통합 단계에 추가

```
├─ 1.6 MarketContext + 통합 (3일)  ← 1일 추가
│   ├─ 기존 EngineRunner 로직 → MarketManager로 이전
│   ├─ main() 진입점 수정
│   ├─ 설정 파일 (config/markets.json) 로딩
│   ├─ 멀티마켓 StartupRecovery 통합  ← 신규
│   │   ├─ MarketManager 초기화 시 각 마켓별 복구
│   │   ├─ AccountManager.syncWithAccount() 연동
│   │   └─ 미체결 주문 정책 적용 (Cancel/KeepOpen)
│   └─ 검증: 멀티마켓 End-to-End 테스트
│       ├─ 재시작 시나리오 테스트 추가  ← 신규
│       └─ 각 마켓별 포지션 복구 검증  ← 신규
```

### 완료 기준 추가

```
Phase 1 완료 기준:
  ✓ 1~5개 마켓 동시 거래 가능
  ✓ 마켓별 독립 KRW 할당
  ✓ WS 이벤트 올바른 마켓으로 라우팅
  ✓ 부분 체결 정확히 누적
  ✓ 부하 테스트 통과 (CPU < 50%, 메모리 안정)
  ✓ 재시작 시 각 마켓별 포지션 복구  ← 신규
  ✓ AccountManager와 실제 계좌 동기화  ← 신규
```

## 테스트 시나리오

### 시나리오 1: 정상 종료 후 재시작

```cpp
TEST(MultiMarketStartup, NormalShutdown) {
    // 1. 3개 마켓 실행
    MarketManager mgr(api, account_mgr, {"KRW-BTC", "KRW-ETH", "KRW-XRP"});

    // 2. KRW-BTC에서 매수 체결 완료 (InPosition)
    // 3. KRW-ETH에서 매수 주문 대기 (PendingEntry)
    // 4. KRW-XRP는 대기 (Flat)

    // 5. 정상 종료
    mgr.shutdown();

    // 6. 재시작
    MarketManager mgr2(api, account_mgr, {"KRW-BTC", "KRW-ETH", "KRW-XRP"});

    // 검증:
    // - KRW-BTC: InPosition 복구, 0.01 BTC 보유
    // - KRW-ETH: Flat (미체결 주문 취소됨)
    // - KRW-XRP: Flat
    // - AccountManager: 실제 잔고 반영
}
```

### 시나리오 2: 비정상 종료 후 재시작

```cpp
TEST(MultiMarketStartup, CrashRecovery) {
    // 1. 3개 마켓 실행 중 크래시
    // 2. 재시작
    MarketManager mgr(api, account_mgr, {"KRW-BTC", "KRW-ETH", "KRW-XRP"});

    // 검증:
    // - API로 실제 잔고 조회
    // - 각 마켓별 포지션 복구
    // - AccountManager 동기화
    // - 미체결 주문 처리 (취소)
}
```

## 참고 사항

- **Phase 2 (PostgreSQL)** 이후: DB에서 전략 스냅샷 로드 가능
- **Phase 3 (AWS 배포)** 이후: systemd 자동 재시작 시 복구 필수
- **KeepOpen 정책** 구현 시: ReservationToken 영속화 또는 재생성 메커니즘 필요

# 파일 생성 규칙
- 모든 텍스트 파일은 한글이 깨지지 않도록 저장
- 오버코딩 금지
- 주석으로 왜 필요한지, 기능과 동작을 간단히 설명할 것
