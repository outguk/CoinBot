<h1 align="center">$\bf{\large{\color{#6580DD} \ CoinBot }}$</h1>

<p align="center">C++20 기반 Upbit 멀티마켓 자동매매 시스템</p>

## 프로젝트 소개

CoinBot은 Upbit 거래소의 REST/WebSocket API와 직접 연결되어 여러 마켓을 동시에 자동 매매하는 트레이딩 봇입니다.

마켓별 워커 스레드, RAII 기반 자금 예약, 주문 상태 복구, SQLite 기록, Streamlit 분석 도구까지 하나의 저장소에서 다룹니다.

## 개발 환경

### Language
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?style=for-the-badge&logo=c%2B%2B&logoColor=white)
![Python](https://img.shields.io/badge/Python-3776AB?style=for-the-badge&logo=python&logoColor=white)

### Build & Runtime
![CMake](https://img.shields.io/badge/CMake-064F8C?style=for-the-badge&logo=cmake&logoColor=white)
![MSVC](https://img.shields.io/badge/MSVC-5C2D91?style=for-the-badge&logo=visualstudio&logoColor=white)
![Ninja](https://img.shields.io/badge/Ninja-000000?style=for-the-badge&logo=ninja&logoColor=white)
![Boost](https://img.shields.io/badge/Boost-000000?style=for-the-badge&logo=boost&logoColor=white)
![OpenSSL](https://img.shields.io/badge/OpenSSL-721412?style=for-the-badge&logo=openssl&logoColor=white)

### Database & Data
![SQLite](https://img.shields.io/badge/SQLite-003B57?style=for-the-badge&logo=sqlite&logoColor=white)
![JSON](https://img.shields.io/badge/nlohmann%2Fjson-000000?style=for-the-badge)

### Analytics & Tooling
![Streamlit](https://img.shields.io/badge/Streamlit-FF4B4B?style=for-the-badge&logo=streamlit&logoColor=white)
![Pandas](https://img.shields.io/badge/Pandas-150458?style=for-the-badge&logo=pandas&logoColor=white)
![Plotly](https://img.shields.io/badge/Plotly-3F4F75?style=for-the-badge&logo=plotly&logoColor=white)

### Ops
![systemd](https://img.shields.io/badge/systemd-000000?style=for-the-badge)
![EC2](https://img.shields.io/badge/AWS_EC2-FF9900?style=for-the-badge&logo=amazon-ec2&logoColor=white)

<hr>

## Key Dependencies and Features

### 1. Upbit Exchange Integration
- `UpbitJwtSigner -> RestClient -> UpbitExchangeRestClient -> SharedOrderApi` 계층으로 외부 거래소 연동을 분리했습니다.
- 엔진은 `IOrderApi` 인터페이스에만 의존하고, 실제 REST 구현 세부 사항은 하위 계층에 캡슐화했습니다.
- private REST, public/private WebSocket, JWT 인증을 하나의 런타임 흐름으로 통합했습니다.

### 2. Market-Per-Worker Concurrency
- `MarketEngineManager`가 마켓당 하나의 `std::jthread`를 소유합니다.
- WebSocket IO 스레드는 raw JSON을 `EventRouter`로 전달하고, `EventRouter`는 마켓별 `BlockingQueue`로 이벤트를 라우팅합니다.
- 각 마켓 상태는 단일 워커만 수정하므로, 멀티마켓 병렬 처리와 마켓 내부 순차 처리를 동시에 만족합니다.

### 3. ReservationToken Pattern
- 매수 주문 전 KRW를 예약하고, 주문 실패/취소 시 예약 금액이 자동 반환되도록 `ReservationToken`을 사용합니다.
- 토큰은 move-only RAII 객체이며, 비정상 경로에서도 잔액 누수를 줄이도록 설계했습니다.
- `available_krw`, `reserved_krw`, `coin_balance`를 분리해 pending 상태와 부분 체결을 명시적으로 처리합니다.

### 4. PositionEffect 기반 상태 전이
- `Filled`, `Canceled`, `Rejected` 같은 주문 상태와 실제 포지션 변화는 같은 의미가 아니므로 분리해서 다룹니다.
- `PositionEffect::Opened`, `Reduced`, `Closed`를 계산해 전략이 terminal 상태 이름이 아니라 실제 계좌 반영 결과로 상태를 확정합니다.
- 이 구조로 부분 체결 후 취소, WS 체결 유실, snapshot 기반 복구에서 잘못된 상태 전이를 줄였습니다.

### 5. Recovery and Operational Resilience
- 시작 복구에서는 봇이 이전에 낸 미체결 주문을 취소하고, 현재 계좌 포지션만 읽어 전략 상태를 복구합니다.
- 런타임에서는 pending timeout 또는 private WS 재연결 뒤 `getOrder()` 재시도로 주문 상태를 다시 확인합니다.
- 치명 상태는 `exit(1)`로 종료하고, Linux 운영 환경에서는 `systemd Restart=on-failure`로 재시작합니다.

<hr>

## 아키텍처

### 소프트웨어 아키텍처

<!-- TODO: 소프트웨어 아키텍처 다이어그램 -->

이 프로젝트는 단일 바이너리 안에서 역할별 계층을 분리한 구조입니다.

| 계층 | 주요 역할 |
| --- | --- |
| `core` | 도메인 타입, 공통 자료구조, `BlockingQueue` |
| `util` | 설정, 로깅, 공통 유틸리티 |
| `api` | JWT, REST, WebSocket, Upbit DTO/Mapper |
| `trading` | 전략, 지표, 자금 관리 |
| `engine` | 주문 처리, 상태 반영, 엔진 이벤트 |
| `app` | 런타임 조립, 마켓 워커 관리, 이벤트 라우팅, 시작 복구 |
| `database` | SQLite RAII 래퍼와 스키마 |

### 시스템 아키텍처

<!-- TODO: 시스템 아키텍처 다이어그램  -->

저장소는 C++ 실거래 런타임만 포함하지 않습니다. 실거래 데이터 적재, 분석, 백테스트, 운영 배포까지 하나의 시스템으로 구성됩니다.

| 영역 | 설명 |
| --- | --- |
| **C++ Runtime** | Upbit REST/WebSocket 연결, 전략 실행, 주문 제출, 상태 복구 |
| **Persistence** | `candles`, `orders`, `signals`를 SQLite WAL DB에 저장 |
| **Analysis Tooling** | Streamlit 대시보드, 과거 캔들 수집기, RSI 백테스트 |
| **Operations** | Linux `systemd` 서비스와 EC2 배포 스크립트 |

### 이벤트 처리 흐름

<!-- TODO: 이벤트 처리 흐름 다이어그램 (WebSocket → EventRouter → BlockingQueue → Worker → AccountManager/SQLite) -->

<br>

### ReservationToken Pattern

이 프로젝트의 핵심 설계는 주문 제출과 자금 상태를 분리하지 않는 데 있습니다.

#### 패턴 개요
- 주문 제출 전에 KRW를 먼저 예약합니다.
- 매수 체결 시 예약 금액을 점진적으로 소비합니다.
- 주문 실패, 취소, 미사용 잔액은 토큰 종료 시 자동으로 반환합니다.

#### 패턴 흐름

<!-- TODO: ReservationToken 생명주기 다이어그램 (reserve → consume → finalize / 소멸자 안전망) -->

#### 1단계: 예약 (reserve)
전략이 매수 신호를 발생시키면 엔진은 주문을 제출하기 전에 KRW를 먼저 예약합니다.
```cpp
// AccountManager::reserve() — 원자적 KRW 잠금
std::optional<ReservationToken> AccountManager::reserve(
    std::string_view market, core::Amount krw_amount)
{
    std::unique_lock lock(mtx_);

    MarketBudget& budget = it->second;
    if (budget.available_krw < krw_amount)
        return std::nullopt;  // 잔액 부족

    // 예약 적용
    budget.available_krw -= krw_amount;
    budget.reserved_krw  += krw_amount;

    return ReservationToken(this, std::string(market), krw_amount, token_id);
}
```

#### 2단계: 소비 (finalizeFillBuy)
체결 이벤트가 올 때마다 토큰의 consumed를 누적합니다. 부분 체결이 여러 번 올 수 있으므로 addConsumed으로 점진적으로 소비합니다.
```cpp
token.addConsumed(executed_krw);   // consumed_ += executed_krw
budget.coin_balance += received_coin;
budget.reserved_krw -= executed_krw;
```

#### 3단계: 정산 (finalizeOrder)
주문이 terminal 상태(Filled/Canceled/Rejected)에 도달하면 미사용 잔액을 반환합니다.
```cpp
// remaining = amount - consumed (부분 체결 후 취소 시 > 0)
releaseWithoutToken(token.market(), token.remaining());
token.deactivate();
```

#### 안전망: 소멸자
비정상 경로(예외, 스레드 종료)에서 토큰이 active 상태로 파괴되면 소멸자가 자동으로 예약을 해제합니다.
```cpp
ReservationToken::~ReservationToken() {
    if (active_ && manager_)
        manager_->releaseWithoutToken(market_, remaining());  // noexcept
}
```

#### 효과
- 중복 매수 방지 — 마켓당 하나의 토큰만 존재
- 실패 경로에서 잔액 누수 방지 — RAII 소멸자가 안전망
- 부분 체결과 주문 종료를 다른 단계로 분리한 정산 가능

<br>

### PositionEffect 기반 주문 상태 처리

거래소가 보내는 주문 상태만으로는 실제 포지션이 열렸는지 닫혔는지 안전하게 알 수 없습니다.
그래서 엔진은 주문 종결 이후 실제 계좌 반영 결과를 기준으로 `PositionEffect`를 계산하고, 전략은 이 effect를 보고 상태를 전이합니다.

#### 왜 필요한가?
| 거래소 상태 | 실제 상황 | PositionEffect |
| --- | --- | --- |
| `Filled` (BID) | 매수 체결 → 코인 보유 | `Opened` |
| `Canceled` (BID) | 부분 체결 후 취소 → 코인 일부 보유 | `Opened` |
| `Filled` (ASK) | 전량 매도 → 코인 0 | `Closed` |
| `Canceled` (ASK) | 부분 매도 → 코인 잔량 존재 | `Reduced` |
| `Canceled` | 체결 없이 취소 | `None` |

같은 `Canceled`라도 체결 유무와 잔고 상태에 따라 포지션 효과가 완전히 다릅니다.

#### 핵심 코드

```cpp
// 주문 상태 이름이 아닌, 실제 계좌 잔고를 기준으로 effect를 결정
PositionEffect MarketEngine::resolvePositionEffect_(const Order& order) const
{
    if (order.executed_volume <= 0.0)
        return PositionEffect::None;       // 체결 없음

    if (order.position == OrderPosition::BID)
        return PositionEffect::Opened;     // 매수 체결 → 진입 확정

    // 매도: 잔고가 dust 이상이면 부분 청산, 아니면 완전 청산
    const bool has_coin = budget->coin_balance >= cfg.coin_epsilon;
    return has_coin ? PositionEffect::Reduced : PositionEffect::Closed;
}
```

#### 전략의 상태 전이

전략은 terminal 상태 이름(`Filled`, `Canceled`)이 아니라 `PositionEffect`를 기준으로 상태를 확정합니다.

```cpp
void RsiMeanReversionStrategy::onOrderUpdate(const OrderStatusEvent& ev)
{
    const PositionEffect effect = ev.position_effect;

    // 체결 없이 종결 → 이전 상태로 복귀
    if (effect == PositionEffect::None) {
        state_ = (state_ == PendingEntry) ? Flat : InPosition;
        return;
    }

    // 매수 진입 확정
    if (state_ == PendingEntry && effect == PositionEffect::Opened) {
        entry_price_ = final_price;
        setStopsFromEntry(*entry_price_);
        state_ = InPosition;
    }

    // 매도 청산 확정
    if (state_ == PendingExit && effect == PositionEffect::Closed) {
        state_ = Flat;
    }
}
```

#### 효과
- 부분 체결 후 취소 처리 안정화
- WS 유실 후 snapshot 복구와 전략 상태 동기화
- terminal 상태 이름과 포지션 변화를 분리한 명확한 모델

<br>

### 트레이딩 전략: RSI Mean Reversion

RSI(Relative Strength Index) 과매도 진입, 과매수/손절/익절 청산의 평균회귀 전략입니다.

#### 전략 상태 머신

<!-- TODO: 전략 상태 머신 다이어그램 (Flat → PendingEntry → InPosition → PendingExit → Flat) -->

#### 진입 조건 (maybeEnter)
확정봉 close 기준으로 판단합니다.
```cpp
// 1. RSI 과매도
if (!(s.rsi.v <= params_.oversold))          // default: 30
    return Decision::noAction();

// 2. 시장 적합성: 추세 강도 ≤ 4%, 변동성 ≥ 0.4%
if (!s.marketOk)
    return Decision::noAction();

// 3. 매수 금액 계산 → 시장가 주문 제출
const double krw_to_use =
    account.krw_available / engine_cfg.reserve_margin * params_.utilization;
return Decision::submit(makeMarketBuyByAmount(krw_to_use, "entry_rsi_oversold"));
```

#### 청산 조건 — 확정봉 (maybeExit)
확정봉이 완성되면 3가지 조건을 동시에 체크합니다.
```cpp
const bool rsiExit  = (s.rsi.v >= params_.overbought);  // default: 70
const bool hitStop  = (close <= *stop_price_);           // entry * (1 - stopLossPct)
const bool hitTarget = (close >= *target_price_);        // entry * (1 + profitTargetPct)

if (!(hitStop || hitTarget || rsiExit))
    return Decision::noAction();

// 복합 사유 태그: "exit_stop_target_rsi_overbought"
return Decision::submit(makeMarketSellByVolume(sellVol, reason_tag));
```

#### 청산 조건 — 미확정 틱 (onIntrabarCandle)
확정봉을 기다리지 않고 손절/익절만 즉시 체크합니다. RSI는 미확정 상태이므로 사용하지 않습니다.
```cpp
// InPosition 상태에서만 동작, 매 틱마다 호출
const bool hitStop   = (intrabar_close <= *stop_price_);
const bool hitTarget = (intrabar_close >= *target_price_);

if (!hitStop && !hitTarget)
    return Decision::noAction();

// 확정봉을 기다리지 않고 즉시 청산 주문 제출
return Decision::submit(makeMarketSellByVolume(account.coin_available, reason_tag));
```

<br>

### 복구 구조

복구는 시작 시점과 런타임 시점을 분리해서 설계했습니다.

| 구분 | 동작 |
| --- | --- |
| **시작 복구** | 봇 미체결 주문 취소 후, 현재 포지션만 읽어 `syncOnStart()` 수행 |
| **런타임 복구** | pending timeout / WS 재연결 뒤 `getOrder()` 재조회 후 `reconcileFromSnapshot()` 수행 |
| **운영 복구** | 치명 상태 감지 시 프로세스 종료, `systemd`가 자동 재시작 |

<hr>

## 로컬 실행 및 도구

### Windows Development
- Visual Studio 2022 + MSVC + CMake preset 기준입니다.
- `CMakePresets.json`의 Windows preset은 로컬 Boost/OpenSSL/nlohmann 경로를 전제로 합니다.
- 개발용 preset: `x64-debug`, `x64-release`

### Local Linux / WSL

```bash
cmake --preset linux-release
cmake --build out/build/linux-release -j$(nproc)
cp .env.local.example .env.local
bash scripts/run_local.sh
```

필수 환경 변수:
- `UPBIT_ACCESS_KEY`
- `UPBIT_SECRET_KEY`
- `UPBIT_MARKETS`

### Streamlit / Tools
- 먼저 봇을 한 번 실행해 `db/coinbot.db`가 생성되어 있어야 합니다.

```bash
pip install -r streamlit/requirements.txt
streamlit run streamlit/app.py
```

```bash
pip install -r tools/requirements.txt
python tools/fetch_candles.py --days 90 --unit 15
python tools/candle_rsi_backtest.py --market KRW-XRP --days 30
```

### Deployment
- `deploy/coinbot.service`, `deploy/deploy.sh` 기준으로 Linux 운영 환경에 배포합니다.
- `Restart=on-failure`, `WorkingDirectory`, mountpoint/sentinel 검증을 사용합니다.

## 저장소 구조

```text
src/
  core/        # 도메인 타입, BlockingQueue
  util/        # Config, Logger
  api/         # JWT, REST, WebSocket, Upbit DTO/Mapper
  trading/     # 전략, 지표, 자금 관리
  engine/      # MarketEngine, OrderStore, 엔진 이벤트
  app/         # CoinBot, MarketEngineManager, EventRouter, StartupRecovery
  database/    # SQLite 래퍼와 스키마

streamlit/
  app.py       # 실거래 분석 대시보드

tools/
  fetch_candles.py
  candle_rsi_backtest.py

deploy/
  coinbot.service
  deploy.sh
```

## Trade-offs / Known Limits

- 백테스트는 실전 엔진의 근사 모델이며, 체결가와 슬리피지 처리에 단순화가 있습니다.
- 봇 외부 수동 거래와 로컬 상태가 어긋날 수 있으므로 기본 전제는 봇 단독 계좌 사용입니다.
