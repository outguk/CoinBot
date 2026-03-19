# CoinBot Software Architecture

## Data Flow (left → right)

```
╔═══════════════╗       ╔══════════════════════╗       ╔════════════════╗       ╔══════════════════════════════════════════════════════════════╗
║               ║  WSS  ║ UpbitWebSocketClient ║       ║                ║       ║              MarketEngineManager                            ║
║               ║◄─────►║        (×2)          ║       ║                ║       ║                                                              ║
║               ║       ║                      ║       ║                ║       ║  ┌────────────────────────────────────────────────────────┐  ║
║               ║       ║  PUBLIC   PRIVATE    ║       ║                ║       ║  │          MarketContext (per market)                    │  ║
║               ║       ║  candle   myOrder    ║       ║                ║       ║  │                                                        │  ║
║               ║       ║    │         │       ║       ║                ║       ║  │  BlockingQueue    handleMarketData_()   MarketEngine   │  ║
║               ║       ║    └────┬────┘       ║       ║   EventRouter  ║       ║  │  <EngineInput> ──► handleMyOrder_()  ──►              │  ║
║               ║       ║         │ raw JSON   ║       ║                ║       ║  │  (cap:5000)       WsMessageParser       order lifecycle│  ║
║   Upbit       ║       ╚═════════╪════════════╝       ║  Fast:  sv추출 ║       ║  │  (drop-oldest)     → Candle            trade dedup    │  ║
║   Exchange    ║                 └────────────────────►║  Slow:  parse  ║──────►║  │                    → MyOrder DTO       buy/sell UUID  │  ║
║               ║                                      ║                ║       ║  │                                        ResvToken 관리  │  ║
║               ║       ╔══════════════════════╗       ╚════════════════╝       ║  │                                            │           │  ║
║               ║       ║   SharedOrderApi     ║                                ║  │                                            │EngineEvent│  ║
║               ║ HTTPS ║   (mutex-serialized) ║◄──────────────────────────────────┤                                            ▼           │  ║
║               ║◄─────►║                      ║        postOrder / getOrder     ║  │                                  RsiMeanReversion     │  ║
║               ║       ║  UpbitExchangeRest   ║                                ║  │                                    Strategy           │  ║
║               ║       ║    Client            ║                                ║  │                                                        │  ║
║               ║       ║  RestClient          ║                                ║  │                                  Flat→PendingEntry     │  ║
║               ║       ║    (Boost.Beast)     ║                                ║  │                                  →InPosition           │  ║
║               ║       ║  UpbitJwtSigner      ║                                ║  │                                  →PendingExit→Flat     │  ║
║               ║       ║    (HMAC-SHA256)     ║                                ║  │                                                        │  ║
╚═══════════════╝       ╚══════════════════════╝                                ║  │                                  RSI, SMA, Volatility  │  ║
                                                                                ║  │                                  stop / target check   │  ║
                                                                                ║  │                                      │                 │  ║
                                                                                ║  │                                      │Decision         │  ║
                                                                                ║  │                                      ▼                 │  ║
                                                                                ║  │                                  submit() → postOrder()│  ║
                                                                                ║  └────────────────────────────────────────────────────────┘  ║
                                                                                ║                                                              ║
                                                                                ║  Reconnect Recovery (inline):                                ║
                                                                                ║    recovery_requested (atomic) → runRecovery_()              ║
                                                                                ║    → poll pending orders via REST → reconcileFromSnapshot()  ║
                                                                                ╚══════════════════════════════════════════════════════════════╝
                                                                                         │                  │                  │
                                                                                         ▼                  ▼                  ▼
                                                                                ┌──────────────────────────────────────────────────────────┐
                                                                                │                     Shared Resources                     │
                                                                                │                                                          │
                                                                                │  AccountManager        OrderStore          Database       │
                                                                                │  (mutex)               (shared_mutex)      (SQLite WAL)   │
                                                                                │                                                          │
                                                                                │  Per-market:           Active orders       Tables:        │
                                                                                │   available_krw         by UUID             candles       │
                                                                                │   reserved_krw                              orders        │
                                                                                │   coin_balance                              signals       │
                                                                                │   avg_entry_price                                         │
                                                                                │   realized_pnl                             db/coinbot.db  │
                                                                                └──────────────────────────────────────────────────────────┘
```

