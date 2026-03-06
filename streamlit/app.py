"""
streamlit/app.py — CoinBot 대시보드

탭 구성:
  Tab 1: 분석
    - [P&L] 서브탭       : orders 테이블 기반 실거래 손익 분석
    - [전략 분석] 서브탭 : signals + candles 기반 차트·통계
  Tab 2: 백테스트
    - candle_rsi_backtest 모듈로 시뮬레이션 결과 표시
    - 실거래 signals 오버레이 (옵션)

실행: streamlit run streamlit/app.py  (repo root 기준)

[레이아웃 개선 사항]
1. render_analysis를 render_pnl / render_strategy 로 분리 → 분석 탭 내 서브탭 구성.
2. P&L 바차트 + 누적곡선을 단일 make_subplots figure로 통합 (차트 2개 → 1개).
3. 마켓별 성과 요약 + 거래 내역을 expander 하나로 묶어 기본 숨김.
4. 전략 분석: RSI 히스토그램·청산사유 도넛을 동등한 2컬럼으로 배치 (기존 3컬럼 → 2컬럼).
   보조 지표(평균 보유·부분청산)는 차트 아래 metric 2개로 배치.
5. 백테스트 결과 영역을 [차트] / [거래내역] 서브탭으로 분리 → 수직 스크롤 제거.
6. 차트 높이 상수(_H_*)로 통일하여 일관성 확보.

[기존 설계 수정 사항 유지]
1. signals에 paid_fee 없음 → P&L은 orders 단독 계산. signals는 전략 컨텍스트 전용.
2. "BID created_at_ms 기준 BUY 창" 정의 불명확 → identifier 기반 페어링 + FIFO fallback 구현.
3. RSI 서브플롯 기준선은 파라미터 슬라이더(oversold/overbought) 값을 동적으로 사용.
4. 사이드바 마켓 multiselect 대신 탭 내부 selectbox 유지 — 탭 간 독립 마켓 선택 가능.
5. 복합 exit_reason은 value_counts() 완전 일치 집계 유지 — 파이차트에서 중복 집계 불가.
"""

import contextlib
import os
import sys
from datetime import date, datetime, timedelta
from zoneinfo import ZoneInfo
import sqlite3

import pandas as pd
import plotly.graph_objects as go
from plotly.subplots import make_subplots
import streamlit as st

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# [수정 #1] 백테스트 모듈 lazy import: 실패 시 앱 전체 크래시 방지
try:
    sys.path.insert(0, os.path.join(_REPO_ROOT, "tools"))
    import candle_rsi_backtest as _backtest
except ImportError:
    _backtest = None

KST = ZoneInfo("Asia/Seoul")
DEFAULT_DB_PATH = os.path.join(_REPO_ROOT, "src", "db", "coinbot.db")

# ─── 청산 이유 한글 매핑 ────────────────────────────────────────────────────────
_EXIT_REASON_KR: dict[str, str] = {
    # 실거래 signals 테이블 (exit_ 접두사)
    "exit_stop":                       "손절가 도달",
    "exit_target":                     "익절가 도달",
    "exit_rsi_overbought":             "RSI 과매수",
    "exit_unknown":                    "알 수 없음",
    "exit_stop_target":                "손절가+익절가 도달",
    "exit_stop_rsi_overbought":        "손절가+RSI 과매수",
    "exit_target_rsi_overbought":      "익절가+RSI 과매수",
    "exit_stop_target_rsi_overbought": "손절가+익절가+RSI 과매수",

    # 백테스트 시뮬레이션 (candle_rsi_backtest.py)
    "stop_loss":                       "손절가 도달",
    "take_profit":                     "익절가 도달",
    "rsi_exit":                        "RSI 과매수",
}

def _kr_exit_reason(reason: str | None) -> str:
    """exit_reason 영문 코드 → 한글 변환. 미등록 코드는 원문 반환."""
    if not reason or reason == "–":
        return "–"
    return _EXIT_REASON_KR.get(reason, reason)

# ─── 차트 높이 상수 ────────────────────────────────────────────────────────────
_H_MAIN  = 560   # 메인 캔들+RSI 복합 차트
_H_COMBO = 440   # P&L 통합 차트 (바 + 누적곡선 서브플롯)
_H_SUB   = 300   # 보조 차트 (히스토그램, 파이, 수익곡선)
_MARGIN  = dict(t=36, b=16, l=8, r=8)


# ─── DB 헬퍼 ──────────────────────────────────────────────────────────────────

def _connect(db_path: str) -> sqlite3.Connection:
    conn = sqlite3.connect(db_path, check_same_thread=False)
    conn.execute("PRAGMA journal_mode=WAL;")
    return conn


@contextlib.contextmanager
def _db(db_path: str):
    """DB 연결 context manager — with 블록 종료 시 자동 close."""
    conn = _connect(db_path)
    try:
        yield conn
    finally:
        conn.close()


@st.cache_data(ttl=60)
def load_markets(db_path: str) -> list[str]:
    """candles 테이블에 데이터가 있는 마켓 목록."""
    with _db(db_path) as conn:
        rows = conn.execute(
            "SELECT DISTINCT market FROM candles ORDER BY market"
        ).fetchall()
    return [r[0] for r in rows]


