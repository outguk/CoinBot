# MarketEngine.cpp 상세 해설

## 1) 구현 목표 요약

`src/engine/MarketEngine.cpp`는 마켓 단위 주문 엔진의 실행 로직을 구현한다.

핵심 흐름:

1. `submit`으로 주문 제출
2. `onMyTrade`로 체결 반영
3. `onOrderStatus`/`onOrderSnapshot`으로 주문 상태 동기화
4. `pollEvents`로 상위 계층에 이벤트 전달

---

## 2) 스레드 소유권 보장

## `bindToCurrentThread`, `assertOwner_`

원본 핵심:

```cpp
void MarketEngine::bindToCurrentThread() {
    owner_thread_ = std::this_thread::get_id();
}

void MarketEngine::assertOwner_() const {
#ifndef NDEBUG
    assert(owner_thread_ != std::thread::id{});
    assert(std::this_thread::get_id() == owner_thread_);
#else
    if (owner_thread_ == std::thread::id{} || std::this_thread::get_id() != owner_thread_) {
        util::Logger::instance().error(...);
        std::terminate();
    }
#endif
}
```

의미:
- 엔진은 단일 스레드 소유 모델
- 잘못된 스레드 호출을 조기에 강하게 차단

---

## 3) submit: 주문 제출 핵심

## 처리 순서

1. 요청 검증 `validateRequest`
2. 마켓 일치 검사 (`req.market == market_`)
3. 포지션별 사전 제약
   - BID: 중복 매수 금지, 활성 매도 동시 금지, reserve 수행
   - ASK: 중복 매도 금지, 활성 매수 동시 금지
4. `api_.postOrder(req)` 호출
5. 실패 시 BID 토큰 해제
6. 성공 시 active order id 기록 + OrderStore upsert

원본 핵심:

```cpp
if (req.position == core::OrderPosition::BID) {
    if (active_buy_token_.has_value()) return Fail(...);
    if (!active_sell_order_id_.empty()) return Fail(...);

    const core::Amount reserve_amount = computeReserveAmount(req);
    auto token = account_mgr_.reserve(market_, reserve_amount);
    if (!token.has_value()) return Fail(InsufficientFunds, ...);
    active_buy_token_ = std::move(*token);
}
...
auto result = api_.postOrder(req);
if (std::holds_alternative<api::rest::RestError>(result)) {
    if (req.position == core::OrderPosition::BID && active_buy_token_.has_value()) {
        account_mgr_.release(std::move(*active_buy_token_));
        active_buy_token_.reset();
        active_buy_order_id_.clear();
    }
    return Fail(...);
}
```

핵심 설계 이유:
- 주문 제출 실패 시 예약 KRW 누수 방지
- 전량 거래 모델에서 BUY/SELL 동시 진입 차단

---

## 4) onMyTrade: 체결 이벤트 처리

## 처리 순서

1. 마켓 불일치 이벤트 무시
2. dedupe key 생성 + 중복 수신 차단
3. OrderStore에 없는 주문이면 외부 주문으로 간주하고 무시
4. identifier 있으면 `EngineFillEvent` 발행
5. side별 AccountManager 정산
   - BID: 활성 토큰 + order_id 일치 시 `finalizeFillBuy`
   - ASK: `finalizeFillSell`

원본 핵심:

```cpp
if (t.market != market_) return;
const std::string dedupeKey = makeTradeDedupeKey_(t);
if (!markTradeOnce(dedupeKey)) return;

auto ordOpt = store_.get(t.order_id);
if (!ordOpt.has_value()) { ... return; } // external trade ignore

if (t.side == core::OrderPosition::BID) {
    if (active_buy_token_.has_value() && active_buy_order_id_ == t.order_id) {
        const core::Amount executed_krw = t.executed_funds + t.fee;
        account_mgr_.finalizeFillBuy(*active_buy_token_, executed_krw, t.volume, t.price);
    }
} else {
    const core::Amount received_krw = std::max<core::Amount>(0.0, t.executed_funds - t.fee);
    account_mgr_.finalizeFillSell(market_, t.volume, received_krw);
}
```

주의 포인트:
- 매수는 `executed_funds + fee`로 비용 반영
- 매도는 `executed_funds - fee`를 최소 0으로 clamp

---

## 5) onOrderStatus: 상태 갱신 경로

## 처리 순서

1. store에서 주문 조회, 없으면 무시
2. 마켓 불일치면 무시
3. 상태 업데이트, Filled면 remaining=0
4. 터미널 전환 시 후처리
   - BID: 현재 활성 주문이면 `finalizeBuyToken_`
   - ASK: 현재 활성 매도 id면 clear
