"""
candle_rsi_backtest.py — 캔들 기반 RSI 평균회귀 전략 백테스트

용도: DB 캔들 기반으로 C++ 전략을 근사 시뮬레이션한다.
      Streamlit app.py Tab3에서 import candle_rsi_backtest as backtest로 사용하며, 독립 CLI로도 실행 가능.
실행: python tools/candle_rsi_backtest.py [--db <path>] [--market KRW-BTC] [--days 30]
한계:
  - 체결가를 close로 단순화 (실전은 시장가 VWAP)
  - 상태머신을 2상태로 축소 (실전: Flat/PendingEntry/InPosition/PendingExit)
  - 슬리피지 미반영
  - 파라미터·신호 판단 규칙은 C++와 정렬 → 전략 방향성 검증 용도에 적합
"""

import argparse
import os
import sqlite3
from datetime import datetime, timedelta
from zoneinfo import ZoneInfo

import numpy as np
import pandas as pd

KST = ZoneInfo("Asia/Seoul")

# ─── 기본 파라미터 (C++ Params / Config.h 정렬) ──────────────────────────────

DEFAULT_PARAMS = {
    "rsi_length":          5,
    "oversold":           50,
    "overbought":         70,

    "trend_look_window":   5,
    "max_trend_strength":  1.0,    # 1.0 = 사실상 비활성 (모든 추세 허용)

    "volatility_window":   5,
    "min_volatility":      0.0,    # 0.0 = 비활성 (모든 변동성 허용)

    "stop_loss_pct":       2.0,
    "profit_target_pct":   3.0,

    "fee_rate":            0.0005, # 0.05% (Config.h EngineConfig::default_trade_fee_rate)

    "utilization":         1.0,    # 가용 KRW 중 사용 비율
    "reserve_margin":      1.001,  # 수수료 여유 (Config.h EngineConfig::reserve_margin)
    "min_notional_krw":    5000.0, # 최소 주문 금액 (Config.h StrategyConfig::min_notional_krw)
}


# ─── DB 경로 결정 ─────────────────────────────────────────────────────────────

def resolve_db_path(cli_path: str | None) -> str:
    """CLI > 환경변수 > repo root 자동 계산 순으로 DB 경로를 결정한다."""
    if cli_path:
        path = cli_path
    elif "COINBOT_DB_PATH" in os.environ:
        path = os.environ["COINBOT_DB_PATH"]
    else:
        repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        path = os.path.join(repo_root, "src", "db", "coinbot.db")

    if not os.path.exists(path):
        raise FileNotFoundError(
            f"[backtest] DB 파일 없음: {path}\n"
            "봇을 한 번 실행해 DB를 먼저 생성하세요."
        )
    return path


# ─── 데이터 로드 ──────────────────────────────────────────────────────────────

def load_candles(
    db_path: str,
    market: str,
    start_ts: str | None,
    end_ts: str | None,
) -> pd.DataFrame:
    """candles 테이블에서 해당 마켓의 캔들을 ts 오름차순으로 로드한다."""
    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA journal_mode=WAL;")
    try:
        query = "SELECT ts, open, high, low, close, volume FROM candles WHERE market = ?"
        params: list = [market]
        if start_ts:
            query += " AND ts >= ?"
            params.append(start_ts)
        if end_ts:
            query += " AND ts <= ?"
            params.append(end_ts)
        query += " ORDER BY ts ASC"
        df = pd.read_sql_query(query, conn, params=params)
    finally:
        conn.close()

    df["ts"] = pd.to_datetime(df["ts"])
    df = df.set_index("ts")
    return df


# ─── 지표 계산 ────────────────────────────────────────────────────────────────

def _compute_wilder_rsi(close_series: pd.Series, length: int) -> tuple[pd.Series, pd.Series]:
    """
    Wilder RSI 계산 (C++ RsiWilder.h 정렬).

    seed 단계: length개 delta를 모아 avgGain/avgLoss를 초기화.
    이후: Wilder smoothing (avgGain = (avgGain*(length-1) + gain) / length).
    경계값: both=0 → 50, loss=0 → 100, gain=0 → 0.

    반환: (rsi Series, ready bool Series)
    """
    closes = close_series.to_numpy(dtype=float)
    n = len(closes)
    rsi_arr   = np.full(n, np.nan)
    ready_arr = np.zeros(n, dtype=bool)

    prev_price  = None
    seed_count  = 0
    seed_gain   = 0.0
    seed_loss   = 0.0
    avg_gain    = 0.0
    avg_loss    = 0.0
    is_ready    = False

    for i, close in enumerate(closes):
        if prev_price is None:
            prev_price = close
            continue

        delta = close - prev_price
        gain  = delta  if delta > 0 else 0.0
        loss  = -delta if delta < 0 else 0.0
        prev_price = close

        if seed_count < length:
            seed_gain += gain
            seed_loss += loss
            seed_count += 1
            if seed_count == length:   # seed 완료: 첫 평균값 산출
                avg_gain = seed_gain / length
                avg_loss = seed_loss / length
                is_ready = True
        else:
            # Wilder smoothing
            avg_gain = (avg_gain * (length - 1) + gain) / length
            avg_loss = (avg_loss * (length - 1) + loss) / length

        if is_ready:
            ready_arr[i] = True
            if avg_gain == 0.0 and avg_loss == 0.0:
                rsi_arr[i] = 50.0
            elif avg_loss == 0.0:
                rsi_arr[i] = 100.0
            elif avg_gain == 0.0:
                rsi_arr[i] = 0.0
            else:
                rs = avg_gain / avg_loss
                rsi_arr[i] = 100.0 - 100.0 / (1.0 + rs)

    return (
        pd.Series(rsi_arr,   index=close_series.index),
        pd.Series(ready_arr, index=close_series.index),
    )


