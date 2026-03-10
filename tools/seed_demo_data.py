"""
tools/seed_demo_data.py — Streamlit UI 전체 검증용 데모 데이터 생성

출력: db/coinbot_demo.db  (메인 coinbot.db 오염 없음)
실행: python tools/seed_demo_data.py [--db PATH]

커버 시나리오
  - 복수 마켓 (KRW-BTC, KRW-ETH): 마켓 selectbox, 마켓별 요약 테이블
  - 90일 다중 분봉 캔들(1/3/5/10/15/30/60/240): 백테스트 분봉 선택 테스트 가능
  - 수익(60%) + 손실(40%) 거래: 바차트 초록/빨강, 승률 KPI
  - identifier 기반 페어링 (짝수 거래): pair_trades 1차 경로
  - FIFO 페어링 (홀수 거래, identifier=None): pair_trades 2차 경로
  - Filled + Canceled(executed_volume>0): orders 필터 조건
  - BUY/SELL 신호, exit_reason 3종: 파이차트
  - is_partial=1 케이스: 부분청산 메트릭, 반투명 SELL 마커
  - RSI 분포 35~52: RSI 히스토그램
"""

import argparse
import math
import os
import sqlite3
import uuid as _uuid_mod
from datetime import datetime, timedelta, timezone

# ─── 스키마 (schema.sql과 동일) ──────────────────────────────────────────────

_SCHEMA = """
CREATE TABLE IF NOT EXISTS candles (
    id     INTEGER PRIMARY KEY,
    market TEXT NOT NULL,
    ts     TEXT NOT NULL,
    open   REAL NOT NULL,
    high   REAL NOT NULL,
    low    REAL NOT NULL,
    close  REAL NOT NULL,
    volume REAL NOT NULL,
    unit   INTEGER NOT NULL DEFAULT 15,
    UNIQUE (market, ts, unit)
);

CREATE TABLE IF NOT EXISTS orders (
    id               INTEGER PRIMARY KEY,
    order_uuid       TEXT NOT NULL UNIQUE,
    identifier       TEXT,
    market           TEXT NOT NULL,
    side             TEXT NOT NULL CHECK (side IN ('BID', 'ASK')),
    order_type       TEXT NOT NULL CHECK (order_type IN ('Market', 'Limit')),
    price            REAL,
    volume           REAL,
    requested_amount REAL,
    executed_volume  REAL    NOT NULL DEFAULT 0,
    executed_funds   REAL    NOT NULL DEFAULT 0,
    paid_fee         REAL    NOT NULL DEFAULT 0,
    status           TEXT    NOT NULL,
    created_at_ms    INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS signals (
    id           INTEGER PRIMARY KEY,
    market       TEXT    NOT NULL,
    identifier   TEXT,
    side         TEXT    NOT NULL CHECK (side IN ('BUY', 'SELL')),
    price        REAL    NOT NULL,
    volume       REAL    NOT NULL,
    krw_amount   REAL    NOT NULL,
    stop_price   REAL,
    target_price REAL,
    rsi            REAL,
    volatility     REAL,
    trend_strength REAL,
    is_partial     INTEGER NOT NULL DEFAULT 0,
    exit_reason    TEXT,
    ts_ms          INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_orders_market  ON orders(market);
CREATE INDEX IF NOT EXISTS idx_orders_status  ON orders(status);
CREATE INDEX IF NOT EXISTS idx_signals_market ON signals(market, ts_ms);
"""

# ─── 마켓 설정 ────────────────────────────────────────────────────────────────

# vol: 15분봉 1-bar 변동성 (σ). BTC 약 0.12%/bar, ETH 약 0.15%/bar
_MARKETS = {
    "KRW-BTC": {"start_price": 80_000_000, "vol": 0.0012, "n_trades": 20},
    "KRW-ETH": {"start_price":  4_000_000, "vol": 0.0015, "n_trades": 18},
}

_DAYS            = 90
_SIGNAL_UNIT_MIN = 15
_CANDLE_UNITS    = [1, 3, 5, 10, 15, 30, 60, 240]
_FEE_RATE        = 0.0005   # 업비트 0.05%
_WIN_RATE        = 0.60


