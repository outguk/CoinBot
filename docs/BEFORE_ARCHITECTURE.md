# CoinBot 아키텍처

## 개요

CoinBot은 C++20 기반의 실시간 암호화폐 자동매매 봇입니다. 업비트(Upbit) 거래소와 연동하여 RSI 평균회귀 전략을 실행합니다.

### 주요 기능

- **실시간 시장 데이터 수신**: WebSocket을 통한 캔들 및 틱커 데이터 스트림
- **자동 주문 실행**: REST API를 통한 매수/매도 주문 제출 및 관리
- **전략 기반 거래**: RSI 지표를 활용한 평균회귀 전략 구현
- **주문 체결 추적**: WebSocket myOrder 스트림을 통한 실시간 체결 이벤트 처리

### 기술 스택

- **언어**: C++20
- **빌드 시스템**: CMake (Ninja 생성기)
- **컴파일러**: MSVC (Visual Studio 2022)
- **주요 라이브러리**:
  - **Boost 1.89.0**: 비동기 I/O (ASIO), 스레딩
  - **OpenSSL**: TLS/HTTPS 암호화
  - **nlohmann JSON**: JSON 파싱

## 시스템 구조

### 전체 아키텍처 다이어그램

```
┌─────────────────────────────────────────────────────────────────┐
│                         Main Thread                              │
│  - CoinBot.cpp 진입점                                            │
│  - EngineRunner 초기화 및 실행                                    │
└─────────────────────────────────────────────────────────────────┘
           │
           ├──────────────────────────────────────────┐
           │                                          │
           ▼                                          ▼
┌─────────────────────┐                    ┌─────────────────────┐
│   WebSocket Thread  │                    │   WebSocket Thread  │
│      (Public)       │                    │      (Private)      │
│  UpbitWebSocketClient                    │  UpbitWebSocketClient
│  - Candle 데이터     │                    │  - myOrder 이벤트    │
│                     │                    │                     │
└─────────────────────┘                    └─────────────────────┘
           │                                          │
           │ MarketDataEventBridge                    │ MyOrderEventBridge
           └──────────────┬───────────────────────────┘
                          │
                          ▼
                ┌─────────────────────┐
                │   EngineRunner      │
                │  (Main Event Loop)  │
                │  - BlockingQueue    │
                └─────────────────────┘
                          │
                          ▼
                ┌─────────────────────┐
                │ RsiMeanReversion    │
                │     Strategy        │
                │  - Decision Engine  │
                └─────────────────────┘
                          │
                          ▼
                ┌─────────────────────┐
                │  RealOrderEngine    │
                │  - Order Submission │
                │  - OrderStore       │
                └─────────────────────┘
                          │
                          ▼
                ┌─────────────────────┐
                │ UpbitExchangeRestClient
                │  (HTTPS API)        │
                └─────────────────────┘
```

### 주요 컴포넌트

- **EngineRunner**: 메인 이벤트 루프, 시장 데이터 및 주문 이벤트 처리
- **RealOrderEngine**: 주문 실행 엔진, 단일 스레드 소유권 모델
- **RsiMeanReversionStrategy**: RSI 기반 평균회귀 전략
- **UpbitWebSocketClient**: WebSocket 클라이언트 (TLS, 자동 재연결)
- **UpbitExchangeRestClient**: REST API 클라이언트 (재시도 로직 포함)
- **EventBridge**: WebSocket 스레드와 메인 스레드 간 이벤트 전달

## 데이터 플로우

### 시장 데이터 플로우

```
1. UpbitWebSocketClient (candles 구독)
    ↓ JSON 메시지 수신
2. MarketDataEventBridge
    ↓ DTO 변환 및 큐 삽입
3. EngineRunner (main event loop)
    ↓ Candle
4. RsiMeanReversionStrategy
    ↓ 지표 계산 및 매매 신호 생성
5. RealOrderEngine
    ↓ 주문 생성 및 제출
6. UpbitExchangeRestClient
    ↓ HTTPS POST /v1/orders
7. Upbit 거래소
```

### 주문 체결 플로우

