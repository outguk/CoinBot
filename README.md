<h1 align="center">CoinBot</h1>

<p align="center">C++20 기반 Upbit 멀티마켓 자동매매 시스템</p>

## 프로젝트 소개

CoinBot은 Upbit 거래소의 REST/WebSocket API와 직접 연결되어 여러 마켓을 동시에 자동 매매하는 트레이딩 봇입니다.

마켓별 워커 스레드, RAII 기반 자금 예약, 주문 상태 복구, SQLite 기록, Streamlit 분석 도구까지 하나의 저장소에서 다룹니다.

**왜 C++인가?** 거래소 WebSocket에서 수신되는 실시간 시세와 주문 이벤트를 지연 없이 처리하려면, IO 스레드와 워커 스레드 간의 메모리 소유권과 생명주기를 명시적으로 제어할 수 있어야 합니다. C++20의 `jthread`, `stop_token`, move semantics, RAII를 활용해 자금 예약/해제와 스레드 종료를 언어 수준에서 안전하게 관리합니다.

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

### 1. Market-Per-Worker Concurrency
- `MarketEngineManager`가 마켓당 하나의 `std::jthread`를 소유합니다.
- WebSocket IO 스레드는 raw JSON을 `EventRouter`로 전달하고, `EventRouter`는 마켓별 `BlockingQueue`로 이벤트를 라우팅합니다.
- 각 마켓 상태는 단일 워커만 수정하므로, 멀티마켓 병렬 처리와 마켓 내부 순차 처리를 동시에 만족합니다.

### 2. ReservationToken Pattern
- 매수 주문 전 KRW를 예약하고, 주문 실패/취소 시 예약 금액이 자동 반환되도록 `ReservationToken`을 사용합니다.
- 토큰은 move-only RAII 객체이며, 비정상 경로에서도 잔액 누수를 줄이도록 설계했습니다.
- `available_krw`, `reserved_krw`, `coin_balance`를 분리해 pending 상태와 부분 체결을 명시적으로 처리합니다.

### 3. PositionEffect 기반 상태 전이
- `Filled`, `Canceled`, `Rejected` 같은 주문 상태와 실제 포지션 변화는 같은 의미가 아니므로 분리해서 다룹니다.
- `PositionEffect::Opened`, `Reduced`, `Closed`를 계산해 전략이 terminal 상태 이름이 아니라 실제 계좌 반영 결과로 상태를 확정합니다.
- 이 구조로 부분 체결 후 취소, WS 체결 유실, snapshot 기반 복구에서 잘못된 상태 전이를 줄였습니다.

### 4. RSI Mean Reversion Strategy
- RSI 과매도 진입, 과매수/손절/익절 청산의 평균회귀 전략입니다.
- 확정봉에서 RSI 기반 진입/청산을 판단하고, 미확정 틱에서는 손절/익절만 즉시 체크합니다.
- 전략 상태 머신(`Flat` → `PendingEntry` → `InPosition` → `PendingExit` → `Flat`)으로 중복 주문을 방지합니다.

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

본 시스템은 Upbit 거래소와 실시간으로 연결되어 복수 마켓을 동시에 자동 매매하는 이벤트 기반 구조로 설계되어 있습니다.
거래소로부터 수신되는 시세와 체결 이벤트는 라우터를 거쳐 마켓별 워커 스레드로 분배되며,
각 워커는 독립된 처리 파이프라인에서 시세 분석, 매매 판단, 주문 실행까지를 순차적으로 수행합니다.
자금 관리, 주문 추적, 거래 기록 등 공유 자원은 스레드 안전한 계층으로 분리되어
마켓 간 병렬 처리와 마켓 내부 순차 처리를 동시에 보장합니다.

<img width="1920" height="1080" alt="Coinbot 소프트웨어 아키텍처" src="https://github.com/user-attachments/assets/15449f07-3246-45cd-813d-cae6df768f8d" />


아래 표는 다이어그램의 각 구성 요소와 이벤트가 처리되는 순서를 함께 나타냅니다.

