# Phase 2 Step 7~9: Python 도구 설계 명세

> **관련 단계**: Phase 2 Steps 1~7 완료 (C++ DB 인프라 + 봇 통합 + 캔들 수집기) — Step 8·9 미시작
> **목적**: 과거 데이터 수집 / Streamlit 대시보드 / 백테스트 모듈 (모두 Python 독립 컴포넌트)

---

## 파일 구조

```
C:\cpp\CoinBot\
├── tools\
│   ├── fetch_candles.py       (Step 7: 과거 캔들 수집)
│   ├── candle_rsi_backtest.py (Step 9: 백테스트 모듈)
│   └── requirements.txt       (Step 7/9 전용 의존성)
└── streamlit\
    ├── app.py                 (Step 8: 대시보드 메인)
    └── requirements.txt       (Step 8: 의존성)
```

---

## Step 7: `tools/fetch_candles.py` — 과거 캔들 수집

### 목적

DB에 과거/최신 캔들을 적재한다. 봇 실행과 독립적으로 실행 가능.

### 의존성 (`tools/requirements.txt`)

| 패키지 | 출처 |
|--------|------|
| `requests` | pip |
| `sqlite3` | stdlib |

### CLI

```bash
# repo 루트에서 실행 (DB 경로는 __file__ 기준으로 자동 계산됨)
python tools/fetch_candles.py [--db <path>] [--markets KRW-ADA,KRW-TRX,KRW-XRP] [--days 90]
```

| 옵션 | 기본값 | 설명 |
|------|--------|------|
| `--db` | `__file__` 기준 자동 계산 | SQLite DB 경로 (아래 참고) |
| `--markets` | `KRW-ADA,KRW-TRX,KRW-XRP` | 수집할 마켓 목록 (쉼표 구분) |
| `--days` | `90` | bootstrap 시 수집 기간 (일 수) |

#### DB 경로 해석 규칙 (우선순위)

1. CLI `--db` 인자
2. 환경변수 `COINBOT_DB_PATH`
3. `__file__` 기준 repo root 자동 계산 (`src/db/coinbot.db`)

경로가 존재하지 않으면 즉시 에러. (자동 생성 금지 — 엉뚱한 위치에 빈 DB 생성 사고 방지)

### 수집 알고리즘

두 모드로 명확히 구분한다.

#### Bootstrap 모드 (DB에 해당 마켓 데이터 없음)

```
# now는 반드시 KST 기준 (DB ts와 동일 기준 유지)
# Python: now = datetime.now(ZoneInfo("Asia/Seoul"))
now    = KST 현재 시각
cutoff = now - timedelta(days=days)
end_ts = 현재 분의 직전 분   # 미확정(진행 중) 캔들 제외 — Incremental과 동일 규칙
to     = None                 # 최신 캔들부터 역방향 시작

while True:
    GET /v1/candles/minutes/15?market=<market>&count=200[&to=<to>]

    for each candle in result:
        if candle_ts <= end_ts:          # 미확정 캔들 제외
            INSERT OR IGNORE INTO candles (market, ts, open, high, low, close, volume)

    oldest_ts = min(candle['candle_date_time_kst'] for candle in result)
    if len(result) < 200 or oldest_ts <= cutoff:
        break
    to = oldest_ts
    sleep(0.1)
```

#### Incremental 모드 (DB에 기존 데이터 있음)

```
# now는 반드시 KST 기준 (DB ts와 동일 기준 유지)
# Python: now = datetime.now(ZoneInfo("Asia/Seoul"))
last_ts     = SELECT MAX(ts) FROM candles WHERE market = ?
start_ts    = last_ts - overlap_minutes (기본 2분)  # 경계 누락 방지용 overlap
end_ts      = 현재 분의 직전 분 (KST now 기준)      # 미확정(진행 중) 캔들 제외
to          = None  # 최신부터 역방향 시작

while True:
    GET /v1/candles/minutes/15?market=<market>&count=200[&to=<to>]

    for each candle in result:
        if candle_ts <= end_ts:          # 미확정 캔들 제외 (현재 진행 중인 분봉 skip)
            INSERT OR IGNORE INTO candles ...  # ON CONFLICT DO NOTHING: 중복 흡수

    oldest_ts = min(candle_date_time_kst for candle in result)
    if len(result) < 200 or oldest_ts <= start_ts:
        break
    to = oldest_ts
    sleep(0.1)
```