```
1. WebSocket (myOrder 스트림)
    ↓ 체결/취소 이벤트 수신
2. MyOrderEventBridge
    ↓ MyTrade/OrderSnapshot DTO 변환
3. EngineRunner
    ↓ onMyTrade/onOrderSnapshot 이벤트
4. RealOrderEngine
    ↓ OrderStore 업데이트
5. RsiMeanReversionStrategy
    ↓ 상태 머신 전환 (PendingEntry → InPosition 등)
```

### 자산 관리 플로우 (전량 거래 모델)

```
1. 전략이 매수 결정
    ↓
2. AccountManager.reserve(krw_amount)
    ↓ available_krw → reserved_krw
3. RealOrderEngine.submit(OrderRequest)
    ↓ REST API POST /v1/orders
4. WebSocket fill 이벤트 수신
    ↓ (부분 체결 가능, 여러 번)
5. AccountManager.finalizeFillBuy(token, executed_krw, received_coin)
    ↓ reserved_krw → 0, coin_balance += coin, avg_entry_price 재계산
6. AccountManager.finalizeOrder(token)
    ↓ 미사용 금액 복구, 토큰 비활성화

[포지션 보유 중]
    ↓ 익절/손절 신호
7. RealOrderEngine.submit(SELL)
    ↓ REST API POST /v1/orders
8. WebSocket fill 이벤트 수신
    ↓ (부분 체결 가능, 여러 번)
9. AccountManager.finalizeFillSell(market, sold_coin, received_krw)
    ↓ coin_balance → 0, available_krw += krw, realized_pnl 갱신

[다시 Flat 상태, 다음 신호 대기]
```

**전량 거래 상태 전이:**
```
Flat (100% KRW) → PendingEntry → InPosition (100% Coin)
                                      ↓
Flat (100% KRW) ← PendingExit ←──────┘
```

**마켓 독립성:**
- KRW-BTC가 수익을 내도 KRW-ETH로 이동하지 않음
- 각 마켓은 할당 자본으로 독립 운영
- 수익/손실은 해당 마켓의 `realized_pnl`에 누적

## 주요 모듈

### src/app/

**목적**: 애플리케이션 진입점 및 메인 이벤트 루프

| 파일 | 설명 |
|------|------|
| `CoinBot.cpp` | 메인 함수, 초기화 및 설정 로딩 |
| `EngineRunner.h/cpp` | 메인 이벤트 루프, 시장 데이터 및 주문 이벤트 처리 |
| `MarketDataEventBridge.h/cpp` | WebSocket → EngineRunner 시장 데이터 전달 |
| `MyOrderEventBridge.h/cpp` | WebSocket → EngineRunner 주문 이벤트 전달 |
| `StartupRecovery.h/cpp` | 재시작 시 미체결 주문 복구 |

### src/core/domain/

**목적**: 도메인 모델 (Value Objects)

| 파일 | 설명 |
|------|------|
| `Order.h` | 주문 정보 (order_id, market, price, volume, status) |
| `Candle.h` | 캔들 데이터 (OHLCV, timestamp) |
| `Account.h` | 계좌 잔고 (KRW, 코인 보유량) |
| `Ticker.h` | 틱커 정보 (현재가, 거래량) |
| `OrderBook.h` | 호가창 데이터 (매수/매도 호가) |
| `MyTrade.h` | 체결 내역 (trade_id, price, volume, fee) |
| `OrderTypes.h` | 주문 타입 enum (Position, OrderType, OrderStatus) |
| `Types.h` | 공통 타입 (Price, Volume, Timestamp) |

### src/api/rest/

**목적**: 범용 HTTPS 클라이언트

| 파일 | 설명 |
|------|------|
| `RestClient.h/cpp` | Boost.Beast 기반 HTTPS 클라이언트 |
| `RetryPolicy.h` | 재시도 정책 (지수 백오프) |
| `RestError.h/cpp` | HTTP 에러 처리 |
| `HttpTypes.h` | HTTP 요청/응답 타입 |

### src/api/ws/

**목적**: WebSocket 클라이언트

| 파일 | 설명 |
|------|------|
| `UpbitWebSocketClient.h/cpp` | Boost.Beast WebSocket (TLS, 자동 재연결) |

### src/api/upbit/

**목적**: 업비트 거래소 API 클라이언트

