"""
fetch_candles.py — 과거/최신 캔들 수집기 (Phase 2 Step 7)

용도: DB에 과거 캔들을 적재한다. 봇 실행과 독립적으로 실행 가능.
실행: python tools/fetch_candles.py [--db <path>] [--markets KRW-ADA,...] [--days 90]

DB 경로 우선순위:
  1. CLI --db 인자
  2. 환경변수 COINBOT_DB_PATH
  3. __file__ 기준 repo root 자동 계산 (src/db/coinbot.db)
"""

import argparse
import os
import sqlite3
import time
from datetime import datetime, timedelta
from zoneinfo import ZoneInfo

import requests

# ─── 상수 ─────────────────────────────────────────────────────────────────────

UPBIT_CANDLES_URL = "https://api.upbit.com/v1/candles/minutes/15"
DEFAULT_MARKETS   = ["KRW-ADA", "KRW-TRX", "KRW-XRP"]
DEFAULT_DAYS      = 90
OVERLAP_MINUTES   = 2    # Incremental: 경계 누락 방지용 overlap (ON CONFLICT DO NOTHING으로 중복 흡수)
REQUEST_INTERVAL  = 0.1  # rate limit: 분당 ~600회 상한 (0.1s 간격)
BATCH_SIZE        = 200  # Upbit API 1회 최대 수신 개수
REQUEST_TIMEOUT   = 10   # 단일 요청 타임아웃 (초)

KST = ZoneInfo("Asia/Seoul")


# ─── DB 경로 결정 ──────────────────────────────────────────────────────────────

def resolve_db_path(cli_path: str | None) -> str:
    """CLI > 환경변수 > repo root 자동 계산 순으로 DB 경로를 결정한다."""
    if cli_path:
        path = cli_path
    elif "COINBOT_DB_PATH" in os.environ:
        path = os.environ["COINBOT_DB_PATH"]
    else:
        # __file__ 기준으로 repo root를 찾아 src/db/coinbot.db 반환
        repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        path = os.path.join(repo_root, "src", "db", "coinbot.db")

    # 경로가 존재하지 않으면 즉시 에러 (자동 생성 금지 — 엉뚱한 위치에 빈 DB 생성 사고 방지)
    if not os.path.exists(path):
        raise FileNotFoundError(
            f"[fetch_candles] DB 파일 없음: {path}\n"
            "봇을 한 번 실행해 DB를 먼저 생성하세요."
        )
    return path


# ─── end_ts 계산 ──────────────────────────────────────────────────────────────

def calc_end_ts(now: datetime) -> str:
    """
    현재 분의 직전 분을 반환한다 (미확정 캔들 제외).
    미확정 캔들을 insert하면 ON CONFLICT DO NOTHING으로 인해
    이후 봇이 완성된 값을 write하지 못해 틀린 데이터가 고착된다.
    """
    # 현재 시간을 분 단위로 내리고 1분을 뺀다 (미확정 캔들을 제외하기 위함)
    prev = now.replace(second=0, microsecond=0) - timedelta(minutes=1) 
    return prev.strftime("%Y-%m-%dT%H:%M:%S")


# ─── Upbit API ─────────────────────────────────────────────────────────────────

def fetch_batch(market: str, to: str | None) -> list[dict]:
    """
    Upbit /v1/candles/minutes/15 1회 호출.
    to: candle_date_time_kst 형식 문자열 (None이면 최신부터 역방향 수집)
    반환: 캔들 dict 리스트 (최신 → 과거 순)

    [명세서 수정] 원본 명세서에는 API 오류 처리가 없음.
    네트워크 실패 시 예외를 그대로 전파해 호출자가 인지할 수 있도록 raise_for_status 추가.
    """
    params: dict = {"market": market, "count": BATCH_SIZE}
    if to:
        params["to"] = to  # candle_date_time_kst 형식("YYYY-MM-DDTHH:MM:SS") 그대로 사용

    # 타임아웃을 걸어 무한 대기 방지
    resp = requests.get(UPBIT_CANDLES_URL, params=params, timeout=REQUEST_TIMEOUT)
    resp.raise_for_status() # HTTP 에러면 예외 발생
    return resp.json()


# ─── DB 헬퍼 ──────────────────────────────────────────────────────────────────

def get_last_ts(conn: sqlite3.Connection, market: str) -> str | None:
    """candles 테이블에서 해당 마켓의 최신 분봉 시간(ts)를 반환. 없으면 None."""
    row = conn.execute(
        "SELECT MAX(ts) FROM candles WHERE market = ?", (market,)
    ).fetchone()
    return row[0] if row and row[0] else None


def insert_candle_row(conn: sqlite3.Connection, market: str, c: dict) -> int:
    """
    캔들 1건 INSERT (ON CONFLICT DO NOTHING — 봇 실시간 write와 충돌 없음).
    반환: 1=실제 삽입, 0=중복으로 무시됨
    """
    cursor = conn.execute(
        "INSERT INTO candles (market, ts, open, high, low, close, volume) "
        "VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(market, ts) DO NOTHING",
        (
            market,
            c["candle_date_time_kst"],   # C++ CandleMapper와 동일한 ts 기준 (KST)
            c["opening_price"],
            c["high_price"],
            c["low_price"],
            c["trade_price"],            # close price
            c["candle_acc_trade_volume"],
        ),
    )
    return cursor.rowcount  # ON CONFLICT DO NOTHING: 삽입 시 1, 중복 무시 시 0


# ─── Bootstrap 모드 ───────────────────────────────────────────────────────────

