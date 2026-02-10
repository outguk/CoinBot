# CoinBot 로드맵 (미래 계획)

> ⚠️ **주의**: 이 문서는 미래 개선 계획입니다. 현재 시스템 아키텍처는 [ARCHITECTURE.md](ARCHITECTURE.md)를 참조하세요.
>
> **구현 현황**: [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md)에서 진행 상황을 확인할 수 있습니다.
>
> **최종 검토**: 2026-02-09 (코드베이스 기준 업데이트, [ROADMAP_MULTIMARKET_REVIEW](reports/ROADMAP_MULTIMARKET_REVIEW.md) 반영)

---

## 개요

이 문서는 CoinBot을 다음과 같이 확장하기 위한 시스템 아키텍처 계획입니다:
1. **멀티마켓 지원**: 1~5개 마켓별 독립 전략 실행 및 비중 배분
2. **24시간 운영**: AWS 클라우드 환경에서 무중단 거래
3. **거래 기록**: PostgreSQL 기반 거래 내역 저장 및 분석

---

## 설계 이슈 분석 및 해결책

### Issue 1: OrderEngine 공유 vs 마켓별 엔진 설계 충돌

#### 문제 분석

현재 RealOrderEngine은 다음 특성을 가짐:
- `bindToCurrentThread()`: 단일 스레드 소유권 강제
- `assertOwner_()`: 다른 스레드 호출 시 `std::terminate()`
- `PrivateOrderApi& api_`: REST API 클라이언트 참조

**충돌 지점**: 마켓별 스레드에서 독립적으로 주문을 제출하려면 엔진이 마켓별로 있어야 하나, REST API 클라이언트와 OrderStore는 공유가 효율적임.

#### 대안 비교

| 대안 | 설명 | 장점 | 단점 |
|------|------|------|------|
| **A. 완전 공유 엔진** | 단일 엔진, 모든 마켓 스레드가 락으로 접근 | 구현 단순 | 병목, 단일 스레드 소유권 위반 |
| **B. 완전 분리 엔진** | 마켓별 독립 엔진 인스턴스 | 스레드 안전 | OrderStore/API 중복, 메모리 낭비 |
| **C. 계층 분리 (선택)** | 공유 계층(API, Store) + 마켓별 Facade | 효율+안전 | 설계 복잡도 증가 |

#### 선택: C. 계층 분리 아키텍처

```
┌─────────────────────────────────────────────────────────────────┐
│                    Shared Layer (Thread-Safe)                   │
├─────────────────────────────────────────────────────────────────┤
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐ │
│  │ SharedOrderApi  │  │   OrderStore    │  │ AccountManager  │ │
│  │ (mutex 직렬화)   │  │ (shared_mutex)  │  │ (shared_mutex)  │ │
│  └────────┬────────┘  └────────┬────────┘  └────────┬────────┘ │
│           │                    │                    │          │
└───────────┼────────────────────┼────────────────────┼──────────┘
            │                    │                    │
            │    thread-safe API 호출만 허용 (외부 락 금지)
            │                    │                    │
         ┌──┴────────────────────┼────────────────────┘
         │                       │
         ▼                       ▼
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│ MarketEngine    │     │ MarketEngine    │     │ MarketEngine    │
│ (KRW-BTC)       │     │ (KRW-ETH)       │     │ (KRW-XRP)       │
│                 │     │                 │     │                 │
│ - 로컬 상태만    │     │ - 로컬 상태만     │     │ - 로컬 상태만    │
│   직접 변경      │     │   직접 변경       │     │   직접 변경     │
│ - 공유 자원은    │     │ - 공유 자원은     │     │ - 공유 자원은    │
│   API 호출만     │     │   API 호출만     │     │   API 호출만    │
└─────────────────┘     └─────────────────┘     └─────────────────┘
```

**선택 이유**:
1. **스레드 소유권 유지**: 각 MarketEngine은 자신의 스레드에서만 로컬 상태 변경
2. **공유 자원 효율**: REST 연결, OrderStore는 하나만 유지
3. **락 책임 명확화**: 공유 자원은 내부적으로 thread-safe, 외부에서 락 금지

#### 구현 설계

**파일**: `src/api/upbit/SharedOrderApi.h` (✅ 완료 - 2026-01-29)

```cpp
// Thread-safe REST API 래퍼 (내부 mutex 직렬화, 호출자는 락 불필요)
// 설계 근거: HTTP/1.1 단일 연결 + Rate Limit(초당 10회) → 직렬화가 유리
// 확장 포인트: HTTP/2 시 mutex 제거, 고빈도 시 연결 풀 + 세마포어
class SharedOrderApi : public IOrderApi {
public:
    // UpbitExchangeRestClient의 unique ownership을 받음
    explicit SharedOrderApi(std::unique_ptr<api::rest::UpbitExchangeRestClient> client);

    // IOrderApi 구현 (variant 반환)
    std::variant<core::Account, api::rest::RestError>
        getMyAccount() override;

    std::variant<std::vector<core::Order>, api::rest::RestError>
        getOpenOrders(std::string_view market) override;

    std::variant<bool, api::rest::RestError>
        cancelOrder(const std::optional<std::string>& uuid,
                    const std::optional<std::string>& identifier) override;

    std::variant<std::string, api::rest::RestError>
        postOrder(const core::OrderRequest& req) override;

private:
    std::unique_ptr<api::rest::UpbitExchangeRestClient> client_;
    mutable std::mutex mtx_;  // 멀티스레드 동시 호출 직렬화
};
```

**파일**: `src/engine/MarketEngine.h` (✅ 완료 - 2026-02-08)

```cpp
// 마켓별 독립 주문 엔진 - 단일 스레드 전용 (bindToCurrentThread로 소유권 설정)
//
// 스레드 안전성 규칙:
//   1. 로컬 상태는 소유 스레드에서만 읽기/쓰기 (owner_thread_ 검증)
//   2. 공유 자원(api, store, account_mgr)은 thread-safe API 호출로만 접근 (외부 락 금지)
//   3. 주문 흐름: reserve → submitOrder → finalizeFill → finalizeOrder (각 단계 독립 락)
//
// 전량 거래 모델:
//   - 마켓당 최대 1개 매수 또는 1개 매도 주문 (active_buy_token_ / active_sell_order_id_)
//   - BUY/SELL 동시 제출 불가 (반대 포지션 방지)
//
// 외부 주문 거부:
//   - OrderStore에 없는 주문의 체결 이벤트는 무시 (로그만 출력)
class MarketEngine {
public:
    MarketEngine(std::string market,
                 IOrderApi& api,
                 OrderStore& store,
                 AccountManager& account_mgr);

    void bindToCurrentThread();

    EngineResult submit(const core::OrderRequest& req);
    void onMyTrade(const core::MyTrade& trade);
    void onOrderStatus(std::string_view order_id, core::OrderStatus s);
    void onOrderSnapshot(const core::Order& snapshot);
    std::vector<EngineEvent> pollEvents();
    std::optional<core::Order> get(std::string_view order_id) const;

private:
    // 주문 검증, KRW 예약 금액 계산, 토큰 정리 등
    static bool validateRequest(const core::OrderRequest& req, std::string& reason);
    static core::Amount computeReserveAmount(const core::OrderRequest& req);
    void finalizeBuyToken_(std::string_view order_id);
};
```

---

### Issue 1.5: 멀티마켓 StartupRecovery 통합

#### 문제 분석

현재 `StartupRecovery`는 단일 마켓/단일 전략만 처리하도록 설계되어 있습니다. 멀티마켓 환경에서는 다음 문제가 발생합니다:

1. **각 마켓별 포지션 복구 필요**: 프로그램 재시작 시 각 마켓별로 포지션 상태를 복구해야 함
2. **AccountManager 동기화 필요**: 실제 계좌 잔고와 각 마켓의 KRW 할당을 동기화해야 함
3. **미체결 주문 처리**: 마켓별로 독립적으로 미체결 주문을 취소/복원해야 함

**재시작 시나리오 예시**:
```
종료 전:
  KRW-BTC: InPosition (0.01 BTC 보유, 평단 100M원)
  KRW-ETH: PendingEntry (300,000원 주문 대기)
  KRW-XRP: Flat (포지션 없음)

재시작 시 필요:
  1. 실제 계좌 잔고 조회 → AccountManager 동기화
  2. 각 마켓별 StartupRecovery 실행
  3. 미체결 주문 취소 후 최종 동기화
```

#### 대안 비교

| 대안 | 설명 | 장점 | 단점 |
|------|------|------|------|
| **A. StartupRecovery 확장** | 멀티마켓용 메서드 추가 | 중앙 집중식 | StartupRecovery가 너무 많은 책임 |
| **B. MarketEngineManager 통합 (선택)** | MarketEngineManager 초기화 시 처리 | 책임 명확, 확장 용이 | MarketEngineManager 복잡도 증가 |

#### 선택: B. MarketEngineManager에서 통합 처리

**선택 이유**:
1. **책임 명확**: MarketEngineManager가 멀티마켓 생명주기 관리
2. **기존 코드 재사용**: StartupRecovery는 단일 마켓 처리 유지
3. **확장성**: 추가 초기화 로직을 MarketEngineManager에 집중
4. **점진 전환**: 기존 StartupRecovery 인터페이스 유지 + `IOrderApi` 오버로드 추가

#### 구현 설계

**파일**: `src/app/MarketEngineManager.h` (신규)