**overlap 이유**: `MAX(ts)` 기준으로만 시작하면 네트워크 지연·분봉 경계 타이밍에 따라 직전 1~2분이 누락될 수 있다. 2분 겹치면 해당 범위가 중복 요청되지만, `ON CONFLICT DO NOTHING`이 이미 존재하는 행을 무시하므로 무해하다. 단, ON CONFLICT는 기존 행을 업데이트하지 않으므로 잘못 저장된 값의 교정에는 사용할 수 없다.

**end_ts (직전 분) 이유**: 현재 진행 중인 분봉은 아직 확정되지 않은 값. 이를 insert하면 `ON CONFLICT DO NOTHING`으로 인해 나중에 봇이 완성된 값을 write 하지 못해 **틀린 데이터가 고착**된다.

### Upbit API 주의사항

- 공개 API — 인증 불필요
- `to` 파라미터: ISO8601 형식 (`2024-01-01T00:00:00`)
- 응답 필드 `candle_date_time_kst` → DB `ts` 컬럼 (C++ `CandleMapper.h:25`의 `start_timestamp`와 동일 기준)
- rate limit: 요청 간 **0.1s** sleep으로 준수 (분당 ~600회 상한, 0.1s 간격이면 600회)

### 핵심 설계 결정

- `ON CONFLICT(market, ts) DO NOTHING`: 봇 실시간 write와 충돌 없음
- `ts` 기준을 KST로 고정: C++ 봇 DB와 1:1 일치, 이중 적재 방지
- 단순 `print`로 진행 현황 출력

---

## Step 9: `tools/candle_rsi_backtest.py` — RSI 백테스트 모듈

### 목적

DB 캔들 기반으로 RSI 평균회귀 전략을 시뮬레이션한다.
Streamlit `app.py` Tab2에서 `import candle_rsi_backtest as backtest`로 사용하며, 독립 CLI로도 실행 가능.

### 의존성 (`tools/requirements.txt`에 포함)

| 패키지 | 출처 |
|--------|------|
| `pandas` | pip |
| `numpy` | pip |
| `sqlite3` | stdlib |

### 공개 인터페이스

```python
def run_backtest(
    db_path: str,
    market: str,
    start_ts: str | None = None,  # ISO8601 KST 문자열, None=전체 기간
    end_ts: str | None = None,
    params: dict | None = None,   # None=DEFAULT_PARAMS 사용
    initial_krw: float = 1_000_000
) -> dict:
    ...
```

#### 반환값

```python
{
    'candles': pd.DataFrame, # index: ts(KST ISO8601), columns: OHLCV + 지표 컬럼
    'trades': pd.DataFrame,  # entry_ts, exit_ts, entry_price, exit_price, pnl, pnl_pct, reason
    'equity': pd.Series,     # index: ts(KST ISO8601) → 자산 KRW
    'summary': dict          # total_trades, win_rate, total_pnl, avg_hold_minutes
}
```

### 기본 파라미터

```python
DEFAULT_PARAMS = {
    'rsi_length':          5,
    'oversold':           50,
    'overbought':         70,
    'trend_look_window':   5,
    'max_trend_strength':  1.0,    # 1.0 = 사실상 비활성 (모든 추세 허용)
    'volatility_window':   5,
    'min_volatility':      0.0,    # 0.0 = 비활성 (모든 변동성 허용)
    'stop_loss_pct':       2.0,
    'profit_target_pct':   3.0,    # C++ Params::profitTargetPct
    'fee_rate':            0.0005, # 0.05% (Config.h EngineConfig::default_trade_fee_rate)
    'utilization':         1.0,    # 가용 KRW 중 사용 비율 (C++ Params::utilization)
    'reserve_margin':      1.001,  # 수수료 여유 (Config.h EngineConfig::reserve_margin)
    'min_notional_krw':    5000.0, # 최소 주문 금액 (Config.h StrategyConfig::min_notional_krw)
}
```

> **참고**: 기본 파라미터에서 `max_trend_strength=1.0`, `min_volatility=0.0`이므로 RSI 필터만 실질적으로 작동한다.

### 지표 구현 — C++ 로직 정렬 (근사 모델)