@st.cache_data(ttl=60)
def load_orders(db_path: str, market: str | None, start_ms: int, end_ms: int) -> pd.DataFrame:
    """
    P&L 대상 주문 로드.

    [수정 #3] 명세 기준 정밀 필터 적용:
      - created_at_ms > 0: 파싱 실패 행(1970-01-01 귀속) 제외
      - Filled 또는 (Canceled AND executed_volume > 0): 완전 미체결 취소 제외
    """
    q = (
        "SELECT * FROM orders "
        "WHERE created_at_ms > 0 "
        "AND (status = 'Filled' OR (status = 'Canceled' AND executed_volume > 0)) "
        "AND created_at_ms BETWEEN ? AND ?"
    )
    params: list = [start_ms, end_ms]
    if market:
        q += " AND market = ?"
        params.append(market)
    q += " ORDER BY created_at_ms ASC"
    with _db(db_path) as conn:
        return pd.read_sql_query(q, conn, params=params)


@st.cache_data(ttl=60)
def load_signals(db_path: str, market: str | None, start_ms: int, end_ms: int) -> pd.DataFrame:
    """BUY/SELL 신호 로드 (ts_ms 기준)."""
    q = "SELECT * FROM signals WHERE ts_ms BETWEEN ? AND ?"
    params: list = [start_ms, end_ms]
    if market:
        q += " AND market = ?"
        params.append(market)
    q += " ORDER BY ts_ms ASC"
    with _db(db_path) as conn:
        return pd.read_sql_query(q, conn, params=params)


@st.cache_data(ttl=60)
def load_candles(db_path: str, market: str, start_ts: str, end_ts: str, unit: int = 15) -> pd.DataFrame:
    """candles 테이블 로드 → ts를 index로 반환. unit으로 분봉 단위 구분."""
    with _db(db_path) as conn:
        df = pd.read_sql_query(
            "SELECT ts, open, high, low, close, volume FROM candles "
            "WHERE market=? AND ts BETWEEN ? AND ? AND unit=? ORDER BY ts ASC",
            conn, params=[market, start_ts, end_ts, unit],
        )
    if df.empty:
        return df
    df["ts"] = pd.to_datetime(df["ts"])
    return df.set_index("ts")


# ─── P&L 페어링 ───────────────────────────────────────────────────────────────

def pair_trades(orders: pd.DataFrame, signals: pd.DataFrame | None = None) -> pd.DataFrame:
    """
    BID↔ASK 페어링 → 거래 사이클 DataFrame 반환.

    단일 포지션 모델 기반 시간 창 FIFO:
      BID 시각 < ASK 시각 < 다음 BID 시각 → 한 사이클.
      부분청산(Canceled+executed_volume>0) ASK도 같은 창에 포함되어 revenue에 합산.

    P&L 산식 (orders 기반):
      cost    = BID.executed_funds + BID.paid_fee
      revenue = SUM(ASK.executed_funds - ASK.paid_fee)
      pnl     = revenue - cost

    exit_reason 결정 우선순위:
      1. ASK.identifier → SELL signal.identifier 직접 조인 (is_partial=0 완전청산 우선)
      2. 조인 실패 시 close_ts_ms 기준 시간 근사 fallback (300초 창)
    """
    if orders.empty:
        return pd.DataFrame()

    # SELL 신호 사전 구성: identifier → {exit_reason, is_partial}
    # ASK order.identifier == SELL signal.identifier (C++ maybeExit에서 동일 cid 주입)
    sell_sig_map: dict[str, dict] = {}
    sell_sigs_ts = pd.DataFrame()  # 시간 근사 fallback용
    if signals is not None and not signals.empty:
        sell_sigs = signals[signals["side"] == "SELL"][
            ["market", "identifier", "ts_ms", "exit_reason", "is_partial"]
        ].copy()
        for _, sig in sell_sigs.iterrows():
            ident = sig["identifier"]
            if ident and pd.notna(ident):
                sell_sig_map[ident] = {
                    "exit_reason": sig["exit_reason"] or "–",
                    "is_partial":  int(sig["is_partial"]),
                }
        sell_sigs_ts = sell_sigs

    result: list[dict] = []

    for market, grp in orders.groupby("market"):
        bids = grp[grp["side"] == "BID"].sort_values("created_at_ms").reset_index(drop=True)
        asks = grp[grp["side"] == "ASK"].sort_values("created_at_ms")

        asks_list = list(asks.iterrows())
        ask_idx   = 0
        bids_ts   = bids["created_at_ms"].tolist()

        for i in range(len(bids)):
            bid         = bids.iloc[i]
            cost        = bid["executed_funds"] + bid["paid_fee"]
            next_bid_ts = bids_ts[i + 1] if i + 1 < len(bids_ts) else None

            matched_rows: list = []
            while ask_idx < len(asks_list):
                _, ask_row = asks_list[ask_idx]
                # BID 이전 ASK는 이전 사이클 orphan → 소비 후 건너뜀
                if ask_row["created_at_ms"] <= bid["created_at_ms"]:
                    ask_idx += 1
                    continue
                if next_bid_ts and ask_row["created_at_ms"] >= next_bid_ts:
                    break
                matched_rows.append(ask_row)
                ask_idx += 1

            if not matched_rows:
                continue

            mdf         = pd.DataFrame(matched_rows)
            revenue     = (mdf["executed_funds"] - mdf["paid_fee"]).sum()
            close_ts_ms = mdf["created_at_ms"].max()

            # exit_reason 결정
            # 1. ASK.identifier → SELL signal 직접 조인 (is_partial=0 완전청산 우선)
            final_reason   = "–"
            partial_reason = "–"
            for _, ask_row in mdf.iterrows():
                ask_id = ask_row.get("identifier") or ""
                if ask_id not in sell_sig_map:
                    continue
                info = sell_sig_map[ask_id]
                if info["is_partial"] == 0:
                    final_reason = info["exit_reason"]
                elif partial_reason == "–":
                    partial_reason = info["exit_reason"]
            exit_reason = final_reason if final_reason != "–" else partial_reason

            # 2. 직접 조인 실패 시 시간 근사 fallback (구버전 데이터 등)
            if exit_reason == "–" and not sell_sigs_ts.empty:
                mkt_sigs = sell_sigs_ts[sell_sigs_ts["market"] == market]
                window   = mkt_sigs[
                    (mkt_sigs["ts_ms"] > bid["created_at_ms"]) &
                    (mkt_sigs["ts_ms"] <= close_ts_ms + 300_000)
                ]
                if not window.empty:
                    # is_partial=0 완전청산 우선, 없으면 is_partial=1으로 내려감
                    candidates  = window[window["is_partial"] == 0]
                    if candidates.empty:
                        candidates = window
                    idx         = (candidates["ts_ms"] - close_ts_ms).abs().idxmin()
                    exit_reason = candidates.loc[idx, "exit_reason"] or "–"

            result.append({
                "market":      market,
                "open_ts_ms":  bid["created_at_ms"],
                "close_ts_ms": close_ts_ms,
                "cost":        cost,
                "revenue":     revenue,
                "pnl":         revenue - cost,
                "pnl_pct":     (revenue - cost) / cost if cost > 0 else 0.0,
                "n_asks":      len(mdf),
                "exit_reason": exit_reason,
            })

    if not result:
        return pd.DataFrame()

    df = pd.DataFrame(result).sort_values("close_ts_ms").reset_index(drop=True)

    def _to_kst_date(col: str) -> pd.Series:
        return pd.to_datetime(df[col], unit="ms", utc=True).dt.tz_convert("Asia/Seoul").dt.date

    df["open_date"]  = _to_kst_date("open_ts_ms")
    df["close_date"] = _to_kst_date("close_ts_ms")
    return df


