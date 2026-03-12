#include "database/Database.h"
#include "database/sqlite3.h"
#include "util/Logger.h"

#include <algorithm>
#include <charconv>
#include <ctime>
#include <stdexcept>
#include <string>

namespace db {

// ─── 스키마 (schema.sql의 embed 버전 — 런타임 파일 의존 없음) ────────────────
// schema.sql과 동기화를 유지할 것. 변경 시 두 파일 모두 수정
static constexpr const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS candles (
    id     INTEGER PRIMARY KEY,
    market TEXT    NOT NULL,
    ts     TEXT    NOT NULL,
    open   REAL    NOT NULL,
    high   REAL    NOT NULL,
    low    REAL    NOT NULL,
    close  REAL    NOT NULL,
    volume REAL    NOT NULL,
    unit   INTEGER NOT NULL DEFAULT 15,
    UNIQUE (market, ts, unit)
);

CREATE TABLE IF NOT EXISTS orders (
    id               INTEGER PRIMARY KEY,
    order_uuid       TEXT    NOT NULL UNIQUE,
    identifier       TEXT,
    market           TEXT    NOT NULL,
    side             TEXT    NOT NULL CHECK (side IN ('BID', 'ASK')),
    order_type       TEXT    NOT NULL CHECK (order_type IN ('Market', 'Limit')),
    price            REAL,
    volume           REAL,
    requested_amount REAL,
    executed_volume  REAL    NOT NULL DEFAULT 0,
    executed_funds   REAL    NOT NULL DEFAULT 0,
    paid_fee         REAL    NOT NULL DEFAULT 0,
    status           TEXT    NOT NULL,
    created_at_ms    INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_orders_market ON orders(market);
CREATE INDEX IF NOT EXISTS idx_orders_status ON orders(status);

CREATE TABLE IF NOT EXISTS signals (
    id           INTEGER PRIMARY KEY,
    market       TEXT    NOT NULL,
    identifier   TEXT,               -- orders.identifier와 동일한 cid (JOIN 연결 고리)
    side         TEXT    NOT NULL CHECK (side IN ('BUY', 'SELL')),
    price        REAL    NOT NULL,
    volume       REAL    NOT NULL,
    krw_amount   REAL    NOT NULL,   -- fee 미포함 순수 체결 금액
    stop_price   REAL,
    target_price REAL,
    rsi            REAL,
    volatility     REAL,
    trend_strength REAL,
    is_partial     INTEGER NOT NULL DEFAULT 0 CHECK (is_partial IN (0, 1)),
    exit_reason  TEXT,               -- SELL 청산 사유. 단일/복합 조합 가능 (ex. exit_stop_target). BUY는 NULL
    ts_ms        INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_signals_market ON signals(market, ts_ms);
)SQL";

// ─── 소멸자 / 이동 ────────────────────────────────────────────────────────────

Database::~Database() 
{
    if (db_) sqlite3_close(db_); // SQLite 객체를 닫는다
}

Database::Database(Database&& other) noexcept : db_(other.db_) 
{
    other.db_ = nullptr;
}

Database& Database::operator=(Database&& other) noexcept 
{
    if (this != &other) {
        if (db_) sqlite3_close(db_);
        db_       = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

// ─── open ─────────────────────────────────────────────────────────────────────

void Database::open(const std::string& path) 
{
    // 재호출 시 기존 핸들 닫기 (누수 방지)
    if (db_) 
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }

	// SQLite 열기 문법(실패 시 throw) 파일이 없으면 생성, 존재하면 열기
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) 
    {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "unknown";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("[DB] open 실패: " + msg);
    }

    // WAL 모드: Streamlit 읽기와 봇 쓰기 비차단
    // 실패 시 db_ 정리 후 재throw → db_ != nullptr = 완전 초기화 보장
    try {
        // 모드 설정
        exec("PRAGMA journal_mode=WAL;");
        exec("PRAGMA synchronous=NORMAL;");
        exec("PRAGMA wal_autocheckpoint=10;"); // 10페이지(~40KB)마다 체크포인트(db 파일 업데이트)
        initSchema();
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
    util::log().info("[DB] 열림: ", path);
}

// sql초기화, PRAGMA(옵션) 설정
void Database::exec(const char* sql) 
{
    char* err = nullptr;
	// 스키마 초기화·PRAGMA 전용: 결과 처리(바인딩) 필요 없는 단순 실행
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) 
    {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error(std::string("[DB] exec 실패: ") + msg);
    }
}

void Database::initSchema()
{
    exec(kSchema);

    // 기존 DB 마이그레이션: identifier 컬럼이 없는 구버전 DB 대응
    // ALTER TABLE ADD COLUMN은 컬럼이 이미 있으면 오류 → 반환값만 무시
    sqlite3_exec(db_, "ALTER TABLE signals ADD COLUMN identifier TEXT;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE signals ADD COLUMN exit_reason TEXT;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE signals ADD COLUMN trend_strength REAL;",
                 nullptr, nullptr, nullptr);

    // candles unit 마이그레이션:
    // unit 컬럼이 없으면 테이블 재작성 — UNIQUE 제약 변경은 ALTER TABLE로 불가
    sqlite3_stmt* stmt = nullptr;
    int has_unit = 0;
    if (sqlite3_prepare_v2(db_,
            "SELECT COUNT(*) FROM pragma_table_info('candles') WHERE name='unit';",
            -1, &stmt, nullptr) == SQLITE_OK)
    {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            has_unit = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    if (!has_unit)
    {
        util::log().info("[DB] candles 마이그레이션: unit 컬럼 추가 및 UNIQUE(market,ts,unit) 재설정");
        exec("BEGIN;");
        exec(R"SQL(
            CREATE TABLE candles_new (
                id     INTEGER PRIMARY KEY,
                market TEXT    NOT NULL,
                ts     TEXT    NOT NULL,
                open   REAL    NOT NULL,
                high   REAL    NOT NULL,
                low    REAL    NOT NULL,
                close  REAL    NOT NULL,
                volume REAL    NOT NULL,
                unit   INTEGER NOT NULL DEFAULT 15,
                UNIQUE (market, ts, unit)
            );
        )SQL");
        exec("INSERT INTO candles_new (id,market,ts,open,high,low,close,volume,unit) "
             "SELECT id,market,ts,open,high,low,close,volume,15 FROM candles;");
        exec("DROP TABLE candles;");
        exec("ALTER TABLE candles_new RENAME TO candles;");
        exec("COMMIT;");
    }
}

// ─── normalizeToEpochMs ───────────────────────────────────────────────────────

int64_t Database::normalizeToEpochMs(const std::string& s) 
{
    if (s.empty()) return 0;

    // 숫자 문자열이면 epoch ms (WS 경로)
    if (std::all_of(s.begin(), s.end(), ::isdigit)) 
    {
        int64_t val = 0;
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
        if (ec == std::errc{}) return val;
        util::log().warn("[DB] created_at 숫자 파싱 실패: ", s);
        return 0;
    }

    // ISO8601 파싱 (REST 경로): "YYYY-MM-DDTHH:MM:SS+HH:MM"
    int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d",
               &year, &mon, &day, &hour, &min, &sec) != 6)
    {
        util::log().warn("[DB] created_at ISO8601 파싱 실패: ", s);
        return 0;
    }

    // 타임존 오프셋 파싱 (+09:00 / -05:30 / Z)
    // "YYYY-MM-DDTHH:MM:SS" = 19자 → s[19]이 부호 문자
    int tz_offset_min = 0;
    if (s.size() > 19 && (s[19] == '+' || s[19] == '-'))
    {
        int tz_h = 0, tz_m = 0;
        if (sscanf(s.c_str() + 20, "%d:%d", &tz_h, &tz_m) == 2)
        {
            tz_offset_min = tz_h * 60 + tz_m;
            if (s[19] == '-') tz_offset_min = -tz_offset_min;
        }
    }

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon  = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min  = min;
    tm.tm_sec  = sec;

    // tm을 UTC epoch seconds로 변환 (MSVC: _mkgmtime, POSIX: timegm)
#ifdef _WIN32
    const int64_t epoch_sec = static_cast<int64_t>(_mkgmtime(&tm));
#else
    const int64_t epoch_sec = static_cast<int64_t>(timegm(&tm));
#endif
    if (epoch_sec < 0)
    {
        util::log().warn("[DB] created_at UTC 변환 실패: ", s);
        return 0;
    }

    return (epoch_sec - static_cast<int64_t>(tz_offset_min) * 60) * 1000;
}

// ─── insertCandle ─────────────────────────────────────────────────────────────

bool Database::insertCandle(const std::string& market, const core::Candle& c, int unit) 
{
    if (!db_) {
        util::log().warn("[DB] insertCandle: DB가 열려 있지 않음");
        return false;
    }

	// ?는 bind로 매개변수를 바인딩하는 자리 표시자 (DO NOTHING은 중복 무시)
    // ON CONFLICT 대상 (market,ts,unit)
    static constexpr const char* sql =
        "INSERT INTO candles (market, ts, open, high, low, close, volume, unit) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(market, ts, unit) DO NOTHING;";

	// 컴파일된 SQL 실행 객체 (prepare가 성공하면 유효 포인터로 채워지고 실패시 nullptr 유지)
    sqlite3_stmt* stmt = nullptr;
    // SQL 문자열을 파싱/컴파일해서 stmt 객체를 만든다
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        util::log().warn("[DB] insertCandle prepare 실패: ", sqlite3_errmsg(db_));
        return false;
    }

