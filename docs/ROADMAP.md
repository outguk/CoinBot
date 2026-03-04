# CoinBot 로드맵 (미래 계획)

> **현재 구조**: [ARCHITECTURE.md](ARCHITECTURE.md)
> **구현 현황**: [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md)
> 최종 갱신: 2026-03-03 (Phase 2 재설계: Streamlit + SQLite 분리 구조)

---

## 전체 진행 상태

```
Phase 0  [완료] 기존 코드 리팩토링
Phase 1  [완료] 멀티마켓 핵심 구현
Phase 1.7 [완료] 장시간 부하/안정화 검증
Phase 2  [미시작] Streamlit 대시보드 + SQLite 기록
Phase 3  [미시작] AWS 24시간 운영
```

---

## Phase 1.7: 장시간 부하/안정화 검증

**전제**: Phase 1 기능 경로 구현 완료
**목적**: 운영 환경에서의 안정성 검증

### 필수 게이트 (Phase 2 진입 전)

- [x] 장시간 부하 테스트 통과 (최소 1시간 연속 실행)
- [x] `unknown_funds` 재시도 시나리오 검증
- [x] pending 장기 고착 대응 정책 확정 — self-heal 제거 + engine token 기반 timeout + Recovery 트리거 조건화 (#4/#15/#18)

### 권장 (Phase 2와 병행 가능)

- [ ] emergency sync (`rebuildFromAccount` 조건부 호출) 구현
- [ ] 큐 포화 관측 지표 강화 (#3)

### 알려진 리스크

1. **unknown_funds 반복 시 pending 장기 유지**
   - 현재: 보류 + 재시도
   - 필요: 조건부 emergency sync

2. **큐 포화 시 이벤트 유실 가능성 (drop-oldest)**
   - 현재 저빈도(1분봉, 소수 마켓) 조건에서는 가능성 낮음
   - 처리량 증가 시 myOrder 전용 큐 분리 필요

3. **봇 외부 거래와 로컬 상태 불일치**
   - 현재 정책: 외부 주문 체결은 무시
   - 운영 전제: 봇 단독 계좌 사용

---

## Phase 2: Streamlit 대시보드 + SQLite 기록

**전제 조건**: Phase 1.7 필수 게이트 통과

### 설계 원칙

```
실시간 현황 (Streamlit) → Upbit API
    현재 잔고, 미체결 주문, 최근 거래 내역

분석/검증 (Streamlit + 백테스트) → SQLite DB
    캔들 이력, 주문 이력, 전략 신호
```

**두 관심사를 분리하는 이유**:
- 실시간 탭은 API만으로 항상 최신 데이터를 보여줄 수 있어 DB 불필요
- 백테스트·감사 추적·운영 분석에는 Upbit API가 제공하지 않는 로컬 데이터 필요
  - 캔들: 매 백테스트 실행 시 API 재호출 비용(1년치 ≈ 7,000회 호출) 회피
  - signals: 전략 진입 시점의 RSI·손절가 등은 API가 제공하지 않음

**의존성**:
- **sqlite3**: amalgamation 단일 파일 번들 (`src/db/sqlite3.h`, `src/db/sqlite3.c`)
- **Streamlit**: Python 패키지 (`pip install streamlit plotly pandas requests`)

---

### 스키마

```sql
-- SQLite 타입 규칙
--   시각(수치) → INTEGER (Unix ms)
--   시각(문자) → TEXT (ISO8601, Candle.start_timestamp 원본)

-- 캔들 (백테스트 + 차트용)
-- 수집 방법: 별도 fetch 스크립트로 과거 데이터 적재 후 봇이 실시간 append
-- UNIQUE(market, ts)가 암묵적 인덱스를 생성하므로 별도 인덱스 불필요
CREATE TABLE candles (
    id     INTEGER PRIMARY KEY,
    market TEXT NOT NULL,
    ts     TEXT NOT NULL,   -- Candle.start_timestamp (ISO8601)
    open   REAL NOT NULL,
    high   REAL NOT NULL,
    low    REAL NOT NULL,
    close  REAL NOT NULL,
    volume REAL NOT NULL,
    UNIQUE (market, ts)
);

-- 주문 이력 (감사 추적)
-- INSERT/UPDATE 시점: onOrderSnapshot() 도착 시 upsert (submit() 아님)
--   이유: submit() 시점엔 created_at이 비어있음 (거래소 응답 전)
--         첫 WS/REST 스냅샷 도착 시 created_at이 채워짐
-- created_at_ms 정규화 (normalizeToEpochMs 함수):
--   WS 경로  → Order.created_at = epoch ms 숫자 문자열 → stoll()
--   REST 경로 → Order.created_at = ISO8601 문자열 (UTC offset 포함) → 파싱 후 epoch ms
--   파싱 실패  → 0 저장 + WARN 로그 (NOT NULL DEFAULT 0 이므로 NULL 없음)
-- 크래시 허용: submit 후 첫 snapshot 전 크래시 시 row 자체 미생성 → Upbit API 복구 가능
-- 소스: src/core/domain/Order.h
CREATE TABLE orders (
    id               INTEGER PRIMARY KEY,
    order_uuid       TEXT NOT NULL UNIQUE,
    identifier       TEXT,
    market           TEXT NOT NULL,
    side             TEXT NOT NULL CHECK (side IN ('BID', 'ASK')),
    order_type       TEXT NOT NULL CHECK (order_type IN ('Market', 'Limit')),
    price            REAL,                        -- Limit만, Market은 NULL
    volume           REAL,                        -- Market BID는 NULL
    requested_amount REAL,                        -- Market BID만
    executed_volume  REAL    NOT NULL DEFAULT 0,
    executed_funds   REAL    NOT NULL DEFAULT 0,
    paid_fee         REAL    NOT NULL DEFAULT 0,
    status           TEXT    NOT NULL,
    created_at_ms    INTEGER NOT NULL DEFAULT 0   -- 파싱 실패 시 0 저장 (1970-01-01 = 미확인 표시)
);

-- 전략 신호 (운영 분석 + 백테스트 비교)
-- Upbit API가 제공하지 않는 봇 내부 컨텍스트만 저장
-- BUY  기록 시점: PendingEntry → InPosition 전이 확정
-- SELL 기록 시점: PendingExit  → Flat        전이 확정 (is_partial=0)
--                PendingExit  → InPosition   전이 확정 (is_partial=1, 부분체결 후 취소)
--                  → pending_filled_volume_ > 0 인 경우에만 기록
-- is_partial=1 분석: 해당 BUY는 이후 SELL(is_partial=0)과 페어링하거나
--                    orders 테이블의 Canceled+executed_volume>0 레코드로 보완
CREATE TABLE signals (
    id           INTEGER PRIMARY KEY,
    market       TEXT    NOT NULL,
    side         TEXT    NOT NULL CHECK (side IN ('BUY', 'SELL')),
    price        REAL    NOT NULL,   -- 체결 VWAP
    volume       REAL    NOT NULL,
    krw_amount   REAL    NOT NULL,
    stop_price   REAL,               -- BUY 시 손절가
    target_price REAL,               -- BUY 시 익절가
    rsi          REAL,               -- 신호 발생 시 RSI
    volatility   REAL,               -- 신호 발생 시 변동성
    is_partial   INTEGER NOT NULL DEFAULT 0,  -- 0: 완전 청산, 1: 부분 청산
    ts_ms        INTEGER NOT NULL
);

CREATE INDEX idx_orders_market  ON orders(market);
CREATE INDEX idx_orders_status  ON orders(status);
CREATE INDEX idx_signals_market ON signals(market, ts_ms);
```

---

### 구현 순서

#### 2.1 SQLite 기반 인프라
- `src/db/Database.h/.cpp`: RAII `sqlite3*` 래퍼
  - 공개 인터페이스: `open(path)`, `insertCandle`, `upsertOrder`, `insertSignal`
  - write 메서드는 `bool` 반환 (true=성공, false=실패+WARN 로그)
  - 초기화 PRAGMA: `journal_mode=WAL`, `synchronous=NORMAL`
  - WAL 모드: Streamlit 읽기와 봇 쓰기가 서로 차단하지 않음
  - `normalizeToEpochMs(str)` 내부 유틸:
    - 숫자 문자열 → `stoll()` (WS 경로)
    - ISO8601 문자열 (UTC offset 포함) → 파싱 후 epoch ms (REST 경로)
    - 파싱 실패 → 0 반환 + WARN 로그
- `src/db/sqlite3.h/.c`: amalgamation 단일 파일 번들
- `src/db/schema.sql`: 스키마 문서 (런타임 로드 아님)
  - 스키마 SQL은 `Database.cpp`의 `kSchema` 문자열로 embed — 배포 시 파일 의존 없음
- `CoinBot.cpp`에서 Database 생성 후 MarketEngineManager에 주입
- **DB 쓰기 원칙**: 모든 DB write는 이벤트 처리 완료 후, 다음 pop 전 수행
  - 이벤트 처리 도중 블로킹하지 않음 (inter-event 구간에서 동기 write)
  - 현재 규모(1분봉, 3마켓)에서 동기 쓰기 허용 — write 빈도 초당 0.05회 미만

#### 2.2 캔들 수집기 (봇 코드 변경 없음)
- `tools/fetch_candles.py`: 독립 스크립트
  - Upbit `/v1/candles/minutes/1` API 호출
  - DB의 마지막 ts 이후 데이터만 append (`ON CONFLICT(market, ts) DO NOTHING`)
  - rate limit 준수 (0.1s 간격)
- 봇 실행 중: `MarketEngineManager`의 `workerLoop_`에서 캔들 처리 완료 후 `candles` append
  - **쓰기 위치**: 이벤트 처리(전략 onCandle) 완료 후, 다음 pop 전

#### 2.3 봇 코드 통합 (orders + signals 기록)
- **orders** (upsert 패턴):
  - `onOrderSnapshot()` 도착 시마다 upsert (submit() 시점 아님):
    ```sql
    INSERT INTO orders (...) VALUES (...)
    ON CONFLICT(order_uuid) DO UPDATE SET
        -- 동적 필드: snapshot마다 갱신 (체결 진행 + 상태 변화)
        status           = excluded.status,
        executed_volume  = excluded.executed_volume,
        executed_funds   = excluded.executed_funds,
        paid_fee         = excluded.paid_fee,
        -- created_at_ms: 기존 유효값(>0)을 0으로 역행 방지
        created_at_ms    = CASE WHEN excluded.created_at_ms > 0
                                THEN excluded.created_at_ms
                                ELSE orders.created_at_ms END;
        -- 정적 필드(market/side/order_type/price/volume/identifier)는
        -- INSERT 시 확정 후 변경 없음 → DO UPDATE 제외
    ```
    - `INSERT OR REPLACE` 금지: UNIQUE 충돌 시 row 삭제 후 재삽입 → id 변경 부작용
  - `created_at_ms = normalizeToEpochMs(o.created_at)`
  - **쓰기 위치**: 모든 `onOrderSnapshot()` 호출 시 upsert
    - 비터미널(Open/Pending): 도착 즉시 중간 상태 갱신
    - 터미널(done/cancel): `store_.erase()` 직전에 최종 상태 확정
- **signals**:
  - `RsiMeanReversionStrategy`에 `setSignalCallback(fn)` 추가
  - BUY: `PendingEntry → InPosition` 전이 시 (entry_price, stop_price, target_price, rsi, volatility)
  - SELL (완전): `PendingExit → Flat` 전이 시 (`is_partial=0`)
  - SELL (부분): `PendingExit → InPosition` 전이 시, `pending_filled_volume_ > 0` 조건 (`is_partial=1`)
  - `MarketEngineManager`에서 콜백 등록 → Database INSERT
  - **쓰기 위치**: 전략 콜백 내 — state 전이 직후, 다음 이벤트 처리 전

#### 2.4 Streamlit 대시보드
- `streamlit/app.py`
- **실시간 탭** (Upbit API):
  - 마켓별 현재 잔고 (`/v1/accounts`)
  - 미체결 주문 (`/v1/orders/open`)
  - 최근 체결 목록 (`/v1/orders/closed`)
- **분석 탭** (SQLite DB):
  - 캔들차트 + BUY/SELL 마커 (candles + signals JOIN)
  - 주문 이력 테이블 (orders)
  - 거래 성과 요약 (signals SQL 집계: 승률, 총손익, 평균 보유 기간)

#### 2.5 백테스트
- `tools/candle_rsi_backtest.py`
  - `candles` 테이블에서 데이터 로드
  - RSI 전략 로직 구현 (봇과 동일 파라미터 기준)
  - 시뮬레이션 결과: 진입/청산 목록, 손익 곡선
- Streamlit **백테스트 탭**에 결과 표시
  - 실거래 signals와 백테스트 시뮬레이션 결과 오버레이

---

### 구현 순서 의존성

```
2.1 DB 인프라
  ├─ 2.2 캔들 수집기 (봇 무관, 독립 진행 가능)
  │    └─ 2.5 백테스트 (캔들 데이터 필요)
  └─ 2.3 봇 통합 (orders + signals 기록)
       └─ 2.4 Streamlit (실시간 탭은 즉시 가능, 분석 탭은 2.3 이후)
            └─ 2.5 백테스트 탭 (2.4 + 2.2 이후)
```

2.1 → 2.2와 2.3 병행 → 2.4 → 2.5 순서가 최적

---

### 단계별 구현 현황

```
Step 1  [완료] src/db/schema.sql 작성                    (2.1)
Step 2  [완료] sqlite3 번들 + Database 클래스 + CMake    (2.1)
Step 3  [완료] SignalRecord + SignalCallback 타입 추가    (2.3 선행)
Step 4  [미시작] RsiMeanReversionStrategy setSignalCallback (2.3)
Step 5  [미시작] MarketEngineManager DB 주입 + write 연결  (2.3)
Step 6  [미시작] CoinBot.cpp Database 생성 + 주입          (2.1/2.3)
Step 7  [미시작] tools/fetch_candles.py                   (2.2)
Step 8  [미시작] streamlit/app.py                         (2.4)
Step 9  [미시작] tools/candle_rsi_backtest.py             (2.5)
```

---

### Phase 2 완료 기준

- [ ] 봇 실행 중 캔들이 DB에 실시간 적재됨
- [ ] `onOrderSnapshot()` 도착 시마다 orders 테이블에 upsert됨
- [ ] BUY/SELL 확정 시 signals 테이블에 기록됨
- [ ] WAL 모드에서 Streamlit과 봇의 동시 읽기/쓰기 정상 동작
- [ ] Streamlit 실시간 탭에서 현재 잔고·주문 확인 가능
- [ ] Streamlit 분석 탭에서 캔들차트 + 신호 마커 표시
- [ ] 백테스트 스크립트로 전략 손익 시뮬레이션 가능

---

## Phase 3: AWS 24시간 운영

**전제 조건**: Phase 2 완료 기준 충족

### 구현 태스크

```
3.1 SignalHandler (1일)
    - SIGTERM/SIGINT 처리
    - stop_flag 전파

3.2 GracefulShutdown (2일)
    - 시장가 주문 체결 대기 (최대 30초)
    - REST API로 주문 상태 확인
    - 미체결 지정가 주문 정책 적용 (Cancel / KeepOpen)
    - DB 쓰기 완료 확인 후 종료 (WAL checkpoint)
    - 종료 요약 로그

3.3 HealthChecker (1일)
    - WS 연결 상태
    - SQLite 파일 접근 상태
    - 마켓 스레드 상태

3.4 Logger 개선 (1일)
    - 구조화된 JSON 로깅
    - CloudWatch 싱크 (선택적)
    - 로그 레벨 동적 변경

3.5 배포 스크립트 (1일)
    - Dockerfile 작성
    - coinbot.service (systemd)
    - 환경 변수 템플릿

3.6 인프라 구성 (2일)
    - EC2 인스턴스 설정 + EBS 볼륨 마운트 (/opt/coinbot/data)
    - CloudWatch 알림 설정
    - Secrets Manager 연동 (API 키)
    - S3 버킷 생성 + IAM 역할 (백업 전용 최소 권한)
    - S3 일일 백업 cron 설정 (WAL 안전 백업):
        sqlite3 coinbot.db ".backup /tmp/coinbot_snapshot.db"
        aws s3 cp /tmp/coinbot_snapshot.db s3://버킷/YYYY-MM-DD/coinbot.db
        rm /tmp/coinbot_snapshot.db
      .backup 명령은 SQLite 온라인 백업 API를 사용 — 봇 실행 중에도
      WAL 포함 일관된 스냅샷 생성 (단순 cp와 달리 -wal 데이터 누락 없음)
      EC2 장애 시 최대 하루치 데이터 손실로 제한
```

### AWS 배포 구성

```
EC2 Instance (t3.small 이상)
  ├─ EBS Volume (/opt/coinbot/data)
  │    └─ coinbot.db (SQLite, WAL 모드)
  │         └─ cron 00:00 → S3 일일 백업
  │
  └─ Systemd coinbot.service
       └─ CoinBot Process
            ├─ MarketEngineManager
            ├─ WebSocket Clients
            └─ MarketEngine × N
                   └──► Database → coinbot.db (candles / orders / signals)

Secrets Manager: API 키 주입
S3:             coinbot.db 일일 스냅샷 보관
CloudWatch:     로그 스트리밍 + 메트릭 수집
```

### systemd 서비스 (예시)

```ini
[Unit]
Description=CoinBot Multi-Market Trading Service
After=network.target

[Service]
Type=simple
User=coinbot
WorkingDirectory=/opt/coinbot
ExecStart=/opt/coinbot/bin/CoinBot
Restart=always
RestartSec=10
EnvironmentFile=/etc/coinbot/env
MemoryMax=512M
TimeoutStopSec=90
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

### Phase 3 완료 기준

- [ ] systemd로 자동 재시작
- [ ] SIGTERM 시 graceful shutdown
- [ ] 진행 중 주문 안전하게 처리
- [ ] CloudWatch에서 메트릭 확인 가능
- [ ] S3 일일 백업 cron 동작 확인
- [ ] 24시간 무중단 테스트 통과

---

## 의존성 흐름

```
Phase 0 [완료] → Phase 1 [완료] → Phase 1.7 [완료] → Phase 2 → Phase 3
```