# ─── 전략 분석 헬퍼 ───────────────────────────────────────────────────────────

def compute_avg_hold_minutes(signals: pd.DataFrame) -> float | None:
    """
    [수정 #5-A] BUY↔SELL(is_partial=0) 페어링으로 평균 보유 기간(분) 계산.

    각 BUY 이후 같은 마켓의 첫 번째 완전청산 SELL과 매칭.
    is_partial=1 SELL은 포지션 유지 중이므로 제외.
    """
    buys  = signals[signals["side"] == "BUY"].sort_values("ts_ms")
    sells = signals[(signals["side"] == "SELL") & (signals["is_partial"] == 0)].sort_values("ts_ms")

    if buys.empty or sells.empty:
        return None

    hold_times_min: list[float] = []
    used_sell_indices: set = set()
    for _, buy in buys.iterrows():
        candidates = sells[
            (sells["market"] == buy["market"]) &
            (sells["ts_ms"] > buy["ts_ms"]) &
            (~sells.index.isin(used_sell_indices))  # 이미 사용된 SELL 제외
        ]
        if not candidates.empty:
            matched_idx = candidates.index[0]
            used_sell_indices.add(matched_idx)
            hold_ms = candidates["ts_ms"].iloc[0] - buy["ts_ms"]
            hold_times_min.append(hold_ms / 60_000)

    return sum(hold_times_min) / len(hold_times_min) if hold_times_min else None


# ─── 차트 헬퍼 ────────────────────────────────────────────────────────────────

# 봇 기본값(Config.h StrategyConfig)과 동기화
_RSI_OVERSOLD  = 30
_RSI_OVERBOUGHT = 70


@st.cache_data(show_spinner=False)
def make_candle_signal_chart(
    candles: pd.DataFrame,
    signals: pd.DataFrame,
    title: str,
) -> go.Figure:
    """
    캔들차트 + BUY/SELL 마커 + RSI 서브플롯.

    [수정 #2] SELL 마커는 is_partial=0(완전 청산)만 표시.
              부분청산은 포지션이 유지되므로 완전청산과 동일 마커로 표시하면 오독 위험.
    [수정 #1] _backtest=None이면 RSI 서브플롯 없이 캔들차트만 반환.
    """
    has_rsi     = _backtest is not None
    rows        = 2 if has_rsi else 1
    row_heights = [0.72, 0.28] if has_rsi else [1.0]
    subtitles   = [title, "RSI (5)"] if has_rsi else [title]

    fig = make_subplots(
        rows=rows, cols=1, shared_xaxes=True,
        row_heights=row_heights, vertical_spacing=0.03,
        subplot_titles=subtitles,
    )

    fig.add_trace(go.Candlestick(
        x=candles.index,
        open=candles["open"], high=candles["high"],
        low=candles["low"],   close=candles["close"],
        name="캔들", showlegend=False,
        increasing_line_color="#ff4455", decreasing_line_color="#4488ff",
    ), row=1, col=1)

    if not signals.empty:
        def _sig_ts(df: pd.DataFrame) -> pd.Series:
            return pd.to_datetime(df["ts_ms"], unit="ms", utc=True).dt.tz_convert("Asia/Seoul")

        buy_sigs     = signals[signals["side"] == "BUY"]
        # is_partial=0: Filled 경로 완전 청산
        sell_full    = signals[(signals["side"] == "SELL") & (signals["is_partial"] == 0)]
        # is_partial=1: Canceled 경로 실질 종료
        sell_partial = signals[(signals["side"] == "SELL") & (signals["is_partial"] == 1)]

        if not buy_sigs.empty:
            fig.add_trace(go.Scatter(
                x=_sig_ts(buy_sigs), y=buy_sigs["price"],
                mode="markers", name="BUY",
                marker=dict(symbol="triangle-up", size=11, color="#00e5a0",
                            line=dict(width=1, color="#008855")),
            ), row=1, col=1)

        if not sell_full.empty:
            fig.add_trace(go.Scatter(
                x=_sig_ts(sell_full), y=sell_full["price"],
                mode="markers", name="SELL (완전)",
                marker=dict(symbol="triangle-down", size=11, color="#ff4d6d",
                            line=dict(width=1, color="#aa0022")),
            ), row=1, col=1)

        if not sell_partial.empty:
            fig.add_trace(go.Scatter(
                x=_sig_ts(sell_partial), y=sell_partial["price"],
                mode="markers", name="SELL (부분/실질종료)",
                marker=dict(symbol="triangle-down", size=11, color="#ff4d6d",
                            line=dict(width=1, color="#aa0022"), opacity=0.45),
            ), row=1, col=1)

    if has_rsi:
        rsi_series, rsi_ready = _backtest._compute_wilder_rsi(candles["close"], length=5)
        fig.add_trace(go.Scatter(
            x=candles.index[rsi_ready],
            y=rsi_series[rsi_ready],
            name="RSI(5)", line=dict(color="#f9c846", width=1.2),
        ), row=2, col=1)

        for level, color in [(_RSI_OVERSOLD, "#5b8dee"), (_RSI_OVERBOUGHT, "#ff4d6d")]:
            fig.add_hline(y=level, line_dash="dot", line_color=color,
                          opacity=0.55, row=2, col=1)

    fig.update_layout(
        xaxis_rangeslider_visible=False,
        height=_H_MAIN,
        margin=_MARGIN,
        legend=dict(orientation="h", yanchor="bottom", y=1.01, xanchor="right", x=1),
        paper_bgcolor="#0d1117",
        plot_bgcolor="#0d1117",
        font=dict(color="#8899aa"),
    )
    fig.update_xaxes(gridcolor="#1a2234", zeroline=False)
    fig.update_yaxes(gridcolor="#1a2234", zeroline=False)
    return fig