| 파일 | 설명 |
|------|------|
| `UpbitPublicRestClient.h/cpp` | 공개 API (시세 조회) |
| `UpbitExchangeRestClient.h/cpp` | 거래 API (주문, 잔고 조회) |
| `SharedOrderApi.h/cpp` | Thread-safe REST 래퍼 (멀티마켓용, Phase 1) |
| `dto/UpbitQuotationDtos.h` | 시세 API 응답 DTO |
| `dto/UpbitAssetOrderDtos.h` | 주문 API 응답 DTO |
| `dto/UpbitWsDtos.h` | WebSocket 메시지 DTO |

### src/api/upbit/mappers/

**목적**: DTO → 도메인 모델 변환

| 파일 | 설명 |
|------|------|
| `CandleMapper.h` | DTO → Candle 변환 |
| `TickerMapper.h` | DTO → Ticker 변환 |
| `MyOrderMapper.h` | DTO → MyTrade 변환 |
| `AccountMapper.h` | DTO → Account 변환 |
| `OrderbookMapper.h` | DTO → OrderBook 변환 |
| `MarketMapper.h` | DTO → MarketInfo 변환 |
| `OpenOrdersMapper.h` | DTO → Order 변환 |
| `TimeFrameMapper.h` | 문자열 → TimeFrame enum 변환 |

### src/api/auth/

**목적**: API 인증

| 파일 | 설명 |
|------|------|
| `UpbitJwtSigner.h/cpp` | JWT 토큰 생성 (HMAC-SHA512) |

### src/engine/

**목적**: 주문 실행 엔진

| 파일 | 설명 |
|------|------|
| `RealOrderEngine.h/cpp` | 주문 제출 및 상태 추적 |
| `OrderStore.h/cpp` | 주문 데이터 저장소 (in-memory) |
| `IOrderEngine.h` | 주문 엔진 인터페이스 |
| `EngineResult.h` | 주문 결과 타입 (Success/Failure) |
| `EngineEvents.h` | 엔진 이벤트 (Fill, OrderStatus) |

### src/engine/upbit/

**목적**: 업비트 전용 주문 API

| 파일 | 설명 |
|------|------|
| `UpbitPrivateOrderApi.h/cpp` | REST API 호출 래퍼 (submit, cancel, getOrder) |

### src/trading/strategies/

**목적**: 거래 전략

| 파일 | 설명 |
|------|------|
| `RsiMeanReversionStrategy.h/cpp` | RSI 평균회귀 전략 (상태 머신) |
| `StrategyTypes.h` | 전략 결과 타입 (Decision) |

### src/trading/allocation/

**목적**: 멀티마켓 자산 관리

| 파일 | 설명 |
|------|------|
| `AccountManager.h/cpp` | 마켓별 자산 할당 및 예약 관리 (thread-safe) |

#### 전량 거래 모델 (Full-Trade Model)

CoinBot은 각 마켓이 할당된 자본 **전체**로 매수 → 매도를 반복하는 방식을 사용합니다.

**핵심 원칙:**
```
불변 조건: (coin_balance > 0) XOR (available_krw > 0)
```

- **Flat 상태**: `available_krw = 100%`, `coin_balance = 0`
- **InPosition 상태**: `coin_balance > 0`, `available_krw ≈ 0`
- **마켓 간 독립**: 자본 이동 없음, 수익/손실은 각 마켓에 누적

#### 주요 기능

1. **초기 자본 배분** (1회, 생성자)
   - 실제 계좌의 `krw_free`를 마켓에 균등 배분
   - 기존 코인 포지션 반영
   - Dust 처리: 가치 기준 (5,000원 미만 무시)

2. **예약 기반 할당**
   ```
   reserve(krw) → submitOrder → finalizeFillBuy → finalizeOrder
        ↓ (실패/취소)
     release()
   ```

3. **체결 정산**
   - `finalizeFillBuy()`: 부분 체결 누적, 가중 평균 단가 계산
   - `finalizeFillSell()`: 코인 → KRW 전환, 실현 손익 추적

4. **Dust 이중 체크**
   - 1차: 수량 기준 (`coin_epsilon = 1e-7`) - 부동소수점 오차
   - 2차: 가치 기준 (`init_dust_threshold_krw = 5,000원`) - 거래 불가 잔량
   - 전략 일관성: `RsiMeanReversionStrategy`도 동일 기준 사용

5. **물리 계좌 동기화**
   - `syncWithAccount()`: REST API 조회 결과 반영
   - 외부 거래 감지: 전체 리셋 → API 응답 적용
   - 재시작 시 실제 계좌 상태 복구

