// app/EventRouter.cpp
#include "EventRouter.h"

#include <json.hpp>

#include "util/Logger.h"

namespace app {

void EventRouter::registerMarket(const std::string& market, PrivateQueue& queue)
{
    routes_[market] = &queue;
    util::log().info("[EventRouter] registered market=", market);
}

// ── 키 기반 문자열 값 추출 (zero-allocation) ──────────────────────────
// JSON 내에서 "key" : "value" 형태를 찾아 value를 string_view로 반환
// 이스케이프(\\) 포함 시 nullopt → fallback 파싱으로 전환
std::optional<std::string_view> EventRouter::extractStringValue_(
    std::string_view json, std::string_view key)
{
    const auto kpos = json.find(key);
    if (kpos == std::string_view::npos) return std::nullopt;

    // key 뒤 공백 스킵 → ':' 확인
    auto pos = kpos + key.size();
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
		++pos;  // json 문자열 안에 있는지, 공백 인지 검사하고 맞으면 건너뛰며 pos 증가 - 의미있는 위치에서 멈춤

    if (pos >= json.size() || json[pos] != ':') return std::nullopt;
    ++pos; // ':' 건너뜀

    // ':' 뒤 공백 스킵 → '"' 확인
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
        ++pos;

    if (pos >= json.size() || json[pos] != '"') return std::nullopt;
    ++pos; // 여는 '"' 건너뜀

    // 닫는 '"' 까지 스캔
    const auto val_start = pos;
    while (pos < json.size()) {
        if (json[pos] == '\\') return std::nullopt; // 이스케이프 → fallback
        if (json[pos] == '"') break;
        ++pos;
    }

    if (pos >= json.size()) return std::nullopt; // 닫는 '"' 없음

    const auto val_len = pos - val_start;
    if (val_len == 0 || val_len > 20) return std::nullopt; // 비정상 길이

    return json.substr(val_start, val_len);
}

// ── Fast path: "code" 또는 "market" 키에서 마켓 코드 추출 ────────────
EventRouter::FastResult EventRouter::extractMarketFast_(std::string_view json) const
{
    auto code_val   = extractStringValue_(json, "\"code\"");
    auto market_val = extractStringValue_(json, "\"market\"");

    // 둘 다 있는 경우: 값 일치 확인
    if (code_val && market_val) {
        if (*code_val == *market_val) {
            return {code_val, false};
        }
        // code 와 market이 다르면 충돌 감지 → fallback 없이 즉시 실패 처리
        util::log().warn("[EventRouter] code/market conflict: code=",
                         *code_val, " market=", *market_val);
        return {std::nullopt, true};
    }

    if (code_val) return {code_val, false};
    if (market_val) return {market_val, false};

    return {std::nullopt, false}; // 둘 다 없음 → fallback
}

// ── Fallback: nlohmann::json 정규 파싱 ───────────────────────────────
std::optional<std::string> EventRouter::extractMarketSlow_(std::string_view json) const
{
	auto parsed = nlohmann::json::parse(json, nullptr, false); // 예외 없이 파싱 시도 이때 실패 시 discarded 상태 json을 반환
    if (parsed.is_discarded()) return std::nullopt; // 파싱 실패 검사

    std::string code_val;
    std::string market_val;

    // if(초기화문; 조건문)
    // - it을 if 내부에서 초기화 후 사용하는 if 문법 패턴
    if (auto it = parsed.find("code"); it != parsed.end() && it->is_string())
        code_val = it->get<std::string>();

    if (auto it = parsed.find("market"); it != parsed.end() && it->is_string())
        market_val = it->get<std::string>();

    // 둘 다 있는 경우: 일치 확인
    if (!code_val.empty() && !market_val.empty()) {
        if (code_val == market_val) return code_val;
        util::log().warn("[EventRouter][slow] code/market conflict: code=",
                         code_val, " market=", market_val);
        return std::nullopt;
    }

    if (!code_val.empty()) return code_val;
    if (!market_val.empty()) return market_val;

    return std::nullopt;
}

// ── 시장 데이터 라우팅 (drop-oldest는 BlockingQueue 내부에서 처리) ────
bool EventRouter::routeMarketData(std::string_view json)
{
    // 1. 마켓 코드 추출 (fast → fallback, 충돌 시 즉시 실패)
    std::string market_key;

	// 아래는 Stats 집계용 플래그와 로직이다.
    // 어떤 방식을 사용했는지 체크용
    bool used_fast = false;
    bool used_fallback = false;

    auto fast = extractMarketFast_(json);
    if (fast.had_conflict) {
        // 충돌 감지 → fallback 없이 즉시 실패
        stats_.conflict_detected.fetch_add(1, std::memory_order_relaxed);
        util::log().warn("[EventRouter] marketData code/market conflict");
        return false;
    }

    if (fast.market) {
        used_fast = true;
        market_key.assign(fast.market->data(), fast.market->size());
    }
    else if (auto slow = extractMarketSlow_(json)) {
        used_fallback = true;
        market_key = std::move(*slow);
    }
    else {
        stats_.parse_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // 2. 라우팅 대상 큐 조회
    auto it = routes_.find(market_key);
    if (it == routes_.end()) {
        stats_.unknown_market.fetch_add(1, std::memory_order_relaxed);
        util::log().warn("[EventRouter] marketData unknown market=", market_key);
        return false;
    }

    if (used_fast) stats_.fast_path_success.fetch_add(1, std::memory_order_relaxed);
    if (used_fallback) stats_.fallback_used.fetch_add(1, std::memory_order_relaxed);

    // push (큐 포화 시 BlockingQueue 내부에서 drop-oldest 처리)
    it->second->push(engine::input::MarketDataRaw{std::string(json)});
    stats_.total_routed.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ── myOrder 라우팅 (유실 불가, 항상 push) ────────────────────────────
bool EventRouter::routeMyOrder(std::string_view json)
{
    // 1. 마켓 코드 추출 (fast → fallback, 충돌 시 즉시 실패)
    std::string market_key;

    // 아래는 Stats 집계용 플래그와 로직이다.
    // 어떤 방식을 사용했는지 체크용
    bool used_fast = false;
    bool used_fallback = false;

    auto fast = extractMarketFast_(json);
    if (fast.had_conflict) {
        // 충돌 감지 → fallback 없이 즉시 실패
        stats_.conflict_detected.fetch_add(1, std::memory_order_relaxed);
        util::log().warn("[EventRouter] myOrder code/market conflict");
        return false;
    }

    if (fast.market) {
        used_fast = true;
        market_key.assign(fast.market->data(), fast.market->size());
    }
    else if (auto slow = extractMarketSlow_(json)) {
        used_fallback = true;
        market_key = std::move(*slow);
    }
    else {
        stats_.parse_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // 2. 라우팅 대상 큐 조회
    auto it = routes_.find(market_key);
    if (it == routes_.end()) {
        stats_.unknown_market.fetch_add(1, std::memory_order_relaxed);
        util::log().warn("[EventRouter] myOrder unknown market=", market_key);
        return false;
    }

    if (used_fast) stats_.fast_path_success.fetch_add(1, std::memory_order_relaxed);
    if (used_fallback) stats_.fallback_used.fetch_add(1, std::memory_order_relaxed);

    // 3. 유실 불가 → 항상 push (백프레셔 없음)
    it->second->push(engine::input::MyOrderRaw{std::string(json)});
    stats_.total_routed.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace app