def make_pnl_combo_chart(
    trades_ts: pd.DataFrame,
    agg: pd.Series,
    cum: pd.Series,
    y_label: str,
) -> go.Figure:
    """
    [레이아웃 개선 #2] 기간별 손익 바차트 + 누적 손익 곡선을 단일 figure 서브플롯으로 통합.
    차트 2개 → 1개로 줄여 수직 공간 절약 및 맥락 파악 용이.
    """
    fig = make_subplots(
        rows=2, cols=1, shared_xaxes=True,
        row_heights=[0.55, 0.45], vertical_spacing=0.04,
        subplot_titles=[y_label, "누적 손익 (KRW)"],
    )

    # row1: 기간별 손익 바차트
    fig.add_trace(go.Bar(
        x=agg.index, y=agg.values,
        marker_color=["#ff4d6d" if v >= 0 else "#5b8dee" for v in agg.values],
        name=y_label, showlegend=False,
    ), row=1, col=1)

    # row2: 누적 손익 곡선
    fig.add_trace(go.Scatter(
        x=cum.index, y=cum.values,
        fill="tozeroy",
        fillcolor="rgba(0,229,160,0.08)",
        line=dict(color="#00e5a0", width=1.5),
        name="누적 손익", showlegend=False,
    ), row=2, col=1)

    fig.update_layout(
        height=_H_COMBO,
        margin=_MARGIN,
        paper_bgcolor="#0d1117",
        plot_bgcolor="#0d1117",
        font=dict(color="#8899aa"),
    )
    fig.update_xaxes(gridcolor="#1a2234", zeroline=False)
    fig.update_yaxes(gridcolor="#1a2234", zeroline=False)

    # 두 서브플롯 사이 구분선 (row_heights=[0.55,0.45], spacing=0.04 기준 경계 y≈0.45)
    fig.add_shape(
        type="line", xref="paper", yref="paper",
        x0=0, x1=1, y0=0.45, y1=0.45,
        line=dict(color="#2a3a4a", width=1),
    )
    return fig


# ─── P&L 서브탭 ───────────────────────────────────────────────────────────────