**목적**: 멀티마켓 환경에서 각 마켓별 엔진, 전략, 워커 스레드를 관리하는 중앙 코디네이터

**필수 요구사항**:

1. **MarketContext 관리**:
   - 각 마켓별 독립적인 컨텍스트 생성 및 생명주기 관리
   - MarketContext 구성 요소:
     - `std::string market` (예: "KRW-BTC")
     - `MarketEngine engine`
     - `RsiMeanReversionStrategy strategy` (또는 다른 전략)
     - `BlockingQueue<MarketEvent> event_queue`
     - `std::thread worker_thread`
     - `std::atomic<bool> stop_flag`

2. **초기화 흐름** (생성자에서 순차 실행):
   ```
   Step 1: syncAccountWithExchange (1차)
       → api.getMyAccount() 호출
       → AccountManager.syncWithAccount(account)
       → 실제 계좌 잔고 및 코인 보유량 동기화

       [실패 처리]
       - RestError 발생 시: 최대 3회 재시도 (1초 간격)
       - 3회 실패 시: 생성자에서 예외 throw
       - 이유: 계좌 동기화 없이 시작 시 KRW 할당 불가능 (치명적 오류)

   Step 2: 각 마켓별 초기화
       for (market : markets)
           ├─ MarketContext 생성
           │   ├─ MarketEngine 생성 (api, store, account_mgr 참조)
           │   ├─ Strategy 생성
           │   └─ BlockingQueue 생성
           │
           ├─ recoverMarketState(context)
           │   ├─ StartupRecovery::run(market, strategy) 호출
           │   │   ├─ cancelBotOpenOrders(market) - 미체결 주문 취소
           │   │   │   [실패 처리] 경고 로그만 출력, 진행 계속
           │   │   ├─ buildPositionSnapshot(market) - 코인 보유량 조회
           │   │   │   [실패 처리] 빈 스냅샷으로 대체 (Flat 상태 가정)
           │   │   └─ strategy.syncOnStart(snapshot) - 전략 상태 복구
           │   │
           │   └─ MarketEngine.bindToCurrentThread() (워커 스레드에서)
           │
           └─ 워커 스레드 시작
               └─ marketWorkerLoop(context)

   Step 3: syncAccountWithExchange (2차)
       → 미체결 주문 취소 후 최종 잔고 동기화

       [실패 처리]
       - RestError 발생 시: 경고 로그만 출력
       - 이유: Step 1에서 이미 동기화됨, Step 2의 취소 작업이 일부 실패해도 운영 가능
               - 권장: 첫 거래 전 재조회로 보정
    ```

3. **워커 스레드 관리**:
   - 각 마켓별 독립 스레드 실행
   - marketWorkerLoop(MarketContext&):
     - event_queue에서 이벤트 대기 (BlockingQueue::pop)
     - MarketEvent 처리 (candle, ticker, myOrder 등)
     - Strategy 업데이트 및 결정
     - MarketEngine.submit() 호출
     - EngineEvent 폴링 및 Strategy 알림
   - stop_flag 확인하여 graceful shutdown 지원

4. **이벤트 라우팅**:
   - EventRouter를 통해 마켓별 event_queue에 이벤트 전달
   - 마켓 코드 기준으로 해당 MarketContext 찾기
   - 마켓 코드 불일치 시 로그 출력 후 무시

5. **StartupRecovery 통합**:
   - 기존 StartupRecovery::run() 템플릿 메서드 재사용
   - 마켓별로 독립적으로 실행
   - Cancel 정책: 미체결 주문 모두 취소 (서버 모드 기본값)
   - KeepOpen 정책: 미체결 주문 유지 (선택적, 설정에서 지정)
     - KeepOpen 구현 시 onOrderSnapshot() 호출은 반드시 워커 스레드에서 수행
     - (MarketEngine::assertOwner_() 조건: bindToCurrentThread() 이후에만 엔진 메서드 허용)
   - **구현 전제조건**: StartupRecovery가 현재 UpbitExchangeRestClient&를 직접 받음
     → Phase 1.5 구현 전 IOrderApi& 시그니처로 변경 필요 (cancelBotOpenOrders, buildPositionSnapshot)

6. **스레드 안전성**:
   - 각 MarketContext는 독립적 (락 불필요)
   - 공유 자원(SharedOrderApi, OrderStore, AccountManager)은 내부 락으로 보호
   - stop_flag는 atomic<bool>

7. **Graceful Shutdown 지원**:
   - stop() 메서드:
     - 모든 context.stop_flag = true 설정
     - 워커 루프는 pop_for(100ms) + stop_flag 확인 패턴으로 종료 감지
       (BlockingQueue에 close() API 불필요, pop_for로 충분)
     - 워커 스레드 join (타임아웃: 30초)
     - 타임아웃 시 경고 로그 후 진행

**인터페이스 (최소)**:
```cpp
class MarketEngineManager {
private:
    // 중첩 타입 전방 선언: private 메서드 시그니처에서 먼저 사용
    struct MarketContext;

public:
    // SharedOrderApi(IOrderApi 구현체)를 단일 진입점으로 사용
    MarketEngineManager(
        SharedOrderApi& api,
        AccountManager& account_mgr,
        const std::vector<std::string>& markets
    );

    ~MarketEngineManager();  // stop() 호출

    // EventRouter에서 호출
    void routeMarketData(std::string_view market, /* MarketData 객체 */);
    void routeMyOrder(std::string_view market, /* MyOrder 객체 */);

    // Graceful shutdown
    void stop();

    // 상태 조회
    size_t getMarketCount() const;
    std::vector<std::string> getMarkets() const;

private:
    void syncAccountWithExchange();
    void recoverMarketState(MarketContext& context);  // 내부에서 StartupRecovery 호출
    void marketWorkerLoop(MarketContext& context);

    struct MarketContext {
        std::string market;
        std::unique_ptr<MarketEngine> engine;
        std::unique_ptr<RsiMeanReversionStrategy> strategy;
        BlockingQueue<MarketEvent> event_queue{5000};  // drop-oldest
        std::thread worker_thread;
        std::atomic<bool> stop_flag{false};
    };

    std::map<std::string, MarketContext> contexts_;
    SharedOrderApi& api_;              // 공유 주문 API (복구 포함)
    AccountManager& account_mgr_;      // 공유 계좌 관리자
    // ... 기타 공유 자원 참조
};
```

**주의**: main()에서 생성 시:
```cpp
// 1. UpbitExchangeRestClient 생성 (unique_ptr)
auto rest_client = std::make_unique<UpbitExchangeRestClient>(...);

// 2. SharedOrderApi로 감싸기 (unique_ptr 이동)
auto shared_api = std::make_shared<SharedOrderApi>(std::move(rest_client));

// 3. MarketEngineManager 생성 (SharedOrderApi& 참조)
MarketEngineManager manager(*shared_api, account_mgr, markets);
```

**초기화 플로우**:

```
MarketEngineManager 생성자:
  │
  ├─ 1. syncAccountWithExchange(api, account_mgr)
  │   └─ AccountManager.syncWithAccount(api.getMyAccount())
  │      → 실제 계좌 잔고 및 코인 보유량 동기화
  │
  ├─ 2. 각 마켓별 MarketContext 생성
  │   └─ for (market : markets)
  │       ├─ MarketContext 생성 (strategy, engine, queue)
  │       └─ recoverMarketState(context)
  │           ├─ StartupRecovery::run(market, strategy)
  │           │   ├─ cancelBotOpenOrders(market)
  │           │   │   → 미체결 주문 취소 (Cancel 정책)
  │           │   ├─ buildPositionSnapshot(market)
  │           │   │   → API에서 코인 보유량 조회
  │           │   └─ strategy.syncOnStart(snapshot)
  │           │       → KRW-BTC: InPosition 복구
  │           │       → KRW-ETH: Flat (주문 취소됨)
  │           │       → KRW-XRP: Flat 유지
  │
  └─ 3. syncAccountWithExchange(api, account_mgr)
      └─ 미체결 주문 취소 후 최종 잔고 동기화
```

**StartupRecovery 인터페이스 정리 (호환 유지)**:
```cpp
// 기존 인터페이스 (하위 호환)
template <class StrategyT>
static void run(
    api::rest::UpbitExchangeRestClient& api,
    std::string_view market,
    const Options& opt,
    StrategyT& strategy
);

// 신규 오버로드 (Phase 1.5): MarketEngineManager/SharedOrderApi와 정합
template <class StrategyT>
static void run(
    api::upbit::IOrderApi& api,
    std::string_view market,
    const Options& opt,
    StrategyT& strategy
);
```

`MarketEngineManager`는 `IOrderApi` 오버로드를 사용하고, 기존 `UpbitExchangeRestClient` 버전은 레거시 경로에서 유지합니다.

**상세 설계**: [reports/STARTUP_RECOVERY_MULTIMARKET.md](reports/STARTUP_RECOVERY_MULTIMARKET.md)

---

### Issue 2: Account 동시 수정 정합성 문제

#### 문제 분석

**시나리오**:
```
시점 T0: KRW 잔고 = 1,000,000원

Thread A (KRW-BTC):          Thread B (KRW-ETH):
T1: 잔고 조회 → 1,000,000    T1: 잔고 조회 → 1,000,000
T2: 매수 결정 (500,000원)    T2: 매수 결정 (500,000원)
T3: 주문 제출                T3: 주문 제출

결과: 1,000,000원으로 총 1,000,000원 주문 → 둘 중 하나 실패 또는 잔고 부족
```

