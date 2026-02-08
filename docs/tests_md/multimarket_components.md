# 멀티마켓 전환 핵심 컴포넌트

> **작성일**: 2026-02-08
> **목적**: SharedOrderApi와 AccountManager의 기능 및 동작 설명

## 개요

CoinBot의 멀티마켓 전환 과정에서 만들어진 두 가지 핵심 컴포넌트:

1. **SharedOrderApi**: 여러 마켓 스레드가 동시에 주문 API를 호출할 때 thread-safe하게 공유할 수 있는 래퍼
2. **AccountManager**: 마켓별 독립적 자산 관리 및 예약 기반 할당을 담당하는 thread-safe 클래스

---

## 1. SharedOrderApi

### 1.1 목적

**문제**:
- 단일 마켓 환경에서는 `UpbitExchangeRestClient`를 하나의 스레드에서만 사용
- 멀티마켓으로 전환 시 여러 마켓 스레드가 동시에 주문 API를 호출해야 함
- `UpbitExchangeRestClient`는 thread-safe하지 않음
- Upbit API는 초당 요청 제한(rate limit)이 있음

**해결책**:
- `UpbitExchangeRestClient`를 내부에 감추고 모든 호출을 `std::mutex`로 직렬화
- 여러 마켓 스레드가 `shared_ptr<SharedOrderApi>`로 안전하게 공유

### 1.2 설계 원칙

```
멀티마켓 스레드 → SharedOrderApi (mutex) → UpbitExchangeRestClient (단일 스레드화)
```

**핵심 특징**:
- **단일 소유권**: `UpbitExchangeRestClient`를 `unique_ptr`로 소유
- **직렬화**: 모든 public 메서드를 `std::lock_guard<std::mutex>`로 보호
- **복사/이동 금지**: `mutex`는 복사 불가, `shared_ptr`로 공유만 가능
- **투명한 래퍼**: `UpbitExchangeRestClient`의 메서드를 그대로 노출 (시그니처 동일)

### 1.3 주요 메서드

#### getMyAccount()
```cpp
std::variant<core::Account, api::rest::RestError> getMyAccount();
```
- **목적**: 계좌 정보 조회 (KRW 잔고, 코인 보유량)
- **API**: `GET /v1/accounts`
- **Thread-Safety**: `lock_guard` 보호
- **반환값**: 성공 시 `core::Account`, 실패 시 `RestError`

#### getOpenOrders(market)
```cpp
std::variant<std::vector<core::Order>, api::rest::RestError>
    getOpenOrders(std::string_view market);
```
- **목적**: 특정 마켓의 미체결 주문 조회
- **API**: `GET /v1/orders/open?market={market}`
- **사용 시나리오**: 재시작 시 미체결 주문 복구, 수동 취소 처리

#### cancelOrder(uuid, identifier)
```cpp
std::variant<bool, api::rest::RestError>
    cancelOrder(const std::optional<std::string>& uuid,
                const std::optional<std::string>& identifier);
```
- **목적**: 주문 취소
- **API**: `DELETE /v1/order?uuid={uuid}` 또는 `identifier={identifier}`
- **파라미터**: `uuid` 또는 `identifier` 중 하나만 필요

#### postOrder(req)
```cpp
std::variant<std::string, api::rest::RestError>
    postOrder(const core::OrderRequest& req);
```
- **목적**: 주문 제출 (매수/매도)
- **API**: `POST /v1/orders`
- **반환값**: 성공 시 Upbit 주문 UUID (`Order.id`로 사용)
- **실패 시**: `RestError` (예: 잔고 부족, rate limit 초과)

### 1.4 동시성 처리

**Mutex 보호**:
```cpp
std::variant<core::Account, api::rest::RestError>
SharedOrderApi::getMyAccount()
{
    std::lock_guard<std::mutex> lock(mtx_);  // 1. 락 획득
    InFlightGuard g(in_flight_, max_in_flight_);  // 2. 동시 실행 카운터 (테스트용)
    return client_->getMyAccount();  // 3. 내부 클라이언트 호출
}
```

**테스트 계측**:
- `in_flight_`: 현재 mutex 안에서 실행 중인 호출 수
- `max_in_flight_`: 최대 동시 실행 수 (테스트로 확인: 항상 1이어야 함)
- `InFlightGuard`: RAII 패턴으로 진입/퇴장 시 자동 증감

### 1.5 확장 포인트

**추후 개선 가능 영역**:

1. **Rate Limiting**: 요청 간 최소 시간 간격 적용
   ```cpp
   std::chrono::steady_clock::time_point last_request_time_;
   const int min_interval_ms_ = 100;  // 100ms 간격 강제
   ```

2. **Request Queueing**: 우선순위 큐로 긴급 주문(취소) 우선 처리
   ```cpp
   std::priority_queue<Request> request_queue_;
   std::thread worker_thread_;
   ```

3. **Circuit Breaker**: 연속 실패 시 일시 중단 및 복구
   ```cpp
   int consecutive_failures_ = 0;
   bool circuit_open_ = false;
   ```

4. **Metrics**: 요청 수, 실패율, 평균 응답 시간 수집
   ```cpp
   std::atomic<uint64_t> total_requests_{0};
   std::atomic<uint64_t> total_failures_{0};
   ```

**현재 Phase 1 설계**:
- 기본적인 직렬화만 구현 (최소 기능)
- 확장은 필요 시 점진적으로 추가