## Startup & Main Loop

```
┌─────────────────────┐     ┌──────────────────────────────────────────────────────────────────────────────────┐
│                     │     │  StartupRecovery (별도 클래스, 초기화 시 1회)                                      │
│  CoinBot.cpp main() ├────►│                                                                                  │
│                     │     │  getMyAccount() ──► AccountManager.rebuildFromAccount() ──► Cancel stale orders   │
│  signal handling    │     │  ──► Build PositionSnapshot ──► strategy.syncOnStart(snapshot)                    │
│  hasFatalWorker()   │     └──────────────────────────────────────────────────────────────────────────────────┘
│  poll (20ms)        │
│                     │     Fatal worker detected → exit(1) → systemd Restart=on-failure
└─────────────────────┘
```

## Thread Model

```
┌──────────────────┐    ┌───────────────────────┐    ┌──────────────────────────────────────────────────┐
│   Main Thread    │    │   ASIO IO Thread      │    │   Market Worker ×N (std::jthread)                │
│                  │    │                       │    │                                                  │
│  signal handling │    │  Boost.Asio           │    │  KRW-ADA worker ◄── BlockingQueue ◄── EventRouter│
│  hasFatalWorker()│    │  io_context.run()     │    │  KRW-ETH worker ◄── BlockingQueue ◄── EventRouter│
│  poll (20ms)     │    │  WS read/write        │    │  KRW-XRP worker ◄── BlockingQueue ◄── EventRouter│
│                  │    │  TLS handshake        │    │                                                  │
└──────────────────┘    └───────────────────────┘    └──────────────────────────────────────────────────┘

Thread Safety:  SharedOrderApi → mutex (REST 직렬화)       OrderStore    → shared_mutex (읽기 우선)
                AccountManager → mutex (자금 할당)          BlockingQueue → mutex + cond_var (생산자-소비자)
                MarketEngine   → 단일 스레드 전용 (assert_owner_)
```

## Fund Reservation Lifecycle

```
┌──────────────────────┐      ┌─────────────────────────────────┐      ┌──────────────────────────────────┐
│  reserve()           │      │  finalizeFillBuy()              │      │  finalizeOrder()                 │
│                      │      │                                 │      │                                  │
│  available_krw -= amt│─────►│  token.addConsumed(executed_krw)│─────►│  남은 reserved → available_krw   │
│  reserved_krw  += amt│      │  coin_balance += coin           │      │  token.deactivate()              │
│                      │      │  avg_entry_price 갱신            │      │                                  │
│  → ReservationToken  │      │                                 │      │  finalizeFillSell()              │
│    (RAII, move-only) │      │                                 │      │   coin_balance -= coin           │
│                      │      │                                 │      │   available_krw += proceeds      │
│                      │      │                                 │      │   realized_pnl 갱신               │
└──────────────────────┘      └─────────────────────────────────┘      └──────────────────────────────────┘
```

## Layer Dependencies

```
┌──────────┐     ┌──────────┐     ┌───────────┐     ┌──────────┐     ┌──────────┐     ┌──────────────┐
│   app    │────►│  engine  │────►│  trading  │────►│   api    │────►│ database │────►│  core / util │
│          │     │          │     │           │     │          │     │          │     │              │
│ Manager  │     │ Market   │     │ Strategy  │     │ Rest     │     │ SQLite   │     │ Domain types │
│ Router   │     │  Engine  │     │ Account   │     │ WsClient │     │ wrapper  │     │ Logger       │
│ Recovery │     │ OrderStore│    │ Indicators│     │ DTOs     │     │          │     │ AppConfig    │
└──────────┘     └──────────┘     └───────────┘     └──────────┘     └──────────┘     │ BlockingQueue│
                                                                                      └──────────────┘
```

