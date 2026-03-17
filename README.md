<h1 align="center">CoinBot</h1>

Upbit REST/WebSocket과 직접 연결되는 C++20 기반 멀티마켓 자동매매 시스템입니다.  
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

### 6. SQLite + Streamlit + Backtest Pipeline
- 봇은 `candles`, `orders`, `signals`를 SQLite WAL DB에 기록합니다.
- `streamlit/app.py`는 실거래 데이터를 기반으로 P&L, 전략 분석, 백테스트 비교 기능을 제공합니다.
- `tools/fetch_candles.py`, `tools/candle_rsi_backtest.py`로 과거 데이터 적재와 전략 근사 검증이 가능합니다.

<hr>

## 아키텍처

### 소프트웨어 아키텍처

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

저장소는 C++ 실거래 런타임만 포함하지 않습니다. 실거래 데이터 적재, 분석, 백테스트, 운영 배포까지 하나의 시스템으로 구성됩니다.

| 영역 | 설명 |
| --- | --- |
| **C++ Runtime** | Upbit REST/WebSocket 연결, 전략 실행, 주문 제출, 상태 복구 |
| **Persistence** | `candles`, `orders`, `signals`를 SQLite WAL DB에 저장 |
| **Analysis Tooling** | Streamlit 대시보드, 과거 캔들 수집기, RSI 백테스트 |
| **Operations** | Linux `systemd` 서비스와 EC2 배포 스크립트 |

### 이벤트 처리 흐름

1. public/private WebSocket이 raw JSON을 수신합니다.
2. `EventRouter`가 JSON에서 마켓 식별자만 추출해 마켓별 큐로 전달합니다.
3. 각 마켓 워커가 이벤트를 순차 처리하며 `MarketEngine`와 전략 상태를 갱신합니다.
4. 주문 및 체결 결과는 `AccountManager`, `OrderStore`, SQLite에 반영됩니다.
5. 저장된 데이터는 Streamlit과 Python 도구가 재사용합니다.

### ReservationToken Pattern

이 프로젝트의 핵심 설계는 주문 제출과 자금 상태를 분리하지 않는 데 있습니다.

#### 패턴 개요
- 주문 제출 전에 KRW를 먼저 예약합니다.
- 매수 체결 시 예약 금액을 점진적으로 소비합니다.
- 주문 실패, 취소, 미사용 잔액은 토큰 종료 시 자동으로 반환합니다.

#### 핵심 코드
```cpp
auto token = account_mgr_.reserve(market_, reserve_amount);
active_buy_token_.emplace(std::move(*token));
```

#### 효과
- 중복 매수 방지
- 실패 경로에서 잔액 누수 방지
- 부분 체결과 주문 종료를 다른 단계로 분리한 정산 가능

### PositionEffect 기반 주문 상태 처리

거래소가 보내는 주문 상태만으로는 실제 포지션이 열렸는지 닫혔는지 안전하게 알 수 없습니다.  
그래서 엔진은 주문 종결 이후 실제 계좌 반영 결과를 기준으로 `PositionEffect`를 계산하고, 전략은 이 effect를 보고 상태를 전이합니다.

#### 핵심 코드
```cpp
ev.position_effect = resolvePositionEffect_(o);
ctx.strategy->onOrderUpdate(out);
```

#### 효과
- 부분 체결 후 취소 처리 안정화
- WS 유실 후 snapshot 복구와 전략 상태 동기화
- terminal 상태 이름과 포지션 변화를 분리한 명확한 모델

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

- 이벤트 큐는 bounded queue + `drop-oldest` 정책이라 극단적 포화 상황에서는 유실 가능성이 있습니다.
- graceful shutdown은 아직 완전하지 않아 종료 직전 pending 주문 정합성 보장은 제한적입니다.
- 백테스트는 실전 엔진의 근사 모델이며, 체결가와 슬리피지 처리에 단순화가 있습니다.
- 봇 외부 수동 거래와 로컬 상태가 어긋날 수 있으므로 기본 전제는 봇 단독 계좌 사용입니다.

## 관련 문서

- [docs/EC2_DEPLOY.md](docs/EC2_DEPLOY.md)
- [docs/IMPLEMENTATION_STATUS.md](docs/IMPLEMENTATION_STATUS.md)
- [docs/PROJECT_FLOWSRUDY.md](docs/PROJECT_FLOWSRUDY.md)
- [docs/review.md](docs/review.md)
