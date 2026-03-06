# CoinBot 로드맵 (미래 계획)

> **현재 구조**: [ARCHITECTURE.md](ARCHITECTURE.md)
> **구현 현황**: [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md)
> 최종 갱신: 2026-03-05 (Phase 2 대시보드 재설계: 실시간 탭 제거, P&L orders 단독 쿼리)

---

## 전체 진행 상태

```
Phase 0  [완료] 기존 코드 리팩토링
Phase 1  [완료] 멀티마켓 핵심 구현
Phase 1.7 [완료] 장시간 부하/안정화 검증
Phase 2  [완료] Streamlit 대시보드 + SQLite 기록
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
분석/백테스트 (Streamlit) → SQLite DB
    캔들 이력, 주문 이력, 전략 신호
```

**Upbit API 의존을 제거한 이유**:
- 실시간 잔고·주문은 Upbit 앱/웹에서 동일하게 확인 가능 → 중복 구현 대비 실용성 낮음
- API 키 관리·JWT 서명 로직이 대시보드에 추가되어 복잡도 증가
- 백테스트·감사 추적·운영 분석은 Upbit API가 제공하지 않는 로컬 데이터 필요
  - 캔들: 매 백테스트 실행 시 API 재호출 비용(1년치 ≈ 7,000회 호출) 회피
  - signals: 전략 진입 시점의 RSI·손절가 등은 API가 제공하지 않음

**의존성**:
- **sqlite3**: amalgamation 단일 파일 번들 (`src/db/sqlite3.h`, `src/db/sqlite3.c`)
- **Streamlit**: Python 패키지 (`pip install streamlit plotly pandas numpy`)

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
-- INSERT 시점: 터미널 상태(Filled/Canceled/Rejected) 확정 시 1회
--   이유: 비터미널 상태(wait/trade)는 로그로 충분, 최종 확정값만 기록
-- ON CONFLICT(order_uuid) DO NOTHING: WS 재연결 중복 수신 대응
-- created_at_ms 정규화 (normalizeToEpochMs 함수):
--   WS 경로  → Order.created_at = epoch ms 숫자 문자열 → stoll()
--   REST 경로 → Order.created_at = ISO8601 문자열 (UTC offset 포함) → 파싱 후 epoch ms
--   파싱 실패  → 0 저장 + WARN 로그 (NOT NULL DEFAULT 0 이므로 NULL 없음)
-- 크래시 허용: submit 후 터미널 전 크래시 시 row 미생성 → Upbit API 복구 가능
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
    identifier   TEXT,               -- orders.identifier와 동일한 cid (orders JOIN 연결 고리)
    side         TEXT    NOT NULL CHECK (side IN ('BUY', 'SELL')),
    price        REAL    NOT NULL,   -- 체결 VWAP
    volume       REAL    NOT NULL,
    krw_amount   REAL    NOT NULL,
    stop_price   REAL,               -- BUY 시 손절가
    target_price REAL,               -- BUY 시 익절가
    rsi          REAL,               -- 신호 발생 시 RSI
    volatility   REAL,               -- 신호 발생 시 변동성
    is_partial   INTEGER NOT NULL DEFAULT 0,  -- 0: 완전 청산, 1: 부분 청산
    exit_reason  TEXT,               -- SELL 청산 사유. 단일: exit_stop/exit_target/exit_rsi_overbought/exit_unknown. 복합(동시 트리거): exit_stop_target 등 조합 가능. BUY는 NULL
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
  - Upbit `/v1/candles/minutes/15` API 호출
  - DB의 마지막 ts 이후 데이터만 append (`ON CONFLICT(market, ts) DO NOTHING`)
  - rate limit 준수 (0.1s 간격)
- 봇 실행 중: `MarketEngineManager`의 `workerLoop_`에서 캔들 처리 완료 후 `candles` append
  - **쓰기 위치**: 이벤트 처리(전략 onCandle) 완료 후, 다음 pop 전