def _compute_volatility(close_series: pd.Series, window: int) -> tuple[pd.Series, pd.Series]:
    """
    변화율의 rolling stdev (C++ ChangeVolatilityIndicator 정렬).
    r = (close - prevClose) / prevClose, ddof=0 (모집단 표준편차).
    """
    pct = close_series.pct_change()             # r[i] = (close[i] - close[i-1]) / close[i-1]
    vol = pct.rolling(window=window).std(ddof=0) # ddof=0: 모집단 표준편차 (C++ stdev_() 정렬)
    return vol.fillna(0.0), vol.notna()


def _compute_trend_strength(close_series: pd.Series, look_window: int) -> tuple[pd.Series, pd.Series]:
    """
    추세 강도 (C++ ClosePriceWindow 정렬).
    trend = abs(close - close[N]) / close[N], N = look_window.
    ready: close[N]가 존재하려면 N+1개 값 필요 → shift(N) 이후 notna().
    """
    close_n = close_series.shift(look_window)
    trend   = (close_series - close_n).abs() / close_n
    return trend.fillna(0.0), close_n.notna()


# ─── 백테스트 핵심 ────────────────────────────────────────────────────────────

def run_backtest(
    db_path: str,
    market: str,
    start_ts: str | None = None,
    end_ts: str | None = None,
    params: dict | None = None,
    initial_krw: float = 1_000_000,
) -> dict:
    """
    RSI 평균회귀 전략 백테스트.

    반환:
        {
            'candles': DataFrame (index=ts, columns: open/high/low/close/volume/rsi/vol/trend/market_ok),
                       Plotly Candlestick + RSI 서브플롯에 직접 사용 가능.
            'trades':  DataFrame (entry_ts, exit_ts, entry_price, exit_price, pnl, pnl_pct, reason),
            'equity':  Series    (index=ts → 평가자산 KRW),
            'summary': dict      (total_trades, win_rate, total_pnl, avg_hold_minutes, open_position),
        }
    """
    p = {**DEFAULT_PARAMS, **(params or {})}

    df = load_candles(db_path, market, start_ts, end_ts)
    if df.empty:
        empty_summary = {
            "total_trades": 0, "win_rate": 0.0,
            "total_pnl": 0.0, "avg_hold_minutes": 0.0,
            "open_position": False,
        }
        return {
            "candles": df,
            "trades": pd.DataFrame(),
            "equity": pd.Series(dtype=float),
            "summary": empty_summary,
        }

    # 지표 계산
    df["rsi"],   df["rsi_ready"]   = _compute_wilder_rsi(df["close"], p["rsi_length"])
    df["vol"],   df["vol_ready"]   = _compute_volatility(df["close"], p["volatility_window"])
    df["trend"], df["trend_ready"] = _compute_trend_strength(df["close"], p["trend_look_window"])

    # market_ok: C++ marketOk 게이트 정렬 (RsiMeanReversionStrategy.cpp:229~239)
    # trendOk: trendReady && trendStrength <= max (trendReady 없으면 false)
    # volOk:   volReady   && vol >= min           (volReady   없으면 false)
    # rsiOk:   rsiReady
    df["market_ok"] = (
        df["rsi_ready"]
        & df["trend_ready"] & (df["trend"] <= p["max_trend_strength"])
        & df["vol_ready"]   & (df["vol"]   >= p["min_volatility"])
    )

    # 시뮬레이션 루프 (Flat ↔ InPosition 2상태 모델)
    trades: list[dict] = []
    equity: dict       = {}

    state       = "Flat"
    krw         = initial_krw
    coin_qty    = 0.0
    entry_price = 0.0
    stop_price  = 0.0
    target_price = 0.0
    total_cost  = 0.0   # 진입 시 실제 지출 (entry_krw + buy_fee)
    entry_ts    = None

    for ts, row in df.iterrows():
        close = float(row["close"])

        if state == "Flat":
            # 진입 조건: marketOk AND RSI 과매도 AND 최소 주문 금액 이상
            if row["market_ok"] and row["rsi"] <= p["oversold"]:
                entry_krw = krw / p["reserve_margin"] * p["utilization"]
                if entry_krw >= p["min_notional_krw"]:
                    buy_fee      = entry_krw * p["fee_rate"]
                    coin_qty     = entry_krw / close
                    total_cost   = entry_krw + buy_fee
                    krw         -= total_cost

                    entry_price  = close
                    stop_price   = close * (1.0 - p["stop_loss_pct"]  / 100.0)
                    target_price = close * (1.0 + p["profit_target_pct"] / 100.0)
                    entry_ts     = ts
                    state        = "InPosition"
                    # elif 구조 덕분에 같은 캔들에서 청산 체크 없이 다음 캔들로 넘어감

        elif state == "InPosition":
            # 보유 수량 가치 체크 (C++ maybeExit min_notional 조건)
            if coin_qty * close >= p["min_notional_krw"]:
                # 청산 조건 (우선순위: stop_loss > take_profit > rsi_exit)
                reason = None
                if close <= stop_price:
                    reason = "stop_loss"
                elif close >= target_price:
                    reason = "take_profit"
                elif row["rsi_ready"] and row["rsi"] >= p["overbought"]:
                    reason = "rsi_exit"

                if reason:
                    sell_gross = coin_qty * close
                    sell_fee   = sell_gross * p["fee_rate"]
                    sell_net   = sell_gross - sell_fee
                    pnl        = sell_net - total_cost
                    pnl_pct    = pnl / total_cost

                    krw += sell_net
                    trades.append({
                        "entry_ts":    entry_ts,
                        "exit_ts":     ts,
                        "entry_price": entry_price,
                        "exit_price":  close,
                        "pnl":         pnl,
                        "pnl_pct":     pnl_pct,
                        "reason":      reason,
                    })
                    coin_qty   = 0.0
                    total_cost = 0.0
                    state      = "Flat"

        # equity curve: 현금 + 미실현 평가금액
        equity[ts] = krw + (coin_qty * close if state == "InPosition" else 0.0)

    # 결과 집계
    trades_df = pd.DataFrame(trades)
    equity_s  = pd.Series(equity)

    open_position = (state == "InPosition")  # 기간 종료 시 미청산 포지션 여부

    if trades_df.empty:
        summary = {
            "total_trades": 0, "win_rate": 0.0,
            "total_pnl": 0.0, "avg_hold_minutes": 0.0,
            "open_position": open_position,
        }
    else:
        total = len(trades_df)
        wins  = int((trades_df["pnl"] > 0).sum())
        hold_min = (
            (trades_df["exit_ts"] - trades_df["entry_ts"])
            .dt.total_seconds() / 60.0
        ).mean()
        summary = {
            "total_trades":     total,
            "win_rate":         wins / total,
            "total_pnl":        float(trades_df["pnl"].sum()),
            "avg_hold_minutes": float(hold_min),
            "open_position":    open_position,
        }

    return {
        "candles": df,          # OHLC + 지표 컬럼 포함 → Plotly 캔들차트 + RSI 서브플롯에 직접 사용
        "trades":  trades_df,
        "equity":  equity_s,
        "summary": summary,
    }


