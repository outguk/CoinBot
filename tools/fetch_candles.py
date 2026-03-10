"""
fetch_candles.py — 과거/최신 캔들 수집기 (Phase 2 Step 7)

용도: DB에 과거 캔들을 적재한다. 봇 실행과 독립적으로 실행 가능.
실행: python tools/fetch_candles.py [--db <path>] [--markets KRW-ADA,...] [--days 90] [--start YYYY-MM-DD] [--end YYYY-MM-DD] [--unit 15]

DB 경로 우선순위:
  1. CLI --db 인자
  2. 환경변수 COINBOT_DB_PATH
  3. __file__ 기준 repo root 자동 계산 (db/coinbot.db)
"""

import argparse
import os
import sqlite3
import time
from datetime import datetime, timedelta
from zoneinfo import ZoneInfo

import requests

# ─── 상수 ─────────────────────────────────────────────────────────────────────

SUPPORTED_UNITS  = [1, 3, 5, 10, 15, 30, 60, 240]  # Upbit 지원 분봉 단위 (모두 1440의 약수)
DEFAULT_UNIT     = 15
DEFAULT_MARKETS  = ["KRW-ADA", "KRW-TRX", "KRW-XRP"]
DEFAULT_DAYS     = 90
REQUEST_INTERVAL = 0.1  # rate limit: 분당 ~600회 상한 (0.1s 간격)
BATCH_SIZE       = 200  # Upbit API 1회 최대 수신 개수
REQUEST_TIMEOUT  = 10   # 단일 요청 타임아웃 (초)

KST = ZoneInfo("Asia/Seoul")


# ─── DB 경로 결정 ──────────────────────────────────────────────────────────────

def resolve_db_path(cli_path: str | None) -> str:
    """CLI > 환경변수 > repo root 자동 계산 순으로 DB 경로를 결정한다."""
    if cli_path:
        path = cli_path
    elif "COINBOT_DB_PATH" in os.environ:
        path = os.environ["COINBOT_DB_PATH"]
    else:
        # __file__ 기준으로 repo root를 찾아 db/coinbot.db 반환
        repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        path = os.path.join(repo_root, "db", "coinbot.db")

    # 경로가 존재하지 않으면 즉시 에러 (자동 생성 금지 — 엉뚱한 위치에 빈 DB 생성 사고 방지)
    if not os.path.exists(path):
        raise FileNotFoundError(
            f"[fetch_candles] DB 파일 없음: {path}\n"
            "봇을 한 번 실행해 DB를 먼저 생성하세요."
        )
    return path


# ─── end_ts 계산 ──────────────────────────────────────────────────────────────

def calc_end_ts(now: datetime, unit: int) -> str:
    """
    마지막으로 완료된 분봉의 시작 시각을 반환한다 (미확정 캔들 제외).

    자정 이후 누적 분을 unit으로 나눈 나머지 = 현재 봉 시작으로부터 경과 분.
    unit이 1440의 약수(Upbit 지원 단위: 1/3/5/10/15/30/60/240)이면 항상 자정 기준 경계와 정렬됨.

    예) unit=15, 10:37 → elapsed=7 → current=10:30 → last=10:15
        unit=60, 10:37 → elapsed=37 → current=10:00 → last=09:00
        unit=240, 10:37 → elapsed=157 → current=08:00 → last=04:00

    미확정 캔들을 insert하면 ON CONFLICT DO NOTHING으로 인해
    이후 봇이 완성된 값을 write하지 못해 틀린 데이터가 고착된다.
    """
    total_minutes    = now.hour * 60 + now.minute   # 자정 이후 누적 분
    elapsed          = total_minutes % unit          # 현재 봉 시작으로부터 경과 분
    current_boundary = now.replace(second=0, microsecond=0) - timedelta(minutes=elapsed)
    last_complete    = current_boundary - timedelta(minutes=unit)
    return last_complete.strftime("%Y-%m-%dT%H:%M:%S")


# ─── Upbit API ─────────────────────────────────────────────────────────────────

def fetch_batch(market: str, to: str | None, unit: int) -> list[dict]:
    """
    Upbit /v1/candles/minutes/{unit} 1회 호출.
    to: candle_date_time_kst 형식 문자열 (None이면 최신부터 역방향 수집)
    반환: 캔들 dict 리스트 (최신 → 과거 순)
    """
    url    = f"https://api.upbit.com/v1/candles/minutes/{unit}"
    params: dict = {"market": market, "count": BATCH_SIZE}
    if to:
        params["to"] = to  # candle_date_time_kst 형식("YYYY-MM-DDTHH:MM:SS") 그대로 사용

    resp = requests.get(url, params=params, timeout=REQUEST_TIMEOUT)
    resp.raise_for_status()
    return resp.json()