#### ReservationToken (RAII 패턴)

```cpp
// 예약 생성
auto token = account_mgr.reserve("KRW-BTC", 100'000);

// 체결 시
account_mgr.finalizeFillBuy(token, 50'000, 0.001, 50'000'000);

// 주문 완료
account_mgr.finalizeOrder(std::move(token));
// 토큰 소멸 시 미사용 금액 자동 해제 (안전망)
```

#### Thread-Safety 보장

- **shared_mutex**: 읽기 병렬, 쓰기 직렬화
- **조회 메서드** (getBudget, snapshot): `shared_lock` (병렬 가능)
- **변경 메서드** (reserve, finalize*): `unique_lock` (직렬화)
- **통계 카운터**: `std::atomic` (lock-free)

#### 테스트 커버리지

`tests/test_account_manager_unified.cpp` (23개 테스트):
- 기본 초기화 (KRW 균등 배분)
- 코인 포지션 반영
- Dust 처리 (초기화, 매도 시)
- 예약/해제 사이클
- 부분 체결 누적 (가중 평균)
- 동시 예약 (멀티스레드)
- 과매도 감지 및 보정
- syncWithAccount() (외부 거래 대응)
- 입력 검증 (0 이하 금액)

#### 외부 입금 처리 정책

**전량 거래 모델의 제약:**
- 각 마켓은 100% 코인 또는 100% KRW 상태만 유지
- 모든 마켓이 포지션 보유 중일 때 외부 입금 시 즉시 할당 불가

**시나리오별 동작:**

| 시나리오 | 동작 | 비고 |
|---------|------|------|
| **일부 마켓 Flat + 외부 입금** | Flat 마켓에 균등 배분 ✅ | 정상 동작 |
| **모든 마켓 포지션 보유 + 외부 입금** | KRW 할당 지연 ⏳ | 다음 매도 후 할당 |
| **실행 중 외부 입금** | 다음 재시작 시 반영 | 주기적 동기화 없음 (Phase 1) |

**모든 마켓 포지션 보유 중 외부 입금 시:**
1. 외부 입금 KRW는 AccountManager에 즉시 할당되지 않음
2. 어떤 마켓이라도 매도 완료 → Flat 전환
3. 다음 `syncWithAccount()` 호출 시 해당 Flat 마켓에 KRW 할당
4. KRW는 거래소 계좌에 유지되며 유실되지 않음

**사용자 권장 사항:**
- 외부 입금 후 최소 1개 마켓을 수동 매도하여 Flat 전환
- 또는 프로그램 재시작 전 모든 포지션 정리 후 입금
- Phase 2에서 주기적 동기화로 개선 예정

**예시:**
```
초기 상태:
  KRW-BTC: 0.01 BTC (InPosition)
  KRW-ETH: 0.1 ETH (InPosition)
  KRW-XRP: 1000 XRP (InPosition)
  KRW 잔고: 0원

외부 입금: +500,000원
  → AccountManager는 아직 인식하지 않음
  → 거래소 계좌 KRW: 500,000원

KRW-XRP 매도 완료:
  KRW-XRP: Flat (0 XRP)
  KRW 잔고: 500,000원 + 매도대금

다음 재시작 시:
  syncWithAccount() 호출
  → KRW-XRP에 전체 KRW 할당 (500,000원 + 매도대금)
  → KRW-BTC, KRW-ETH는 여전히 포지션 보유
```

### src/trading/indicators/

**목적**: 기술적 지표

| 파일 | 설명 |
|------|------|
| `RsiWilder.h/cpp` | Wilder의 RSI (Relative Strength Index) |
| `Sma.h/cpp` | 단순 이동평균 (Simple Moving Average) |
| `ChangeVolatilityIndicator.h/cpp` | 변동성 지표 |
| `ClosePriceWindow.h/cpp` | 종가 윈도우 (링 버퍼) |
| `RingBuffer.h` | 고정 크기 링 버퍼 |
| `IndicatorTypes.h` | 지표 공통 타입 |

### src/util/

**목적**: 유틸리티