# ─── CLI 진입점 ───────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="RSI 평균회귀 전략 백테스트")
    parser.add_argument("--db",     default=None,        help="SQLite DB 경로 (기본: src/db/coinbot.db)")
    parser.add_argument("--market", default="KRW-BTC",   help="마켓 (기본: KRW-BTC)")
    parser.add_argument("--days",   type=int, default=30, help="분석 기간 일 수 (기본: 30)")
    args = parser.parse_args()

    db_path  = resolve_db_path(args.db)
    start_ts = (datetime.now(KST) - timedelta(days=args.days)).strftime("%Y-%m-%dT%H:%M:%S")

    print(f"[backtest] DB: {db_path}")
    print(f"[backtest] 마켓: {args.market}, 기간: {args.days}일 ({start_ts} ~ )")

    result = run_backtest(db_path, args.market, start_ts=start_ts)

    s = result["summary"]
    print("\n=== 백테스트 결과 ===")
    print(f"  총 거래 수:       {s['total_trades']}")
    print(f"  승률:             {s['win_rate']:.1%}")
    print(f"  총손익:           {s['total_pnl']:,.0f} KRW")
    print(f"  평균 보유 시간:   {s['avg_hold_minutes']:.1f} 분")

    if not result["trades"].empty:
        csv_path = f"trades_{args.market.replace('-', '_')}.csv"
        result["trades"].to_csv(csv_path, index=False, encoding="utf-8-sig")
        print(f"\n  → 거래 목록 저장: {csv_path}")

    print("\n[backtest] 완료")


if __name__ == "__main__":
    main()
