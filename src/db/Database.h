#pragma once

// Database - SQLite RAII 래퍼
// - open()으로 초기화, 소멸자에서 자동 닫힘
// - WAL 모드: Streamlit 읽기와 봇 쓰기가 서로 차단하지 않음
// - 모든 write는 이벤트 처리 완료 후 inter-event 구간에서 동기 수행

#include <cstdint>
#include <string>

#include "core/domain/Candle.h"
#include "core/domain/Order.h"
#include "trading/strategies/StrategyTypes.h"

// sqlite3 전방 선언 (sqlite3.h를 공개 헤더에 노출하지 않기 위함)
struct sqlite3;

namespace db {

class Database {
public:
    Database() = default;
    ~Database();

    // 이동만 허용 (RAII 단일 소유로 안전하게 보관)
    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;

    // DB 파일 열기 + WAL PRAGMA 설정 + 스키마 초기화(생성)
    // 실패 시 std::runtime_error
    void open(const std::string& path);

    // candle INSERT — ON CONFLICT(market, ts) DO NOTHING (중복 무시)
    // 반환: true=성공(중복 포함), false=prepare/step 실패
    bool insertCandle(const std::string& market, const core::Candle& c);

    // order INSERT — 터미널 상태(Filled/Canceled/Rejected) 확정 시 1회 호출
    // ON CONFLICT(order_uuid) DO NOTHING: UNIQUE 중복만 무시 (WS 재연결 중복 수신 대응)
    // created_at_ms: WS(숫자 문자열) / REST(ISO8601) 양쪽 자동 정규화
    // 반환: true=성공(중복 무시 포함), false=prepare/step 실패
    bool insertOrder(const core::Order& o);

    // signal INSERT — BUY/SELL 전이 확정 시 호출
    // 반환: true=성공, false=prepare/step 실패
    bool insertSignal(const trading::SignalRecord& sig);

private:
    sqlite3* db_{ nullptr };

    // 단순 SQL 실행 (스키마 초기화·PRAGMA 전용)
    void exec(const char* sql);

    // 테이블·인덱스 생성 (open() 내부에서만 호출)
    void initSchema();

    // created_at 문자열 → epoch ms
    //   숫자 문자열 (WS)  : stoll()
    //   ISO8601 (REST)    : 파싱 후 epoch ms
    //   파싱 실패          : 0 반환 + WARN 로그
    static int64_t normalizeToEpochMs(const std::string& s);
};

} // namespace db