---

## 2. AccountManager

### 2.1 목적

**문제**:
- 단일 마켓: `RealOrderEngine`이 `core::Account`를 직접 조작
- 멀티마켓: 여러 마켓이 동시에 잔고를 수정하면 race condition 발생
- 예약 없이 주문 제출 시 잔고 부족 주문 가능 (Upbit API 에러 발생)

**해결책**:
- 마켓별 독립적 자산 관리 (초기화 시 균등 배분)
- 예약 기반 할당: `reserve()` → `submitOrder()` → `finalizeFill*()` → `finalizeOrder()`
- Thread-safe: `shared_mutex`로 읽기 병렬, 쓰기 직렬화

### 2.2 전량 거래 모델

**핵심 원칙**:
```
불변 조건: (coin_balance > 0) XOR (available_krw > 0)
```

**상태 전이**:
```
Flat (100% KRW, 0 Coin)
    ↓ reserve + buy
PendingEntry (예약 KRW)
    ↓ fill
InPosition (0 KRW, 100% Coin)
    ↓ sell
PendingExit
    ↓ fill
Flat (100% KRW, 0 Coin)
```

**마켓 독립성**:
- 각 마켓은 할당 자본으로만 거래
- 마켓 간 자금 이동 없음 (rebalance 제거)
- 수익/손실은 각 마켓의 `realized_pnl`에 누적

**예시**:
```
초기 자본: 3,000,000원
마켓: ["KRW-BTC", "KRW-ETH", "KRW-XRP"]

초기 배분:
  KRW-BTC: 1,000,000원 (Flat)
  KRW-ETH: 1,000,000원 (Flat)
  KRW-XRP: 1,000,000원 (Flat)

KRW-BTC 매수 후:
  KRW-BTC: 0원, 0.01 BTC (InPosition)
  KRW-ETH: 1,000,000원 (Flat, 영향 없음)
  KRW-XRP: 1,000,000원 (Flat, 영향 없음)

KRW-BTC +10% 수익 후 매도:
  KRW-BTC: 1,100,000원 (Flat, realized_pnl = +100,000원)
  KRW-ETH: 1,000,000원 (Flat, 수익 공유 안 함)
  KRW-XRP: 1,000,000원 (Flat)
```

### 2.3 MarketBudget 구조

```cpp
struct MarketBudget {
    std::string market;             // 마켓 코드 (예: "KRW-BTC")

    // 현재 상태 (전량 거래: 둘 중 하나만 0이 아님)
    core::Amount available_krw{0};  // 거래 가능 KRW
    core::Amount reserved_krw{0};   // 예약된 KRW (주문 대기)
    core::Volume coin_balance{0};   // 보유 코인 수량
    core::Price avg_entry_price{0}; // 평균 매수 단가

    // 통계
    core::Amount initial_capital{0}; // 초기 자본 (고정, ROI 계산용)
    core::Amount realized_pnl{0};    // 실현 손익 누적
};
```

**계산 메서드**:
```cpp
// 현재 평가자산 (저장하지 않고 필요 시 계산)
core::Amount getCurrentEquity(core::Price current_price) const {
    return available_krw + reserved_krw + (coin_balance * current_price);
}

// 수익률 (%)
double getROI(core::Price current_price) const {
    if (initial_capital == 0) return 0.0;
    return (getCurrentEquity(current_price) - initial_capital) / initial_capital * 100.0;
}

// 실현 수익률 (%)
double getRealizedROI() const {
    if (initial_capital == 0) return 0.0;
    return realized_pnl / initial_capital * 100.0;
}
```

### 2.4 ReservationToken (RAII 패턴)

**목적**: KRW 예약을 나타내는 move-only 토큰

**생명주기**:
```
1. reserve() 성공 → 토큰 생성
2. 주문 제출 후 체결 대기
3. finalizeFillBuy() 호출 (부분 체결 시 여러 번)
4. finalizeOrder() 호출 → 토큰 비활성화
   또는
   소멸자 호출 → 미사용 금액 자동 해제 (안전망)
```

**안전망**:
```cpp
~ReservationToken() {
    // active 상태로 파괴되면 자동 해제
    if (active_ && manager_ != nullptr) {
        manager_->releaseWithoutToken(market_, amount_ - consumed_);
    }
}
```

**주요 메서드**:
```cpp
const std::string& market() const noexcept;  // 마켓 코드
core::Amount amount() const noexcept;        // 총 예약 금액
core::Amount consumed() const noexcept;      // 사용된 금액
core::Amount remaining() const noexcept;     // 남은 금액
bool isActive() const noexcept;              // 활성 상태
```

**사용 예시**:
```cpp
// 1. 예약
auto token = account_mgr.reserve("KRW-BTC", 100'000);
if (!token) {
    // 잔액 부족 또는 마켓 미등록
    return;
}

// 2. 주문 제출
auto result = api.postOrder(OrderRequest{...});

// 3. 체결 (부분 체결 가능)
account_mgr.finalizeFillBuy(*token, 50'000, 0.001, 50'000'000);
account_mgr.finalizeFillBuy(*token, 50'000, 0.001, 50'000'000);

// 4. 주문 완료
account_mgr.finalizeOrder(std::move(*token));
// 토큰 소멸 시 미사용 금액 자동 해제 (안전망)
```

### 2.5 주요 메서드 상세