#### 대안 비교

| 대안 | 설명 | 장점 | 단점 |
|------|------|------|------|
| **A. 낙관적 락 + 롤백** | 주문 후 실패 시 전략 롤백 | 구현 단순 | 빈번한 롤백, 전략 상태 복잡 |
| **B. 중앙 Coordinator** | 주문 전 승인 요청 | 정합성 보장 | 병목, 지연 증가 |
| **C. 사전 KRW 분배 (선택)** | 시작 시 마켓별 KRW 할당 | 완전 독립, 락 불필요 | 유연성 감소, 재분배 필요 |
| **D. 예약 기반 할당** | 주문 시 KRW 예약 후 제출 | 정합성+유연성 | 구현 복잡도 |

#### 선택: C + D 하이브리드 (사전 분배 + 예약 기반)

**Note**: 주기적 재분배는 전량 거래 모델에서 불필요하므로 제외됨.
각 마켓은 할당된 자본으로 독립적으로 운영되며, 수익/손실은 마켓 내에서 누적됩니다.

**선택 이유**:
1. **완전한 독립성**: 마켓 스레드 간 경합 제거
2. **단순한 전략 로직**: 각 전략은 자신의 할당량만 고려
3. **실패 없는 주문**: 할당량 내에서 항상 주문 가능
4. **마켓 독립성**: 전량 거래로 유휴 자금 없음 (항상 100% 투자 또는 100% 현금)
5. **단순성**: 재분배 로직 불필요로 복잡도 감소

#### 구현 설계

**파일**: `src/trading/allocation/AccountManager.h` (신규)

```cpp
// 마켓별 KRW 할당 및 예약 관리자 (모든 메서드 thread-safe, 내부 shared_mutex)
//
// 주문 생명주기:
//   reserve() ──► submitOrder() ──► finalizeFillBuy/Sell() ──► finalizeOrder()
//       │              │
//       │         [실패/취소]
//       └────────► release()
//
// MarketBudget: { allocated_krw, reserved_krw, available_krw, coin_balance, avg_entry_price }
class AccountManager {
public:
    AccountManager(Account&, markets, reserve_ratio=0.1);

    // 마켓 스레드에서 호출
    getBudget(market)                              → MarketBudget (복사본)
    reserve(market, krw_amount)                    → optional<ReservationToken>
    release(token)                                 → void
    finalizeFillBuy(token, executed_krw, coin, price) → void  // 부분 체결 누적 가능
    finalizeFillSell(market, sold_coin, received_krw) → void  // 부분 체결 누적 가능
    finalizeOrder(token)                           → void     // 토큰 비활성화

    // 초기화 및 동기화
    syncWithAccount(account)                       → void     // 물리 계좌 동기화
    snapshot()                                     → map<market, MarketBudget>

    // Note: rebalance() 메서드 없음 (전량 거래 모델에서 불필요)
    // 각 마켓은 할당된 자본으로 독립 운영, 마켓 간 자본 이동 없음
};

// ReservationToken: RAII, move-only (복사 금지)
// - 소멸자에서 미확정 시 자동 release (안전망)
// - market(), amount(), isActive() 조회 제공
```

**흐름 예시**:

```
초기화:
  총 KRW = 1,000,000
  예비금 = 100,000 (10%)
  할당 가능 = 900,000

  KRW-BTC: 300,000 (33%)
  KRW-ETH: 300,000 (33%)
  KRW-XRP: 300,000 (33%)

매수 주문 시:
  1. reserve("KRW-BTC", 150,000) → Token
     - available: 300,000 → 150,000
     - reserved: 0 → 150,000

  2. api.submitOrder(...) → 성공

  3. WS fill 이벤트 수신 (부분 체결 가능)
     - finalizeFillBuy(token, 50,000, 0.001, 50,000,000)  // 1차
     - finalizeFillBuy(token, 100,000, 0.002, 50,000,000) // 2차
     - reserved: 150,000 → 0 (점진적 차감)
     - coin_balance: 0 → 0.003
     - avg_entry_price 재계산

  4. finalizeOrder(token) → 토큰 비활성화

매도 주문 시:
  1. (KRW 예약 불필요, 코인 보유량 확인만)
  2. api.submitOrder(SELL, ...)
  3. finalizeFillSell("KRW-BTC", 0.001, 49,900)  // 수수료 차감
  4. available: 150,000 → 199,900
  5. Dust 처리: 잔량이 5,000원 미만이면 0으로 처리 (전략 일관성)

전량 거래 완료 사이클:
  1. Flat: available_krw = 199,900 (이전 매도 수익 반영)
  2. 다음 진입 신호 대기 (RSI < 30)
  3. 수익/손실은 해당 마켓에 영구 귀속

  ** 마켓 간 자본 이동 없음: 각 마켓은 독립적으로 복리 운영 **

외부 입금 처리 (전량 거래 모델 제약):
  시나리오 1: 일부 마켓 Flat 상태
    → Flat 마켓에 즉시 균등 배분 (정상)

  시나리오 2: 모든 마켓 포지션 보유 중
    → KRW 할당 지연 (다음 매도 후 할당)
    → 권장: 최소 1개 마켓 수동 매도 후 입금
    → 상세: docs/ARCHITECTURE.md "외부 입금 처리 정책" 참조
```

---

### Issue 3: EventRouter JSON 파싱 취약성

#### 문제 분석

현재 접근법의 취약점:
```cpp
// 취약한 방식: "KRW-" 하드코딩
auto pos = json.find("\"code\":\"KRW-");  // BTC-XXX 마켓 미지원
```

**문제점**:
- `{"code" : "KRW-BTC"}` (공백) → 실패
- `{"code":"BTC-ETH"}` (다른 기준 통화) → 미지원
- 유니코드 이스케이프 → 실패

#### 대안 비교

| 대안 | 설명 | 장점 | 단점 |
|------|------|------|------|
| **A. 정규 JSON 파싱** | nlohmann/json 등 사용 | 정확함 | WS 스레드 부하, 할당 발생 |
| **B. simdjson 사용** | SIMD 가속 파서 | 빠름, 정확함 | 외부 의존성 |
| **C. WS 채널 분리** | 마켓별 WS 연결 | 파싱 불필요 | 연결 5개, 복잡도 증가 |
| **D. 일반화된 키 추출 (선택)** | 키 기반 따옴표 추출 + fallback | 빠름, 확장 가능 | 완벽하지 않음 |

#### 선택: D. 일반화된 키 기반 추출 + Fallback

**선택 이유**:
1. **성능**: WS 스레드에서 할당 최소화
2. **확장성**: "KRW-" 하드코딩 제거, 모든 마켓 코드 지원
3. **Fallback 안전망**: 실패 시 정규 파서로 재시도

#### 구현 설계

**파일**: `src/app/EventRouter.h` (신규)

**목적**: WebSocket에서 수신한 JSON 메시지를 파싱하여 해당 마켓의 MarketEngine으로 라우팅

**필수 요구사항**:

1. **WS 스레드 처리 범위** (아키텍처 핵심):
   - EventRouter는 **WS 수신 스레드**에서 실행됨
   - **처리 범위**: "마켓 코드 추출 + BlockingQueue::push" **만** 수행
   - **금지 사항**: 전략 업데이트, 엔진 호출, DB 쓰기 등 무거운 작업
   - **이유**: WS 스레드 블로킹 시 메시지 수신 지연 → 거래 기회 손실
   - **워커 스레드**: MarketEngineManager의 marketWorkerLoop에서 큐를 pop하고 실제 처리
   - **참고**: 현재 EventBridge 패턴과 동일 (src/app/EngineRunner.h:16 주석)

2. **반환값 정책**:
   - `bool routeMarketData(string_view json)`: 성공/실패만 반환
   - **실패 원인 구분**: Stats 카운터 + 로그 (복잡한 variant 반환 금지)
   - **이유**: WS 스레드에서 빠른 처리, 복잡한 에러 타입은 오버헤드

3. **마켓 코드 추출**:
   - **Fast path**: 키 기반 문자열 값 추출 (하드코딩 없음, "KRW-" 제거)
   - **키 순서**: "code" → "market" 순서로 시도
   - **공백 허용**: `{"code" : "KRW-BTC"}` 지원
   - **일반화**: BTC-XXX 등 비KRW 마켓도 지원

   **code/market 충돌 정책**:
   - 둘 다 존재하고 값이 **같음**: 허용 (정상)
   - 둘 다 존재하고 값이 **다름**: 드롭 + parse_failures 증가 + WARN 로그
   - **이유**: Upbit API에서 ticker(`code`), orderbook(`market`) 혼용 가능

4. **Fallback 파싱**:
   - Fast path 실패 시 nlohmann::json 정규 파싱 사용
   - **즉시 전환 조건** (토큰 경계 검증 실패):
     - `"key":value` 형식 불일치
     - 이스케이프 문자 발견 (`\"`, `\n` 등)
     - 애매한 케이스 (값에 `"` 포함 등)
   - **이유**: 문자열 스캔만으로는 `"desc":"market crashed"` 오인식 가능

5. **성능 최적화**:
   - WS 스레드 실행이므로 동적 할당 최소화
   - string_view 기반 추출 (원본 참조)
   - **길이 제한**: 최대 20자 (임시 최적화, 안전망용)
   - **최종 검증**: 등록 마켓 목록 대조 (길이 제한만으로는 부족)
   - **이유**: 거래소 포맷 변경 시 길이 제한만으로는 취약