def render_pnl(
    db_path: str, markets: list[str],
    start_ms: int, end_ms: int,
) -> None:
    """
    [레이아웃 개선 #1] 기존 render_analysis에서 P&L 섹션만 분리.
    분석 탭 내 [P&L] 서브탭으로 렌더링.
    """
    # 컨트롤 바: 마켓 | 표시단위
    c_mkt, c_mode = st.columns([3, 1])
    with c_mkt:
        market_sel = st.selectbox("마켓", ["전체"] + markets, key="pnl_mkt")
    with c_mode:
        chart_mode = st.radio("표시 단위", ["KRW", "%"], horizontal=True, key="pnl_mode")

    market_filter = None if market_sel == "전체" else market_sel
    orders  = load_orders(db_path, market_filter, start_ms, end_ms)
    signals = load_signals(db_path, market_filter, start_ms, end_ms)
    trades  = pair_trades(orders, signals)

    if trades.empty:
        st.info("거래 데이터 없음. 봇을 실행한 뒤 다시 확인하세요.")
        return

    trades["close_ts"] = (
        pd.to_datetime(trades["close_ts_ms"], unit="ms", utc=True)
        .dt.tz_convert("Asia/Seoul")
    )
    trades_ts = trades.set_index("close_ts")

    # ── 전체 기간 요약 KPI ────────────────────────────────────────────────────
    total_pnl = float(trades["pnl"].sum())
    twr       = float((trades["pnl_pct"] + 1).prod() - 1)
    win_rate  = float((trades["pnl"] > 0).mean())
    n_trades  = len(trades)

    # 누적 손익 (차트 + MDD 기반 계산용)
    cum_pnl = trades_ts["pnl"].cumsum()

    # Profit Factor: 총 수익 / 총 손실 절대값 (1.0 이상이면 수익 전략)
    gross_profit = trades.loc[trades["pnl"] > 0, "pnl"].sum()
    gross_loss   = trades.loc[trades["pnl"] < 0, "pnl"].abs().sum()
    pf           = gross_profit / gross_loss if gross_loss > 0 else float("inf")

    # RR Ratio: 평균 수익거래 / 평균 손실거래 절대값
    avg_win      = trades.loc[trades["pnl"] > 0, "pnl"].mean()
    avg_loss_val = trades.loc[trades["pnl"] < 0, "pnl"].abs().mean()
    rr = avg_win / avg_loss_val if (pd.notna(avg_win) and pd.notna(avg_loss_val) and avg_loss_val > 0) else float("nan")

    k1, k2, k3, k4, k5 = st.columns(5)
    k1.metric("총 손익",
              f"{total_pnl:,.0f} KRW",
              delta=f"{total_pnl:+,.0f}" if total_pnl != 0 else None)
    k2.metric("총 수익률 (TWR)",
              f"{twr:.2%}",
              delta=f"{twr:+.2%}" if twr != 0 else None,
              help="거래별 수익률 체인 곱 — 재투자 효과 반영")
    k3.metric("승률",
              f"{win_rate:.1%}",
              help=f"총 {n_trades}건 중 수익 {int(win_rate * n_trades)}건")
    k4.metric("Profit Factor",
              f"{pf:.2f}" if pf != float('inf') else "∞",
              help="총 수익 / 총 손실 — 1.0 이상이면 수익 전략")
    k5.metric("RR Ratio",
              f"{rr:.2f}" if pd.notna(rr) else "–",
              help="평균 수익거래 / 평균 손실거래 — 2.0이면 수익이 손실의 2배")

    # ── 차트 집계: 90일 이하 일별, 초과 주별 자동 결정 ─────────────────────────
    range_days = (end_ms - start_ms) / (1000 * 86400)
    if range_days <= 90:
        chart_freq, freq_label = "D",     "일"
    else:
        chart_freq, freq_label = "W-MON", "주"

    chart_groups = trades_ts.resample(chart_freq)
    if chart_mode == "KRW":
        agg     = chart_groups["pnl"].sum()
        y_label = f"{freq_label}별 손익 (KRW)"
    else:
        agg = chart_groups["pnl_pct"].apply(
            lambda x: float((1 + x).prod() - 1) * 100 if len(x) > 0 else 0.0
        ).fillna(0)
        y_label = f"{freq_label}별 수익률 (%)"

    cum = cum_pnl  # MDD 계산에서 이미 생성

    # [레이아웃 개선 #2] 바차트 + 누적곡선 통합 figure
    fig_combo = make_pnl_combo_chart(trades_ts, agg, cum, y_label)
    st.plotly_chart(fig_combo, use_container_width=True)

    # [레이아웃 개선 #3] 마켓별 성과 요약 + 거래 내역을 expander 하나로 묶음
    with st.expander("마켓별 성과 / 거래 내역"):
        def _market_summary(g: pd.DataFrame) -> pd.Series:
            cost_sum      = g["cost"].sum()
            pnl_sum       = g["pnl"].sum()
            valid_reasons = g["exit_reason"].dropna()
            top_reason    = _kr_exit_reason(
                valid_reasons.value_counts().index[0] if not valid_reasons.empty else "–"
            )
            twr = float((g["pnl_pct"] + 1).prod() - 1)
            return pd.Series({
                "거래수":      len(g),
                "승률":        f"{(g['pnl'] > 0).mean():.1%}",
                "총손익":      pnl_sum,
                "총손익률":    twr,
                "주요청산이유": top_reason,
            })

        summary = trades.groupby("market").apply(_market_summary).reset_index()

        def _color_summary_row(row) -> list[str]:
            try:
                val = float(row["총손익"])
            except (TypeError, ValueError):
                val = 0
            color = (
                "rgba(255,80,80,0.15)"  if val > 0 else
                "rgba(80,130,255,0.15)" if val < 0 else ""
            )
            return [f"background-color: {color}"] * len(row)

        styled_summary = (
            summary.style
            .apply(_color_summary_row, axis=1)
            .format({"총손익": "{:,.0f}", "총손익률": "{:.2%}"})
        )
        st.dataframe(styled_summary, use_container_width=True, hide_index=True)

        st.divider()

        disp = trades[[
            "market", "open_date", "close_date", "cost", "revenue", "pnl", "pnl_pct", "n_asks", "exit_reason",
        ]].copy()
        disp["exit_reason"] = disp["exit_reason"].map(_kr_exit_reason)
        disp.columns = ["마켓", "진입일", "청산일", "매수비용", "매도수익", "손익", "손익률", "매도횟수", "청산이유"]

        def _color_disp_row(row) -> list[str]:
            try:
                val = float(row["손익"])
            except (TypeError, ValueError):
                val = 0
            color = (
                "rgba(255,80,80,0.15)"  if val > 0 else
                "rgba(80,130,255,0.15)" if val < 0 else ""
            )
            return [f"background-color: {color}"] * len(row)

        styled_disp = (
            disp.style
            .apply(_color_disp_row, axis=1)
            .format({"손익": "{:,.0f}", "손익률": "{:.2%}", "매수비용": "{:,.0f}", "매도수익": "{:,.0f}"})
        )
        st.dataframe(styled_disp, use_container_width=True, hide_index=True)


# ─── 전략 분석 서브탭 ─────────────────────────────────────────────────────────