	// ? 자리에 값을 바인딩한다 (1-based index), SQLite는 1부터 시작
    sqlite3_bind_text  (stmt, 1, market.c_str(),            -1, SQLITE_STATIC);
    sqlite3_bind_text  (stmt, 2, c.start_timestamp.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 3, c.open_price);
    sqlite3_bind_double(stmt, 4, c.high_price);
    sqlite3_bind_double(stmt, 5, c.low_price);
    sqlite3_bind_double(stmt, 6, c.close_price);
    sqlite3_bind_double(stmt, 7, c.volume);
    sqlite3_bind_int   (stmt, 8, unit);

    // step을 통해 실행한다
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!ok) util::log().warn("[DB] insertCandle step 실패: ", sqlite3_errmsg(db_));

	// stmt 객체를 해제한다 (메모리 누수 방지)
    sqlite3_finalize(stmt);
    return ok;
}

// ─── insertOrder ─────────────────────────────────────────────────────────────

bool Database::insertOrder(const core::Order& o) 
{
    if (!db_) 
    {
        util::log().warn("[DB] insertOrder: DB가 열려 있지 않음");
        return false;
    }

    // ON CONFLICT(order_uuid) DO NOTHING: UNIQUE 중복만 무시 (WS 재연결 중복 수신 대응)
    // INSERT OR IGNORE 대신 사용 — CHECK/NOT NULL 위반은 정상 오류로 노출
    static constexpr const char* sql = R"SQL(
        INSERT INTO orders
            (order_uuid, identifier, market, side, order_type,
             price, volume, requested_amount,
             executed_volume, executed_funds, paid_fee, status, created_at_ms)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(order_uuid) DO NOTHING
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) 
    {
        util::log().warn("[DB] insertOrder prepare 실패: ", sqlite3_errmsg(db_));
        return false;
    }

    const char* side = (o.position == core::OrderPosition::BID) ? "BID" : "ASK";
    const char* type = (o.type    == core::OrderType::Market)   ? "Market" : "Limit";
    const int64_t created_ms = normalizeToEpochMs(o.created_at);

    sqlite3_bind_text(stmt, 1, o.id.c_str(), -1, SQLITE_STATIC);

    if (o.identifier) sqlite3_bind_text(stmt, 2, o.identifier->c_str(), -1, SQLITE_STATIC);
    else              sqlite3_bind_null(stmt, 2);

    sqlite3_bind_text(stmt, 3, o.market.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, side,              -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, type,              -1, SQLITE_STATIC);

    if (o.price)            sqlite3_bind_double(stmt, 6, *o.price);
    else                    sqlite3_bind_null(stmt, 6);

    if (o.volume)           sqlite3_bind_double(stmt, 7, *o.volume);
    else                    sqlite3_bind_null(stmt, 7);

    if (o.requested_amount) sqlite3_bind_double(stmt, 8, *o.requested_amount);
    else                    sqlite3_bind_null(stmt, 8);

    sqlite3_bind_double(stmt,  9, o.executed_volume);
    sqlite3_bind_double(stmt, 10, o.executed_funds);
    sqlite3_bind_double(stmt, 11, o.paid_fee);
    sqlite3_bind_text  (stmt, 12, to_string(o.status), -1, SQLITE_STATIC);
    sqlite3_bind_int64 (stmt, 13, created_ms);

    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!ok) util::log().warn("[DB] insertOrder step 실패: ", sqlite3_errmsg(db_));

    sqlite3_finalize(stmt);
    return ok;
}