6. **통계 수집** (운영 디버깅용):
   - fast_path_success: 빠른 경로 성공 횟수
   - fallback_used: Fallback 파싱 사용 횟수 (5% 이하 권장)
   - parse_failures: 파싱 완전 실패 횟수 (0 목표)
   - unknown_market: 등록되지 않은 마켓 코드 수신 횟수
   - total_routed: 총 라우팅 성공 횟수
   - atomic<uint64_t> + memory_order_relaxed 사용
   - drop-oldest 계측: BlockingQueue 내부에서 처리 (EventRouter 통계 외부)

   **장애 시 원인 추적**:
   - parse_failures > 0 → JSON 형식 변경 또는 버그
   - unknown_market > 0 → 설정 누락 또는 외부 마켓 이벤트 수신

7. **구현 주의사항**:
   - `std::isspace`에 `unsigned char` 캐스팅 필수 (음수 UB 방지)
   - 키 패턴은 string_view 리터럴로 구성 (동적 할당 제거)
   - 마켓 코드 대소문자 구분 (거래소 API 정책 따름)
   - Fast path 토큰 경계 검증 실패 시 즉시 fallback (안전성 우선)

**인터페이스 (최소)**:
```cpp
class EventRouter {
public:
    explicit EventRouter(MarketEngineManager& manager);

    // 마켓 데이터 라우팅 (candle, ticker 등)
    // @return true=성공, false=실패 (원인은 Stats+로그로 확인)
    // 주의: WS 스레드에서 호출됨 - 빠른 처리 필수
    bool routeMarketData(std::string_view json);

    // myOrder 이벤트 라우팅
    // @return true=성공, false=실패 (원인은 Stats+로그로 확인)
    // 주의: WS 스레드에서 호출됨 - 빠른 처리 필수
    bool routeMyOrder(std::string_view json);

    // 통계 조회 (운영 모니터링용)
    struct Stats {
        std::atomic<uint64_t> fast_path_success{0};  // 빠른 경로 성공
        std::atomic<uint64_t> fallback_used{0};      // Fallback 파싱 사용
        std::atomic<uint64_t> parse_failures{0};     // 파싱 실패 (0 목표)
        std::atomic<uint64_t> unknown_market{0};     // 미등록 마켓
        std::atomic<uint64_t> total_routed{0};       // 총 라우팅 성공
    };
    const Stats& stats() const;

private:
    MarketEngineManager& manager_;
    Stats stats_;

    // Fast path: 키 기반 마켓 코드 추출 (할당 없음)
    std::optional<std::string_view> extractMarketFast_(std::string_view json);

    // Fallback: 정규 JSON 파싱 (할당 발생, 토큰 경계 실패 시만 사용)
    std::optional<std::string> extractMarketSlow_(std::string_view json);
};
```

---

### Issue 4: PostgreSQL DDL 문법 불일치

#### 문제 분석

원본 DDL의 문제:
```sql
CREATE TABLE trades (
    ...
    INDEX idx_trades_market (market),  -- PostgreSQL에서 문법 오류!
);
```

추가 문제: `OrderSize`가 `VolumeSize` 또는 `AmountSize` variant이므로, 주문 테이블에서 이를 구분하여 저장해야 함.

#### 해결책: 표준 PostgreSQL DDL + OrderSize variant 저장

```sql
-- ============================================================
-- CoinBot Database Schema
-- PostgreSQL 14+
-- ============================================================

-- 거래 기록 테이블
CREATE TABLE IF NOT EXISTS trades (
    id              BIGSERIAL PRIMARY KEY,
    trade_id        VARCHAR(64) NOT NULL,
    order_id        VARCHAR(64) NOT NULL,
    market          VARCHAR(20) NOT NULL,
    position        VARCHAR(10) NOT NULL CHECK (position IN ('BID', 'ASK')),
    price           DECIMAL(20, 8) NOT NULL,
    volume          DECIMAL(20, 8) NOT NULL,
    fee             DECIMAL(20, 8) NOT NULL DEFAULT 0,
    executed_at     TIMESTAMPTZ NOT NULL,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT uq_trades_trade_id UNIQUE (trade_id)
);

CREATE INDEX IF NOT EXISTS idx_trades_market ON trades(market);
CREATE INDEX IF NOT EXISTS idx_trades_order_id ON trades(order_id);
CREATE INDEX IF NOT EXISTS idx_trades_executed_at ON trades(executed_at DESC);

-- 주문 기록 테이블
-- OrderSize variant 처리: order_volume XOR order_amount_krw
CREATE TABLE IF NOT EXISTS orders (
    id                  BIGSERIAL PRIMARY KEY,
    order_id            VARCHAR(64) NOT NULL,
    identifier          VARCHAR(64),
    market              VARCHAR(20) NOT NULL,
    position            VARCHAR(10) NOT NULL CHECK (position IN ('BID', 'ASK')),
    order_type          VARCHAR(20) NOT NULL,
    price               DECIMAL(20, 8),       -- NULL if order_type='market' (시장가)

    -- OrderSize variant: 둘 중 하나만 NOT NULL
    -- VolumeSize: order_volume에 값, order_amount_krw는 NULL
    -- AmountSize: order_amount_krw에 값, order_volume은 NULL
    order_volume        DECIMAL(20, 8),       -- VolumeSize 사용 시
    order_amount_krw    DECIMAL(20, 8),       -- AmountSize 사용 시 (시장가 매수)

    executed_volume     DECIMAL(20, 8) NOT NULL DEFAULT 0,
    remaining_volume    DECIMAL(20, 8),       -- NULL if AmountSize 시장가 매수 (금액 기반 주문)
    status              VARCHAR(20) NOT NULL,
    created_at          TIMESTAMPTZ NOT NULL,
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT uq_orders_order_id UNIQUE (order_id),

    -- OrderSize variant 무결성: 둘 중 하나만 NOT NULL
    CONSTRAINT chk_order_size CHECK (
        (order_volume IS NOT NULL AND order_amount_krw IS NULL) OR
        (order_volume IS NULL AND order_amount_krw IS NOT NULL)
    )
);

CREATE INDEX IF NOT EXISTS idx_orders_market ON orders(market);
CREATE INDEX IF NOT EXISTS idx_orders_status ON orders(status);
CREATE INDEX IF NOT EXISTS idx_orders_created_at ON orders(created_at DESC);
CREATE INDEX IF NOT EXISTS idx_orders_identifier ON orders(identifier)
    WHERE identifier IS NOT NULL;

-- 전략 상태 스냅샷 (복구용)
CREATE TABLE IF NOT EXISTS strategy_snapshots (
    id              BIGSERIAL PRIMARY KEY,
    market          VARCHAR(20) NOT NULL,
    state           VARCHAR(20) NOT NULL,
    entry_price     DECIMAL(20, 8),
    stop_price      DECIMAL(20, 8),
    target_price    DECIMAL(20, 8),
    position_volume DECIMAL(20, 8),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_snapshots_market_created
    ON strategy_snapshots(market, created_at DESC);

-- 일별 성과 요약 테이블
CREATE TABLE IF NOT EXISTS performance_daily (
    id              BIGSERIAL PRIMARY KEY,
    date            DATE NOT NULL,
    market          VARCHAR(20) NOT NULL,
    total_trades    INT NOT NULL DEFAULT 0,
    winning_trades  INT NOT NULL DEFAULT 0,
    losing_trades   INT NOT NULL DEFAULT 0,
    total_pnl       DECIMAL(20, 8) NOT NULL DEFAULT 0,
    total_fees      DECIMAL(20, 8) NOT NULL DEFAULT 0,
    max_drawdown    DECIMAL(10, 4),
    sharpe_ratio    DECIMAL(10, 4),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT uq_performance_date_market UNIQUE (date, market)
);
```

**TradeLogger에서 OrderSize variant 처리**:

| OrderSize variant | order_volume 컬럼 | order_amount_krw 컬럼 |
|---|---|---|
| VolumeSize | `vs.volume` 값 | NULL |
| AmountSize | NULL | `as.krw_amount` 값 |

`std::visit`로 variant를 분기하여 해당 컬럼만 INSERT에 바인딩.

---

### Issue 5: TradeLogger 백프레셔/드롭 정책 부재

#### 문제 분석

**시나리오**:
```
정상: 큐 → DB 쓰기 (10ms) → 큐 비움
장애: 큐 → DB 쓰기 (5000ms) → 큐 적체 → 메모리 폭발

24시간 × 5마켓 × 분당 10거래 = 72,000 거래/일
DB 장애 1시간 = 3,000 거래 적체 (메모리 ~수 MB)
DB 장애 지속 = 무한 증가
```

#### 선택: 배치 쓰기 + 백프레셔 + 로컬 WAL

**설계 요구사항**:

1. **BoundedQueue**: 크기 제한 (예: 10,000개)
   - 큐 초과 시 백프레셔 메트릭 증가
   - 드롭하지 않고 WAL로 폴백

2. **배치 쓰기**: N개 또는 T초마다 플러시
   - 기본값: 100개 또는 5초
   - 단일 트랜잭션으로 INSERT (성능 개선)

3. **WAL (Write-Ahead Log)**: DB 장애 시 로컬 파일
   - 형식: JSON Lines (한 줄 = 한 거래)
   - 위치: `COINBOT_WAL_DIR` 환경변수 (기본: `./data/wal`)
   - 복구: 재시작 시 WAL 파일 읽어 DB에 재시도

