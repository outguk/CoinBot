-- CoinBot SQLite 스키마
-- 타입 규칙:
--   시각(수치) → INTEGER (Unix ms)
--   시각(문자) → TEXT (ISO8601, Candle.start_timestamp 원본)
-- 초기화 PRAGMA는 Database.cpp에서 수행 (journal_mode=WAL, synchronous=NORMAL)


-- 캔들 (백테스트 + 차트용)
-- 수집 방법: fetch_candles.py로 과거 데이터 적재 후 봇이 실시간 append
-- UNIQUE(market, ts)가 암묵적 인덱스를 생성하므로 별도 인덱스 불필요
CREATE TABLE IF NOT EXISTS candles (
    id     INTEGER PRIMARY KEY,
    market TEXT    NOT NULL,
    ts     TEXT    NOT NULL,   -- Candle.start_timestamp (ISO8601)
    open   REAL    NOT NULL,
    high   REAL    NOT NULL,
    low    REAL    NOT NULL,
    close  REAL    NOT NULL,
    volume REAL    NOT NULL,
    unit   INTEGER NOT NULL DEFAULT 15,  -- 분봉 단위 (1/3/5/10/15/30/60/240)
    UNIQUE (market, ts, unit)            -- 같은 마켓·시각이라도 단위가 다르면 별도 행
);


-- 주문 이력 (감사 추적)
-- INSERT 시점: 터미널 상태(Filled/Canceled/Rejected) 확정 시 1회
--   이유: 터미널 시점은 state != "trade"이므로 origin 필드가 항상 올바르게 채워짐
--         중간 상태(wait/trade)는 로그로 충분
-- created_at_ms 정규화 (normalizeToEpochMs):
--   WS 경로  → Order.created_at = epoch ms 숫자 문자열 → stoll()
--   REST 경로 → Order.created_at = ISO8601 문자열 (UTC offset 포함) → 파싱 후 epoch ms
--   파싱 실패  → 0 저장 + WARN 로그 (NOT NULL DEFAULT 0)
-- ON CONFLICT(order_uuid) DO NOTHING: UNIQUE 중복만 무시 (WS 재연결 중복 수신 대응)
CREATE TABLE IF NOT EXISTS orders (
    id               INTEGER PRIMARY KEY,
    order_uuid       TEXT    NOT NULL UNIQUE,
    identifier       TEXT,
    market           TEXT    NOT NULL,
    side             TEXT    NOT NULL CHECK (side IN ('BID', 'ASK')),
    order_type       TEXT    NOT NULL CHECK (order_type IN ('Market', 'Limit')),
    price            REAL,                        -- Limit만, Market은 NULL
    volume           REAL,                        -- Market BID는 NULL
    requested_amount REAL,                        -- Market BID만
    executed_volume  REAL    NOT NULL DEFAULT 0,
    executed_funds   REAL    NOT NULL DEFAULT 0,
    paid_fee         REAL    NOT NULL DEFAULT 0,
    status           TEXT    NOT NULL,
    created_at_ms    INTEGER NOT NULL DEFAULT 0   -- 파싱 실패 시 0 (1970-01-01 = 미확인 표시)
);

CREATE INDEX IF NOT EXISTS idx_orders_market ON orders(market);
CREATE INDEX IF NOT EXISTS idx_orders_status ON orders(status);


-- 전략 신호 (운영 분석 + 백테스트 비교)
-- Upbit API가 제공하지 않는 봇 내부 컨텍스트만 저장
-- BUY  기록 시점: PendingEntry → InPosition 전이 확정
-- SELL 기록 시점: PendingExit  → Flat       전이 확정 (is_partial=0)
--                PendingExit  → InPosition  전이 확정 (is_partial=1, 부분체결 후 취소)
--                  → pending_filled_volume_ > 0 인 경우에만 기록
CREATE TABLE IF NOT EXISTS signals (
    id           INTEGER PRIMARY KEY,
    market       TEXT    NOT NULL,
    identifier   TEXT,               -- orders.identifier와 동일한 cid (JOIN 연결 고리)
    side         TEXT    NOT NULL CHECK (side IN ('BUY', 'SELL')),
    price        REAL    NOT NULL,   -- 체결 VWAP
    volume       REAL    NOT NULL,
    krw_amount   REAL    NOT NULL,   -- fee 미포함 순수 체결 금액
    stop_price   REAL,               -- BUY 시 손절가
    target_price REAL,               -- BUY 시 익절가
    rsi            REAL,               -- 신호 발생 시 RSI
    volatility     REAL,               -- 신호 발생 시 변동성
    trend_strength REAL,               -- 신호 발생 시 추세 강도
    is_partial   INTEGER NOT NULL DEFAULT 0 CHECK (is_partial IN (0, 1)),  -- 0: 완전 청산, 1: 부분 청산
    exit_reason  TEXT,               -- SELL 청산 사유. 단일: exit_stop/exit_target/exit_rsi_overbought/exit_unknown. 복합(동시 트리거): exit_stop_target 등 조합 가능. BUY는 NULL
    ts_ms        INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_signals_market ON signals(market, ts_ms);