| 단계 | 구성 요소 | 설명 |
| --- | --- | --- |
| **① 수신** | `UpbitWebSocketClient` | 거래소와의 실시간 연결을 담당하며, 시세 스트림(Public)과 체결 알림(Private) 두 채널을 수신 |
| **② 라우팅** | `EventRouter` | 수신된 raw JSON에서 마켓 코드를 식별하고 해당 마켓의 `BlockingQueue`로 이벤트를 전달 |
| **③ 파싱** | `WsMessageParser` | 워커 스레드 내에서 JSON을 도메인 객체(`Candle`, `MyOrder`)로 변환 |
| **④ 주문 관리** | `MarketEngine` | 체결 중복을 제거하고, 주문의 생성부터 체결/취소까지 생명주기를 추적하며 잔액 반영을 요청 |
| **⑤ 매매 판단** | `RsiMeanReversionStrategy` | 확정봉에서 RSI 기반 진입/청산을 판단하고, 미확정 틱에서는 손절/익절만 즉시 체크 |
| **⑥ 주문 실행** | `SharedOrderApi` | 전략의 `Decision`이 주문을 포함하면 JWT 서명 후 Upbit REST API로 주문 전송 |
| **⑦ 자금 관리** | `AccountManager` | 마켓별 가용/예약 자금과 코인 잔량을 관리하며, RAII 패턴으로 자금 예약/해제를 보장 |
| **⑧ 기록** | `Database` | 시세, 주문, 전략 신호를 SQLite에 기록하며, WAL 모드로 봇 실행 중에도 Streamlit의 동시 읽기를 허용 |

<br>


### 시스템 아키텍처

C++ 트레이딩 봇은 AWS EC2 인스턴스에서 systemd 서비스로 운영되며, Upbit 거래소와는 WSS/HTTPS로 통신합니다.
거래 데이터는 EBS 볼륨의 SQLite DB에 기록되고, 같은 인스턴스에서 실행되는 Streamlit 대시보드가
WAL 모드를 통해 봇 실행 중에도 DB를 동시 읽기하여 실시간 분석과 백테스트를 제공합니다.
배포는 로컬에서 Linux 크로스 빌드 후 deploy 스크립트를 통해 바이너리 전송과 서비스 재시작까지 자동화되어 있습니다.

<img width="870" height="502" alt="image" src="https://github.com/user-attachments/assets/4394decf-7ad2-47fc-9d47-e9fa32f928bb" />


| 구성 요소 | 설명 |
| --- | --- |
| **CoinBot (C++ Binary)** | EC2에서 systemd로 관리되는 트레이딩 봇 프로세스. 장애 시 자동 재시작 |
| **Upbit Exchange** | WSS로 실시간 시세/체결 수신, HTTPS REST로 주문 생성/조회/취소. JWT 인증 |
| **SQLite (EBS Volume)** | 시세, 주문, 전략 신호를 WAL 모드로 저장. EBS 마운트 + sentinel 파일로 볼륨 안전성 검증 |
| **Streamlit Dashboard** | 실거래 P&L 분석, 전략 지표 시각화, RSI 백테스트를 제공하는 Python 웹 대시보드 |
| **Deploy Pipeline** | cmake 빌드 → scp 전송 → systemd 재시작을 자동화하는 배포 스크립트 |
| **systemd** | `Restart=on-failure`로 프로세스 생존 보장. 마운트포인트/sentinel 사전 검증 |

<br>


### 복구 구조

복구는 시작 시점과 런타임 시점을 분리해서 설계했습니다.

<!-- TODO: 복구 구조 다이어그램 -->

| 구분 | 트리거 | 동작 |
| --- | --- | --- |
| **시작 복구** | 프로세스 시작 | 봇 미체결 주문 취소 → 계좌 조회 → 포지션 복원 |
| **재연결 복구** | Private WS 재연결 | pending 주문 REST 재조회 → delta 정산 |
| **타임아웃 복구** | 주문 pending > 120초 | 자동으로 `runRecovery_()` 실행 |
| **운영 복구** | Worker 비정상 종료 | `exit(1)` → systemd 자동 재시작 |

<br>



### SQLite + Streamlit + Backtest Pipeline

봇이 SQLite에 기록한 실거래 데이터를 Python 도구들이 읽어 분석, 시각화, 전략 검증까지 수행합니다.

<img width="966" height="546" alt="image" src="https://github.com/user-attachments/assets/616f24f5-3d90-4dcc-a950-6b99d0987033" />


#### Streamlit Dashboard (`streamlit/app.py`)

실거래 DB를 실시간으로 읽어 두 개 탭으로 분석을 제공합니다.

| 탭 | 데이터 소스 | 처리 흐름 |
| --- | --- | --- |
| **P&L 분석** | `orders`, `signals` 테이블 | Filled/부분체결 주문 조회 → BID↔ASK 페어링(단일 포지션 모델) → 수수료 반영 P&L 계산 → 승률, Profit Factor, RR Ratio 산출 → 일별/주별 수익 차트 |
| **전략 분석** | `signals`, `candles` 테이블 | 매수/매도 신호를 캔들스틱 위에 오버레이 → RSI 분포 히스토그램 → 청산 사유(손절/익절/RSI 과매수) 비율 파이차트 → 평균 보유 시간 계산 |
| **백테스트** | `candles` 테이블 | RSI/추세/변동성 파라미터 슬라이더 조정 → `candle_rsi_backtest.py` 호출 → 시뮬레이션 매매 결과를 캔들스틱+RSI 차트로 시각화 |