// ─── insertSignal ─────────────────────────────────────────────────────────────

bool Database::insertSignal(const trading::SignalRecord& sig)       
{
    if (!db_) 
    {
        util::log().warn("[DB] insertSignal: DB가 열려 있지 않음");
        return false;
    }

    static constexpr const char* sql =
        "INSERT INTO signals "
        "(market, identifier, side, price, volume, krw_amount, "
        " stop_price, target_price, rsi, volatility, trend_strength, is_partial, exit_reason, ts_ms) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        util::log().warn("[DB] insertSignal prepare 실패: ", sqlite3_errmsg(db_));
        return false;
    }

    const char* side = (sig.side == trading::SignalSide::BUY) ? "BUY" : "SELL";

    sqlite3_bind_text  (stmt, 1, sig.market.c_str(),     -1, SQLITE_STATIC);
    sig.identifier.empty()
        ? sqlite3_bind_null(stmt, 2)
        : sqlite3_bind_text(stmt, 2, sig.identifier.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text  (stmt, 3, side,                   -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 4, sig.price);
    sqlite3_bind_double(stmt, 5, sig.volume);
    sqlite3_bind_double(stmt, 6, sig.krw_amount);

    sig.stop_price   ? sqlite3_bind_double(stmt, 7, *sig.stop_price)   : sqlite3_bind_null(stmt, 7);
    sig.target_price ? sqlite3_bind_double(stmt, 8, *sig.target_price) : sqlite3_bind_null(stmt, 8);
    sig.rsi            ? sqlite3_bind_double(stmt,  9, *sig.rsi)            : sqlite3_bind_null(stmt,  9);
    sig.volatility     ? sqlite3_bind_double(stmt, 10, *sig.volatility)     : sqlite3_bind_null(stmt, 10);
    sig.trend_strength ? sqlite3_bind_double(stmt, 11, *sig.trend_strength) : sqlite3_bind_null(stmt, 11);

    sqlite3_bind_int  (stmt, 12, sig.is_partial);

    sig.exit_reason.empty()
        ? sqlite3_bind_null(stmt, 13)
        : sqlite3_bind_text(stmt, 13, sig.exit_reason.c_str(), -1, SQLITE_STATIC);

    sqlite3_bind_int64(stmt, 14, sig.ts_ms);

    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!ok) util::log().warn("[DB] insertSignal step 실패: ", sqlite3_errmsg(db_));

    sqlite3_finalize(stmt);
    return ok;
}

} // namespace db