#### 생성자
```cpp
AccountManager(const core::Account& account,
               const std::vector<std::string>& markets);
```

**동작**:
1. 마켓별 예산 초기화 (0으로)
2. 실제 계좌의 코인 포지션 반영 (`account.positions`)
   - Dust 체크: 가치 기준 (5,000원 미만 무시)
   - 코인 보유 시: `coin_balance > 0`, `available_krw = 0`
3. 남은 KRW를 코인 없는 마켓에 균등 배분
   - `per_market = krw_free / markets_without_coin`

**예시**:
```
account.krw_free = 2,000,000원
account.positions = [
    { currency: "BTC", free: 0.01, avg_buy_price: 100,000,000 }
]
markets = ["KRW-BTC", "KRW-ETH", "KRW-XRP"]

결과:
  KRW-BTC: coin_balance=0.01, available_krw=0, initial_capital=1,000,000
  KRW-ETH: coin_balance=0, available_krw=1,000,000, initial_capital=1,000,000
  KRW-XRP: coin_balance=0, available_krw=1,000,000, initial_capital=1,000,000
```

#### reserve()
```cpp
std::optional<ReservationToken> reserve(std::string_view market,
                                        core::Amount krw_amount);
```

**동작**:
1. 입력 검증: `krw_amount <= 0` → 실패
2. 마켓 존재 확인 → 없으면 실패
3. 잔액 확인: `available_krw < krw_amount` → 실패
4. 예약 적용:
   - `available_krw -= krw_amount`
   - `reserved_krw += krw_amount`
5. 토큰 생성 및 반환

**실패 조건**:
- 마켓 미등록
- 잔액 부족
- 0 이하 금액 (로직 오류 감지)

#### finalizeFillBuy()
```cpp
void finalizeFillBuy(ReservationToken& token,
                     core::Amount executed_krw,
                     core::Volume received_coin,
                     core::Price fill_price);
```

**동작**:
1. 입력 검증: `executed_krw <= 0` 또는 `received_coin <= 0` → 무시
2. 예약 초과 체크: `executed_krw > token.remaining()` → clamp
3. `reserved_krw -= executed_krw`
4. 평균 매수가 재계산 (가중 평균):
   ```cpp
   new_avg = (old_balance * old_price + received_coin * fill_price)
             / (old_balance + received_coin)
   ```
5. `coin_balance += received_coin`
6. `token.consumed += executed_krw`

**부분 체결 지원**:
```cpp
// 100,000원 예약
auto token = reserve("KRW-BTC", 100'000);

// 1차 체결: 50,000원
finalizeFillBuy(token, 50'000, 0.0005, 100'000'000);
// token.consumed = 50,000, remaining = 50,000

// 2차 체결: 50,000원
finalizeFillBuy(token, 50'000, 0.0005, 100'000'000);
// token.consumed = 100,000, remaining = 0
```

#### finalizeFillSell()
```cpp
void finalizeFillSell(std::string_view market,
                      core::Volume sold_coin,
                      core::Amount received_krw);
```

**동작**:
1. 입력 검증: `sold_coin <= 0` 또는 `received_krw <= 0` → 무시
2. `coin_balance -= sold_coin`
3. **과매도 감지 및 보정**:
   ```cpp
   if (coin_balance < 0) {
       // 실제 매도 가능량 계산
       actual_sold = balance_before;
       oversold = sold_coin - actual_sold;

       // 과매도분 KRW 차감 (비율 계산)
       actual_received_krw = (received_krw / sold_coin) * actual_sold;

       coin_balance = 0;
   }
   ```
4. `available_krw += actual_received_krw`
5. **Dust 이중 체크** (아래 참조)
6. Dust 처리 시:
   - `coin_balance = 0`
   - `avg_entry_price = 0`
   - `realized_pnl = available_krw - initial_capital`

**과매도 시나리오**:
```
보유: 0.001 BTC
매도 시도: 0.002 BTC → 200,000원

과매도 감지:
  actual_sold = 0.001 BTC
  oversold = 0.001 BTC
  actual_received_krw = 200,000 * (0.001 / 0.002) = 100,000원

결과:
  coin_balance = 0
  available_krw += 100,000원 (과매도분 KRW는 받지 않음)
```

**원인**:
- 중복 체결 이벤트 (RealOrderEngine의 `seen_trades_`로 1차 방어)
- 외부 거래 미동기화
- 로직 오류

#### finalizeOrder()
```cpp
void finalizeOrder(ReservationToken&& token);
```

**동작**:
1. 미사용 잔액 복구: `available_krw += token.remaining()`
2. `reserved_krw -= token.remaining()`
3. KRW Dust 정리:
   - `reserved_krw < krw_dust_threshold (10원)` → `available_krw`로 이동
   - formatDecimalFloor로 인한 원 단위 이하 잔량 처리
4. 토큰 비활성화

#### syncWithAccount()
```cpp
void syncWithAccount(const core::Account& account);
```

**목적**: 물리 계좌와 동기화 (API 조회 결과 반영)

**동작**:
1. **1단계: 전체 리셋**
   - 모든 마켓의 `coin_balance = 0`, `avg_entry_price = 0`
   - 외부 거래로 사라진 포지션 감지
2. **2단계: 포지션 설정**
   - `account.positions`에 있는 코인만 설정
   - Dust 체크: 가치 기준 (5,000원 미만 무시)
   - 코인 보유 시: `available_krw = 0`, `reserved_krw = 0`