def render_strategy(
    db_path: str, markets: list[str],
    start_ms: int, end_ms: int,
    start_ts: str, end_ts: str,
) -> None:
    """
    [레이아웃 개선 #1] 기존 render_analysis에서 전략 분석 섹션만 분리.
    분석 탭 내 [전략 분석] 서브탭으로 렌더링.
    """
    if not markets:
        st.info("캔들 데이터 없음.")
        return

    # 컨트롤 바: 마켓 선택
    market_sig = st.selectbox("마켓", markets, key="sig_mkt")

    signals = load_signals(db_path, market_sig, start_ms, end_ms)
    candles = load_candles(db_path, market_sig, start_ts, end_ts, unit=15)  # 봇은 15분봉 전용

    if candles.empty:
        st.info("캔들 데이터 없음. fetch_candles.py로 먼저 수집하세요.")
        return

    # 메인 캔들차트 — 전체 폭 (RSI 기준선은 봇 기본값 oversold=50, overbought=70 고정)
    fig_chart = make_candle_signal_chart(
        candles, signals, f"{market_sig} 차트",
    )
    st.plotly_chart(fig_chart, use_container_width=True)

    if signals.empty:
        st.info("신호 데이터 없음.")
        return

    # [레이아웃 개선 #4] RSI 히스토그램 + 청산사유 도넛을 2컬럼 동등 배치
    col_rsi, col_pie = st.columns(2)

    buy_rsi   = signals[(signals["side"] == "BUY") & signals["rsi"].notna()]["rsi"]
    sell_sigs = signals[signals["side"] == "SELL"]

    with col_rsi:
        if not buy_rsi.empty:
            fig_rsi = go.Figure(go.Histogram(
                x=buy_rsi, nbinsx=20,
                marker_color="#5b8dee",
                marker_line=dict(color="#3366cc", width=0.5),
            ))
            fig_rsi.update_layout(
                title="진입 RSI 분포",
                height=_H_SUB,
                showlegend=False,
                margin=_MARGIN,
                paper_bgcolor="#0d1117",
                plot_bgcolor="#0d1117",
                font=dict(color="#8899aa"),
                xaxis=dict(gridcolor="#1a2234"),
                yaxis=dict(gridcolor="#1a2234"),
            )
            st.plotly_chart(fig_rsi, use_container_width=True)
        else:
            st.info("BUY 신호 RSI 데이터 없음.")

    with col_pie:
        if not sell_sigs.empty:
            reason_counts = sell_sigs["exit_reason"].value_counts()
            kr_labels     = [_kr_exit_reason(r) for r in reason_counts.index]
            fig_pie = go.Figure(go.Pie(
                labels=kr_labels,
                values=reason_counts.values,
                hole=0.45,
                marker=dict(
                    colors=["#00e5a0", "#ff4d6d", "#f9c846", "#5b8dee",
                            "#c084fc", "#fb923c", "#34d399", "#f472b6"],
                    line=dict(color="#0d1117", width=2),
                ),
                textfont=dict(size=11),
            ))
            fig_pie.update_layout(
                title="청산 사유",
                height=_H_SUB,
                margin=_MARGIN,
                paper_bgcolor="#0d1117",
                font=dict(color="#8899aa"),
                legend=dict(font=dict(size=10)),
            )
            st.plotly_chart(fig_pie, use_container_width=True)
        else:
            st.info("SELL 신호 데이터 없음.")

    # 보조 지표 metric 2개
    avg_hold    = compute_avg_hold_minutes(signals)
    partial_cnt = int(signals[(signals["side"] == "SELL") & (signals["is_partial"] == 1)].shape[0])
    full_cnt    = int(signals[(signals["side"] == "SELL") & (signals["is_partial"] == 0)].shape[0])

    m1, m2 = st.columns(2)
    m1.metric("평균 보유 기간", f"{avg_hold:.0f}분" if avg_hold is not None else "–")
    m2.metric("부분청산",       f"{partial_cnt}건",
              help=f"완전청산 {full_cnt}건 중 부분청산 {partial_cnt}건")


# ─── 백테스트 탭 ──────────────────────────────────────────────────────────────