> **한계**: 체결가를 close로 단순화(실전은 시장가 VWAP), 상태머신을 2상태로 축소(실전은 4상태), 슬리피지 미반영.
> 파라미터와 신호 판단 규칙은 C++와 정렬되어 있으므로 전략 방향성 검증 용도에 적합하다.

#### WilderRSI (`RsiWilder.h` 정렬)

```
1. Seed 단계 (length개 delta 수집):
   avg_gain = sum(양의 delta) / length
   avg_loss = sum(|음의 delta|) / length

2. Wilder smoothing (seed 완료 후):
   avg_gain = (avg_gain * (length-1) + gain) / length
   avg_loss = (avg_loss * (length-1) + loss) / length

3. RSI 계산:
   RS  = avg_gain / avg_loss
   RSI = 100 - 100 / (1 + RS)

경계값:
   avg_gain == 0 & avg_loss == 0  →  RSI = 50 (중립)
   avg_loss == 0 & avg_gain  > 0  →  RSI = 100
   avg_gain == 0 & avg_loss  > 0  →  RSI = 0
```

#### ChangeVolatility (`ChangeVolatilityIndicator.h` 정렬)

```
r = (close - prev_close) / prev_close
rolling stdev(window=5, ddof=0)  ← 모집단 표준편차
```

#### TrendStrength (`ClosePriceWindow.h` 기반)

```
trend_strength = abs(close - close[N봉 전]) / close[N봉 전]
N = trend_look_window
```

### 시뮬레이션 로직

상태: `Flat` / `InPosition`

#### 체결 모델

**close 기준** 단순화 모델. C++ 봇의 신호 판단 규칙과 정렬하되, 실전과의 차이를 인지하고 사용한다.

| 항목 | 백테스트 (근사) | 실전 C++ |
|------|----------------|---------|
| 체결가 | 신호 캔들 close | 시장가 VWAP (executed_funds/volume) |
| 상태머신 | Flat / InPosition | Flat / PendingEntry / InPosition / PendingExit |
| 슬리피지 | 미반영 (fee_rate 흡수 가정) | 실제 반영 |
| 손절/익절 판단 | close 기준 | close 기준 (`RsiMeanReversionStrategy.cpp:317`) |

#### 진입 조건 및 규칙 (Flat → InPosition)

C++ `maybeEnter()` 정렬 (`RsiMeanReversionStrategy.cpp:258~265`):

진입 전 **marketOk 게이트** 통과 필수 (C++ `RsiMeanReversionStrategy.cpp:227~241`):

```
rsi_ok   = rsi 지표 준비 완료 (seed 단계 통과)
trend_ok = trend_strength <= max_trend_strength   # 추세가 너무 강하면 진입 금지
vol_ok   = volatility >= min_volatility            # 변동성이 너무 낮으면 진입 금지
market_ok = rsi_ok AND trend_ok AND vol_ok
```

> 기본 파라미터(`max_trend_strength=1.0`, `min_volatility=0.0`)에서는 trend_ok·vol_ok가 항상 true이므로 RSI만 실질 필터로 동작한다. 파라미터 변경 시 해당 필터가 즉시 활성화된다.

- 진입 금액: `entry_krw = current_krw / reserve_margin × utilization`
- 진입 조건: `market_ok AND entry_krw >= min_notional_krw`
- 체결가: 신호 캔들 close
- stop/target: `entry_price × (1 - stop_loss_pct / 100)` / `entry_price × (1 + profit_target_pct / 100)`
- 수수료 및 수량 계산:

```
# 매수: entry_krw 전액이 거래소에 전달, 수수료는 별도 차감 (Upbit 시장가 매수 모델)
buy_fee   = entry_krw × fee_rate
coin_qty  = entry_krw / close          # 거래소가 entry_krw 전액으로 매수
total_cost = entry_krw + buy_fee       # 실제 총 지출 = 주문금액 + 수수료
current_krw -= total_cost
```

#### 청산 조건 및 규칙 (InPosition → Flat)

C++ `maybeExit()` 정렬 (`RsiMeanReversionStrategy.cpp:314~323`):

- 청산 전제: `보유 수량 × close >= min_notional_krw`
- 청산 트리거 (close 기준, 아래 순서로 평가하여 **첫 번째 매칭 reason을 기록**):