3. **3단계: KRW 배분**
   - 코인 없는 마켓 식별 (`coin_balance < coin_epsilon`)
   - 실제 KRW 균등 분배: `per_market = account.krw_free / krw_markets.size()`

**외부 거래 대응**:
```
초기 상태:
  KRW-BTC: 0.01 BTC (InPosition)
  KRW-ETH: 0.1 ETH (InPosition)

외부 앱에서 BTC 전량 매도 → positions에 없음

syncWithAccount() 호출 시:
  1단계: 모든 마켓 리셋
    KRW-BTC: coin_balance = 0
    KRW-ETH: coin_balance = 0

  2단계: positions 적용
    KRW-BTC: 여전히 0 (positions에 없음)
    KRW-ETH: 0.1 ETH (복구)

  3단계: KRW 배분
    KRW-BTC: available_krw = account.krw_free (Flat 전환)
```

**사용 시나리오**:
- 프로그램 재시작 시 실제 계좌 상태 복구
- 외부 수동 거래 후 동기화
- AccountManager와 실제 계좌 불일치 해소

### 2.6 Dust 처리 정책

**이중 체크 원칙**:
- AccountManager와 전략이 동일한 기준으로 "의미 있는 포지션" 판단
- 상태 불일치 방지

**설정값** (`Config.h`):
| 설정 | 값 | 용도 |
|------|-----|------|
| `coin_epsilon` | 1e-7 | 수량 기준 dust (부동소수점 오차) |
| `init_dust_threshold_krw` | 5,000원 | 가치 기준 dust (거래 불가 잔량) |
| `krw_dust_threshold` | 10원 | KRW dust (원 단위 이하) |
| `min_notional_krw` | 5,000원 | 최소 주문 금액 (전략) |

**사용 위치**:
- **생성자**: 가치 기준 (`init_dust_threshold_krw`)
- **syncWithAccount**: 가치 기준 (`init_dust_threshold_krw`)
- **finalizeFillSell**: 이중 체크 (`coin_epsilon` + `init_dust_threshold_krw`)
- **finalizeOrder**: KRW 기준 (`krw_dust_threshold`)
- **전략 hasMeaningfulPos**: 가치 기준 (`min_notional_krw`)

**Dust 체크 로직** (`finalizeFillSell`):
```cpp
bool should_clear_coin = false;

// 1차: 수량 기준 (부동소수점 오차)
if (coin_balance < cfg.coin_epsilon) {
    should_clear_coin = true;
}
// 2차: 가치 기준 (저가 코인 보호)
else {
    core::Amount remaining_value = coin_balance * avg_entry_price;
    if (remaining_value < cfg.init_dust_threshold_krw) {
        should_clear_coin = true;
    }
}

if (should_clear_coin) {
    coin_balance = 0;
    avg_entry_price = 0;
    realized_pnl = available_krw - initial_capital;
}
```

**예시**:
```
1차 (수량 기준):
  coin_balance = 0.00000001 BTC
  → coin_balance < 1e-7 → dust 처리

2차 (가치 기준):
  coin_balance = 1,000 DOGE
  avg_entry_price = 3원
  remaining_value = 3,000원
  → remaining_value < 5,000원 → dust 처리
```

**전략 일관성**:
```cpp
// RsiMeanReversionStrategy::hasMeaningfulPos()
bool hasMeaningfulPos() const {
    if (state_ != State::InPosition) return false;
    double posNotional = position_size_ * position_entry_price_;
    return posNotional >= kMinNotionalKrw;  // 5,000원
}

// AccountManager::finalizeFillSell()
if (remaining_value < cfg.init_dust_threshold_krw) {  // 5,000원
    should_clear_coin = true;
}
```

### 2.7 Thread-Safety

**Shared Mutex 패턴**:
```cpp
mutable std::shared_mutex mtx_;

// 조회 메서드: shared_lock (읽기 병렬)
std::optional<MarketBudget> getBudget(std::string_view market) const {
    std::shared_lock lock(mtx_);  // 여러 스레드 동시 읽기 가능
    return budgets_.find(market)->second;
}

// 변경 메서드: unique_lock (쓰기 직렬)
std::optional<ReservationToken> reserve(...) {
    std::unique_lock lock(mtx_);  // 단독 접근, 다른 읽기/쓰기 차단
    // ...
}
```

**통계 카운터**: Lock-free atomic
```cpp
struct Stats {
    std::atomic<uint64_t> total_reserves{0};
    std::atomic<uint64_t> total_releases{0};
    std::atomic<uint64_t> total_fills_buy{0};
    std::atomic<uint64_t> total_fills_sell{0};
    std::atomic<uint64_t> reserve_failures{0};
};
```

### 2.8 주문 흐름

**매수 흐름**:
```
1. Strategy: 매수 신호 생성
    ↓
2. AccountManager.reserve(krw_amount)
    → available_krw -= krw_amount
    → reserved_krw += krw_amount
    → ReservationToken 생성
    ↓
3. SharedOrderApi.postOrder(BUY)
    → REST API POST /v1/orders
    ↓
4. WebSocket: myOrder fill 이벤트 수신 (부분 체결 가능)
    ↓
5. AccountManager.finalizeFillBuy(token, executed_krw, received_coin)
    → reserved_krw -= executed_krw
    → coin_balance += received_coin
    → avg_entry_price 재계산 (가중 평균)
    → token.consumed += executed_krw
    ↓ (모든 체결 완료)
6. AccountManager.finalizeOrder(token)
    → available_krw += token.remaining()
    → reserved_krw dust 정리
    → token.deactivate()
```