def render_backtest(
    db_path: str, markets: list[str],
    start_ts: str, end_ts: str,
) -> None:

    # [수정 #1] 모듈 import 실패 시 탭 내 안내로 처리, 앱 크래시 없음
    if _backtest is None:
        st.error(
            "백테스트 모듈 로드 실패.\n\n"
            "`tools/candle_rsi_backtest.py`와 의존 패키지(`numpy`, `pandas`)를 확인하세요.\n\n"
            "```bash\npip install numpy pandas\n```"
        )
        return

    if not markets:
        st.info("캔들 데이터 없음. fetch_candles.py로 먼저 수집하세요.")
        return

    # 파라미터 좌측 [1], 결과 우측 [3]
    col_param, col_result = st.columns([1, 3])

    with col_param:
        st.markdown("**기본**")
        bt_market  = st.selectbox("마켓",     markets, key="bt_mkt")
        bt_unit    = st.selectbox("분봉 단위", [1, 3, 5, 10, 15, 30, 60, 240],
                                  index=4,    key="bt_unit")
        init_krw   = st.number_input("초기 자본 (KRW)", value=1_000_000, step=100_000)

        st.markdown("**RSI**")
        rsi_len    = st.slider("기간",    3,   20,  14, key="rsi_len")
        oversold   = st.slider("과매도", 0,  100, 30, key="oversold")
        overbought = st.slider("과매수", 0,  100, 70, key="overbought")

        st.markdown("**추세 강도**",
                    help="abs(close − close[N]) / close[N]\n\n"
                         "예) 0.03 = N봉 전 대비 현재가가 3% 벌어진 상태.\n\n"
                         "값이 클수록 강한 추세(급등·급락 중). "
                         "RSI 평균회귀는 횡보장에서 유효하므로 추세가 강할 때 진입을 차단합니다.")
        trend_win  = st.slider("윈도우 (N봉)", 2, 50,  30, key="trend_win",
                               help="현재가와 비교할 과거 기준봉까지의 거리.\n\n"
                                    "예) 5 → 5봉 전 가격과 현재가를 비교.")
        max_trend  = st.slider("최대값",  0.0, 1.0, 0.04, step=0.01, key="max_trend",
                               help="이 값 이하인 경우에만 진입 허용.\n\n"
                                    "예) 0.03 → 추세 강도 3% 초과 시 진입 차단.\n"
                                    "1.0 = 비활성 (모든 추세 허용).")

        st.markdown("**변동성**",
                    help="std(pct_change, N봉)\n\n"
                         "예) 0.01 = 최근 N봉 수익률의 표준편차가 1% (가격 변동이 작은 구간).\n\n"
                         "값이 클수록 가격 진폭이 큰 구간. "
                         "변동성이 너무 낮으면 수익 기회가 없으므로 기준 미달 구간 진입을 차단합니다.")
        vol_win    = st.slider("윈도우 (N봉)", 2, 50,  20, key="vol_win",
                               help="표준편차를 계산할 봉 수.\n\n"
                                    "예) 5 → 최근 5봉의 수익률로 변동성 계산.")
        min_vol    = st.slider("최솟값", 0.0, 0.05, 0.004, step=0.001, format="%.3f", key="min_vol",
                               help="이 값 이상인 경우에만 진입 허용.\n\n"
                                    "예) 0.005 → 변동성 0.5% 미만 구간 진입 차단.\n"
                                    "0.0 = 비활성 (모든 변동성 허용).")

        st.markdown("**손익**")
        stop_pct   = st.slider("손절 (%)", 1, 50, 10, step=1, key="stop_pct")
        target_pct = st.slider("익절 (%)", 1, 50, 20, step=1, key="target_pct")

        run_btn    = st.button("백테스트 실행", type="primary")

    # 버튼 클릭 시 계산 후 session_state 저장 — 이후 다른 UI 조작에도 결과 유지
    if run_btn:
        params = {
            **_backtest.DEFAULT_PARAMS,
            "rsi_length":        rsi_len,
            "trend_look_window": trend_win,
            "volatility_window": vol_win,
            "oversold":          oversold,
            "overbought":        overbought,
            "stop_loss_pct":     stop_pct,
            "profit_target_pct": target_pct,
            "max_trend_strength": max_trend,
            "min_volatility":     min_vol,
        }
        with st.spinner("백테스트 실행 중..."):
            st.session_state["bt_result"] = _backtest.run_backtest(
                db_path, bt_market,
                start_ts=start_ts, end_ts=end_ts,
                params=params, initial_krw=float(init_krw),
                unit=bt_unit,
            )

    with col_result:
        if "bt_result" not in st.session_state:
            st.info("파라미터를 설정하고 '백테스트 실행'을 누르세요.")
            return

        result = st.session_state["bt_result"]

        candles_bt = result["candles"]
        if candles_bt.empty:
            st.warning("캔들 데이터 없음. fetch_candles.py로 먼저 수집하세요.")
            return

        # 캔들 커버리지 체크: 실제 데이터 범위가 요청 범위를 벗어나면 경고
        _actual_start = candles_bt.index.min()
        _actual_end   = candles_bt.index.max()
        _req_start    = pd.Timestamp(start_ts)
        _req_end      = pd.Timestamp(end_ts)
        _tolerance    = pd.Timedelta(minutes=bt_unit)  # 선택한 분봉 1개 오차 허용

        if _actual_start > _req_start + _tolerance:
            st.warning(
                f"캔들 데이터가 요청 시작일보다 늦게 시작합니다. "
                f"요청: {_req_start.date()} / 실제: {_actual_start.date()}  \n"
                f"`fetch_candles.py --start {_req_start.date()} --markets {bt_market}` 로 보강하세요."
            )
        if _actual_end < _req_end - _tolerance:
            st.warning(
                f"캔들 데이터가 요청 종료일보다 일찍 끝납니다. "
                f"요청: {_req_end.date()} / 실제: {_actual_end.date()}  \n"
                f"`fetch_candles.py --end {_req_end.date()} --markets {bt_market}` 로 보강하세요."
            )

        trades_bt = result["trades"]
        rsi_col   = candles_bt.get("rsi")
        rdy_col   = candles_bt.get("rsi_ready")
        equity    = result["equity"]

        # KPI 5개
        s = result["summary"]
        total_return = s["total_pnl"] / equity.iloc[0] if not equity.empty and equity.iloc[0] != 0 else 0.0
        m1, m2, m3, m4, m5 = st.columns(5)
        m1.metric("총 거래",   f"{s['total_trades']}건")
        m2.metric("승률",      f"{s['win_rate']:.1%}")
        m3.metric("총 손익",   f"{s['total_pnl']:,.0f} KRW",
                  delta=f"{s['total_pnl']:+,.0f}" if s['total_pnl'] != 0 else None)
        m4.metric("총 손익률", f"{total_return:.2%}",
                  delta=f"{total_return:+.2%}" if total_return != 0 else None)
        m5.metric("평균 보유", f"{s['avg_hold_minutes']:.0f}분")

        if s.get("open_position"):
            st.warning("기간 종료 시 미청산 포지션 있음 — 손익 미확정. 마지막에 청산되지 않은 진입은 차트의 시뮬 BUY/SELL 마커에 포함되지 않을 수 있습니다.")

        # [레이아웃 개선 #5] 결과를 [차트] / [거래내역] 서브탭으로 분리
        rt_chart, rt_trades = st.tabs(["차트", "거래내역"])

        # ── 차트 탭 ──────────────────────────────────────────────────────────
        with rt_chart:
            fig = make_subplots(
                rows=2, cols=1, shared_xaxes=True,
                row_heights=[0.72, 0.28], vertical_spacing=0.03,
                subplot_titles=[f"{bt_market} 백테스트", f"RSI ({rsi_len})"],
            )

            fig.add_trace(go.Candlestick(
                x=candles_bt.index,
                open=candles_bt["open"], high=candles_bt["high"],
                low=candles_bt["low"],   close=candles_bt["close"],
                name="캔들", showlegend=False,
                increasing_line_color="#ff4455",
                decreasing_line_color="#4488ff",
            ), row=1, col=1)

            if not trades_bt.empty:
                fig.add_trace(go.Scatter(
                    x=trades_bt["entry_ts"], y=trades_bt["entry_price"],
                    mode="markers", name="시뮬 BUY",
                    marker=dict(symbol="triangle-up", size=9, color="#00e5a0",
                                line=dict(width=1, color="#008855")),
                ), row=1, col=1)
                fig.add_trace(go.Scatter(
                    x=trades_bt["exit_ts"], y=trades_bt["exit_price"],
                    mode="markers", name="시뮬 SELL",
                    marker=dict(symbol="triangle-down", size=9, color="#f9c846",
                                line=dict(width=1, color="#aa8800")),
                ), row=1, col=1)

            # RSI 서브플롯
            if rsi_col is not None and rdy_col is not None:
                mask = rdy_col.astype(bool)
                fig.add_trace(go.Scatter(
                    x=candles_bt.index[mask], y=rsi_col[mask],
                    name=f"RSI({rsi_len})", line=dict(color="#f9c846", width=1.2),
                ), row=2, col=1)

                for level, color in [(oversold, "#5b8dee"), (overbought, "#ff4d6d")]:
                    fig.add_hline(y=level, line_dash="dot", line_color=color,
                                  opacity=0.55, row=2, col=1)

            fig.update_layout(
                xaxis_rangeslider_visible=False,
                height=_H_MAIN,
                margin=_MARGIN,
                legend=dict(orientation="h", yanchor="bottom", y=1.01, xanchor="right", x=1),
                paper_bgcolor="#0d1117",
                plot_bgcolor="#0d1117",
                font=dict(color="#8899aa"),
            )
            fig.update_xaxes(gridcolor="#1a2234", zeroline=False)
            fig.update_yaxes(gridcolor="#1a2234", zeroline=False)
            st.plotly_chart(fig, use_container_width=True)

        # ── 거래내역 탭 ──────────────────────────────────────────────────────
        with rt_trades:
            if not trades_bt.empty:
                disp = trades_bt.copy()
                disp["reason"] = disp["reason"].map(_kr_exit_reason)
                disp.columns   = ["진입시각", "청산시각", "진입가", "청산가", "손익", "손익률", "청산사유"]

                def _color_bt_row(row) -> list[str]:
                    try:
                        val = float(row["손익"])
                    except (TypeError, ValueError):
                        val = 0
                    color = (
                        "rgba(255,80,80,0.15)"  if val > 0 else
                        "rgba(80,130,255,0.15)" if val < 0 else ""
                    )
                    return [f"background-color: {color}"] * len(row)

                styled = (
                    disp.style
                    .apply(_color_bt_row, axis=1)
                    .format({"손익": "{:,.0f}", "손익률": "{:.2%}"})
                )
                st.dataframe(styled, use_container_width=True, hide_index=True)
            else:
                st.info("거래 내역 없음.")


