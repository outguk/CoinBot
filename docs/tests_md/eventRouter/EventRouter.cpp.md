# EventRouter.cpp 상세 해설

## 1) 구현 목표

`src/app/EventRouter.cpp`는 다음을 동시에 만족하도록 구현됐다.

1. 빠른 마켓 추출(fast path)
2. 복잡 케이스 fallback 파싱(slow path)
3. 멀티마켓 큐 분기
4. 이벤트 종류별 정책 분리
   - marketData: 백프레셔 드롭 허용
   - myOrder: 유실 불가

---

## 1.1) 핵심 코드 패턴

```cpp
auto fast = extractMarketFast_(json);
auto it = routes_.find(market_key);
it->second->push(engine::input::MarketDataRaw{std::string(json)}); // marketData
it->second->push(engine::input::MyOrderRaw{std::string(json)});    // myOrder
```

---

## 2) 함수별 상세

## `registerMarket`

원본:

```cpp
void EventRouter::registerMarket(const std::string& market, PrivateQueue& queue)
{
    routes_[market] = &queue;
    util::log().info("[EventRouter] registered market=", market);
}
```

의미:
- 마켓 -> 큐 매핑 등록
- 같은 key 재등록 시 덮어쓰기

---

## `extractStringValue_(json, key)`

역할:
- `"key":"value"` 패턴 문자열을 빠르게 추출
- escape(`\`)가 있으면 fast path를 포기하고 fallback으로 넘김

핵심:

```cpp
const auto kpos = json.find(key);
if (kpos == std::string_view::npos) return std::nullopt;
...
if (json[pos] == '\\') return std::nullopt;
...
if (val_len == 0 || val_len > 20) return std::nullopt;
```

왜 필요한가:
- 대량 WS 트래픽에서 full JSON parse 비용 절감

---

## `extractMarketFast_`

역할:
- `code`/`market` 키를 fast 추출
- 둘 다 있으면 충돌까지 검증

핵심:

```cpp
if (code_val && market_val) {
    if (*code_val == *market_val) return {code_val, false};
    return {std::nullopt, true}; // conflict
}
if (code_val) return {code_val, false};
if (market_val) return {market_val, false};
```

---

## `extractMarketSlow_`

역할:
- fast 실패 시 nlohmann 파서로 재시도

핵심:

```cpp
auto parsed = nlohmann::json::parse(json, nullptr, false);
if (parsed.is_discarded()) return std::nullopt;
...
if (!code_val.empty() && !market_val.empty()) {
    if (code_val == market_val) return code_val;
    return std::nullopt; // conflict
}
```

---

## `routeMarketData`

현재 구현 순서:

1. `extractMarketFast_`
2. conflict면 즉시 실패(`conflict_detected++`)
3. fast 성공이면 `used_fast = true`
4. fast 실패면 slow 시도
   - slow 성공: `used_fallback = true`
   - 실패: `parse_failures++`, false
5. `routes_.find(market_key)`
   - 미등록: `unknown_market++`, false
6. 이 시점에만 경로 카운터 반영
   - `used_fast`면 `fast_path_success++`
   - `used_fallback`면 `fallback_used++`
7. 백프레셔 검사(`size >= 5000`)
   - 포화: `route_queue_full++`, true(드롭)
8. push + `total_routed++`

원본 핵심 분기:

```cpp
bool used_fast = false;
bool used_fallback = false;

if (fast.market) {
    used_fast = true;
    market_key.assign(...);
} else if (auto slow = extractMarketSlow_(json)) {
    used_fallback = true;
    market_key = std::move(*slow);
} else {
    stats_.parse_failures.fetch_add(1, std::memory_order_relaxed);
    return false;
}

auto it = routes_.find(market_key);
if (it == routes_.end()) {
    stats_.unknown_market.fetch_add(1, std::memory_order_relaxed);
    return false;
}

if (used_fast) stats_.fast_path_success.fetch_add(1, std::memory_order_relaxed);
if (used_fallback) stats_.fallback_used.fetch_add(1, std::memory_order_relaxed);
```

핵심 해석:
- marketData에서는 unknown이면 fast/fallback 카운터를 올리지 않는다.

---

## `routeMyOrder`

순서는 marketData와 유사하지만 차이점 2개가 중요하다.

1. fast/fallback 카운터 증가 시점
   - fast/slow 파싱 성공 직후 즉시 증가
2. 백프레셔 없음
   - 항상 `MyOrderRaw` push

원본:

```cpp
if (fast.market) {
    stats_.fast_path_success.fetch_add(1, std::memory_order_relaxed);
    market_key.assign(...);
} else if (auto slow = extractMarketSlow_(json)) {
    stats_.fallback_used.fetch_add(1, std::memory_order_relaxed);
    market_key = std::move(*slow);
}
...
it->second->push(engine::input::MyOrderRaw{std::string(json)});
stats_.total_routed.fetch_add(1, std::memory_order_relaxed);
```

핵심 해석:
- myOrder는 unknown이어도 fast/fallback 카운터가 먼저 증가할 수 있다.

---

## 3) 통계 카운터 읽는 법

중요:
- `fast_path_success`/`fallback_used`는 함수별 증가 시점이 다르다.
  - `routeMarketData`: unknown 통과 후 증가
  - `routeMyOrder`: unknown 체크 전 증가

빠른 표:

| 상황 | fast_path_success | unknown_market | total_routed |
|---|---:|---:|---:|
| marketData + unknown | +0 | +1 | +0 |
| myOrder + unknown (fast 파싱 성공) | +1 | +1 | +0 |
| marketData + queue full | +1 또는 +0 | +0 | +0 (`route_queue_full +1`) |

---

## 4) 설계 이유

1. fast path로 평균 처리량 확보
2. fallback으로 정확성 보완
3. marketData는 최신성 우선이라 드롭 허용
4. myOrder는 정합성 우선이라 드롭 금지

---

## 5) 학습 순서 추천

1. `routeMarketData` 분기 읽기
2. `routeMyOrder`와 차이점 비교
3. `extractMarketFast_`/`extractMarketSlow_`로 파싱 정책 확인
4. 마지막으로 테스트 매핑 문서 대조

---

## 6) 함께 보면 좋은 파일

- `src/app/EventRouter.h`
- `tests/test_event_router.cpp`
- `docs/tests_md/eventRouter/test_event_router_mapping.md`