**매도 흐름**:
```
1. Strategy: 매도 신호 생성 (익절/손절)
    ↓
2. SharedOrderApi.postOrder(SELL)
    → REST API POST /v1/orders
    ↓
3. WebSocket: myOrder fill 이벤트 수신 (부분 체결 가능)
    ↓
4. AccountManager.finalizeFillSell(market, sold_coin, received_krw)
    → coin_balance -= sold_coin
    → 과매도 감지 및 보정
    → available_krw += actual_received_krw
    → Dust 이중 체크
    → realized_pnl 갱신 (Dust 처리 시)
    ↓
5. Strategy: 상태 전환 (InPosition → Flat)
```

---

## 3. 멀티마켓 통합: MarketEngine

### 3.1 개요

**MarketEngine**: 마켓별 독립 주문 엔진
- SharedOrderApi와 AccountManager를 사용하는 RealOrderEngine의 멀티마켓 버전
- IOrderEngine을 구현하지 않음 (반환 타입 다름)

### 3.2 아키텍처

```
┌─────────────────────────────────────────────────────────────────┐
│  Main Thread (EngineRunner or MarketManager)                    │
└─────────────────────────────────────────────────────────────────┘
                          │
                          ▼
        ┌─────────────────────────────────────────┐
        │   Shared Services (Thread-Safe)         │
        │  - SharedOrderApi (mutex)               │
        │  - AccountManager (shared_mutex)        │
        │  - OrderStore (shared_mutex)            │
        └─────────────────────────────────────────┘
                          │
          ┌───────────────┼───────────────┐
          │               │               │
          ▼               ▼               ▼
    ┌──────────┐    ┌──────────┐    ┌──────────┐
    │ Market   │    │ Market   │    │ Market   │
    │ Engine   │    │ Engine   │    │ Engine   │
    │ (BTC)    │    │ (ETH)    │    │ (XRP)    │
    └──────────┘    └──────────┘    └──────────┘
         │               │               │
         ▼               ▼               ▼
    ┌──────────┐    ┌──────────┐    ┌──────────┐
    │Strategy  │    │Strategy  │    │Strategy  │
    │ (RSI)    │    │ (RSI)    │    │ (RSI)    │
    └──────────┘    └──────────┘    └──────────┘
```

### 3.3 MarketEngine vs RealOrderEngine

| 항목 | RealOrderEngine | MarketEngine |
|------|----------------|--------------|
| **API 클라이언트** | UpbitExchangeRestClient (직접 소유) | SharedOrderApi (참조) |
| **잔고 관리** | Account (직접 조작) | AccountManager (예약 기반) |
| **Thread-Safety** | 단일 스레드 소유권 | SharedOrderApi/AccountManager 보호 |
| **예약 메커니즘** | 없음 | ReservationToken (RAII) |
| **마켓 스코프** | 단일 마켓 가정 | 자기 마켓 이벤트만 처리 |
| **중복 매수 방지** | 없음 | active_buy_token_ 존재 시 거부 |

### 3.4 주요 차이점

#### 주문 제출 (BUY)
```cpp
// RealOrderEngine
EngineResult RealOrderEngine::submit(const OrderRequest& req) {
    // 잔고 체크 없음 (전략이 알아서 함)
    auto result = api_.postOrder(req);
    // ...
}

// MarketEngine
EngineResult MarketEngine::submit(const OrderRequest& req) {
    // 1. 중복 매수 방지
    if (active_buy_token_.has_value()) {
        return Failure("already has active buy order");
    }

    // 2. 예약 금액 계산
    core::Amount reserve_amount = computeReserveAmount(req);

    // 3. KRW 예약
    auto token = account_mgr_.reserve(market_, reserve_amount);
    if (!token) {
        return Failure("insufficient KRW");
    }

    // 4. 주문 제출
    auto result = api_.postOrder(req);
    if (!std::holds_alternative<std::string>(result)) {
        account_mgr_.release(std::move(*token));  // 실패 시 해제
        return Failure("API error");
    }

    // 5. 토큰 저장
    active_buy_token_ = std::move(token);
    active_buy_order_id_ = std::get<std::string>(result);

    return Success(active_buy_order_id_);
}
```

#### 체결 처리 (onMyTrade)
```cpp
// RealOrderEngine
void RealOrderEngine::onMyTrade(const MyTrade& t) {
    // Account 직접 수정
    if (t.side == Side::Bid) {
        account_.krw_free -= executed_krw;
        // ...
    }
}

// MarketEngine
void MarketEngine::onMyTrade(const MyTrade& t) {
    // 1. 마켓 스코프 검증
    if (t.market != market_) {
        return;  // 다른 마켓 이벤트 무시
    }

    // 2. 중복 방지
    if (!markTradeOnce(makeTradeDedupeKey_(t))) {
        return;
    }

    // 3. AccountManager 호출
    if (t.side == Side::Bid) {
        if (!active_buy_token_) return;
        account_mgr_.finalizeFillBuy(*active_buy_token_, executed_krw, ...);
    } else {
        account_mgr_.finalizeFillSell(market_, sold_coin, received_krw);
    }

    // 4. OrderStore 업데이트
    store_.updateFill(t);
}
```