| 조건 | reason |
|------|--------|
| `close <= stop_price` | `stop_loss` |
| `close >= target_price` | `take_profit` |
| `rsi >= overbought` | `rsi_exit` |

> **참고**: C++ 봇은 복합 태그(`"exit_stop_target"` 등)를 지원하지만, 백테스트에서는 단일 reason으로 단순화한다. 성과 분석 목적으로는 충분하며, 동시 트리거 시 손절이 가장 중요한 정보이므로 우선순위 순서가 적절하다.

- 체결가: 청산 캔들 close
- 수수료 및 PnL 계산:

```
# 매도: 보유 수량 전량 매도
sell_gross   = coin_qty × close        # 매도 총액
sell_fee     = sell_gross × fee_rate    # 매도 수수료
sell_net     = sell_gross - sell_fee    # 매도 순수익
current_krw += sell_net

# 손익 계산
pnl     = sell_net - total_cost        # total_cost = entry_krw + buy_fee (진입 시 기록)
pnl_pct = pnl / total_cost
```

#### Equity 곡선

각 캔들 시점의 평가 자산 = 현금 + 미실현 평가금액 (보유 중이면 `수량 × close`, 아니면 0)

### CLI (독립 실행)

```bash
# repo 루트에서 실행 (DB 경로는 __file__ 기준으로 자동 계산됨)
python tools/candle_rsi_backtest.py --market KRW-BTC --days 30
```

`--days N` → 내부적으로 `start_ts = (KST_now - N일).isoformat()`, `end_ts = None` (전체 기간 끝까지)으로 변환하여 `run_backtest()` 호출. (`KST_now = datetime.now(ZoneInfo("Asia/Seoul"))`)

출력: 콘솔에 `summary` 출력 + `trades.csv` 저장

---

## Step 8: `streamlit/app.py` — 대시보드

### 목적

DB 기반 P&L 분석 + 전략 분석 + 백테스트 UI (Upbit API 의존 없음)

### 의존성 (`streamlit/requirements.txt`)

```
streamlit
plotly
pandas
numpy          # candle_rsi_backtest.py 의존성 (Tab2 import)
```

---

### `app.py` 구조

#### Sidebar

| 요소 | 설명 |
|------|------|
| DB 경로 입력 | `__file__` 기준 자동 계산 (Step 7과 동일 규칙) |
| 마켓 선택 | multiselect |

#### Tab 1 — 분석

**섹션 A: P&L** (`orders` 기반 — 실측 수수료 포함)

- 마켓 + 기간 선택 (일별/주별/월별 토글)
- **손익 바차트** (Plotly): 기간별 총손익(KRW) 기본, 수익률(%) 토글 전환 가능
  - KRW: `SUM(pnl)` / 수익률(%): `SUM(pnl) / SUM(cost) * 100` (가중 수익률. SQL은 0~1 비율 반환, UI에서 *100 변환)
- **누적 손익 곡선** (Plotly): 거래 완료 시점 기준
- **마켓별 성과 요약 카드**: 승률 / 총손익(KRW) / 평균 손익률(%)
- **BUY↔SELL 페어링 거래 내역 테이블** (아래 쿼리 참고)

**섹션 B: 전략 분석** (`signals` 기반 — 봇 내부 컨텍스트)

- **Plotly 캔들차트** (`go.Candlestick`)
  - BUY 마커: `signals WHERE side='BUY'` → 삼각형 ▲ (green)
  - SELL 마커: `signals WHERE side='SELL' AND is_partial=0` → 삼각형 ▼ (red)
- **신호 통계 카드**: 손절/익절/RSI청산 비율, 평균 보유 기간, 부분청산 건수

#### Tab 2 — 백테스트

`candle_rsi_backtest` import 실패 시 앱 전체 크래시 없이 탭 내 안내 메시지로 처리. (`tools/` 경로를 `sys.path`에 추가한 후 import)

파라미터 입력 UI (import 성공 시에만 활성):

| 파라미터 | 기본값 |
|----------|--------|
| RSI period | 5 |
| oversold | 50 |
| overbought | 70 |
| stop loss % | 2.0 |
| target profit % | 3.0 |
| 분석 기간 | (date picker) |

[실행] 버튼 → `backtest.run_backtest()` 호출