---

# CoinBot System Architecture

## Infrastructure Overview

```
┌─ Developer PC (Windows) ────────────────┐                              ┌─ AWS EC2 (Ubuntu) ──────────────────────────────────────────────────────────────────┐
│                                          │    deploy/deploy.sh          │                                                                                     │
│  Visual Studio + Ninja                   │    1. cmake --preset         │  ┌─ EBS Volume (/home/ubuntu/coinbot) ──────────────────────────────────────────┐   │
│  cmake --preset x64-debug                │       linux-release          │  │                                                                              │   │
│                                          │    2. scp CoinBot → EC2     │  │  .coinbot_volume_sentinel                                                    │   │
│  Dependencies:                           │    3. systemctl restart      │  │                                                                              │   │
│   Boost / OpenSSL / nlohmann             │       coinbot                │  │  ┌─ CoinBot Process ──────────────────────┐    ┌─ SQLite DB ──────────────┐  │   │
│   (CMakePresets.json)                    ├────────────────────────────► │  │  │                                        │    │                          │  │   │
│                                          │                              │  │  │  /home/ubuntu/coinbot/CoinBot          │    │  db/coinbot.db           │  │   │
│  ┌─ Local Execution ──────────────────┐  │                              │  │  │                                        │ RW │  (WAL mode)              │  │   │
│  │                                    │  │                              │  │  │  systemd: coinbot.service             │◄──►│                          │  │   │
│  │  scripts/run_local.sh              │  │                              │  │  │   Restart=on-failure (5s)             │    │  Tables:                 │  │   │
│  │   → .env.local (API keys)         │  │                              │  │  │   User: ubuntu                        │    │   candles                │  │   │
│  │   → CoinBot binary                │  │                              │  │  │                                        │    │   orders                 │  │   │
│  │                                    │  │                              │  │  │  Env: .env                            │    │   signals                │  │   │
│  │  Streamlit (localhost:8501)        │  │                              │  │  │   UPBIT_ACCESS_KEY                    │    │                          │  │   │
│  │   → reads db/coinbot.db           │  │                              │  │  │   UPBIT_SECRET_KEY                    │    └──────────┬───────────────┘  │   │
│  └────────────────────────────────────┘  │                              │  │  │   UPBIT_MARKETS                      │               │ Read-only        │   │
│                                          │                              │  │  │                                        │               │ (WAL concurrent) │   │
└──────────────────────────────────────────┘                              │  │  │  Logging → journalctl -u coinbot      │               │                  │   │
                                                                          │  │  └──────────┬─────────────────────────────┘               │                  │   │
                                                                          │  │             │                                             ▼                  │   │
                                                                          │  │             │                                ┌─ Streamlit Dashboard ───────┐ │   │
                                                                          │  │             │                                │                             │ │   │
                                                                          │  │             │                                │  Port 8501                  │ │   │
                                                                          │  │             │                                │                             │ │   │
                                                                          │  │             │                                │  Tab 1: P&L Analysis        │ │   │
                                                                          │  │             │                                │   Win Rate, Profit Factor   │ │   │
                                                                          │  │             │                                │   RSI 분포, 보유 시간          │ │   │
                                                                          │  │             │                                │                             │ │   │
                                                                          │  │             │                                │  Tab 2: Backtest            │ │   │
                                                                          │  │             │                                │   RSI 파라미터 시뮬레이션       │ │   │
                                                                          │  │             │                                │   Plotly 차트               │ │   │
                                                                          │  │             │                                └─────────────────────────────┘ │   │
                                                                          │  └─────────────┼──────────────────────────────────────────────────────────────┘   │
                                                                          │                │                                                                   │
                                                                          └────────────────┼───────────────────────────────────────────────────────────────────┘
                                                                                           │
                                                                                           │ WSS (TLS) + HTTPS (TLS)
                                                                                           │ Outbound only
                                                                                           │
                                                                                           │  WSS: Public candle.15m (시세) + Private myOrder (체결)
                                                                                           │  REST: accounts / orders (JWT HMAC-SHA256 인증)
                                                                                           │
                                                                                           ▼
                                                                          ┌─ Upbit Exchange ──────────────────────────────────────┐
                                                                          │                                                       │
                                                                          │   WebSocket Server             REST API Server         │
                                                                          │   (실시간 시세/체결)              (주문/계좌 관리)          │
                                                                          │                                                       │
                                                                          │   Markets: KRW-ADA, KRW-ETH, KRW-XRP, ...             │
                                                                          └───────────────────────────────────────────────────────┘
```