| 파일 | 설명 |
|------|------|
| `BlockingQueue.h` | 스레드 안전 블로킹 큐 (생산자-소비자 패턴) |
| `ThreadSafeRingBuffer.h` | 스레드 안전 링 버퍼 |
| `Logger.h` | 로깅 유틸리티 (info, warn, error 레벨) |
| `Config.h` | 설정 상수 관리 (kMinNotionalKrw, 타임아웃 등) |

## 스레딩 모델

CoinBot은 멀티스레드 아키텍처를 사용하여 네트워크 I/O와 거래 로직을 분리합니다.

### 스레드 구조

```
┌─────────────────────────────────────────────────────────────────┐
│  Main Thread                                                     │
│  - 초기화                                                         │
│  - EngineRunner::run() 메인 루프                                 │
│  - BlockingQueue에서 이벤트 소비                                  │
│  - Strategy 실행 및 RealOrderEngine 호출                         │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  ASIO Thread  (현재는 존재 x)                                    │
│  - io_context.run()                                             │
│  - 네트워크 I/O 처리 (타이머, 비동기 소켓 작업)                      │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  WebSocket Thread 1 (Public)                                     │
│  - UpbitWebSocketClient::connect()                               │
│  - Candle/Ticker 메시지 수신                                     │
│  - MarketDataEventBridge::onCandle() 호출                        │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  WebSocket Thread 2 (Private)                                    │
│  - UpbitWebSocketClient::connect()                               │
│  - myOrder 메시지 수신                                           │
│  - MyOrderEventBridge::onMyOrder() 호출                          │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  Shared Services Layer (Thread-Safe)                            │
│  - AccountManager (shared_mutex) ← Phase 1.2 완료               │
│  - SharedOrderApi (mutex) ← Phase 1.1 완료                      │
│  - OrderStore (shared_mutex)                                    │
│  - TradeLogger (Phase 2, 배치 쓰기 + WAL)                       │
└─────────────────────────────────────────────────────────────────┘
```

### 스레드 간 통신

- **BlockingQueue**: WebSocket 스레드 → Main 스레드 이벤트 전달
- **shared_mutex**: AccountManager, OrderStore 공유 자원 보호 (읽기 병렬, 쓰기 직렬)
- **mutex**: SharedOrderApi 직렬화 (HTTP/1.1 단일 연결)
- **std::atomic**: 통계 카운터, stop_flag

### 스레드 소유권 모델

RealOrderEngine은 `bindToCurrentThread()`를 통해 단일 스레드 소유권을 강제합니다:

```cpp
void RealOrderEngine::bindToCurrentThread() {
    owner_thread_id_ = std::this_thread::get_id();
}

void RealOrderEngine::assertOwner_() const {
    if (std::this_thread::get_id() != owner_thread_id_) {
        std::terminate();  // 다른 스레드에서 호출 시 즉시 종료
    }
}
```

## 전략 상태 머신

`RsiMeanReversionStrategy`는 4단계 상태 머신을 사용하여 거래 흐름을 관리합니다.

### 상태 전이도

```
┌──────┐  Entry Signal   ┌──────────────┐  Order Filled   ┌────────────┐
│ Flat │ ───────────────>│ PendingEntry │ ──────────────>│ InPosition │
└──────┘                 └──────────────┘                 └────────────┘
   ▲                            │                              │
   │                            │ Cancel/Fail                  │ Stop/Target Hit
   │                            └──────────────────────────────┤
   │                                                           │
   │                                          ┌──────────────┐ │
   └──────────────────────────────────────────│ PendingExit  │<┘
                                Order Filled  └──────────────┘
```

### 상태 설명

| 상태 | 설명 | 다음 전이 |
|------|------|----------|
| **Flat** | 포지션 없음, 진입 신호 대기 | RSI < oversold → PendingEntry |
| **PendingEntry** | 매수 주문 제출됨, 체결 대기 | 체결 완료 → InPosition<br>취소/실패 → Flat |
| **InPosition** | 포지션 보유, 익절/손절 가격 설정 | 가격이 stop/target 도달 → PendingExit |
| **PendingExit** | 매도 주문 제출됨, 체결 대기 | 체결 완료 → Flat<br>취소/실패 → InPosition (재시도) |

### 신호 생성 로직