# ─── DB 헬퍼 ──────────────────────────────────────────────────────────────────

def insert_candle_row(conn: sqlite3.Connection, market: str, c: dict, unit: int) -> int:
    """
    캔들 1건 INSERT (ON CONFLICT DO NOTHING — 봇 실시간 write와 충돌 없음).
    반환: 1=실제 삽입, 0=중복으로 무시됨
    """
    cursor = conn.execute(
        "INSERT INTO candles (market, ts, open, high, low, close, volume, unit) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(market, ts, unit) DO NOTHING",
        (
            market,
            c["candle_date_time_kst"],   # C++ CandleMapper와 동일한 ts 기준 (KST)
            c["opening_price"],
            c["high_price"],
            c["low_price"],
            c["trade_price"],            # close price
            c["candle_acc_trade_volume"],
            unit,
        ),
    )
    return cursor.rowcount  # ON CONFLICT DO NOTHING: 삽입 시 1, 중복 무시 시 0


# ─── 구간 수집 ────────────────────────────────────────────────────────────────

def fetch_range(conn: sqlite3.Connection, market: str, start_ts: str, end_ts: str, unit: int) -> int:
    """
    start_ts ~ end_ts 구간 캔들을 end_ts 기준 역방향 수집.

    bootstrap / incremental / backfill 모두 동일한 역방향 수집 로직이므로 하나로 통합.
    범위 밖 캔들은 skip, 중복은 ON CONFLICT DO NOTHING으로 흡수.
    반환: 실제 삽입된 캔들 수 (ON CONFLICT DO NOTHING으로 무시된 건 제외)
    """
    to    = end_ts  # end_ts부터 역방향으로 시작
    count = 0

    while True:
        candles = fetch_batch(market, to, unit)
        if not candles:
            break

        batch_count = 0
        for c in candles:
            ts = c["candle_date_time_kst"]
            if start_ts <= ts <= end_ts:
                batch_count += insert_candle_row(conn, market, c, unit)
        count += batch_count

        # 배치마다 커밋: 수집 중 중단 시 진행분 보존
        conn.commit()

        latest_ts = max(c["candle_date_time_kst"] for c in candles)
        oldest_ts = min(c["candle_date_time_kst"] for c in candles)
        print(f"    {oldest_ts} ~ {latest_ts} (+{batch_count}건, 누적 {count}건)")

        if len(candles) < BATCH_SIZE or oldest_ts <= start_ts:
            break

        to = oldest_ts
        time.sleep(REQUEST_INTERVAL)

    return count


# ─── 공백 탐색 ────────────────────────────────────────────────────────────────

def find_gaps(conn: sqlite3.Connection, market: str, unit: int,
              start_ts: str, end_ts: str) -> list[tuple[str, str]]:
    """
    start_ts ~ end_ts 구간에서 DB에 누락된 캔들 구간 목록을 반환한다.

    Upbit 분봉은 unit 간격 고정이므로 예상 ts 시퀀스와 DB 실제 ts를 비교해
    누락된 ts를 찾고, 연속된 누락을 하나의 구간 (gap_start, gap_end)으로 묶는다.

    start_ts가 unit 경계가 아닐 경우 첫 번째 경계로 올림 처리한다.
    반환: [(gap_start, gap_end), ...] — 없으면 []
    """
    # DB에서 해당 구간 ts 전부 조회
    rows = conn.execute(
        "SELECT ts FROM candles WHERE market=? AND unit=? AND ts>=? AND ts<=?",
        (market, unit, start_ts, end_ts)
    ).fetchall()
    existing = {row[0] for row in rows}

    # start_ts를 unit 경계로 올림
    t = datetime.strptime(start_ts, "%Y-%m-%dT%H:%M:%S").replace(tzinfo=KST)
    t = t.replace(second=0, microsecond=0)
    remainder = (t.hour * 60 + t.minute) % unit
    if remainder != 0:
        t += timedelta(minutes=(unit - remainder))

    end = datetime.strptime(end_ts, "%Y-%m-%dT%H:%M:%S").replace(tzinfo=KST)

    # 누락 ts 수집
    missing = []
    while t <= end:
        ts_str = t.strftime("%Y-%m-%dT%H:%M:%S")
        if ts_str not in existing:
            missing.append(ts_str)
        t += timedelta(minutes=unit)

    if not missing:
        return []

    # 연속된 누락을 구간으로 묶기
    gaps: list[tuple[str, str]] = []
    gap_start = gap_end = missing[0]
    for ts in missing[1:]:
        prev = datetime.strptime(gap_end, "%Y-%m-%dT%H:%M:%S")
        curr = datetime.strptime(ts,      "%Y-%m-%dT%H:%M:%S")
        if int((curr - prev).total_seconds() / 60) == unit:
            gap_end = ts
        else:
            gaps.append((gap_start, gap_end))
            gap_start = gap_end = ts
    gaps.append((gap_start, gap_end))

    return gaps