## Failure & Recovery

```
CoinBot 정상 운영 ──► 장애 발생 (WS 끊김 / Worker 비정상 종료) ──► hasFatalWorker() 감지 (20ms poll) ──► exit(1)
                                                                                                             │
                                                                                                             ▼
정상 운영 재개 ◄── StartupRecovery (계좌 동기화 → 미체결 취소 → 포지션 복원) ◄── CoinBot 재시작 ◄── systemd Restart=on-failure (5s)
```

---

# Event Processing Flow

```
                                                                              ┌──────────────────────────────────────────────────────────────────────────────────────────────────────┐
                                                                              │                              Worker Thread (per market)                                              │
                                                                              │                                                                                                      │
╔═══════════════╗       ╔════════════════════╗       ╔═══════════════╗        │  ┌──────────────┐     ┌────────────────┐     ┌──────────────┐     ┌───────────────────┐                │
║               ║  WSS  ║ UpbitWebSocket     ║       ║               ║        │  │              │     │                │     │              │     │                   │                │
║               ║◄─────►║ Client (×2)        ║       ║  EventRouter  ║        │  │    Ws        │     │                │     │  Rsi         │     │                   │                │
║               ║       ║                    ║       ║               ║        │  │  Message     │     │  Market        │     │  MeanRev     │     │  AccountManager   │                │
║   Upbit       ║       ║  PUBLIC: candle    ║       ║  마켓 코드 추출 ║        │  │  Parser      │     │  Engine        │     │  Strategy    │     │                   │                │
║   Exchange    ║       ║  PRIVATE: myOrder  ╠──────►║  → 마켓별 큐   ╠───────►│  │              │     │                │     │              │     │  reserve()         │                │
║               ║       ║                    ║ JSON  ║  라우팅        ║ route  │  │  JSON →      │     │  체결 중복 제거  │     │  RSI 분석    │     │  finalizeFill*()   │                │
║               ║       ╚════════════════════╝       ╚═══════════════╝        │  │  Candle      ├────►│  주문 상태 추적  ├────►│  손절/익절    ├────►│  finalizeOrder()   │                │
║               ║                                                             │  │  MyOrder DTO │     │  ResvToken     │     │  상태 머신    │     │                   │                │
║               ║       ╔════════════════════╗                                │  │              │     │  관리           │     │              │     │  available_krw     │                │
║               ║ HTTPS ║  SharedOrderApi    ║◄───────────────────────────────┼──┼──────────────┼─────┼────────────────┼─────┤  Decision    │     │  reserved_krw      │                │
║               ║◄─────►║                    ║      postOrder / getOrder      │  │              │     │                │     │  → submit()  │     │  coin_balance      │                │
║               ║       ║  JWT 서명           ║                                │  └──────────────┘     └──────────────┘     └───────────────────┘     └───────────────────┘                │
║               ║       ║  주문 생성/조회/취소 ║                                │         ①                    ②                     ③                         ④                          │
╚═══════════════╝       ╚════════════════════╝                                │       파싱                 주문 관리               매매 판단                   자금 관리                      │
                                                                              └──────────────────────────────────────────────────────────────────────────────────────────────────────┘
       ⓪                                                                                                                              │
     수신                                                                                                                              │ ⑤ 기록
                                                                                                                                       ▼
                                                                              ┌───────────────────────────────────────────────────────────────────────────────────────┐
                                                                              │   Database (SQLite WAL)                                                             │
                                                                              │                                                                                     │
                                                                              │   candles (시세)          orders (주문)          signals (전략 신호)                   │
                                                                              │                                                                                     │
                                                                              │                          ◄──── Streamlit Dashboard (Read-only, WAL concurrent) ──── │
                                                                              └───────────────────────────────────────────────────────────────────────────────────────┘
```