```cpp
// RSI 임계값
const double oversold = 30.0;   // 과매도 (진입)
const double overbought = 70.0; // 과매수 (청산)

// 진입 신호
if (state == Flat && rsi < oversold) {
    return Decision::Buy(size, identifier);
}

// 청산 신호
if (state == InPosition) {
    if (current_price <= stop_price) {
        return Decision::Sell(position_size, "stop_loss");
    }
    if (current_price >= target_price) {
        return Decision::Sell(position_size, "take_profit");
    }
}
```

## 패턴 및 설계 원칙

### DTO + Mapper 패턴

API 응답을 도메인 모델로 변환하여 비즈니스 로직과 API 스키마를 분리합니다.

```
API Response (JSON)
    ↓
DTO Struct (api/upbit/dto/)
    ↓
Mapper (api/upbit/mappers/)
    ↓
Domain Model (core/domain/)
```

**예시**:
```cpp
// DTO
struct UpbitCandleDto {
    std::string market;
    double opening_price;
    double high_price;
    // ...
};

// Domain Model
struct Candle {
    Timestamp time;
    Price open;
    Price high;
    // ...
};

// Mapper
Candle CandleMapper::fromDto(const UpbitCandleDto& dto) {
    return Candle{
        .time = parseTime(dto.candle_date_time_utc),
        .open = Price(dto.opening_price),
        // ...
    };
}
```

### 이벤트 주도 아키텍처

시장 데이터 및 주문 이벤트는 `BlockingQueue`를 통해 비동기적으로 전달됩니다.

```cpp
// Producer (WebSocket Thread)
void MarketDataEventBridge::onCandle(const CandleDto& dto) {
    Candle candle = CandleMapper::fromDto(dto);
    event_queue_.push(MarketDataEvent{candle});
}

// Consumer (Main Thread)
void EngineRunner::run() {
    while (!stop_flag_) {
        auto event = event_queue_.pop();  // 블로킹
        handleEvent(event);
    }
}
```

### Strategy 패턴

`IOrderEngine` 인터페이스를 통해 전략과 실행 엔진을 분리합니다.

```cpp
// 인터페이스
class IOrderEngine {
public:
    virtual EngineResult submit(const OrderRequest& req) = 0;
    virtual void onMyTrade(const MyTrade& trade) = 0;
};

// 전략
Decision decision = strategy.decide(candle, account);
if (decision.action == Action::Buy) {
    engine.submit(OrderRequest{...});
}
```

## 빌드 및 실행

### 빌드 환경 요구사항

- **OS**: Windows 10/11
- **컴파일러**: Visual Studio 2022 (MSVC)
- **빌드 도구**: CMake 3.20+, Ninja
- **필수 라이브러리**:
  - Boost 1.89.0 (`C:/git-repository/boost_1_89_0`)
  - OpenSSL Win64 (`C:/git-repository/OpenSSL-Win64`)
  - nlohmann JSON (`C:/git-repository/nlohmann_json`)

### 빌드 명령

```bash
# Visual Studio Developer Command Prompt에서 실행

# 1. 프로젝트 구성
cmake --preset x64-debug    # Debug 빌드
cmake --preset x64-release  # Release 빌드

# 2. 빌드 실행
cmake --build out/build/x64-debug
cmake --build out/build/x64-release

# 3. 실행
./out/build/x64-debug/CoinBot.exe
```

### 실행 전 설정

API 키 및 환경 변수 설정:

```bash
# 환경 변수 또는 설정 파일에 추가
UPBIT_ACCESS_KEY=your_access_key
UPBIT_SECRET_KEY=your_secret_key
```

## 관련 문서

- **미래 로드맵**: [ROADMAP.md](ROADMAP.md) - 멀티마켓, PostgreSQL, AWS 배포 계획
- **구현 현황**: [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) - 개발 진행 상황 추적
- **개발자 가이드**: [../CLAUDE.md](../CLAUDE.md) - Claude Code를 위한 프로젝트 지침

# 파일 생성 규칙
- 모든 텍스트 파일은 한글이 깨지지 않도록 저장
- 오버코딩 금지
- 주석으로 왜 필요한지, 기능과 동작을 간단히 설명할 것
- 우선 테스트 코드는 작성, 수정하지 말고 요청 시 작성
- 코드 작성 시 가독성 우선
- 함수, 변수, 클래스 네이밍은 직관적으로
- 보완점이나 수정할 점이 큰 영향이 없으면 기존 코드를 최대한 유지