4. **백프레셔 메트릭**:
   ```cpp
   struct BackpressureStats {
       std::atomic<uint64_t> total_queued{0};
       std::atomic<uint64_t> current_queue_size{0};
       std::atomic<uint64_t> wal_fallback_count{0};
       std::atomic<uint64_t> db_write_failures{0};
   };
   ```

5. **플러시 타임아웃**: Graceful shutdown 시 최대 30초 대기
   - 타임아웃 시 남은 항목 WAL에 저장
   - 종료 로그에 WAL 파일 경로 기록

---

## Phase 1: 멀티마켓 스레딩 아키텍처

### 1.1 목표 아키텍처

```
┌─────────────────────────────────────────────────────────────────┐
│                      Main Thread (Coordinator)                  │
│  - 초기화 및 설정 로딩                                            │
│  - MarketEngineManager 생성 및 관리                              │
│  - AccountManager: 초기 자본 배분 (startup 1회)                   │
│  - 마켓별 완전 독립 운영 (no rebalancing)                          │
│  - Graceful shutdown 관리                                        │
└─────────────────────────────────────────────────────────────────┘
           │
           ├─────────────────────────────────────────────┐
           │                                             │
           ▼                                             ▼
┌─────────────────────┐                    ┌─────────────────────┐
│   WebSocket Thread  │                    │   WebSocket Thread  │
│      (Public)       │                    │      (Private)      │
│  - Candle 데이터     │                    │  - myOrder 이벤트    │
│  - 전체 마켓 구독     │                    │  - 전체 마켓 구독    │
└─────────────────────┘                    └─────────────────────┘
           │                                             │
           │ MarketDataRaw                               │ MyOrderRaw
           └──────────────────┬──────────────────────────┘
                              │
                              ▼
                    ┌─────────────────────┐
                    │   EventRouter       │
                    │  - 키 기반 추출      │
                    │  - Fallback 파싱    │
                    └─────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     ▼                     ▼
┌───────────────┐     ┌───────────────┐     ┌───────────────┐
│ Market Thread │     │ Market Thread │     │ Market Thread │
│   (KRW-BTC)   │     │   (KRW-ETH)   │     │   (KRW-XRP)   │
│               │     │               │     │               │
│ MarketEngine  │     │ MarketEngine  │     │ MarketEngine  │
│ (로컬 상태)    │     │ (로컬 상태)     │     │ (로컬 상태)   │
│ Strategy      │     │ Strategy      │     │ Strategy      │
│ BlockingQueue │     │ BlockingQueue │     │ BlockingQueue │
└───────────────┘     └───────────────┘     └───────────────┘
        │                     │                     │
        │    thread-safe API 호출만 (외부 락 금지)
        │                     │                     │
        └─────────────────────┼─────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    Shared Layer (Thread-Safe)                    │
├─────────────────────────────────────────────────────────────────┤
│  SharedOrderApi     │  OrderStore        │  AccountManager      │
│  (mutex 직렬화)     │  (shared_mutex)    │  (shared_mutex)      │
│  [확장: HTTP/2]     │                    │  reserve/release/    │
│                     │                    │  finalizeFill*       │
├─────────────────────────────────────────────────────────────────┤
│  TradeLogger (배치 쓰기 + WAL)           │  DatabasePool        │
└─────────────────────────────────────────────────────────────────┘
```

### 1.2 구현 단계