---

# Streamlit / Tools Pipeline

```
╔═══════════════════╗   /v1/candles    ┌─ fetch_candles.py ─────────────────────────────────────────┐   INSERT    ┌─ SQLite DB ────────────────────┐
║   Upbit REST API  ╠────────────────►│                                                             ├───────────►│ db/coinbot.db (WAL mode)       │
║                   ║   200개/batch    │  갭 탐지 → 역순 배치 조회 → 미확정봉 제외 → ON CONFLICT SKIP │            │                                │
╚═══════════════════╝   0.1s 간격      └───────────────────────────────────────────────────────────────┘            │  candles   orders   signals    │
                                                                                                                   │  (OHLCV)  (체결)    (신호)     │
╔═══════════════════╗   candles        ┌─ CoinBot (C++ Runtime) ────────────────────────────────────┐    RW       │                                │
║   Upbit WS/REST   ╠────────────────►│                                                             ├───────────►│  ◄── CoinBot writes (RW)       │
║                   ║   orders         │  실시간 시세 수신 → 전략 판단 → 주문 실행 → 상태 기록        │            │  ◄── fetch_candles inserts     │
╚═══════════════════╝   signals        └───────────────────────────────────────────────────────────────┘            │                                │
                                                                                                                   └────┬───────────┬──────────┬───┘
                                                                                                                        │           │          │
                                                                                                          SELECT candles│  SELECT   │  SELECT  │candles
                                                                                                         +signals       │  orders   │  signals │
                                                                                                                        │  +signals │          │
                                       ┌────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
                                       │   Streamlit Dashboard (app.py, Port 8501)                                                                    │
                                       │                                                                                                              │
                                       │   ┌─ P&L 분석 ──────────────────────┐  ┌─ 전략 분석 ────────────────────┐  ┌─ 백테스트 ──────────────────────┐│
                                       │   │                                  │  │                                │  │                                ││
                                       │   │  Filled/부분체결 주문 필터       │  │  캔들스틱 + 매수/매도 신호      │  │  candle_rsi_backtest.py 호출   ││
                                       │   │           │                      │  │  오버레이 차트                  │  │           │                    ││
                                       │   │           ▼                      │  │           │                    │  │           ▼                    ││
                                       │   │  BID↔ASK 페어링                 │  │           ▼                    │  │  지표 계산 (C++ 동일 로직)     ││
                                       │   │  exit_reason 2단계 매칭         │  │  RSI 분포 히스토그램            │  │  Wilder RSI / 변동성 / 추세   ││
                                       │   │  (identifier → 300s fallback)   │  │  청산 사유 파이차트             │  │           │                    ││
                                       │   │           │                      │  │  평균 보유 시간                 │  │           ▼                    ││
                                       │   │           ▼                      │  │                                │  │  Flat ↔ InPosition 시뮬레이션 ││
                                       │   │  승률, Profit Factor, RR Ratio  │  │                                │  │  미확정봉: 손절/익절           ││
                                       │   │  일별/주별 수익 차트 (Plotly)    │  │                                │  │  확정봉: RSI 청산              ││
                                       │   │                                  │  │                                │  │  슬리피지 ±0.05%  수수료 0.05%││
                                       │   │                                  │  │                                │  │           │                    ││
                                       │   │                                  │  │                                │  │           ▼                    ││
                                       │   │                                  │  │                                │  │  거래 내역 + 자산 곡선 차트   ││
                                       │   └──────────────────────────────────┘  └────────────────────────────────┘  └────────────────────────────────┘│
                                       │                                                                                                              │
                                       └──────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

# Recovery Structure

```
┌─ 트리거 ──────────────────────────────┐       ┌─ 복구 경로 ──────────────────────────────────────────────────────────────────────┐       ┌─ 결과 ──────────────────────┐
│                                        │       │                                                                                  │       │                             │
│  ┌──────────────────────────────────┐  │       │  ┌─ StartupRecovery (프로세스 시작 시 1회) ─────────────────────────────────────┐ │       │                             │
│  │ ① 프로세스 시작                   ├──┼──────►│  │                                                                            │ │       │                             │
│  │    (최초 실행 / systemd 재시작)    │  │       │  │  cancelBotOpenOrders() ──► buildPositionSnapshot() ──► syncOnStart()       │ │       │                             │
│  └──────────────────────────────────┘  │       │  │  봇 미체결 주문 취소         getMyAccount() 조회         전략 상태 복원       ├─┼──────►│                             │
│                                        │       │  │  (identifier prefix 필터)   (코인 잔량/평단가 추출)     (InPosition or Flat) │ │       │                             │
│                                        │       │  └─────────────────────────────────────────────────────────────────────────────┘ │       │                             │
│                                        │       │                                                                                  │       │      정상 운영               │
│  ┌──────────────────────────────────┐  │       │  ┌─ runRecovery_() (런타임 복구 공통 경로) ─────────────────────────────────────┐ │       │                             │
│  │ ② Private WS 재연결              │  │       │  │                                                                            │ │       │  MarketEngine               │
│  │    재연결 콜백 → pending 마켓만   ├──┼──┐    │  │  activePendingIds()  ──►  queryOrderWithRetry_()  ──►  reconcileFromSnapshot│ │       │    +                        │
│  │    recovery_requested = true     │  │  │    │  │  buy/sell UUID 확보       getOrder() REST 조회         delta 정산            ├─┼──────►│  RsiMeanReversionStrategy   │
│  └──────────────────────────────────┘  │  ├───►│  │                           (최대 3회, 500ms 간격)       (체결분 반영/취소 해제)│ │       │    +                        │
│                                        │  │    │  │                                                                            │ │       │  AccountManager              │
│  ┌──────────────────────────────────┐  │  │    │  └─────────────────────────────────────────────────────────────────────────────┘ │       │                             │
│  │ ③ Pending Timeout (>120s)        │  │  │    │                                                                                  │       │                             │
│  │    checkPendingTimeout_()        ├──┼──┘    │                                                                                  │       │                             │
│  │    매 이벤트 처리 후 경과 시간 체크│  │       │                                                                                  │       │                             │
│  └──────────────────────────────────┘  │       │                                                                                  │       │                             │
│                                        │       └──────────────────────────────────────────────────────────────────────────────────┘       │                             │
│  ┌──────────────────────────────────┐  │       ┌─ Fatal Detection (운영 복구) ───────────────────────────────────────────────────┐       │                             │
│  │ ④ Worker 비정상 종료              │  │       │                                                                                  │       │                             │
│  │    ~AbnormalExitGuard() RAII     ├──┼──────►│  exited_abnormally = true ──► hasFatalWorker() (20ms poll) ──► exit(1)           │       │                             │
│  │    stop_token 없이 루프 탈출      │  │       │                                                                                  ├──────►│  systemd Restart=on-failure │
│  └──────────────────────────────────┘  │       │                              Main Thread 감지                 프로세스 종료       │       │  (5s) → ① 로 순환           │
│                                        │       └──────────────────────────────────────────────────────────────────────────────────┘       │                             │
└────────────────────────────────────────┘                                                                                                 └─────────────────────────────┘
```