#### 주문 완료 (터미널 상태)
```cpp
void MarketEngine::onOrderSnapshot(const Order& snapshot) {
    // 터미널 상태: done, cancelled
    if (snapshot.status == OrderStatus::Done ||
        snapshot.status == OrderStatus::Cancelled) {

        // 매수 토큰 정리 (미사용 KRW 복구)
        if (snapshot.side == Side::Bid) {
            finalizeBuyToken_(snapshot.id);
        }
    }
}

void MarketEngine::finalizeBuyToken_(std::string_view order_id) {
    if (!active_buy_token_) return;
    if (active_buy_order_id_ != order_id) return;  // 검증

    // 미사용 금액 복구 및 토큰 비활성화
    account_mgr_.finalizeOrder(std::move(*active_buy_token_));
    active_buy_token_.reset();
    active_buy_order_id_.clear();
}
```

### 3.5 단일 소유권 모델

**bindToCurrentThread()**:
```cpp
void MarketEngine::bindToCurrentThread() {
    owner_thread_ = std::this_thread::get_id();
}

void MarketEngine::assertOwner_() const {
    if (std::this_thread::get_id() != owner_thread_) {
        std::terminate();  // 다른 스레드에서 호출 시 즉시 종료
    }
}

// 모든 public 메서드에서 호출
EngineResult MarketEngine::submit(const OrderRequest& req) {
    assertOwner_();  // 소유자 검증
    // ...
}
```

**목적**:
- MarketEngine 자체는 thread-safe하지 않음
- SharedOrderApi/AccountManager가 thread-safety 제공
- 각 MarketEngine은 단일 스레드(마켓 스레드)에서만 호출

---

## 4. 데이터 플로우

### 4.1 초기화 플로우

```
1. main() or EngineRunner::setup()
    ↓
2. SharedOrderApi 생성 (UpbitExchangeRestClient 소유)
    ↓
3. getMyAccount() → 실제 계좌 조회
    ↓
4. AccountManager 생성 (account, markets)
    → 초기 자본 배분 (KRW 균등, 코인 포지션 반영)
    ↓
5. MarketEngine 생성 (market, api, account_mgr)
    → 마켓별 독립 엔진 인스턴스
    ↓
6. bindToCurrentThread() → 소유권 바인딩
    ↓
7. Strategy 초기화 (각 MarketEngine에 연결)
```

### 4.2 매수 플로우

```
WebSocket (Candle) → MarketEngine::onCandle()
    ↓
Strategy::decide(candle) → Decision::Buy(size)
    ↓
MarketEngine::submit(BUY OrderRequest)
    ↓ ┌──────────────────────────┐
    │ 1. active_buy_token_ 체크 │ (중복 매수 방지)
    │ 2. computeReserveAmount()  │ (예약 금액 계산)
    │ 3. account_mgr_.reserve()  │ (KRW 예약)
    └──────────────────────────┘
    ↓
SharedOrderApi::postOrder(req) [mutex 보호]
    ↓
UpbitExchangeRestClient::postOrder()
    ↓
REST API: POST /v1/orders
    ↓ (주문 접수)
OrderStore::insert(order)
    ↓
active_buy_token_ 저장
    ↓
WebSocket (myOrder) → fill 이벤트
    ↓
MarketEngine::onMyTrade(MyTrade)
    ↓ ┌──────────────────────────────────┐
    │ 1. 마켓 스코프 검증 (market == ?) │
    │ 2. 중복 방어 (markTradeOnce)      │
    │ 3. finalizeFillBuy()              │
    │ 4. OrderStore::updateFill()       │
    └──────────────────────────────────┘
    ↓
AccountManager::finalizeFillBuy() [unique_lock]
    → reserved_krw -= executed_krw
    → coin_balance += received_coin
    → avg_entry_price 재계산
    → token.consumed += executed_krw
    ↓
Strategy 상태 전환: PendingEntry → InPosition
    ↓
(주문 완료 시)
MarketEngine::finalizeBuyToken_()
    → AccountManager::finalizeOrder()
    → active_buy_token_.reset()
```

### 4.3 매도 플로우

```
WebSocket (Candle) → MarketEngine::onCandle()
    ↓
Strategy::decide() → Decision::Sell(size)
    ↓
MarketEngine::submit(SELL OrderRequest)
    ↓
SharedOrderApi::postOrder(req) [mutex 보호]
    ↓
REST API: POST /v1/orders
    ↓
OrderStore::insert(order)
    ↓
WebSocket (myOrder) → fill 이벤트
    ↓
MarketEngine::onMyTrade(MyTrade)
    ↓ ┌──────────────────────────────────┐
    │ 1. 마켓 스코프 검증               │
    │ 2. 중복 방어                      │
    │ 3. finalizeFillSell()             │
    │ 4. OrderStore::updateFill()       │
    └──────────────────────────────────┘
    ↓
AccountManager::finalizeFillSell() [unique_lock]
    → coin_balance -= sold_coin
    → 과매도 감지 및 보정
    → available_krw += actual_received_krw
    → Dust 이중 체크
    → realized_pnl 갱신
    ↓
Strategy 상태 전환: InPosition → Flat
```

### 4.4 동기화 플로우

