"""
candle_rsi_backtest.py — 캔들 기반 RSI 평균회귀 전략 백테스트

용도: DB 캔들 기반으로 C++ 전략을 근사 시뮬레이션한다.
      Streamlit app.py Tab3에서 import candle_rsi_backtest as backtest로 사용하며, 독립 CLI로도 실행 가능.
실행: python tools/candle_rsi_backtest.py [--db <path>] [--market KRW-BTC] [--days 30]
한계:
  - 체결가를 close로 단순화 (실전은 시장가 VWAP)
  - 상태머신을 2상태로 축소 (실전: Flat/PendingEntry/InPosition/PendingExit)
  - 슬리피지: 고정 비율(slippage_rate) 반영 (매수=close×1.0005, 매도=close×0.9995)
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
    "slippage_rate":       0.0005, # 0.05% 슬리피지: 매수는 close보다 비싸게, 매도는 싸게 체결

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
        path = os.path.join(repo_root, "db", "coinbot.db")

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
    unit: int = 15,
) -> pd.DataFrame:
    """candles 테이블에서 해당 마켓·unit의 캔들을 ts 오름차순으로 로드한다."""
    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA journal_mode=WAL;")
    try:
        query = "SELECT ts, open, high, low, close, volume FROM candles WHERE market = ? AND unit = ?"
        params: list = [market, unit]
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


def detect_candle_gaps(index: pd.Index, unit: int) -> dict:
    """인접 캔들 간격을 검사해 누락 구간을 경고용으로 요약한다."""
    empty = {
        "has_gaps": False,
        "gap_count": 0,
        "missing_candles": 0,
        "max_gap_minutes": 0,
        "samples": [],
    }
    if len(index) < 2 or unit <= 0:
        return empty

    ts_index = pd.DatetimeIndex(index)
    expected_gap = pd.Timedelta(minutes=unit)
    diffs = ts_index.to_series().diff().dropna()
    gap_rows = diffs[diffs > expected_gap]
    if gap_rows.empty:
        return empty

    missing_total = 0
    max_gap_minutes = 0
    samples: list[str] = []
    for gap_end, gap_delta in gap_rows.items():
        gap_minutes = int(gap_delta.total_seconds() // 60)
        missing = max((gap_minutes // unit) - 1, 0)
        missing_total += missing
        max_gap_minutes = max(max_gap_minutes, gap_minutes)

        if len(samples) < 3:
            gap_start = gap_end - gap_delta
            samples.append(
                f"{gap_start.strftime('%Y-%m-%d %H:%M')} -> "
                f"{gap_end.strftime('%Y-%m-%d %H:%M')} "
                f"(누락 {missing}캔들)"
            )

    return {
        "has_gaps": True,
        "gap_count": int(len(gap_rows)),
        "missing_candles": int(missing_total),
        "max_gap_minutes": int(max_gap_minutes),
        "samples": samples,
    }


# ─── 지표 계산 ────────────────────────────────────────────────────────────────

def _compute_wilder_rsi(close_series: pd.Series, length: int) -> tuple[pd.Series, pd.Series]:
    """
    Wilder RSI 계산 (C++ RsiWilder.h 정렬).

    seed 단계: length개 delta를 모아 avgGain/avgLoss를 초기화.
    이후: Wilder smoothing (avgGain = (avgGain*(length-1) + gain) / length).
    경계값: both=0 → 50, loss=0 → 100, gain=0 → 0.

    반환: (rsi Series, ready bool Series)

    [최적화] delta/gain/loss를 numpy 벡터 연산으로 한 번에 계산.
    seed 합산도 np.sum으로 처리. Wilder smoothing 루프만 유지 (재귀적 특성상 벡터화 불가).
    """
    closes = close_series.to_numpy(dtype=float)
    n      = len(closes)
    rsi_arr   = np.full(n, np.nan)
    ready_arr = np.zeros(n, dtype=bool)

    # 데이터가 seed를 채우기에 부족하면 전부 NaN/False 반환
    if n <= length:
        return (
            pd.Series(rsi_arr,   index=close_series.index),
            pd.Series(ready_arr, index=close_series.index),
        )

    # delta/gain/loss 배열을 벡터 연산으로 한 번에 계산 (Python 루프 제거)
    deltas = np.diff(closes)                           # shape: n-1
    gains  = np.where(deltas > 0,  deltas,  0.0)
    losses = np.where(deltas < 0, -deltas,  0.0)

    # seed 단계: 첫 length개 delta로 초기 avg 계산 (np.sum → 벡터화)
    avg_gain = float(np.sum(gains[:length])  / length)
    avg_loss = float(np.sum(losses[:length]) / length)

    def _rsi(ag: float, al: float) -> float:
        if ag == 0.0 and al == 0.0:
            return 50.0
        if al == 0.0:
            return 100.0
        if ag == 0.0:
            return 0.0
        return 100.0 - 100.0 / (1.0 + ag / al)

    # seed 완료 시점: deltas[length-1] 처리 후 → closes 인덱스 = length
    rsi_arr[length]   = _rsi(avg_gain, avg_loss)
    ready_arr[length] = True

    # Wilder smoothing: seed 이후 구간만 루프 (재귀 특성으로 순차 처리 필수)
    alpha = (length - 1) / length
    for i in range(length, n - 1):   # i = deltas 인덱스
        avg_gain = alpha * avg_gain + gains[i]  / length
        avg_loss = alpha * avg_loss + losses[i] / length
        ci = i + 1                    # closes 인덱스
        rsi_arr[ci]   = _rsi(avg_gain, avg_loss)
        ready_arr[ci] = True

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
    unit: int = 15,
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
            'data_quality': dict (중간 캔들 누락 경고 정보),
        }
    """
    p = {**DEFAULT_PARAMS, **(params or {})}

    df = load_candles(db_path, market, start_ts, end_ts, unit)
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
            "data_quality": detect_candle_gaps(df.index, unit),
        }

    # 결측 캔들은 자동 보정하지 않으므로, 실행은 계속하되 경고 정보를 별도로 만든다.
    data_quality = detect_candle_gaps(df.index, unit)

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

    # itertuples(): iterrows() 대비 5~10배 빠름 (매 행마다 Series 생성 오버헤드 제거)
    for row in df.itertuples():
        ts    = row.Index
        close = float(row.close)

        if state == "Flat":
            # 진입 조건: marketOk AND RSI 과매도 AND 최소 주문 금액 이상
            if row.market_ok and row.rsi <= p["oversold"]:
                entry_krw = krw / p["reserve_margin"] * p["utilization"]
                if entry_krw >= p["min_notional_krw"]:
                    buy_price    = close * (1.0 + p["slippage_rate"])  # 매수: close보다 비싸게 체결
                    buy_fee      = entry_krw * p["fee_rate"]
                    coin_qty     = entry_krw / buy_price
                    total_cost   = entry_krw + buy_fee
                    krw         -= total_cost

                    entry_price  = buy_price
                    stop_price   = buy_price * (1.0 - p["stop_loss_pct"]  / 100.0)
                    target_price = buy_price * (1.0 + p["profit_target_pct"] / 100.0)
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
                elif row.rsi_ready and row.rsi >= p["overbought"]:
                    reason = "rsi_exit"

                if reason:
                    sell_price = close * (1.0 - p["slippage_rate"])  # 매도: close보다 싸게 체결
                    sell_gross = coin_qty * sell_price
                    sell_fee   = sell_gross * p["fee_rate"]
                    sell_net   = sell_gross - sell_fee
                    pnl        = sell_net - total_cost
                    pnl_pct    = pnl / total_cost

                    krw += sell_net
                    trades.append({
                        "entry_ts":    entry_ts,
                        "exit_ts":     ts,
                        "entry_price": entry_price,
                        "exit_price":  sell_price,
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
        "data_quality": data_quality,
    }


# ─── CLI 진입점 ───────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="RSI 평균회귀 전략 백테스트")
    parser.add_argument("--db",     default=None,        help="SQLite DB 경로 (기본: db/coinbot.db)")
    parser.add_argument("--market", default="KRW-BTC",   help="마켓 (기본: KRW-BTC)")
    parser.add_argument("--days",   type=int, default=30, help="분석 기간 일 수 (기본: 30, --start 미지정 시 사용)")
    parser.add_argument("--start",  default=None,        help="시작 날짜 (YYYY-MM-DD, 지정 시 --days 무시)")
    parser.add_argument("--end",    default=None,        help="종료 날짜 (YYYY-MM-DD, 미지정 시 현재까지)")
    parser.add_argument("--unit",   type=int, default=15,
                        choices=[1, 3, 5, 10, 15, 30, 60, 240],
                        help="분봉 단위 (기본: 15)")
    args = parser.parse_args()

    db_path = resolve_db_path(args.db)

    # start_ts: --start 지정 시 해당 날짜 00:00:00, 미지정 시 현재 기준 --days 전
    if args.start:
        start_ts = f"{args.start}T00:00:00"
    else:
        start_ts = (datetime.now(KST) - timedelta(days=args.days)).strftime("%Y-%m-%dT%H:%M:%S")

    # end_ts: --end 지정 시 해당 날짜 23:59:59, 미지정 시 None (현재까지)
    end_ts = f"{args.end}T23:59:59" if args.end else None

    # 로그: 요청 기간 표시
    period_desc = f"{args.start} ~ {args.end or '현재'}" if args.start else f"최근 {args.days}일 ({start_ts} ~ )"
    print(f"[backtest] DB: {db_path}")
    print(f"[backtest] 마켓: {args.market}, 요청 기간: {period_desc}")

    result = run_backtest(db_path, args.market, start_ts=start_ts, end_ts=end_ts, unit=args.unit)

    candles = result["candles"]
    if candles.empty:
        print("[backtest] 데이터 없음. fetch_candles.py로 캔들을 먼저 수집하세요.")
        return

    actual_start = candles.index.min()
    actual_end   = candles.index.max()
    actual_days  = (actual_end - actual_start).total_seconds() / 86400
    print(f"[backtest] 실제 데이터: {actual_start} ~ {actual_end} ({len(candles)}캔들, {actual_days:.1f}일)")

    dq = result.get("data_quality", {})
    if dq.get("has_gaps"):
        print(
            f"[backtest] ⚠ 중간 데이터 누락 구간 {dq['gap_count']}개, "
            f"누락 캔들 {dq['missing_candles']}개, 최대 공백 {dq['max_gap_minutes']}분"
        )
        for sample in dq.get("samples", []):
            print(f"  - {sample}")

    # 요청 기간 대비 실제 데이터 비율 체크 (모든 케이스 공통)
    if args.start:
        # --start/--end 지정 시: 요청 기간(일) 계산 후 비교
        end_dt    = datetime.strptime(args.end, "%Y-%m-%d") if args.end else datetime.now(KST).replace(tzinfo=None)
        start_dt  = datetime.strptime(args.start, "%Y-%m-%d")
        requested_days = (end_dt - start_dt).total_seconds() / 86400
    else:
        requested_days = args.days

    if actual_days < requested_days * 0.9:
        print(f"[backtest] ⚠ 요청({requested_days:.0f}일)보다 짧은 데이터({actual_days:.1f}일)입니다. fetch_candles.py --days {int(requested_days) + 1} 로 보강하세요.")

    s = result["summary"]
    print("\n=== 백테스트 결과 ===")
    print(f"  총 거래 수:       {s['total_trades']}")
    print(f"  승률:             {s['win_rate']:.1%}")
    print(f"  총손익:           {s['total_pnl']:,.0f} KRW")
    print(f"  평균 보유 시간:   {s['avg_hold_minutes']:.1f} 분")

    if not result["trades"].empty:
        results_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "backtest_results")
        os.makedirs(results_dir, exist_ok=True)
        csv_path = os.path.join(results_dir, f"trades_{args.market.replace('-', '_')}.csv")
        result["trades"].to_csv(csv_path, index=False, encoding="utf-8-sig")
        print(f"\n  → 거래 목록 저장: {csv_path}")

    print("\n[backtest] 완료")


if __name__ == "__main__":
    main()