def bootstrap(conn: sqlite3.Connection, market: str, days: int) -> int:
    """
    DB에 해당 마켓 데이터 없음(get_last_ts가 None) → 과거 days일치 캔들을 역방향 수집.
    반환: 실제 삽입된 캔들 수 (ON CONFLICT DO NOTHING으로 무시된 건 제외)
    """
    now       = datetime.now(KST)
    cutoff    = now - timedelta(days=days)
    end_ts    = calc_end_ts(now)
    cutoff_str = cutoff.strftime("%Y-%m-%dT%H:%M:%S")

    print(f"  [Bootstrap] {market}: {cutoff_str} ~ {end_ts} ({days}일)")

    to      = None  # 최신부터 역방향 시작
    count   = 0     # 누적 삽입 건수 계산

    while True:
        candles = fetch_batch(market, to)

        # candles=[]이면 min() 호출이 ValueError 발생하므로 guard 추가.
        if not candles:
            break

        for c in candles:
            ts = c["candle_date_time_kst"]
            # bootstrap은 지정 기간(cutoff~end_ts) 안에 드는 캔들만 적재한다.
            if cutoff_str <= ts <= end_ts:  # 현재 진행 중인 분봉(미확정) 제외
                count += insert_candle_row(conn, market, c)  # 실제 삽입 건수만 합산

        # 배치마다 커밋: 수집 중 중단 시 진행분 보존.
        conn.commit()

        oldest_ts = min(c["candle_date_time_kst"] for c in candles)
        print(f"    ~ {oldest_ts} ({count}건 삽입)")

        # oldest_ts와 cutoff_str 모두 "YYYY-MM-DDTHH:MM:SS" 형식이므로 문자열 비교 가능
        if len(candles) < BATCH_SIZE or oldest_ts <= cutoff_str:
            break

        to = oldest_ts
        time.sleep(REQUEST_INTERVAL)

    return count


# ─── Incremental 모드 ─────────────────────────────────────────────────────────

def incremental(conn: sqlite3.Connection, market: str, last_ts: str) -> int:
    """
    DB에 기존 데이터 있음 → last_ts 이후 누락 캔들을 수집해 append.
    overlap 2분: 경계 누락 방지 (ON CONFLICT DO NOTHING으로 중복 흡수).
    반환: 실제 삽입된 캔들 수 (ON CONFLICT DO NOTHING으로 무시된 건 제외)
    """
    now    = datetime.now(KST)
    end_ts = calc_end_ts(now)

    # 마지막 시간 문자열을 실제 시간으로 변환해 계산 가능하도록 함
    last_dt  = datetime.strptime(last_ts, "%Y-%m-%dT%H:%M:%S").replace(tzinfo=KST)
    # start_ts: last_ts에서 OVERLAP_MINUTES을 빼서 overlap 이전부터 시작 (경계 누락 방지)
    start_ts = (last_dt - timedelta(minutes=OVERLAP_MINUTES)).strftime("%Y-%m-%dT%H:%M:%S")

    print(f"  [Incremental] {market}: {start_ts} ~ {end_ts} (last={last_ts})")

    to    = None  # 최신부터 역방향 시작
    count = 0

    while True:
        # to는 맨밑에서 갱신되며 최신 -> 과거 순으로 진행
        candles = fetch_batch(market, to)

        # [명세서 수정] 빈 결과 guard
        if not candles:
            break

        # 각 배치의 캔들을 순회하며 start_ts ~ end_ts 범위 안에 드는 캔들만 적재
        for c in candles:
            ts = c["candle_date_time_kst"]
            # incremental은 start_ts 이후 구간만 보강한다.
            if start_ts <= ts <= end_ts:  # 미확정 캔들 제외
                count += insert_candle_row(conn, market, c)  # 실제 삽입 건수만 합산

        # 배치마다 커밋
        conn.commit()

        oldest_ts = min(c["candle_date_time_kst"] for c in candles)

        if len(candles) < BATCH_SIZE or oldest_ts <= start_ts:
            break

        to = oldest_ts
        time.sleep(REQUEST_INTERVAL)

    return count


# ─── 마켓별 진입점 ────────────────────────────────────────────────────────────

def fetch_market(conn: sqlite3.Connection, market: str, days: int) -> None:
    last_ts = get_last_ts(conn, market)

    if last_ts is None:
        # 참고: --days는 bootstrap(초기 적재) 범위에만 적용된다.
        count = bootstrap(conn, market, days)
    else:
        # 참고: incremental에서는 --days를 사용하지 않고, last_ts 이후만 보강한다.
        count = incremental(conn, market, last_ts)

    print(f"  → {market}: {count}건 삽입 완료")


# ─── CLI 진입점 ───────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="Upbit 분봉 캔들 수집기")
    parser.add_argument("--db",      default=None,
                        help="SQLite DB 경로 (기본: src/db/coinbot.db)")
    parser.add_argument("--markets", default=",".join(DEFAULT_MARKETS),
                        help="마켓 목록 (쉼표 구분, 기본: KRW-ADA,KRW-TRX,KRW-XRP)")
    parser.add_argument("--days",    type=int, default=DEFAULT_DAYS,
                        help="Bootstrap 시 수집 기간 일 수 (기본: 90)")
    args = parser.parse_args()

    db_path = resolve_db_path(args.db)
    markets = [m.strip() for m in args.markets.split(",") if m.strip()]

    print(f"[fetch_candles] DB: {db_path}")
    print(f"[fetch_candles] 마켓: {markets}, 기간: {args.days}일")

    # [명세서 수정] Python 연결에도 WAL pragma 설정.
    # 봇(C++)이 WAL 모드로 쓰는 도중 Python이 기본 모드로 열면 read/write 충돌 가능.
    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA journal_mode=WAL;")

    try:
        for market in markets:
            print(f"\n[{market}]")
            fetch_market(conn, market, args.days)
    finally:
        conn.close()

    print("\n[fetch_candles] 완료")


if __name__ == "__main__":
    main()
