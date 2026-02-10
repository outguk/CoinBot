# EventRouter.h 상세 해설

## 1) 파일 역할

`src/app/EventRouter.h`는 WS JSON 원문에서 마켓 코드를 추출해
마켓별 전용 큐로 이벤트를 전달하는 라우터의 인터페이스를 정의한다.

핵심 책임:

1. 마켓 등록(`registerMarket`)
2. 시장 데이터 라우팅(`routeMarketData`)
3. 주문 이벤트 라우팅(`routeMyOrder`)
4. 통계 카운터 제공(`stats`)

---

## 1.1) 핵심 인터페이스 코드

```cpp
class EventRouter {
public:
    using PrivateQueue = core::BlockingQueue<engine::input::EngineInput>;

    void registerMarket(const std::string& market, PrivateQueue& queue);
    [[nodiscard]] bool routeMarketData(std::string_view json);
    [[nodiscard]] bool routeMyOrder(std::string_view json);
};
```

`PrivateQueue`는 `EngineInput` variant를 담는다:

```cpp
using EngineInput = std::variant<MyOrderRaw, MarketDataRaw>;
```

즉 라우터는 파싱된 도메인 객체가 아니라 JSON 원문을 타입(`MyOrderRaw`/`MarketDataRaw`)으로 구분해 전달한다.

---

## 2) 연결 구조

직접 의존:

- `core/BlockingQueue.h`: 마켓별 입력 큐
- `engine/input/EngineInput.h`: 큐 payload 타입

호출 흐름:

```cpp
EventRouter router;
EventRouter::PrivateQueue btc_q, eth_q;

router.registerMarket("KRW-BTC", btc_q);
router.registerMarket("KRW-ETH", eth_q);

router.routeMarketData(R"({"type":"ticker","code":"KRW-BTC"})"); // -> btc_q
router.routeMyOrder(R"({"type":"myOrder","code":"KRW-ETH"})");   // -> eth_q
```

---

## 3) 공개 API 의도

## `registerMarket`

- 마켓 문자열 -> 큐 포인터 매핑 등록
- 전제: WS 시작 전에 등록 완료, 이후 `routes_` 읽기 전용

왜 이런 계약인가:
- 런타임 락/맵 변경 없이 빠른 조회를 보장하기 위해

## `routeMarketData`

- 시장 데이터 전용 경로
- 큐 포화 시 드롭 허용(백프레셔)
- 반환:
  - `true`: 파싱 성공(실제 push 또는 드롭)
  - `false`: 파싱 실패/충돌/미등록 마켓

## `routeMyOrder`

- 주문 이벤트 전용 경로
- 유실 불가 정책으로 항상 push 시도
- 반환:
  - `true`: 파싱 성공 + push
  - `false`: 파싱 실패/충돌/미등록 마켓

---

## 4) Stats 구조와 해석

원본 선언:

```cpp
struct Stats {
    std::atomic<uint64_t> fast_path_success{0};
    std::atomic<uint64_t> fallback_used{0};
    std::atomic<uint64_t> parse_failures{0};
    std::atomic<uint64_t> conflict_detected{0};
    std::atomic<uint64_t> unknown_market{0};
    std::atomic<uint64_t> route_queue_full{0};
    std::atomic<uint64_t> total_routed{0};
};
```

해석 포인트:

- `fast_path_success`: fast 경로 사용 성공 수
  - `routeMarketData`: 미등록 마켓이면 증가하지 않음(unknown 체크 뒤 증가)
  - `routeMyOrder`: fast 파싱 직후 증가(unknown 여부와 무관)
- `fallback_used`: slow 경로 사용 수
  - `routeMarketData`: unknown 전에 증가하지 않음
  - `routeMyOrder`: slow 성공 직후 증가
- `total_routed`: 실제 push 완료 수
- `route_queue_full`: marketData 드롭 수(orders에는 적용 안 됨)

즉 카운터는 함수별 증가 시점이 다르므로 같은 의미로 보면 안 된다.

---

## 5) 내부 도우미 함수 계약

```cpp
FastResult extractMarketFast_(std::string_view json) const;
std::optional<std::string> extractMarketSlow_(std::string_view json) const;
static std::optional<std::string_view> extractStringValue_(std::string_view json, std::string_view key);
```

설계 의도:

1. fast path: 문자열 스캔 기반 고속 추출
2. slow path: JSON 파서 기반 정확성 보완
3. 충돌(`code != market`)은 즉시 실패 처리

---

## 6) 멤버 설계

```cpp
std::unordered_map<std::string, PrivateQueue*> routes_;
Stats stats_;
static constexpr std::size_t kMaxBacklog = 5000;
```

- `routes_`: 소유권 없는 포인터 매핑(큐 수명은 외부 계약)
- `kMaxBacklog`: marketData 드롭 임계치

---

## 7) 왜 이렇게 설계했는가

1. WS 이벤트는 빈도가 높아 파싱 비용 최적화가 필요
2. 하지만 escape/경계 케이스는 정확성 보완이 필요
3. 시장 데이터와 주문 이벤트는 유실 정책이 다름

결론:
- `fast + fallback` 파싱
- `marketData`/`myOrder` 정책 분리
- 운영 가시화를 위한 stats

---

## 8) 함께 보면 좋은 파일

- `src/app/EventRouter.cpp`
- `tests/test_event_router.cpp`
- `docs/tests_md/eventRouter/test_event_router_mapping.md`