P&L 계산에서 청산 사유(exit_reason)는 2단계로 매칭합니다.
먼저 매도 주문의 `identifier`로 `signals` 테이블과 직접 조인하고, 매칭 실패 시 300초 시간 근접 기준으로 fallback 합니다.

#### 캔들 수집기 (`tools/fetch_candles.py`)

Upbit REST API에서 과거 캔들을 역순으로 배치 조회해 SQLite에 적재합니다.

| 단계 | 동작 |
| --- | --- |
| **갭 탐지** | DB에 있는 캔들과 예상 시퀀스를 비교해 누락 구간만 식별 |
| **역순 배치 조회** | 최신 → 과거 방향으로 200개씩 Upbit API 호출 (0.1초 간격, rate limit 준수) |
| **충돌 해소** | `ON CONFLICT DO NOTHING` — 봇이 실시간으로 기록한 캔들이 우선 |
| **미확정봉 제외** | 현재 진행 중인 캔들은 제외하고 마지막 확정봉까지만 저장 |

#### RSI 백테스트 (`tools/candle_rsi_backtest.py`)

SQLite에 적재된 캔들로 RSI 평균회귀 전략을 시뮬레이션합니다.

| 단계 | 동작 |
| --- | --- |
| **지표 계산** | Wilder RSI, 변동성(rolling stdev), 추세강도(price window) — C++ 구현과 동일한 로직 |
| **진입 게이트** | `rsi_ready AND trend ≤ max AND vol ≥ min` 을 모두 만족해야 진입 허용 |
| **시뮬레이션** | `Flat ↔ InPosition` 2-state 모델. 미확정봉(OHLC)에서 손절/익절 먼저 체크, 확정봉에서 RSI 청산 |
| **슬리피지/수수료** | 매수 `close × 1.0005`, 매도 `close × 0.9995`, 수수료 0.05% 양방향 반영 |
| **출력** | 거래 내역, 자산 곡선, 요약(승률/수익/평균 보유 시간), 데이터 품질 리포트 |

> C++ 봇은 4-state(`Flat→PendingEntry→InPosition→PendingExit`) 모델이지만, 백테스트는 즉시 체결을 가정한 2-state 근사 모델입니다.

#### 실제 대시보드 화면

##### db 기반 성과 분석
<img width="2253" height="1265" alt="image" src="https://github.com/user-attachments/assets/ac465296-bd4d-4420-9121-394614cedd74" />

##### 현재 전략 기반 백테스트
<img width="2286" height="1264" alt="image" src="https://github.com/user-attachments/assets/2076572d-fde2-44c8-b63a-a383c6c5623c" />
<img width="1580" height="520" alt="image" src="https://github.com/user-attachments/assets/6a708bee-84c1-4a43-aec9-204f5ed2ca3c" />




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


### Deployment
- `deploy/coinbot.service`, `deploy/deploy.sh` 기준으로 Linux 운영 환경에 배포합니다.
- `Restart=on-failure`, `WorkingDirectory`, mountpoint/sentinel 검증을 사용합니다.

## 저장소 구조

```text
src/
  core/        # 도메인, thread-safe 큐
  util/        # 설정 파일, Logger
  api/         # JWT, REST, WebSocket, Upbit DTO/Mapper
  trading/     # 전략, 전략 지표, 자금 관리
  engine/      # 로컬 엔진, 로컬 저장소, 엔진 이벤트
  app/         # 조립부(Coinbot), 로컬 엔진 매니저, 메시지 라우터, 복구 정책
  database/    # SQLite 래퍼와 스키마

streamlit/
  app.py       # 실거래 분석 대시보드

tools/
  fetch_candles.py        # 캔들 수집기
  candle_rsi_backtest.py  # 백테스트

deploy/
  coinbot.service         # AWS 배포
  deploy.sh               # AWS 배포 자동화
```

## Trade-offs / Known Limits

- 백테스트는 실전 엔진의 근사 모델이며, 체결가와 슬리피지 처리에 단순화가 있습니다.
- 봇 외부 수동 거래와 로컬 상태가 어긋날 수 있으므로 기본 전제는 봇 단독 계좌 사용입니다.