# ─── 유틸 ─────────────────────────────────────────────────────────────────────

def _new_uuid(prefix: str) -> str:
    return f"{prefix}-{_uuid_mod.uuid4()}"


def _ts_to_ms(ts_str: str) -> int:
    """'YYYY-MM-DDTHH:MM:SS' (UTC) → epoch ms."""
    dt = datetime.strptime(ts_str, "%Y-%m-%dT%H:%M:%S").replace(tzinfo=timezone.utc)
    return int(dt.timestamp() * 1000)


def _rng(seed: int):
    """재현 가능한 시드 기반 random 모듈 인스턴스."""
    import random
    r = random.Random(seed)
    return r


# ─── 캔들 생성 ────────────────────────────────────────────────────────────────

def generate_candles(market: str, start_price: float, vol: float, unit: int) -> list[dict]:
    """
    GBM(Geometric Brownian Motion) 기반 분봉 캔들 생성.

    drift=0 (중립), vol=σ per bar.
    OHLC: close 기준 spread를 랜덤하게 구성.
    """
    rng     = _rng(hash(f"{market}:{unit}"))
    n_bars  = _DAYS * 24 * (60 // unit)

    # 시작 시각: 현재로부터 DAYS일 전 UTC 자정
    now_utc   = datetime.now(timezone.utc).replace(second=0, microsecond=0)
    start_utc = (now_utc - timedelta(days=_DAYS)).replace(hour=0, minute=0)

    candles = []
    close   = float(start_price)
    ts      = start_utc

    for _ in range(n_bars):
        close  = close * math.exp(rng.gauss(0, vol))
        spread = close * rng.uniform(0.0005, 0.003)
        high   = close + spread * rng.uniform(0.3, 1.0)
        low    = close - spread * rng.uniform(0.3, 1.0)
        open_  = low + (high - low) * rng.random()
        volume = rng.uniform(0.05, 2.0) * (start_price / close)

        candles.append({
            "market": market,
            "ts":     ts.strftime("%Y-%m-%dT%H:%M:%S"),
            "open":   round(open_,  0),
            "high":   round(high,   0),
            "low":    round(low,    0),
            "close":  round(close,  0),
            "volume": round(volume, 6),
            "unit":   unit,
        })
        ts += timedelta(minutes=unit)

    return candles


# ─── 거래/신호 생성 ───────────────────────────────────────────────────────────

def generate_trades(
    market: str,
    candles: list[dict],
    n_trades: int,
) -> tuple[list[dict], list[dict]]:
    """
    orders + signals 동시 생성.

    페어링 케이스:
      - 짝수 인덱스(0,2,4,...): identifier(cid) 포함 → pair_trades 1차 경로
      - 홀수 인덱스(1,3,5,...): identifier=None    → pair_trades FIFO 2차 경로

    부분청산:
      - 인덱스 i % 7 == 6 (약 14%): is_partial=1, ASK status=Canceled

    exit_reason 분포:
      - 손실 거래 → exit_stop
      - 수익 거래 → exit_target(70%) / exit_rsi_overbought(30%)
    """
    rng     = _rng(hash(market + "trades"))
    # 캔들 전체를 n_trades 구간으로 균등 분할, 마지막 40봉은 여유분
    step    = max(1, (len(candles) - 40) // n_trades)

    orders:  list[dict] = []
    signals: list[dict] = []

    for i in range(n_trades):
        is_win     = rng.random() < _WIN_RATE
        is_partial = (i % 7 == 6)              # ~14% 부분청산
        use_cid    = (i % 2 == 0)              # 짝수: identifier 기반 페어링
        cid        = _new_uuid("cid") if use_cid else None

        # 진입/청산 캔들 인덱스
        entry_idx = i * step + rng.randint(0, max(1, step // 2))
        entry_idx = min(entry_idx, len(candles) - 35)
        hold_bars = rng.randint(2, 32)          # 30분~8시간 보유
        exit_idx  = min(entry_idx + hold_bars, len(candles) - 1)

        entry_c     = candles[entry_idx]
        exit_c      = candles[exit_idx]
        entry_price = entry_c["close"]
        entry_ts_ms = _ts_to_ms(entry_c["ts"])
        exit_ts_ms  = _ts_to_ms(exit_c["ts"])

        # 손절/익절 가격
        stop_pct     = rng.uniform(0.015, 0.025)
        target_pct   = rng.uniform(0.025, 0.045)
        stop_price   = round(entry_price * (1 - stop_pct), 0)
        target_price = round(entry_price * (1 + target_pct), 0)

        # 청산가 및 청산 사유
        if is_win:
            exit_price  = round(entry_price * (1 + rng.uniform(0.005, target_pct)), 0)
            exit_reason = rng.choices(
                ["exit_target", "exit_rsi_overbought"], weights=[0.7, 0.3]
            )[0]
        else:
            exit_price  = round(entry_price * (1 - rng.uniform(0.005, stop_pct)), 0)
            exit_reason = "exit_stop"

        # 수량/금액 계산 (200~500만원 투자)
        invest_krw   = rng.uniform(2_000_000, 5_000_000)
        entry_volume = round(invest_krw / entry_price, 8)
        entry_funds  = round(entry_volume * entry_price, 0)
        entry_fee    = round(entry_funds * _FEE_RATE, 2)

        # 부분청산: 실체결량 = 전체의 70~90%, Canceled 상태
        if is_partial:
            filled_ratio = rng.uniform(0.70, 0.90)
            exit_volume  = round(entry_volume * filled_ratio, 8)
            ask_status   = "Canceled"
        else:
            exit_volume  = entry_volume
            ask_status   = "Filled"

        exit_funds = round(exit_volume * exit_price, 0)
        exit_fee   = round(exit_funds * _FEE_RATE, 2)

        bid_uuid = _new_uuid("bid")
        ask_uuid = _new_uuid("ask")

        # ── orders ───────────────────────────────────────────────────────────
        # Market BID: volume=NULL, requested_amount=KRW
        orders.append({
            "order_uuid":       bid_uuid,
            "identifier":       cid,
            "market":           market,
            "side":             "BID",
            "order_type":       "Market",
            "price":            None,
            "volume":           None,
            "requested_amount": round(invest_krw, 0),
            "executed_volume":  entry_volume,
            "executed_funds":   entry_funds,
            "paid_fee":         entry_fee,
            "status":           "Filled",
            "created_at_ms":    entry_ts_ms,
        })
        # Market ASK: volume=코인수량, requested_amount=NULL
        orders.append({
            "order_uuid":       ask_uuid,
            "identifier":       cid,
            "market":           market,
            "side":             "ASK",
            "order_type":       "Market",
            "price":            None,
            "volume":           entry_volume,
            "requested_amount": None,
            "executed_volume":  exit_volume,
            "executed_funds":   exit_funds,
            "paid_fee":         exit_fee,
            "status":           ask_status,
            "created_at_ms":    exit_ts_ms,
        })

        # ── signals ──────────────────────────────────────────────────────────
        volatility     = round(rng.uniform(0.01, 0.04), 4)
        trend_strength = round(rng.uniform(0.002, 0.035), 4)

        signals.append({
            "market":         market,
            "identifier":     cid,
            "side":           "BUY",
            "price":          entry_price,
            "volume":         entry_volume,
            "krw_amount":     entry_funds,
            "stop_price":     stop_price,
            "target_price":   target_price,
            "rsi":            round(rng.uniform(35, 52), 1),
            "volatility":     volatility,
            "trend_strength": trend_strength,
            "is_partial":     0,
            "exit_reason":    None,
            "ts_ms":          entry_ts_ms,
        })
        signals.append({
            "market":         market,
            "identifier":     cid,
            "side":           "SELL",
            "price":          exit_price,
            "volume":         exit_volume,
            "krw_amount":     exit_funds,
            "stop_price":     None,
            "target_price":   None,
            "rsi":            round(rng.uniform(55, 75), 1),
            "volatility":     volatility,
            "trend_strength": trend_strength,
            "is_partial":     1 if is_partial else 0,
            "exit_reason":    exit_reason,
            "ts_ms":          exit_ts_ms,
        })

    return orders, signals


# ─── DB 생성 ──────────────────────────────────────────────────────────────────

def seed(db_path: str) -> None:
    # 기존 demo DB는 삭제 후 재생성 (항상 깨끗한 상태)
    if os.path.exists(db_path):
        os.remove(db_path)
        print(f"기존 DB 삭제: {db_path}")

    os.makedirs(os.path.dirname(db_path), exist_ok=True)
    conn = sqlite3.connect(db_path)
    conn.executescript(_SCHEMA)

    for market, cfg in _MARKETS.items():
        print(f"\n[{market}] 캔들 생성 중...")
        candles_by_unit: dict[int, list[dict]] = {}
        total_candles = 0
        for unit in _CANDLE_UNITS:
            candles = generate_candles(market, cfg["start_price"], cfg["vol"], unit)
            candles_by_unit[unit] = candles
            conn.executemany(
                "INSERT OR IGNORE INTO candles (market,ts,open,high,low,close,volume,unit) "
                "VALUES (:market,:ts,:open,:high,:low,:close,:volume,:unit)",
                candles,
            )
            total_candles += len(candles)
            print(f"  {unit:>3}분봉 {len(candles):,}행 삽입")
        print(f"  캔들 합계 {total_candles:,}행 삽입")

        print(f"[{market}] 거래/신호 생성 중...")
        orders, signals = generate_trades(market, candles_by_unit[_SIGNAL_UNIT_MIN], cfg["n_trades"])
        conn.executemany(
            "INSERT OR IGNORE INTO orders "
            "(order_uuid,identifier,market,side,order_type,price,volume,requested_amount,"
            " executed_volume,executed_funds,paid_fee,status,created_at_ms) "
            "VALUES (:order_uuid,:identifier,:market,:side,:order_type,:price,:volume,"
            " :requested_amount,:executed_volume,:executed_funds,:paid_fee,:status,:created_at_ms)",
            orders,
        )
        conn.executemany(
            "INSERT INTO signals "
            "(market,identifier,side,price,volume,krw_amount,stop_price,target_price,"
            " rsi,volatility,trend_strength,is_partial,exit_reason,ts_ms) "
            "VALUES (:market,:identifier,:side,:price,:volume,:krw_amount,:stop_price,"
            " :target_price,:rsi,:volatility,:trend_strength,:is_partial,:exit_reason,:ts_ms)",
            signals,
        )

        n_wins     = sum(1 for o in orders if o["side"] == "ASK" and o["executed_funds"] > 0
                        and _pnl(o, orders) > 0)
        n_partial  = sum(1 for s in signals if s["side"] == "SELL" and s["is_partial"] == 1)
        n_fifo     = sum(1 for o in orders if o["side"] == "BID" and o["identifier"] is None)
        print(f"  orders {len(orders)}행  |  signals {len(signals)}행")
        print(f"  부분청산 {n_partial}건  |  FIFO 페어링 {n_fifo}건  |  signals 기준 {_SIGNAL_UNIT_MIN}분봉")

    conn.commit()
    conn.close()

    print(f"\n완료 → {db_path}")
    print("─" * 60)
    print("실행 방법:")
    print("  streamlit run streamlit/app.py")
    print(f"  → 사이드바 DB 경로: {db_path}")
    print(f"  → 시작일을 {_DAYS}일 전으로 설정하면 전체 데이터 확인 가능")


def _pnl(ask_order: dict, all_orders: list[dict]) -> float:
    """요약 출력용 단순 손익 계산 (seed 내부 전용)."""
    cid = ask_order.get("identifier")
    if cid:
        bid = next((o for o in all_orders if o["side"] == "BID" and o["identifier"] == cid), None)
    else:
        bid = None
    if bid is None:
        return 0.0
    cost    = bid["executed_funds"] + bid["paid_fee"]
    revenue = ask_order["executed_funds"] - ask_order["paid_fee"]
    return revenue - cost


# ─── 엔트리포인트 ─────────────────────────────────────────────────────────────

if __name__ == "__main__":
    _repo_root  = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    _default_db = os.path.join(_repo_root, "db", "coinbot_demo.db")

    parser = argparse.ArgumentParser(description="Streamlit UI 검증용 데모 데이터 생성")
    parser.add_argument(
        "--db", default=_default_db,
        help=f"출력 DB 경로 (기본: {_default_db})",
    )
    args = parser.parse_args()
    seed(args.db)