결과:
- 캔들차트 + 백테스트 진입(↑) / 청산(↓) 마커 (Plotly)
- **실거래 signals 오버레이**: `signals WHERE market=? AND ts_ms BETWEEN start AND end`
  - BUY 마커 ▲ (green, 실선) / SELL 마커 ▼ (red, 실선) — 백테스트 마커와 색상/형태 구분
- 자산 곡선 (equity curve)
- 거래 요약 테이블

---

## 쿼리 설계

### 섹션 A: P&L 쿼리 (`orders` 단독)

#### 설계 원칙

- **대상**: `created_at_ms > 0` 필터로 파싱 실패 행 제외
- **포함 범위**: `status = 'Filled'` + `status = 'Canceled' AND executed_volume > 0`
  - cancel_after_trade(부분체결 후 취소) 자동 포함
- **BUY 창 방식**: BID `created_at_ms` 기준으로 LEAD()로 다음 BUY 시각 계산
  - 창 내 모든 ASK 합산 → 부분청산(복수 SELL) 자동 포함
  - ROW_NUMBER 1:1 매칭보다 데이터 누락에 강건
  - `created_at_ms` = 주문 생성 시각 기준 (체결 완료 시각 아님). 시장가 위주 운영 시 차이는 수백ms 이내로 분석 결과에 실질 영향 없음
- P&L 산식:
  - `cost    = executed_funds + paid_fee`  (BID: 실제 지출)
  - `revenue = executed_funds - paid_fee`  (ASK: 실제 수령)
  - `pnl     = Σrevenue - cost`

```sql
-- BUY↔SELL 페어링 + P&L (orders 단독)
WITH buys AS (
    SELECT
        market,
        executed_funds + paid_fee  AS cost,
        created_at_ms              AS buy_ts,
        COALESCE(
            LEAD(created_at_ms) OVER (PARTITION BY market ORDER BY created_at_ms),
            9999999999999
        )                          AS next_buy_ts
    FROM orders
    WHERE side = 'BID'
      AND created_at_ms > 0
      AND (status = 'Filled' OR (status = 'Canceled' AND executed_volume > 0))
),
sells AS (
    SELECT
        market,
        executed_funds - paid_fee  AS revenue,
        created_at_ms              AS sell_ts
    FROM orders
    WHERE side = 'ASK'
      AND created_at_ms > 0
      AND (status = 'Filled' OR (status = 'Canceled' AND executed_volume > 0))
)
-- 각 BUY 창에 속한 모든 SELL 합산 (부분청산 포함)
SELECT
    b.market,
    b.buy_ts,
    b.cost,
    SUM(s.revenue)                        AS total_revenue,
    SUM(s.revenue) - b.cost              AS pnl,
    (SUM(s.revenue) - b.cost) / b.cost   AS pnl_pct,
    MAX(s.sell_ts)                        AS last_sell_ts
FROM buys b
JOIN sells s
  ON  s.market   = b.market
  AND s.sell_ts  > b.buy_ts
  AND s.sell_ts <= b.next_buy_ts
GROUP BY b.market, b.buy_ts, b.cost
ORDER BY b.buy_ts
```

#### 일별/주별/월별 집계

```sql
-- 위 페어링 결과를 CTE `pairs`로 감싸서 집계
-- 귀속 기준: last_sell_ts (청산일) — 실현손익은 포지션을 닫는 시점에 귀속
-- SQLite: datetime(ts/1000, 'unixepoch', '+9 hours') → KST 변환

-- 일별
SELECT
    strftime('%Y-%m-%d', datetime(last_sell_ts/1000, 'unixepoch', '+9 hours')) AS day,
    SUM(pnl)                                                           AS total_pnl,
    SUM(pnl) / SUM(cost)                                               AS period_return_pct,  -- 가중 수익률 (0~1 비율, UI 표시 시 *100 하여 % 변환. AVG(pnl_pct) 사용 금지: 소액 거래 왜곡)
    COUNT(*)                                                           AS trades,
    SUM(CASE WHEN pnl > 0 THEN 1 ELSE 0 END) * 1.0 / COUNT(*)        AS win_rate
FROM pairs
GROUP BY day ORDER BY day

-- 주별: '%Y-%m-%d' → '%Y-%W'
-- 월별: '%Y-%m-%d' → '%Y-%m'
```

---

### 섹션 B: 전략 분석 쿼리 (`signals` 기반)