```
프로그램 재시작 or 수동 동기화 요청
    ↓
SharedOrderApi::getMyAccount() [mutex 보호]
    ↓
REST API: GET /v1/accounts
    ↓
AccountManager::syncWithAccount(account) [unique_lock]
    ↓ ┌──────────────────────────────────────┐
    │ 1단계: 모든 마켓 코인 잔고 리셋      │
    │ 2단계: account.positions 반영        │
    │ 3단계: KRW 균등 배분 (Flat 마켓만)  │
    └──────────────────────────────────────┘
    ↓
각 MarketEngine의 Strategy 상태 복구
    → coin_balance > 0 → InPosition
    → coin_balance == 0 → Flat
```

---

## 5. 테스트 전략

### 5.1 SharedOrderApi 테스트

**test_shared_order_api_advanced.cpp**:

1. **동시성 테스트**:
   - 여러 스레드에서 동시에 `postOrder()` 호출
   - `max_in_flight_` 카운터로 직렬화 검증 (항상 1)

2. **Rate Limit 시뮬레이션**:
   - 빠른 연속 요청 시 Upbit 429 응답 처리

3. **에러 처리**:
   - REST API 실패 시 `RestError` 반환 확인
   - 네트워크 타임아웃 처리

### 5.2 AccountManager 테스트

**test_account_manager_unified.cpp** (23개 테스트):

#### 기본 기능
- **TEST 1-3**: 초기화 (KRW 균등 배분, 코인 포지션 반영)
- **TEST 4-5**: 예약/해제 사이클
- **TEST 6-7**: 부분 체결 누적 (가중 평균 단가)

#### Dust 처리
- **TEST 8**: 저가 코인 finalizeFillSell dust 처리
- **TEST 9**: 고가 코인 정상 잔량 유지

#### 엣지 케이스
- **TEST 10-11**: 입력 검증 (0 이하 금액, reserve 실패)
- **TEST 12**: finalizeFillSell 과매도 감지 및 보정
- **TEST 13**: syncWithAccount 포지션 사라짐 (외부 거래)

#### 멀티스레드
- **TEST 14**: 동시 예약 (여러 스레드 → 각 마켓)
- **TEST 15**: 동시 조회 (shared_lock 병렬 읽기)

#### 통합 시나리오
- **TEST 16-20**: 매수 → 체결 → 매도 → 체결 전체 사이클
- **TEST 21-23**: syncWithAccount 다양한 시나리오

### 5.3 MarketEngine 테스트

**TODO**: 향후 추가 예정

1. **중복 매수 방지**:
   - active_buy_token_ 존재 시 두 번째 매수 거부

2. **마켓 스코프 검증**:
   - 다른 마켓의 MyTrade 이벤트 무시

3. **토큰 정리**:
   - 주문 취소 시 KRW 자동 복구
   - 터미널 상태 도달 시 finalizeOrder() 호출

---

## 6. 설계 결정 및 트레이드오프

### 6.1 SharedOrderApi: 읽기 병렬화 미적용

**현재**: 모든 메서드를 `std::mutex`로 직렬화

**대안**: `shared_mutex`로 읽기 전용 API 병렬화
```cpp
std::shared_mutex mtx_;

// 읽기 전용: 병렬 가능
std::variant<core::Account, RestError> getMyAccount() {
    std::shared_lock lock(mtx_);  // 병렬 읽기
    return client_->getMyAccount();
}

// 쓰기 (주문 제출): 직렬화
std::variant<std::string, RestError> postOrder(const OrderRequest& req) {
    std::unique_lock lock(mtx_);  // 단독 접근
    return client_->postOrder(req);
}
```

**선택 이유**:
- Phase 1 목표: 최소 기능 구현
- 읽기 빈도 낮음 (주문 제출이 대부분)
- HTTP/1.1 단일 연결: 병렬화 이득 미미
- 추후 성능 측정 후 최적화

### 6.2 AccountManager: 예약 기반 할당

**문제**: 예약 없이 주문 시 잔고 부족 가능

**대안 1**: 주문 제출 후 Upbit API 에러로 처리
```cpp
auto result = api.postOrder(req);
if (std::holds_alternative<RestError>(result)) {
    // 잔고 부족 에러 처리
}
```

**대안 2**: 예약 기반 (현재 방식)
```cpp
auto token = account_mgr.reserve(krw_amount);
if (!token) {
    return Failure("insufficient KRW");
}
auto result = api.postOrder(req);
```

**선택 이유**:
- **로컬 검증**: API 호출 전에 잔고 부족 감지
- **멀티마켓 안전성**: 동시 주문 시 경쟁 조건 방지
- **명확한 자원 관리**: RAII 패턴으로 자동 해제
- **디버깅 용이**: 예약 실패 위치 명확

**트레이드오프**:
- 복잡도 증가: ReservationToken 관리 필요
- 메모리 오버헤드: 토큰 객체 (무시할 수준)

### 6.3 AccountManager: 전량 거래 모델

**대안 1**: 부분 거래 (예: 50% 매수, 30% 매도)
```cpp
MarketBudget {
    available_krw = 500,000원  // 50%
    coin_balance = 0.005 BTC   // 50%
}
```

**대안 2**: 전량 거래 (현재 방식)
```cpp
MarketBudget {
    // Flat: 100% KRW
    available_krw = 1,000,000원
    coin_balance = 0

    // InPosition: 100% Coin
    available_krw = 0
    coin_balance = 0.01 BTC
}
```

