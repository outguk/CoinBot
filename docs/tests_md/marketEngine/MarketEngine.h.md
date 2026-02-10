# MarketEngine.h 상세 해설

## 1) 파일 역할

`src/engine/MarketEngine.h`는 마켓 단위 주문 엔진의 인터페이스를 정의한다.

핵심 목적:

1. 단일 마켓(`KRW-BTC` 등) 범위에서 주문/체결/상태 이벤트 처리
2. 주문 API(`IOrderApi`)와 자산 관리자(`AccountManager`)를 연결
3. 엔진 이벤트(`EngineEvent`) 배출

---

## 1.1) 핵심 인터페이스 코드

```cpp
class MarketEngine final {
public:
    MarketEngine(std::string market,
                 api::upbit::IOrderApi& api,
                 OrderStore& store,
                 trading::allocation::AccountManager& account_mgr);

    void bindToCurrentThread();
    EngineResult submit(const core::OrderRequest& req);
    void onMyTrade(const core::MyTrade& t);
    void onOrderStatus(std::string_view order_id, core::OrderStatus s);
    void onOrderSnapshot(const core::Order& snapshot);
    std::vector<EngineEvent> pollEvents();
    std::optional<core::Order> get(std::string_view order_id) const;
};
```

이 인터페이스만 보면 엔진의 핵심 루프가 명확하다:

```text
submit -> (myTrade/snapshot/status 반영) -> pollEvents
```

---

## 2) 의존성 연결 구조

## 2.1 외부 협력 객체

```cpp
api::upbit::IOrderApi& api_;
OrderStore& store_;
trading::allocation::AccountManager& account_mgr_;
```

각 역할:
- `api_`: 거래소 주문 발송/조회 (`postOrder` 등)
- `store_`: 주문 로컬 상태 저장소(upsert/update/get/cleanup)
- `account_mgr_`: reserve/finalize 기반 잔고 정산

## 2.2 마켓 스코프

```cpp
std::string market_;
```

엔진은 하나의 마켓만 담당하며, 이벤트 처리 시 `market_` 일치 여부를 검사한다.

---

## 3) 상태 멤버 설계 의도

## 3.1 스레드 소유권

```cpp
std::thread::id owner_thread_{};
```

- `bindToCurrentThread()`로 오너 스레드 고정
- 이후 모든 API는 오너 스레드에서만 호출되어야 함

## 3.2 중복 체결 방지

```cpp
std::unordered_set<std::string> seen_trades_;
std::deque<std::string> seen_trade_fifo_;
```

- 동일 체결 이벤트 중복 수신 시 1회만 처리
- FIFO로 오래된 key 제거

## 3.3 이벤트 큐

```cpp
std::deque<EngineEvent> events_;
```

- 엔진 내부에서 발생한 fill/status 이벤트를 누적
- `pollEvents()`에서 배치로 꺼냄

## 3.4 전량 거래 모델용 활성 주문 상태

```cpp
std::optional<trading::allocation::ReservationToken> active_buy_token_;
std::string active_buy_order_id_;
std::string active_sell_order_id_;
```

의미:
- 매수는 예약 토큰 기반으로 진행(동시 1개)
- 매도도 동시 1개만 허용
- BUY/SELL 동시 활성 금지

---

## 4) 내부 헬퍼 함수

```cpp
static bool validateRequest(const core::OrderRequest& req, std::string& reason) noexcept;
static core::Amount computeReserveAmount(const core::OrderRequest& req);
void assertOwner_() const;
void pushEvent_(EngineEvent ev);
static std::string extractCurrency(std::string_view market);
static std::string makeTradeDedupeKey_(const core::MyTrade& t);
bool markTradeOnce(std::string_view trade_id);
void finalizeBuyToken_(std::string_view order_id);
```

핵심 의도:
- 검증/예약계산/중복방지/토큰정리를 메인 흐름(`submit`, `onMyTrade`, `onOrderSnapshot`)에서 분리
- 함수 단위 책임을 분명히 유지

---

## 5) 설계 핵심 규칙

1. 마켓 격리: 다른 마켓 이벤트는 무시
2. 단일 스레드: 오너 스레드 위반 시 assert/terminate
3. 전량 모델: 동시 매수 1개, 동시 매도 1개, 반대 포지션 동시 금지
4. 주문 실패 시 매수 예약 즉시 복구
5. 터미널 상태(Filled/Canceled/Rejected)에서 토큰/활성 ID 정리

---

## 6) 사용 흐름 예시

```cpp
engine.bindToCurrentThread();

auto r = engine.submit(buy_req);               // reserve + postOrder + store upsert
engine.onMyTrade(my_trade_event);              // fill 정산 + EngineFillEvent
engine.onOrderSnapshot(order_snapshot_event);  // 터미널 상태면 토큰 정리 + status event

auto events = engine.pollEvents();             // 상위가 처리
```

---

## 7) 왜 이런 구조인가

1. 주문 API/잔고/주문상태를 하나의 마켓 경계 안에서 일관되게 관리하기 위해
2. 멀티마켓에서 크로스 오염(다른 마켓 이벤트 반영)을 차단하기 위해
3. 네트워크/WS 지연, 중복 수신, 순서 뒤집힘 같은 운영 이슈에 대비하기 위해

---

## 8) 함께 보면 좋은 파일

- `src/engine/MarketEngine.cpp`
- `src/engine/OrderStore.h`
- `src/trading/allocation/AccountManager.h`
- `src/api/upbit/IOrderApi.h`
- `tests/test_market_engine.cpp`