# ─── 메인 ─────────────────────────────────────────────────────────────────────

def main() -> None:
    st.set_page_config(page_title="CoinBot", layout="wide")
    st.title("CoinBot 대시보드")

    with st.sidebar:
        st.header("설정")
        db_path = st.text_input("DB 경로", value=DEFAULT_DB_PATH)
        st.divider()
        today      = date.today()
        start_date = st.date_input("시작일",  value=today - timedelta(days=30))
        end_date   = st.date_input("종료일",  value=today)

    if not os.path.exists(db_path):
        st.error(f"DB 파일 없음: `{db_path}`\n봇을 한 번 실행해 DB를 먼저 생성하세요.")
        st.stop()

    # 공통 시간 범위 (KST 기준)
    start_dt = datetime(start_date.year, start_date.month, start_date.day, tzinfo=KST)
    end_dt   = datetime(end_date.year,   end_date.month,   end_date.day, 23, 59, 59, tzinfo=KST)
    start_ms = int(start_dt.timestamp() * 1000)
    end_ms   = int(end_dt.timestamp()   * 1000)
    start_ts = start_dt.strftime("%Y-%m-%dT%H:%M:%S")
    end_ts   = end_dt.strftime("%Y-%m-%dT%H:%M:%S")

    markets = load_markets(db_path)

    tab_analysis, tab_backtest = st.tabs(["분석", "백테스트"])

    # [레이아웃 개선 #1] 분석 탭 내부를 P&L / 전략 분석 서브탭으로 분리
    with tab_analysis:
        sub_pnl, sub_strategy = st.tabs(["P&L", "전략 분석"])

        with sub_pnl:
            render_pnl(db_path, markets, start_ms, end_ms)

        with sub_strategy:
            render_strategy(db_path, markets, start_ms, end_ms, start_ts, end_ts)

    with tab_backtest:
        render_backtest(db_path, markets, start_ts, end_ts)


if __name__ == "__main__":
    main()