# ─── 마켓별 진입점 ────────────────────────────────────────────────────────────

def fetch_market(conn: sqlite3.Connection, market: str, start_ts: str, end_ts: str, unit: int) -> None:
    """
    start_ts ~ end_ts 구간에서 누락된 캔들을 모두 수집한다.

    find_gaps로 실제 공백 구간만 추출해 fetch_range 호출.
    Bootstrap / Backfill / Incremental / 중간 공백 모두 동일 로직으로 처리된다.
    """
    gaps = find_gaps(conn, market, unit, start_ts, end_ts)

    if not gaps:
        print(f"  [Skip]  {market} (unit={unit}): 요청 구간이 이미 수집됨 ({start_ts} ~ {end_ts})")
        print(f"  → {market} (unit={unit}): 0건 삽입 완료")
        return

    total = 0
    for g_start, g_end in gaps:
        print(f"  [Fetch] {market} (unit={unit}): {g_start} ~ {g_end}")
        total += fetch_range(conn, market, g_start, g_end, unit)

    print(f"  → {market} (unit={unit}): {total}건 삽입 완료")


# ─── CLI 진입점 ───────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="Upbit 분봉 캔들 수집기")
    parser.add_argument("--db",      default=None,
                        help="SQLite DB 경로 (기본: db/coinbot.db)")
    parser.add_argument("--markets", default=",".join(DEFAULT_MARKETS),
                        help="마켓 목록 (쉼표 구분, 기본: KRW-ADA,KRW-TRX,KRW-XRP)")
    parser.add_argument("--days",    type=int, default=DEFAULT_DAYS,
                        help="수집 기간 일 수 (기본: 90, --start 미지정 시 사용)")
    parser.add_argument("--start",   default=None,
                        help="수집 시작 날짜 (YYYY-MM-DD, 지정 시 --days 무시)")
    parser.add_argument("--end",     default=None,
                        help="수집 종료 날짜 (YYYY-MM-DD, 미지정 시 현재까지)")
    parser.add_argument("--unit",    type=int, default=DEFAULT_UNIT,
                        choices=SUPPORTED_UNITS,
                        help=f"분봉 단위 (기본: {DEFAULT_UNIT}, 선택: {SUPPORTED_UNITS})")
    args = parser.parse_args()

    db_path = resolve_db_path(args.db)
    markets = [m.strip() for m in args.markets.split(",") if m.strip()]
    unit    = args.unit

    now = datetime.now(KST)

    start_ts = (
        f"{args.start}T00:00:00"
        if args.start
        else (now - timedelta(days=args.days)).strftime("%Y-%m-%dT%H:%M:%S")
    )
    if args.end:
        cli_end = f"{args.end}T23:59:59"
        # 오늘 날짜를 명시한 경우 미확정 봉이 포함되지 않도록 unit 기준 상한을 cap
        end_ts  = min(cli_end, calc_end_ts(now, unit))
    else:
        end_ts = calc_end_ts(now, unit)

    period_desc = (
        f"{args.start} ~ {args.end or '현재'}"
        if args.start
        else f"최근 {args.days}일 ({start_ts} ~)"
    )

    print(f"[fetch_candles] DB: {db_path}")
    print(f"[fetch_candles] 마켓: {markets}, 분봉: {unit}분, 요청 기간: {period_desc}")

    # Python 연결에도 WAL pragma 설정.
    # 봇(C++)이 WAL 모드로 쓰는 도중 Python이 기본 모드로 열면 read/write 충돌 가능.
    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA journal_mode=WAL;")

    try:
        for market in markets:
            print(f"\n[{market}]")
            fetch_market(conn, market, start_ts, end_ts, unit)
    finally:
        conn.close()

    print("\n[fetch_candles] 완료")


if __name__ == "__main__":
    main()