**선택 이유**:
- **단순성**: 상태가 명확 (Flat vs InPosition)
- **위험 관리**: 전부 or 아무것도 (All-or-Nothing)
- **일관성**: RsiMeanReversionStrategy와 자연스럽게 매칭
- **테스트 용이**: 불변 조건 검증 간단

**트레이드오프**:
- 유연성 감소: 분할 진입/청산 불가
- 추후 확장: 부분 거래 전략 지원 시 수정 필요

### 6.4 MarketEngine: IOrderEngine 미구현

**문제**: SharedOrderApi 반환 타입 `std::variant` vs IOrderEngine 예외 기반

**대안 1**: IOrderEngine 구현 (RealOrderEngine 호환)
```cpp
class IOrderEngine {
    virtual EngineResult submit(const OrderRequest& req) = 0;
};

// 내부에서 variant를 EngineResult로 변환
EngineResult MarketEngine::submit(const OrderRequest& req) {
    auto result = api_.postOrder(req);
    if (std::holds_alternative<RestError>(result)) {
        return Failure(std::get<RestError>(result).message);
    }
    return Success(std::get<std::string>(result));
}
```

**대안 2**: 독립 인터페이스 (현재 방식)
```cpp
// IOrderEngine을 구현하지 않음
class MarketEngine {
    EngineResult submit(const OrderRequest& req);
    // SharedOrderApi의 variant 반환 직접 처리
};
```

**선택 이유**:
- **Phase 1 범위**: 단일 마켓 유지 (EngineRunner + RealOrderEngine)
- **Phase 2 계획**: MarketManager 도입 시 인터페이스 재설계
- **타입 안전성**: variant 에러 정보 보존 (메시지만 전달하지 않음)

**트레이드오프**:
- RealOrderEngine과 호환 불가 (당장 필요 없음)
- Phase 2에서 통합 인터페이스 재설계 필요

---

## 7. 향후 개선 사항

### 7.1 SharedOrderApi 확장

1. **Rate Limiting**: 요청 간 최소 시간 간격
   - Upbit 초당 제한: 30 req/s (일반), 10 req/s (주문)
   - 구현: `std::chrono::steady_clock` + sleep

2. **Request Queueing**: 우선순위 큐
   - 긴급 주문 (취소, 손절) 우선 처리
   - 구현: `std::priority_queue` + worker thread

3. **Circuit Breaker**: 연속 실패 시 일시 중단
   - 5회 연속 실패 → 10초 대기 → 재시도
   - 구현: state machine (Closed/Open/HalfOpen)

4. **Metrics**: 성능 측정
   - 요청 수, 실패율, 평균 응답 시간
   - 구현: Prometheus 또는 자체 로깅

### 7.2 AccountManager 확장

1. **주기적 동기화**: 외부 거래 자동 감지
   - 현재: 재시작 시에만 syncWithAccount()
   - 개선: 주기적 (예: 5분마다) REST API 조회

2. **리밸런싱**: 마켓 간 자본 재분배
   - 현재: 마켓 간 독립 (수익 공유 안 함)
   - 개선: 일일 1회 수익/손실 기반 재분배

3. **외부 입금 처리**: 모든 마켓 포지션 보유 중 입금
   - 현재: 다음 매도 후 할당 (지연)
   - 개선: 예비금 풀 (reserve pool) 도입

4. **트랜잭션 로그**: 모든 자금 이동 기록
   - 현재: 메모리만 (재시작 시 소실)
   - 개선: SQLite 또는 파일 기반 WAL (Write-Ahead Log)

### 7.3 MarketEngine 통합

1. **MarketManager**: 마켓 엔진 관리자
   - 여러 MarketEngine 생성 및 생명주기 관리
   - 이벤트 라우팅: WebSocket → 각 MarketEngine

2. **통합 인터페이스**: IOrderEngine v2
   - RealOrderEngine과 MarketEngine 통합
   - 공통 테스트 스위트

3. **동적 마켓 추가/제거**: 런타임 마켓 변경
   - 현재: 초기화 시 고정
   - 개선: REST API로 마켓 활성화/비활성화

---

## 8. 참고 자료

### 8.1 관련 문서

- **프로젝트 구조**: [../docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md)
- **개발 로드맵**: [../docs/ROADMAP.md](../docs/ROADMAP.md)
- **구현 현황**: [../docs/IMPLEMENTATION_STATUS.md](../docs/IMPLEMENTATION_STATUS.md)
- **개발 가이드**: [../CLAUDE.md](../CLAUDE.md)

### 8.2 테스트 파일

- **SharedOrderApi**: `tests/test_shared_order_api_advanced.cpp`
- **AccountManager**: `tests/test_account_manager_unified.cpp`
- **MarketEngine**: (향후 추가 예정)

### 8.3 소스 파일

- **SharedOrderApi**: `src/api/upbit/SharedOrderApi.{h,cpp}`
- **AccountManager**: `src/trading/allocation/AccountManager.{h,cpp}`
- **MarketEngine**: `src/engine/MarketEngine.{h,cpp}`
- **Config**: `src/util/Config.h`

---

## 변경 이력

| 날짜 | 작성자 | 내용 |
|------|--------|------|
| 2026-02-08 | Claude | 초안 작성 |