Phase 1 구현 단계는 [Phase 4.3 (Phase 1: 멀티마켓)](#43-phase-1-멀티마켓-phase-0-완료-후)에 상세하게 기술되어 있습니다.

**요약**:
1. SharedOrderApi (완료)
2. AccountManager (완료)
3. MarketEngine (완료)
4. EventRouter (예정)
5. MarketEngineManager + StartupRecovery 통합 (예정)
6. MarketContext + 통합 (예정)
7. 멀티마켓 테스트 (예정)

---

## Phase 3: AWS 24시간 운영 인프라

### 3.1 배포 아키텍처

```
┌─────────────────────────────────────────────────────────────────┐
│                         AWS 배포 아키텍처                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  [CloudWatch Logs/Metrics]                                       │
│          ▲                                                       │
│          │ (로그 스트리밍, 메트릭 수집)                          │
│          │                                                       │
│  ┌──────────────────────────────────────────┐                   │
│  │  EC2 Instance (t3.small 이상)            │                   │
│  │                                          │                   │
│  │  ┌────────────────────────────────────┐  │                   │
│  │  │  Systemd Service (coinbot.service) │  │                   │
│  │  │                                    │  │                   │
│  │  │  CoinBot Process                   │  │                   │
│  │  │  - MarketEngineManager             │  │                   │
│  │  │  - WebSocket Clients               │  │                   │
│  │  │  - MarketEngine × N                │  │                   │
│  │  └────────────────────────────────────┘  │                   │
│  │                │                          │                   │
│  └────────────────┼──────────────────────────┘                   │
│                   │                                              │
│                   ├──► [RDS PostgreSQL]                          │
│                   │     - 거래 기록 저장                         │
│                   │     - 전략 스냅샷 저장                       │
│                   │                                              │
│                   └──► [Secrets Manager]                         │
│                         - Upbit API Keys                         │
│                         - DB 연결 정보                           │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

**주요 구성 요소**:
1. **EC2**: CoinBot 실행 (t3.small 이상 권장)
2. **RDS PostgreSQL**: 거래 기록 및 전략 스냅샷 저장
3. **Secrets Manager**: API 키 및 민감 정보 관리
4. **CloudWatch**: 로그 수집 및 메트릭 모니터링
5. **Systemd**: 프로세스 자동 재시작 및 생명주기 관리

### 3.2 Systemd 서비스 파일

**파일**: `deploy/coinbot.service`

```ini
[Unit]
Description=CoinBot Multi-Market Trading Service
After=network.target

[Service]
Type=simple
User=coinbot
WorkingDirectory=/opt/coinbot
ExecStart=/opt/coinbot/bin/CoinBot --config /opt/coinbot/config/production.json
Restart=always
RestartSec=10

# 환경 변수 (Secrets Manager에서 주입)
EnvironmentFile=/etc/coinbot/env

# 리소스 제한
MemoryMax=512M
CPUQuota=80%

# 로그 설정
StandardOutput=journal
StandardError=journal
SyslogIdentifier=coinbot

# Graceful shutdown 타임아웃
TimeoutStopSec=90

[Install]
WantedBy=multi-user.target
```

**환경 변수 파일**: `/etc/coinbot/env`
```bash
COINBOT_DB_HOST=<RDS 엔드포인트>
COINBOT_DB_PORT=5432
COINBOT_DB_NAME=coinbot
COINBOT_DB_USER=coinbot
COINBOT_DB_PASSWORD=<Secrets Manager에서 주입>
COINBOT_UPBIT_ACCESS_KEY=<Secrets Manager에서 주입>
COINBOT_UPBIT_SECRET_KEY=<Secrets Manager에서 주입>
COINBOT_WAL_DIR=/opt/coinbot/data/wal
COINBOT_LOG_LEVEL=INFO
COINBOT_LIMIT_ORDER_POLICY=keep_open
```

### 3.3 Graceful Shutdown (강화)

```
┌─────────────────────────────────────────────────────────────────┐
│                    Graceful Shutdown 절차                        │
├─────────────────────────────────────────────────────────────────┤
│ 설계 원칙:                                                       │
│ 1. 진행 중 시장가 주문은 체결 완료까지 대기                       │
│ 2. 대기 타임아웃 시 REST API로 상태 확인                         │
│ 3. 미체결 주문은 정책에 따라 취소 또는 유지                       │
│ 4. 모든 거래 기록은 DB에 확실히 저장                             │
└─────────────────────────────────────────────────────────────────┘
```

**상세 흐름**:

```
SIGTERM/SIGINT 수신
    │
    ▼
1. SignalHandler: stop_flag = true, 로그 기록
    │ LOG: "Shutdown initiated, stopping new order submissions"
    │
    ▼
2. MarketEngineManager::stop() - 각 마켓별 처리
    │
    ├─► 모든 MarketContext.stop_flag = true
    │   (새 주문 제출 차단)
    │
    ├─► 진행 중 주문 대기 (마켓별 병렬)
    │   │
    │   ├─ 시장가 주문 체결 대기
    │   │   ├─ WS 이벤트 대기 (최대 ORDER_FILL_TIMEOUT = 30초)
    │   │   │
    │   │   └─ 타임아웃 시 REST 확인
    │   │       │
    │   │       ├─ api.getOrder(order_id)
    │   │       │
    │   │       ├─ Filled → 정상 종료
    │   │       │   LOG: "Order {id} filled, proceeding"
    │   │       │
    │   │       ├─ Open/Pending → 정책 적용
    │   │       │   │
    │   │       │   ├─ 시장가: 추가 대기 (최대 order_fill_timeout × 2; 초과 시 경고 로그 후 종료 진행)
    │   │       │   │   LOG: "Market order {id} still open, extended wait..."
    │   │       │   │
    │   │       │   └─ 지정가: 취소 또는 유지 (설정에 따라)
    │   │       │       LOG: "Limit order {id} left open per policy"
    │   │       │
    │   │       └─ 조회 실패 → 경고 후 진행
    │   │           LOG: "WARN: Could not verify order {id} status"
    │   │
    │   └─ 최종 상태 AccountManager에 반영
    │       - finalizeFillBuy/Sell 또는 release
    │
    ├─► 워커 스레드 join (최대 THREAD_JOIN_TIMEOUT = 30초)
    │   │
    │   └─ 타임아웃 시: 경고 로그 후 강제 진행
    │       LOG: "WARN: Thread {market} did not terminate gracefully"
    │
    │
    ▼
3. WebSocket 연결 종료
    │ LOG: "Closing WebSocket connections"
    ├─ UpbitWebSocketClient::close()
    └─ 정상 close frame 전송
    │
    ▼
4. TradeLogger::flush() - 타임아웃 및 로그 포함
    │
    ├─► 메모리 큐 플러시 (최대 DB_FLUSH_TIMEOUT = 30초)
    │   │
    │   ├─ 성공: LOG: "Flushed {N} records to database"
    │   │
    │   └─ 타임아웃/실패:
    │       ├─ 남은 항목 WAL에 저장
    │       │   LOG: "WARN: DB flush timeout, {M} records written to WAL"
    │       │
    │       └─ WAL 파일 경로 로그 기록
    │           LOG: "WAL file: {path}, recover on next startup"
    │
    ├─► 최종 전략 스냅샷 저장 (각 마켓)
    │   LOG: "Saved strategy snapshot for {market}: state={state}"
    │
    └─► WAL 파일 정리 (복구 완료된 것만)
    │
    ▼
5. DatabasePool 종료
    │ LOG: "Closing database connections"
    └─ 모든 연결 정상 반환
    │
    ▼
6. 종료 요약 로그 및 exit
    │
    LOG: "=== Shutdown Summary ==="
    LOG: "  Markets processed: {N}"
    LOG: "  Orders completed: {X}"
    LOG: "  Orders left open: {Y}"
    LOG: "  Records flushed to DB: {Z}"
    LOG: "  Records in WAL: {W}"
    LOG: "  Exit code: 0"
    │
    ▼
exit(0)
```

**설정 옵션**:

```cpp
struct ShutdownConfig {
    // 주문 체결 대기 타임아웃
    std::chrono::seconds order_fill_timeout{30};

    // 스레드 종료 대기 타임아웃
    std::chrono::seconds thread_join_timeout{30};

    // DB 플러시 타임아웃
    std::chrono::seconds db_flush_timeout{30};

    // 미체결 지정가 주문 정책
    enum class LimitOrderPolicy {
        Cancel,     // 모두 취소
        KeepOpen,   // 그대로 유지 (다음 시작 시 복구)
        AskUser     // 대화형 모드 전용 (로컬 개발/디버깅용)
                    // 주의: systemd/서버 모드에서는 금지 (비대화형 환경)
    };
    LimitOrderPolicy limit_order_policy{LimitOrderPolicy::KeepOpen};

    // 서버 모드 검증: AskUser 선택 시 런타임 에러 발생
    // 권장: AWS 배포 시 Cancel 또는 KeepOpen만 사용

    // 종료 시 계좌 스냅샷 저장
    bool save_account_snapshot{true};
};
```

**GracefulShutdown 계약**:

| 입력 | 출력 (Result) |
|---|---|
| MarketEngineManager, WebSocketClient, TradeLogger, SharedOrderApi, ShutdownConfig | exit_code, orders_completed, orders_left_open, records_flushed, records_in_wal, warnings |

`execute()` 호출 시 위 상세 흐름(1~6단계)을 순차 실행하고 Result를 반환.

---

## Phase 4: 구현 우선순위 및 의존성

### 4.1 전체 구현 로드맵

```
┌─────────────────────────────────────────────────────────────────┐
│                    구현 순서 개요                                 │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Phase 0          Phase 1          Phase 2          Phase 3     │
│  기존 코드    →   멀티마켓     →   PostgreSQL   →   AWS 배포       │
│  리팩토링         신규 구현         거래 기록         운영 환경      │
│                                                                 │
│  ┌─────────┐     ┌─────────┐     ┌─────────┐     ┌─────────┐    │
│  │ 정리     │────►│ 확장    │────►│ 영속화   │────►│ 배포     │    │
│  │ 단순화   │     │ 병렬화   │     │ 분석    │     │ 모니터링 │     │
│  └─────────┘     └─────────┘     └─────────┘     └─────────┘    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 4.2 Phase 0: 기존 코드 리팩토링 (선행 필수)

**목적**: 새 아키텍처 도입 전 기존 코드 정리 및 단순화

```
Phase 0: 기존 코드 리팩토링
│
├
│   
│   
│   
│
├─ 0.1 EngineRunner 분해 (2일)
│   ├─ handleOne_() → handleMyOrder_(), handleMarketData_() 분리
│   ├─ 이벤트별 핸들러 함수 추출
│   ├─ static 변수 제거 (CandleDeduplicator 클래스로 이동)
│   └─ 검증: 단일 마켓 거래 정상 동작
│
├─ 0.2 인터페이스 정리 (0.5일)
│   ├─ PrivateOrderApi → IOrderApi로 통합 (테스트 Mock용 유지)
│   ├─ IOrderApi: variant 반환 타입으로 에러 처리 통일
│   └─ 검증: 컴파일 및 기능 테스트
│
├─ 0.3 에러 처리 통합 (1일)
│   ├─ Logger 클래스 구현 (src/util/Logger.h 개선)
│   ├─ std::cout/cerr → Logger 교체
│   ├─ 에러 레벨 통일 (DEBUG, INFO, WARN, ERROR)
│   └─ 검증: 로그 출력 일관성 확인
│
├─ 0.4 테스트 코드 분리 (0.5일)
│   ├─ src/app/test/ → tests/ 디렉토리 이동
│   ├─ CMakeLists.txt 업데이트
│   └─ 검증: 테스트 빌드 및 실행
│
├─ 0.5 이벤트 타입 통합 (1일)
│   ├─ EngineFillEvent ≈ trading::FillEvent 통합
│   ├─ EngineOrderStatusEvent ≈ trading::OrderStatusEvent 통합
│   ├─ 불필요한 변환 로직 제거
│   └─ 검증: 이벤트 흐름 테스트
│
└─ 0.6 설정 외부화 (0.5일)
    ├─ 하드코딩 상수 → Config 구조체
    │   - kMinNotionalKrw = 5000.0
    │   - kVolumeSafetyEps = 1e-12
    │   - 타임아웃 값들
    ├─ config/defaults.json 생성
    └─ 검증: 설정 로딩 테스트

Phase 0 완료 기준: (2026-02-09 최종 평가)

**필수 항목** (Phase 1 시작 전제조건):
  ✅ EngineRunner::handleOne_() ≤ 100줄 → 완료 (20줄, 적절히 분리됨)
  ✅ 핵심 로그가 Logger 통해 출력 → 완료 (EngineRunner, RealOrderEngine)
  ✅ tests/ 디렉토리에 테스트 코드 분리 → 완료
  □ 단일 마켓 거래 정상 동작 테스트 → **미확인** (실행 필요)

**권장 항목** (Phase 1과 병행 가능):
  ⚖️ IOrderApi 인터페이스 정리 → 유지 권장 (테스트 Mock용)
  ⚖️ 설정 외부화 (config/defaults.json) → Phase 1에서 함께 진행 가능

**게이트 판정**: ✅ **조건부 통과** (4/5 필수 완료, 단일 마켓 테스트만 미확인)
  - Phase 1 시작 가능
  - 단일 마켓 테스트는 Phase 1.3 (MarketEngine) 통합 시 병행 검증
```

### 4.3 Phase 1: 멀티마켓 (Phase 0 완료 후)

**전제 조건**: Phase 0 완료 기준 충족

```
Phase 1: 멀티마켓
│
├─ 1.1 SharedOrderApi (1일) ✅ 완료 (2026-01-29)
│   ├─ UpbitExchangeRestClient를 감싸는 thread-safe 래퍼 ✅
│   ├─ mutex 직렬화 + 주석 (확장 포인트 명시) ✅
│   └─ 검증: 멀티스레드 주문 제출 테스트 ✅
│   └─ 관련 파일: src/api/upbit/SharedOrderApi.h/cpp, tests/test_shared_order_api_advanced.cpp
│
├─ 1.2 AccountManager (3일) ✅ 완료 (2026-02-03)
│   ├─ MarketBudget 구조체 ✅
│   ├─ reserve/release/finalizeFillBuy/finalizeFillSell API ✅
│   ├─ 부분 체결 누적 로직 (가중 평균 단가) ✅
│   ├─ syncWithAccount() 물리 계좌 동기화 ✅
│   ├─ Dust 이중 체크 (수량 + 가치 기준) ✅
│   ├─ ReservationToken RAII 패턴 ✅
│   ├─ 전량 거래 모델 완전 구현 ✅
│   ├─ Note: rebalance() 의도적 제외 (전량 거래로 불필요)
│   └─ 검증: test_account_manager_unified.cpp (23개 테스트) ✅
│
├─ 1.3 MarketEngine (2일)
│   ├─ RealOrderEngine 로직 추출 + 리팩토링
│   ├─ 로컬 상태만 직접 변경, 공유 자원은 API 호출만
│   ├─ bindToCurrentThread() 유지
│   └─ 검증: 단일 마켓 엔진 동작 테스트
│
├─ 1.4 EventRouter (1일)
│   ├─ extractStringValue() 일반화 함수
│   ├─ "KRW-" 하드코딩 제거
│   ├─ Fast path + Fallback 파싱
│   └─ 검증: 다양한 JSON 포맷 테스트
│
├─ 1.5 MarketEngineManager (3일) ← 1일 추가
│   ├─ MarketContext 생성/관리
│   ├─ 워커 스레드 생명주기
│   ├─ 이벤트 라우팅
│   ├─ 초기 자본 배분 (AccountManager 생성자 호출, 1회)
│   ├─ 마켓 독립성 보장 (마켓 간 간섭 없음, no rebalancing)
│   ├─ 멀티마켓 StartupRecovery 통합 ← 신규 (1일)
│   │   ├─ syncAccountWithExchange() 메서드 구현
│   │   │   - 초기화 시: AccountManager.syncWithAccount(api.getMyAccount())
│   │   │   - 최종화 시: 미체결 주문 취소 후 재동기화
│   │   ├─ 각 마켓별 StartupRecovery::run() 호출
│   │   │   - 미체결 주문 취소 (Cancel 정책)
│   │   │   - 포지션 스냅샷 생성
│   │   │   - 전략 상태 복구 (strategy.syncOnStart)
│   │   └─ 설계 문서: reports/STARTUP_RECOVERY_MULTIMARKET.md
│   └─ 검증: 3개 마켓 동시 실행 테스트
│
├─ 1.6 MarketContext + 통합 (2일)
│   ├─ 기존 EngineRunner 로직 → MarketEngineManager로 이전
│   ├─ main() 진입점 수정
│   ├─ 설정 파일 (config/markets.json) 로딩
│   ├─ 검증: 멀티마켓 End-to-End 테스트
│   └─ 재시작 시나리오 테스트 ← 신규
│       ├─ 각 마켓별 포지션 복구 검증
│       ├─ AccountManager 잔고 동기화 검증
│       └─ 미체결 주문 처리 검증
│
└─ 1.7 멀티마켓 테스트 (2일)
    ├─ 단위 테스트 작성
    ├─ 통합 테스트 작성
    ├─ 부하 테스트 (5마켓, 1시간)
    └─ 검증: 모든 테스트 통과

Phase 1 완료 기준:

**기능 검증** (필수):
  □ 1~5개 마켓 동시 거래 가능 (최소 3개 마켓 동시 실행)
  □ 마켓별 독립 KRW 할당 (AccountManager 사전 분배)
  □ WS 이벤트 올바른 마켓으로 라우팅 (EventRouter fast path > 95%)
  □ 부분 체결 정확히 누적 (finalizeFillBuy 가중 평균 단가)
  □ 재시작 시 각 마켓별 포지션 복구 (StartupRecovery 통합)
  □ AccountManager와 실제 계좌 동기화 (초기화 1차/2차)

**성능 검증** (권장, 측정 환경: AWS t3.small 기준):
  □ 부하 테스트 통과 (5마켓, 1시간 연속 운영)
     - CPU 평균 < 30%, 최대 < 70%
     - 메모리 안정 (RSS < 200MB, 메모리 누수 없음)
     - EventRouter fallback 비율 < 5%
     - 주문 제출 지연 p95 < 500ms

**게이트 전환**:
  - 6개 기능 검증 모두 통과 시 Phase 2 시작 가능
  - 성능 검증은 Phase 2와 병행 가능 (AWS 환경에서 재검증)
```

### 4.4 Phase 2: PostgreSQL (Phase 1 완료 후)

**전제 조건**: Phase 1 완료 기준 충족

```
Phase 2: PostgreSQL
│
├─ 2.1 스키마 생성 (0.5일)
│   ├─ sql/schema.sql 작성
│   ├─ OrderSize variant (order_volume/order_amount_krw) 반영
│   ├─ CHECK 제약 조건
│   └─ 검증: psql로 스키마 적용 테스트
│
├─ 2.2 DatabasePool (1일)
│   ├─ libpqxx 연결 풀
│   ├─ RAII Connection 가드
│   ├─ 타임아웃 처리
│   └─ 검증: 연결 획득/반환 테스트
│
├─ 2.3 TradeLogger (3일)
│   ├─ BoundedQueue + 배치 쓰기
│   ├─ WAL 파일 폴백
│   ├─ 백프레셔 메트릭
│   ├─ OrderSize variant 분기 저장
│   └─ 검증: DB 장애 시 WAL 복구 테스트
│
├─ 2.4 기존 코드 통합 (1일)
│   ├─ MarketEngine에서 TradeLogger 호출
│   ├─ 거래/주문 로깅 삽입
│   ├─ 전략 스냅샷 저장
│   └─ 검증: 실거래 시 DB 기록 확인
│
└─ 2.5 복구 테스트 (1일)
    ├─ WAL 복구 시나리오 테스트
    ├─ DB 재연결 테스트
    ├─ 백프레셔 동작 확인
    └─ 검증: 모든 복구 시나리오 통과

Phase 2 완료 기준:
  ✓ 모든 거래가 DB에 기록됨
  ✓ DB 장애 시 WAL로 폴백
  ✓ 재시작 시 WAL에서 복구
  ✓ 백프레셔 메트릭 정상 동작
```

### 4.5 Phase 3: AWS 배포 (Phase 2 완료 후)

**전제 조건**: Phase 2 완료 기준 충족

```
Phase 3: AWS 배포
│
├─ 3.1 SignalHandler (1일)
│   ├─ SIGTERM/SIGINT 처리
│   ├─ stop_flag 전파
│   └─ 검증: kill -TERM 시 정상 종료
│
├─ 3.2 GracefulShutdown (2일)
│   ├─ 시장가 주문 체결 대기
│   ├─ REST API로 주문 상태 확인
│   ├─ 미체결 주문 정책 적용
│   ├─ TradeLogger flush + 타임아웃
│   ├─ 종료 요약 로그
│   └─ 검증: 진행 중 주문 있을 때 종료 테스트
│
├─ 3.3 HealthChecker (1일)
│   ├─ WS 연결 상태
│   ├─ DB 연결 상태
│   ├─ 마켓 스레드 상태
│   ├─ TradeLogger 백프레셔
│   └─ 검증: 각 서비스 장애 시 감지 테스트
│
├─ 3.4 Logger 개선 (1일)
│   ├─ 구조화된 JSON 로깅
│   ├─ CloudWatch 싱크 (선택적)
│   ├─ 로그 레벨 동적 변경
│   └─ 검증: 로그 포맷 검증
│
├─ 3.5 배포 스크립트 (1일)
│   ├─ Dockerfile 작성
│   ├─ coinbot.service (systemd)
│   ├─ 환경 변수 템플릿
│   └─ 검증: Docker 빌드 및 실행
│
└─ 3.6 인프라 구성 (2일)
    ├─ EC2 인스턴스 설정
    ├─ RDS PostgreSQL 설정
    ├─ CloudWatch 알림 설정
    ├─ Secrets Manager 연동
    └─ 검증: 24시간 운영 테스트

Phase 3 완료 기준:
  ✓ systemd로 자동 재시작
  ✓ SIGTERM 시 graceful shutdown
  ✓ 진행 중 주문 안전하게 처리
  ✓ CloudWatch에서 메트릭 확인 가능
  ✓ 24시간 무중단 테스트 통과
```

### 4.6 의존성 그래프

```
┌─────────────────────────────────────────────────────────────────┐
│                        Phase 0 (선행)                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│                                                                   │
│                                                                 │
│  EngineRunner 분해 ─┼──────► 기존 코드 정리 완료                 │
│                     │                                            │
│  인터페이스 제거 ───┤                                            │
│                     │                                            │
│  Logger 통합 ───────┤                                            │
│                     │                                            │
│  테스트 분리 ───────┘                                            │
│                                                                  │
└──────────────────────────────┬──────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                        Phase 1 (멀티마켓)                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  SharedOrderApi ────────┐                                        │
│  (mutex + 주석)         │                                        │
│                         │                                        │
│  AccountManager ────────┼──────► MarketEngine                    │
│  (reserve/finalize)     │        (로컬 상태만)                   │
│                         │              │                         │
│  EventRouter ───────────┘              │                         │
│  (일반화 키 추출)                      │                         │
│                                        ▼                         │
│                                MarketEngineManager               │
│                                        │                         │
└────────────────────────────────────────┼────────────────────────┘
                                         │
                                         ▼
┌─────────────────────────────────────────────────────────────────┐
│                        Phase 2 (PostgreSQL)                      │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  DatabasePool ──────────────────► TradeLogger                    │
│                                   (배치+WAL+OrderSize)           │
│                                                                  │
└────────────────────────────────────────┬────────────────────────┘
                                         │
                                         ▼
┌─────────────────────────────────────────────────────────────────┐
│                        Phase 3 (AWS 배포)                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  SignalHandler ─────────────────► GracefulShutdown               │
│                                   (시장가 대기 + REST)           │
│                                         │                        │
│                                         ▼                        │
│                                   HealthChecker                  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 4.7 Phase 간 게이트 체크

```
┌─────────────────────────────────────────────────────────────────┐
│                    Phase 전환 체크리스트                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Phase 0 → Phase 1 게이트: (2026-02-09 최종 평가)                │
│                                                                   │
│  ✅ EngineRunner::handleOne_() ≤ 100줄                           │
│  ✅ Logger 클래스로 핵심 로그 출력                               │
│  ✅ tests/ 디렉토리 존재                                         │
│  □ 단일 마켓 거래 테스트 통과 (미확인)                           │
│                                                                   │
│  **게이트 상태**: ✅ **조건부 통과** (4/5 필수 완료)             │
│  - Phase 1 시작 가능                                             │
│  - 단일 마켓 테스트는 Phase 1.3 통합 시 병행 검증               │
│                                                                  │
│  Phase 1 → Phase 2 게이트:                                       │
│  **기능 검증** (필수, 6개 모두 통과 필요):                       │
│  □ 3개 마켓 동시 거래 성공                                      │
│  □ 마켓별 독립 KRW 할당 정상 동작                               │
│  □ EventRouter fast path 성공률 > 95%                           │
│  □ 부분 체결 누적 정확성 (가중 평균 단가)                       │
│  □ 재시작 시 포지션 복구 정상 동작                              │
│  □ AccountManager 계좌 동기화 검증 (1차/2차)                    │
│                                                                  │
│  **성능 검증** (권장, Phase 2와 병행 가능):                      │
│  □ 5마켓 1시간 연속 운영                                        │
│     (측정 환경: AWS t3.small 또는 로컬 동등)                    │
│     - CPU 평균 < 30%, 최대 < 70%                                │
│     - 메모리 안정 (RSS < 200MB)                                 │
│                                                                  │
│  Phase 2 → Phase 3 게이트:                                       │
│  □ 모든 거래 DB에 기록됨                                        │
│  □ WAL 복구 테스트 통과                                         │
│  □ 백프레셔 메트릭 정상                                         │
│                                                                  │
│  Phase 3 완료 체크:                                              │
│  □ 24시간 무중단 운영 성공                                      │
│  □ Graceful shutdown 정상 동작                                  │
│  □ CloudWatch 알림 동작 확인                                    │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Phase 5: 테스트 전략

### 5.1 단위 테스트

| 컴포넌트 | 테스트 항목 |
|----------|-------------|
| AccountManager | reserve/release/finalizeFillBuy/finalizeFillSell, 부분 체결 누적, 동시성 |
| EventRouter | 키 기반 추출 (공백, 순서 변형), Fallback 정확성 |
| TradeLogger | 배치 쓰기, WAL 복구, OrderSize variant 저장, 백프레셔 |
| MarketEngine | 로컬 상태 변경, 공유 자원 API 호출만 |
| GracefulShutdown | 시장가 대기 타임아웃, REST 확인, DB 플러시 |

### 5.2 통합 테스트 (수동 테스트 스타일)

**참고**: CoinBot 프로젝트는 GTest 금지 정책에 따라 수동 테스트 함수 스타일을 사용합니다.
(상세: [IMPLEMENTATION_STATUS.md - 테스트 작성 지침](IMPLEMENTATION_STATUS.md#테스트-작성-지침))

```cpp
namespace test {

void testAccountManagerPartialFill() {
    // 1. AccountManager 생성 및 초기화
    // 2. reserve("KRW-BTC", 100000)
    // 3. finalizeFillBuy(token, 30000, 0.001, 30000000)
    // 4. finalizeFillBuy(token, 70000, 0.002, 35000000)
    // 5. getBudget("KRW-BTC") 조회
    // 6. assert(coin_balance == 0.003)
    // 7. assert(avg_entry_price 가중 평균 검증)
    std::cout << "Test: AccountManager 부분 체결 누적 - PASSED\n";
}

void testGracefulShutdownTimeout() {
    // 1. Mock SharedOrderApi 준비 (지연 시뮬레이션)
    // 2. MarketEngine으로 시장가 매수 제출
    // 3. GracefulShutdown::execute() 호출
    // 4. REST getOrder 호출 확인 (타임아웃 후)
    // 5. assert(종료 완료 && 로그 기록됨)
    std::cout << "Test: GracefulShutdown 타임아웃 처리 - PASSED\n";
}

void testEventRouterJsonFormats() {
    // 1. EventRouter 생성
    // 2. routeMarketData(R"({"code":"KRW-BTC"})") → assert(성공)
    // 3. routeMarketData(R"({"code": "KRW-BTC"})") → assert(성공, 공백 허용)
    // 4. routeMarketData(R"({"market":"KRW-BTC"})") → assert(성공)
    // 5. routeMarketData(R"({"code":"BTC-ETH"})") → assert(성공, 비KRW)
    // 6. routeMarketData(R"({invalid})") → assert(실패, fallback 사용)
    // 7. stats() 확인: fast_path_success, fallback_used
    std::cout << "Test: EventRouter JSON 다형성 - PASSED\n";
}

} // namespace test

int main() {
    test::testAccountManagerPartialFill();
    test::testGracefulShutdownTimeout();
    test::testEventRouterJsonFormats();
    std::cout << "All integration tests passed.\n";
    return 0;
}
```

---

## 부록 A: 신규 파일 목록 (구현 상태)

```
src/
├── app/
│   ├── MarketEngineManager.h/cpp      (Phase 1.5 - 예정)
│   ├── EventRouter.h/cpp              (Phase 1.4 - 예정)
│   ├── SignalHandler.h/cpp            (Phase 3.1 - 예정)
│   └── GracefulShutdown.h/cpp         (Phase 3.2 - 예정)
├── core/
│   └── MarketContext.h                (Phase 1.6 - 예정)
├── engine/
│   └── MarketEngine.h/cpp             (✅ 완료 - 2026-02-08)
├── api/upbit/
│   ├── SharedOrderApi.h/cpp           (✅ 완료 - 2026-01-29)
│   └── IOrderApi.h                    (✅ 완료 - 2026-02-08)
├── persistence/
│   ├── DatabasePool.h/cpp             (Phase 2.2 - 예정)
│   └── TradeLogger.h/cpp              (Phase 2.3 - 예정)
├── trading/
│   └── allocation/
│       └── AccountManager.h/cpp       (✅ 완료 - 2026-02-03)
└── monitoring/
    └── HealthChecker.h/cpp            (Phase 3.3 - 예정)

config/
├── markets.json                       (Phase 1.6 - 예정)
└── production.json                    (Phase 3 - 예정)

deploy/
├── coinbot.service                    (Phase 3.5 - 예정)
└── Dockerfile                         (Phase 3.5 - 예정)

sql/
└── schema.sql                         (Phase 2.1 - 예정)

tests/
├── test_market_engine.cpp             (✅ 진행 중 - 2026-02-08)
├── test_account_manager_unified.cpp   (✅ 완료 - 2026-02-06)
└── test_shared_order_api_advanced.cpp (✅ 완료 - 2026-01-29)
```

## 부록 B: 외부 의존성

| 라이브러리 | 버전 | 용도 |
|-----------|------|------|
| libpqxx | 7.x | PostgreSQL C++ 클라이언트 |
| nlohmann/json | 3.x | JSON 파싱 (기존 사용 중) |
| spdlog | 1.12.x | 고성능 로깅 (선택적) |

## 부록 C: 환경 변수

```bash
# Database
COINBOT_DB_HOST=localhost
COINBOT_DB_PORT=5432
COINBOT_DB_NAME=coinbot
COINBOT_DB_USER=coinbot
COINBOT_DB_PASSWORD=<secret>

# Upbit API
COINBOT_UPBIT_ACCESS_KEY=<secret>
COINBOT_UPBIT_SECRET_KEY=<secret>

# Logging
COINBOT_WAL_DIR=/opt/coinbot/data/wal
COINBOT_LOG_LEVEL=INFO

# Shutdown
COINBOT_ORDER_FILL_TIMEOUT=30
COINBOT_DB_FLUSH_TIMEOUT=30
COINBOT_LIMIT_ORDER_POLICY=keep_open
```

---

## 관련 문서

- **현재 아키텍처**: [ARCHITECTURE.md](ARCHITECTURE.md) - 현재 시스템 구조 및 주요 컴포넌트
- **구현 현황**: [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) - 각 Phase별 진행 상황 추적
- **멀티마켓 포지션 복구 설계**: [reports/STARTUP_RECOVERY_MULTIMARKET.md](reports/STARTUP_RECOVERY_MULTIMARKET.md) - 재시작 시 포지션 복구 상세 설계
- **로드맵 검토 보고서**: [reports/ROADMAP_MULTIMARKET_REVIEW.md](reports/ROADMAP_MULTIMARKET_REVIEW.md) - 멀티마켓 구조 및 Phase 타당성 평가
- **개발자 가이드**: [../CLAUDE.md](../CLAUDE.md) - Claude Code를 위한 프로젝트 지침

---

# 파일 생성 규칙
- 모든 텍스트 파일은 한글이 깨지지 않도록 저장
- 오버코딩 금지
- 주석으로 왜 필요한지, 기능과 동작을 간단히 설명할 것
- 우선 테스트 코드는 작성, 수정하지 말고 요청 시 작성
- 코드 작성 시 가독성 우선
- 함수, 변수, 클래스 네이밍은 직관적으로
- 보완점이나 수정할 점이 큰 영향이 없으면 기존 코드를 최대한 유지

# 테스트 작성 규칙

**CoinBot 프로젝트는 수동 테스트 스타일을 사용합니다.**
- ❌ GoogleTest/GTest 사용 금지
- ✅ 수동 함수 기반 (namespace test, void testXXX(), assert(), std::cout)
- 상세 지침: [IMPLEMENTATION_STATUS.md - 테스트 작성 지침](IMPLEMENTATION_STATUS.md#테스트-작성-지침)