#### 2.3 봇 코드 통합 (orders + signals 기록)
- **orders** (terminal INSERT 패턴):
  - 터미널 상태(Filled/Canceled/Rejected) 확정 시 1회 INSERT:
    ```sql
    INSERT INTO orders (...) VALUES (...)
    ON CONFLICT(order_uuid) DO NOTHING
    -- WS 재연결 중복 수신 대응, CHECK/NOT NULL 위반은 정상 오류로 노출
    ```
  - `created_at_ms = normalizeToEpochMs(o.created_at)`
  - **쓰기 위치**: `onOrderSnapshot()` 내 터미널 상태 전이 시, `store_.erase()` 직전
- **signals**:
  - `RsiMeanReversionStrategy`에 `setSignalCallback(fn)` 추가
  - BUY: `PendingEntry → InPosition` 전이 시 (entry_price, stop_price, target_price, rsi, volatility)
  - SELL (완전): `PendingExit → Flat` 전이 시 (`is_partial=0`)
  - SELL (부분): `PendingExit → InPosition` 전이 시, `pending_filled_volume_ > 0` 조건 (`is_partial=1`)
  - `MarketEngineManager`에서 콜백 등록 → Database INSERT
  - **쓰기 위치**: 전략 콜백 내 — state 전이 직후, 다음 이벤트 처리 전

#### 2.4 Streamlit 대시보드
- `streamlit/app.py`
- **분석 탭** (SQLite DB):
  - **P&L 섹션** (orders 단독):
    - 일별/주별/월별 수익률 표 (청산일 `last_sell_ts` 기준)
    - 누적 손익 곡선
    - 마켓별 성과 요약 (승률, 평균 손익률)
    - BUY↔SELL 페어링 거래 내역 테이블
    - 페어링: BID `created_at_ms` 기준 BUY 창 → 창 내 모든 ASK 합산 (cancel_after_trade·부분청산 포함)
    - P&L 산식: `cost = executed_funds + paid_fee` / `revenue = executed_funds - paid_fee`
  - **전략 분석 섹션** (signals 기반):
    - 캔들차트 + BUY/SELL 마커 (candles + signals JOIN)
    - 진입 RSI 분포 / 손절·익절·RSI청산 비율
    - 부분청산 건수 (is_partial=1)
- **백테스트 탭**: 기존 유지 (시뮬레이션 결과 + 실거래 signals 오버레이)

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
       └─ 2.4 Streamlit (분석 탭은 2.3 이후)
            └─ 2.5 백테스트 탭 (2.4 + 2.2 이후)
```

2.1 → 2.2와 2.3 병행 → 2.4 → 2.5 순서가 최적

---

### 단계별 구현 현황

```
Step 1  [완료] src/db/schema.sql 작성                    (2.1)
Step 2  [완료] sqlite3 번들 + Database 클래스 + CMake    (2.1)
Step 3  [완료] SignalRecord + SignalCallback 타입 추가    (2.3 선행)
Step 4  [완료] RsiMeanReversionStrategy setSignalCallback   (2.3)
Step 5  [완료] MarketEngineManager DB 주입 + write 연결     (2.3)
Step 6  [완료] CoinBot.cpp Database 생성 + 주입             (2.1/2.3)
Step 7  [완료] tools/fetch_candles.py                      (2.2)
Step 8  [완료] streamlit/app.py                            (2.4)
Step 9  [완료] tools/candle_rsi_backtest.py                (2.5)
```

---

### Phase 2 완료 기준

- [ ] 봇 실행 중 캔들이 DB에 실시간 적재됨
- [ ] 터미널 상태(Filled/Canceled/Rejected) 확정 시 orders 테이블에 1회 INSERT됨
- [ ] BUY/SELL 확정 시 signals 테이블에 기록됨
- [ ] WAL 모드에서 Streamlit과 봇의 동시 읽기/쓰기 정상 동작
- [ ] Streamlit 분석 탭 P&L 섹션에서 주별/월별 수익률 확인 가능
- [ ] Streamlit 분석 탭 전략 분석 섹션에서 캔들차트 + 신호 마커 표시
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