> **목적**: 봇 진입 컨텍스트(RSI·손절가 등) 기반 패턴 진단. Upbit API가 제공하지 않는 내부 데이터.

#### BUY/SELL 신호 페어링 (보유 기간 계산용)

- `is_partial=0` SELL만 완전 청산으로 간주하여 BUY와 매칭
- `is_partial=1` SELL(부분체결 후 취소)은 집계에서 제외하고 건수만 별도 표시

```sql
SELECT
    b.ts_ms        AS buy_ts,
    b.price        AS buy_price,
    b.rsi          AS entry_rsi,
    b.stop_price,
    b.target_price,
    s.ts_ms        AS sell_ts,
    s.price        AS sell_price
FROM signals b
JOIN signals s
  ON  s.market     = b.market
  AND s.side       = 'SELL'
  AND s.is_partial = 0
  AND s.ts_ms      = (
      SELECT MIN(ts_ms) FROM signals
       WHERE market     = b.market
         AND side       = 'SELL'
         AND is_partial = 0
         AND ts_ms      > b.ts_ms
  )
WHERE b.side = 'BUY'
ORDER BY b.ts_ms
```

#### 전략 통계 산식

| 지표 | 계산 |
|------|------|
| 평균 보유 기간 | `Σ(sell_ts - buy_ts) / count` [ms → minutes] |
| 진입 RSI 분포 | `signals WHERE side='BUY'`의 `rsi` 히스토그램 |
| 청산 reason 비율 | `signals WHERE side='SELL'`의 `exit_reason` 컬럼 집계. 단일값 4종(`exit_stop` / `exit_target` / `exit_rsi_overbought` / `exit_unknown`) + 복합값(`exit_stop_target` / `exit_stop_rsi_overbought` / `exit_target_rsi_overbought` / `exit_stop_target_rsi_overbought`). 복합값은 다중 조건 동시 트리거 시 생성되며, Python 집계 시 단순 일치가 아닌 `LIKE` 또는 `startswith` 분류 필요 |
| 부분청산 건수 | `SELECT COUNT(*) FROM signals WHERE is_partial=1` |

---

## 구현 순서 및 의존성

```
Step 7 (fetch_candles.py)
    └─ DB에 데이터 적재 (Streamlit/백테스트 실행 전 필수)

Step 9 (candle_rsi_backtest.py)
    └─ Streamlit Tab2에서 import하므로 app.py보다 먼저 구현

Step 8 (app.py)
    └─ Step 7 데이터 + Step 9 모듈 모두 의존
```

---

## 검증 방법

### Step 7 검증

```bash
# repo 루트에서 실행
python tools/fetch_candles.py --days 7 --markets KRW-BTC
sqlite3 src/db/coinbot.db \
  "SELECT COUNT(*), MIN(ts), MAX(ts) FROM candles WHERE market='KRW-BTC'"
# 약 672건 기대 (7일 × 96봉/일, 15분봉 기준)
# ts 값이 KST 형식("2024-01-01T09:00:00")인지 확인
```

### Step 9 검증

```bash
python tools/candle_rsi_backtest.py --market KRW-BTC --days 30
# summary 출력 확인, trades 수 > 0 확인
```

### Step 8 검증

```bash
cd streamlit && streamlit run app.py
# Tab1 섹션A: 주별/월별 수익률 바차트 + 거래 내역 테이블 표시 확인
# Tab1 섹션B: 캔들차트 + 신호 마커 / 부분청산 건수 표시 확인
# Tab2: [실행] 클릭 후 백테스트 결과 차트 표시 확인
```

### 통합 검증

- 봇 실행 중 `fetch_candles.py` 동시 실행 → WAL 모드로 충돌 없음 확인
- Streamlit + 봇 동시 실행 → DB 읽기 차단 없음 확인
- `ts` 기준 통일 확인:
  ```sql
  -- 포맷 검증: candle_date_time_kst = "YYYY-MM-DDTHH:MM:SS" (19자, 오프셋 없음)
  SELECT COUNT(*) FROM candles WHERE ts NOT GLOB '????-??-??T??:??:??';
  -- 결과: 0 이어야 함

  -- UTC/오프셋 혼입 검증
  SELECT COUNT(*) FROM candles WHERE instr(ts,'Z') > 0 OR instr(ts,'+') > 0;
  -- 결과: 0 이어야 함
  ```