5. 완료 건수 100개마다 `store_.cleanup()`

핵심 코드:

```cpp
const bool isTerminal = (s == Filled || s == Canceled || s == Rejected);
if (old_status != s && isTerminal) {
    if (o.position == BID && o.id == active_buy_order_id_) finalizeBuyToken_(o.id);
    if (o.position == ASK && o.id == active_sell_order_id_) active_sell_order_id_.clear();
    ...
}
```

---

## 6) onOrderSnapshot: WS/REST 스냅샷 동기화

## 처리 순서

1. 마켓 불일치면 무시
2. store에 없으면 upsert 후 종료
3. 기존 주문에 snapshot 필드 병합
4. 터미널 상태 전환이면 `EngineOrderStatusEvent` 발행
5. BID/ASK 활성 상태 정리

핵심 코드:

```cpp
if (isTerminal && o.status != old_status) {
    if (o.identifier.has_value() && !o.identifier->empty()) {
        EngineOrderStatusEvent ev{...};
        pushEvent_(EngineEvent{ std::move(ev) });
    }
    if (o.position == BID && o.id == active_buy_order_id_) finalizeBuyToken_(o.id);
    if (o.position == ASK && o.id == active_sell_order_id_) active_sell_order_id_.clear();
}
```

---

## 7) 이벤트 배출

## `pushEvent_`, `pollEvents`

```cpp
void MarketEngine::pushEvent_(EngineEvent ev) {
    events_.emplace_back(std::move(ev));
}

std::vector<EngineEvent> MarketEngine::pollEvents() {
    std::vector<EngineEvent> out;
    out.reserve(events_.size());
    while (!events_.empty()) {
        out.emplace_back(std::move(events_.front()));
        events_.pop_front();
    }
    return out;
}
```

의미:
- 내부 큐 누적 -> 외부로 일괄 전달
- poll 후 내부 큐 비움

---

## 8) 요청 검증 로직 `validateRequest`

Upbit 정책을 코드화한 검증:

1. Limit: `price` 필수 + `VolumeSize` 필수
2. Market BID: `AmountSize` 필수, price 금지
3. Market ASK: `VolumeSize` 필수, price 금지
4. amount/volume/price 모두 `> 0`

핵심 코드:

```cpp
if (req.type == core::OrderType::Limit) {
    if (!req.price.has_value()) return false;
    if (!isVolume) return false;
} else {
    if (req.price.has_value()) return false;
    if (req.position == BID && !isAmount) return false;
    if (req.position == ASK && !isVolume) return false;
}
```

---

## 9) 예약 금액 계산 `computeReserveAmount`

```cpp
if (std::holds_alternative<core::AmountSize>(req.size)) {
    return amount * cfg.reserve_margin;
}
return price * volume * cfg.reserve_margin;
```

의도:
- 수수료/슬리피지 여유분 확보를 위해 `reserve_margin` 적용

---

## 10) 토큰 정리 `finalizeBuyToken_`

핵심:

```cpp
if (!active_buy_token_.has_value()) return;
if (active_buy_order_id_.empty() || active_buy_order_id_ != order_id) return;

account_mgr_.finalizeOrder(std::move(*active_buy_token_));
active_buy_token_.reset();
active_buy_order_id_.clear();
```

의미:
- 잘못된 order_id로 토큰을 정리하지 않도록 이중 검증
- 미사용 예약 KRW는 `AccountManager::finalizeOrder`에서 복구

---

## 11) 중복 체결 방지

## `makeTradeDedupeKey_`, `markTradeOnce`

- trade_id가 있으면 그대로 key 사용
- 없으면 주문/side/price/volume/funds/fee 조합으로 fallback key 생성
- `seen_trades_` + FIFO로 중복 방지 및 메모리 상한 유지

핵심:

```cpp
auto [it, inserted] = seen_trades_.emplace(trade_id);
if (!inserted) return false;
...
while (seen_trade_fifo_.size() > cfg.max_seen_trades) { ... }
```

---

## 12) 학습 포인트 요약

1. submit은 "검증 -> 제약 -> 예약 -> API -> 저장" 순서
2. onMyTrade는 "중복차단 + 외부주문 차단 + fill 정산"이 핵심
3. 터미널 상태 처리(onOrderStatus/onOrderSnapshot)에서 토큰 정리 누락이 없어야 자산 정합성이 유지
4. 모든 public API는 오너 스레드 가정을 강제

---

## 13) 함께 보면 좋은 파일

- `src/engine/MarketEngine.h`
- `src/trading/allocation/AccountManager.cpp`
- `src/engine/OrderStore.h`
- `tests/test_market_engine.cpp`

