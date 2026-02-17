# 매수 금액 편차 수정 HYBRID v2 구현 스펙

> 작성일: 2026-02-16
> 최종 수정: 2026-02-16 (구현 완료 반영)
> 상태: 구현 완료 (빌드 검증 대기)

## 1. 목적

문제 A/B를 동시에 해결한다.

- 문제 A: Private WS 재연결 과다로 recovery/sync 빈발
- 문제 B: 런타임 sync 재분배로 마켓별 `krw_available` 출렁임

운영 원칙:

1. 10% 예비금은 거래에 사용하지 않는다.
2. 각 마켓의 손익은 해당 마켓이 감수한다.
3. 런타임 복구는 "주문 단위 복구"로 제한하고, 계좌 전체 재분배는 시작 시점에만 허용한다.

## 2. 현재 구조 적합성 검토 (멀티마켓)

현재 프로젝트 구조:

1. `MarketEngineManager`는 마켓별 독립 워커 + 공유 `OrderStore`/`AccountManager` 구조다.
2. `MarketEngine`는 마켓별 단일 스레드 소유권(`assertOwner_`)을 강제한다.
3. `AccountManager::syncWithAccount`는 전체 계좌를 기준으로 마켓 KRW를 재분배한다.

HYBRID v2 적합성 판단:

1. 런타임 복구를 마켓 로컬 주문 단위로 제한하면 멀티마켓 격리를 유지할 수 있다.
2. 정산 경로를 `MarketEngine` 단일 경로로 통일하면 중복 정산/누락 리스크를 줄일 수 있다.
3. 런타임에서 `syncWithAccount`를 배제하면 문제 B의 근본 원인을 제거할 수 있다.

결론: HYBRID v2는 현재 멀티마켓 구조에 적절하다.

## 3. 최종 설계 방향

복잡한 신규 회계 상태(`owned_krw`, `unattributed_krw_`)는 도입하지 않는다.

1. WS read timeout wake-up 복원으로 문제 A를 완화한다.
2. 런타임 복구는 `getOrder(uuid)` 중심으로 전환한다.
3. 체결 정산은 정상/복구 모두 `MarketEngine`을 단일 진입점으로 사용한다.
4. 런타임에서 계좌 전체 재분배(`syncWithAccount`) 호출을 금지한다.
5. 전략 매수 금액은 `reserve_margin` 기반 동적 계산으로 바꿔 설정 결합을 줄인다.

## 4. 상세 구현 스펙

### 4.1 WS 안정화 (문제 A)

파일: `src/api/ws/UpbitWebSocketClient.cpp`

변경:

1. `ws_->read()` 직전에 TCP timeout 설정
2. timeout 에러는 reconnect 사유로 처리하지 않고 루프 지속

```cpp
beast::get_lowest_layer(*ws_).expires_after(
    util::AppConfig::instance().websocket.idle_timeout);

if (ec == beast::error::timeout ||
    ec == boost::asio::error::timed_out)
    continue;
```

### 4.2 계좌 전체 재분배 API 분리

핵심: 런타임 복구에서 전체 재분배를 코드 레벨로 차단한다.

파일: `src/trading/allocation/AccountManager.h`

```cpp
// 시작/수동점검 전용: 계좌 전체 기준 재구축
void rebuildFromAccount(const core::Account& account);
```

정책:

1. 기존 `syncWithAccount`는 `rebuildFromAccount`로 rename한다.
2. `MarketEngineManager` 생성자 초기 동기화에서만 호출한다.
3. `runRecovery_`에서는 절대 호출하지 않는다.

### 4.3 복구 요청 우선 처리 (제어 채널)

문제: 일반 큐 push 방식은 drop-oldest/지연에 취약하다.

파일: `src/app/MarketEngineManager.h`

```cpp
struct MarketContext {
    // ...
    std::atomic<bool> recovery_requested{false};
};
```

파일: `src/app/MarketEngineManager.h`

```cpp
void requestReconnectRecovery();
```

파일: `src/app/MarketEngineManager.cpp`

```cpp
void MarketEngineManager::requestReconnectRecovery()
{
    for (auto& [_, ctx] : contexts_)
        ctx->recovery_requested.store(true, std::memory_order_release);
}
```

```cpp
// workerLoop_ 반복 시작부
if (ctx.recovery_requested.exchange(false, std::memory_order_acq_rel))
    runRecovery_(ctx);
```

파일: `src/app/CoinBot.cpp`

- reconnect callback에서 `requestAccountSync()` 대신 `requestReconnectRecovery()` 호출

### 4.4 주문 단건 조회 API 추가

파일: `src/api/upbit/IOrderApi.h`

```cpp
virtual std::variant<core::Order, api::rest::RestError>
    getOrder(std::string_view uuid) = 0;
```

구현 파일:

1. `src/api/upbit/SharedOrderApi.h`
2. `src/api/upbit/SharedOrderApi.cpp`
3. `src/api/upbit/UpbitExchangeRestClient.h`
4. `src/api/upbit/UpbitExchangeRestClient.cpp`

REST: `GET /v1/order?uuid=...`

### 4.5 MarketEngine 복구 정산 단일 경로

추가 API:

파일: `src/engine/MarketEngine.h`

```cpp
struct PendingIds {
    std::string buy_id;
    std::string sell_id;
};

PendingIds activePendingIds() const noexcept;

// REST snapshot 누적값과 OrderStore 누적값의 차이(delta)만 정산
bool reconcileFromSnapshot(const core::Order& snapshot);
```

정산 규칙:

1. `delta_volume = snapshot.executed_volume - prev.executed_volume`
2. `delta_funds = snapshot.executed_funds - prev.executed_funds`
3. `delta_paid_fee = snapshot.paid_fee - prev.paid_fee`
4. delta 정산 후 snapshot 반영 (`onOrderSnapshot`) 순서로 처리한다.

중요 순서:

1. delta 정산
2. `onOrderSnapshot(snapshot)` 호출

이유:

- 터미널 snapshot은 `onOrderSnapshot` 내부에서 토큰을 정리할 수 있다.
- snapshot 반영을 먼저 하면 매수 delta 정산 시 토큰이 사라져 누락될 수 있다.

의사코드:

```cpp
bool MarketEngine::reconcileFromSnapshot(const core::Order& snapshot)
{
    assertOwner_();
    if (!snapshot.market.empty() && snapshot.market != market_)
        return false;

    auto prev = store_.get(snapshot.id);
    if (!prev.has_value())
        return false;

    const double delta_volume = snapshot.executed_volume - prev->executed_volume;
    const double delta_funds = snapshot.executed_funds - prev->executed_funds;
    const double delta_paid_fee = snapshot.paid_fee - prev->paid_fee;

    const double safe_delta_volume = std::max(0.0, delta_volume);
    const double safe_delta_funds = std::max(0.0, delta_funds);
    const double safe_delta_fee = std::max(0.0, delta_paid_fee);

    if (safe_delta_volume > 0.0)
    {
        if (snapshot.position == core::OrderPosition::BID)
        {
            if (!active_buy_token_.has_value() || active_buy_order_id_ != snapshot.id)
                return false;

            const double fill_price =
                (safe_delta_funds > 0.0) ? (safe_delta_funds / safe_delta_volume) : 0.0;
            if (fill_price > 0.0)
            {
                const double delta_krw = safe_delta_funds + safe_delta_fee;
                account_mgr_.finalizeFillBuy(
                    *active_buy_token_, delta_krw, safe_delta_volume, fill_price);
            }
        }
        else
        {
            const double received_krw = std::max(0.0, safe_delta_funds - safe_delta_fee);
            if (received_krw > 0.0)
                account_mgr_.finalizeFillSell(market_, safe_delta_volume, received_krw);
        }
    }

    onOrderSnapshot(snapshot);
    return true;
}
```

멱등성:

- 동일 snapshot 재주입 시 delta가 0이므로 재정산되지 않는다.

### 4.6 runRecovery_ 전환 (런타임 재분배 제거)

파일: `src/app/MarketEngineManager.cpp`

새 흐름:

1. `ctx.engine->activePendingIds()` 조회
2. pending 주문이 없으면 종료
3. 각 pending 주문에 대해 복구 시도
4. 우선순위 1: `getOrder(uuid)` (최대 3회 재시도)
5. 우선순위 2: `getOpenOrders(market)`에서 동일 uuid 탐색
6. snapshot 확보 시 `ctx.engine->reconcileFromSnapshot(snapshot)` 호출
7. open 상태면 pending 유지, 터미널이면 엔진/전략 상태 정리

중요:

1. 런타임 `runRecovery_`에서 `rebuildFromAccount` 호출 금지
2. 타 마켓 KRW 재분배 금지

터미널 처리:

- snapshot이 `Filled/Canceled/Rejected`이면 `reconcileFromSnapshot` 후  
  `clearPendingState()` + `strategy.syncOnStart(position_snapshot)` 수행

open 처리:

- partial fill delta는 이미 정산됨
- pending 유지 후 WS 이벤트로 자연 완결

### 4.7 복구 실패 fallback 정책

fallback은 "정산 추정"이 아니라 "안전 우선"으로 설계한다.

1차: `getOrder(uuid)` 재시도  
2차: `getOpenOrders(market)` 조회  
3차: `getMyAccount()`는 관측/로그용으로만 사용 (상태 직접 덮어쓰기 금지)

3차 규칙:

1. 계좌 스냅샷은 로그 진단에만 사용한다.
2. `AccountManager`의 단일 마켓 강제 덮어쓰기 메서드는 도입하지 않는다.
3. 주문 귀속을 확정할 수 없으면 pending 유지 후 다음 recovery 주기로 넘긴다.

이유:

- 마켓별 KRW/예약 불변식을 깨지 않고, 오정산보다 미정산을 선택한다.

### 4.8 10% 예비금 정책

파일: `src/util/Config.h`

```cpp
double global_reserve_ratio = 0.10;
```

파일: `src/trading/allocation/AccountManager.cpp`

`rebuildFromAccount` 초기 분배에서:

1. `reserve = account.krw_free * global_reserve_ratio`
2. `distributable = account.krw_free - reserve`
3. `distributable`만 flat 마켓에 균등 분배

런타임 복구에서는 reserve 재계산/재분배 금지.

### 4.9 전략 매수 금액 동적 계산

목표: `riskPercent` 고정값(예: 99.8) 제거.

파일: `src/trading/strategies/RsiMeanReversionStrategy.h`

```cpp
double utilization{1.0}; // 배분 자본 사용 비율
```

파일: `src/trading/strategies/RsiMeanReversionStrategy.cpp`

```cpp
const auto& engine_cfg = util::AppConfig::instance().engine;
const double krw_to_use =
    account.krw_available / engine_cfg.reserve_margin * params_.utilization;
```

관계:

- 전략 계산: `krw_to_use = available / reserve_margin * utilization`
- 엔진 예약: `reserve = krw_to_use * reserve_margin`
- 결과: `reserve = available * utilization`

## 5. 수용 기준 (Acceptance Criteria)

1. reconnect가 발생해도 flat 마켓 수 변화만으로 `krw_available`가 급변하지 않는다.
2. 유실 체결 복구 시 해당 마켓만 자산 변화가 발생한다.
3. open 주문 부분 체결 상태에서도 delta 정산 누락이 없다.
4. 동일 snapshot 중복 주입 시 계좌 값이 추가로 변하지 않는다.
5. 복구 요청은 WS 이벤트 폭주 중에도 유실되지 않는다.
6. 런타임 recovery 경로에서 계좌 전체 재분배 호출이 발생하지 않는다.

## 6. 구현 순서

| # | 항목 | 상태 |
|---|------|------|
| 1 | WS timeout 패치 적용 | 완료 |
| 2 | `syncWithAccount` → `rebuildFromAccount` rename + 호출 경계 분리 | 완료 |
| 3 | 복구 요청 제어 채널(`recovery_requested`) 적용 | 완료 |
| 4 | `IOrderApi::getOrder` 추가 및 REST 구현 | 완료 |
| 5 | `MarketEngine::activePendingIds` + `reconcileFromSnapshot` 추가 | 완료 |
| 6 | `MarketEngineManager::runRecovery_` 주문 단건 조회 기반 교체 | 완료 |
| 7 | reconnect callback → `requestReconnectRecovery` 변경 | 완료 |
| 8 | 글로벌 예비금(10%) 초기 분리 반영 | 완료 |
| 9 | 전략 매수 금액 utilization 기반 전환 | 완료 |
| 10 | 빌드 검증 | 대기 |

## 7. 수정 파일 목록

### 7.1 WS 안정화 (§4.1)

| 파일 | 변경 내용 |
|------|----------|
| `src/api/ws/UpbitWebSocketClient.cpp` | `ws_->read()` 전 TCP timeout 설정, timeout 에러 시 reconnect 대신 루프 continue |

### 7.2 계좌 API 분리 (§4.2)

| 파일 | 변경 내용 |
|------|----------|
| `src/trading/allocation/AccountManager.h` | `syncWithAccount` → `rebuildFromAccount` rename, 시작 전용 주석 추가 |
| `src/trading/allocation/AccountManager.cpp` | 메서드 rename, 주석에 "런타임 복구에서 호출 금지" 명시 |
| `src/app/MarketEngineManager.h` | `syncAccountWithExchange_` → `rebuildAccountOnStartup_` rename |
| `src/app/MarketEngineManager.cpp` | 모든 `syncWithAccount` 호출을 `rebuildFromAccount`로 변경 |

### 7.3 복구 제어 채널 (§4.3)

| 파일 | 변경 내용 |
|------|----------|
| `src/app/MarketEngineManager.h` | `MarketContext`에 `std::atomic<bool> recovery_requested` 추가, `requestReconnectRecovery()` 선언 |
| `src/app/MarketEngineManager.cpp` | `requestReconnectRecovery()` 구현 (모든 ctx에 atomic flag 설정), `workerLoop_`에서 큐 폴링 전 flag 체크 |
| `src/app/CoinBot.cpp` | reconnect callback에서 `requestAccountSync()` → `requestReconnectRecovery()` 변경 |

### 7.4 주문 단건 조회 API (§4.4)

| 파일 | 변경 내용 |
|------|----------|
| `src/api/upbit/IOrderApi.h` | `getOrder(uuid)` 순수 가상 메서드 추가 |
| `src/api/upbit/SharedOrderApi.h` | `getOrder` override 선언 |
| `src/api/upbit/SharedOrderApi.cpp` | mutex 직렬화 기반 `getOrder` 구현 |
| `src/api/upbit/UpbitExchangeRestClient.h` | `getOrder` 선언 |
| `src/api/upbit/UpbitExchangeRestClient.cpp` | `GET /v1/order?uuid=...` REST 구현, `WaitOrderResponseDto` + `OpenOrdersMapper::toDomain()` 재사용 |

### 7.5 MarketEngine 복구 정산 (§4.5)

| 파일 | 변경 내용 |
|------|----------|
| `src/engine/MarketEngine.h` | `PendingIds` 구조체, `activePendingIds()`, `reconcileFromSnapshot()`, `clearPendingState()` 선언 |
| `src/engine/MarketEngine.cpp` | `activePendingIds()` 구현 (active_buy/sell_order_id_ 반환), `reconcileFromSnapshot()` 구현 (delta 정산 → onOrderSnapshot 순서 보장, safe_delta clamping) |

### 7.6 runRecovery_ 전환 (§4.6, §4.7)

| 파일 | 변경 내용 |
|------|----------|
| `src/app/MarketEngineManager.h` | `queryOrderWithRetry_()`, `findOrderInOpenOrders_()` 헬퍼 선언 |
| `src/app/MarketEngineManager.cpp` | `runRecovery_` 전면 교체 (getOrder 기반 3단계 fallback), `queryOrderWithRetry_` (3회 재시도, 1초 간격), `findOrderInOpenOrders_` (getOpenOrders에서 uuid 탐색) 구현 |

### 7.7 예비금 + 전략 계산 (§4.8, §4.9)

| 파일 | 변경 내용 |
|------|----------|
| `src/util/Config.h` | `AccountConfig`에 `global_reserve_ratio = 0.10` 추가 |
| `src/trading/allocation/AccountManager.cpp` | 생성자 3단계 + `rebuildFromAccount` 4단계에서 분배 전 예비금(10%) 분리 |
| `src/trading/strategies/RsiMeanReversionStrategy.h` | `riskPercent{90}` → `utilization{1.0}` 변경 |
| `src/trading/strategies/RsiMeanReversionStrategy.cpp` | `krw_available * pct/100` → `krw_available / reserve_margin * utilization` 변경 |
| `tests/test_candle_web.cpp` | `riskPercent = 10.0` → `utilization = 0.1` (빌드 호환) |
| `tests/test_strategy.cpp` | `riskPercent = 50.0` → `utilization = 0.5` (빌드 호환) |

## 8. 비범위

이번 변경에서 제외:

1. 자동 리밸런싱
2. 마켓 간 자금 이동 정책
3. 외부 수동 거래 자동 동기화 정책
4. 테스트 코드 신규 작성 (기존 테스트의 필드명 변경만 수행)

## 9. 알려진 이슈 (Known Issues)

### 9.1 [HIGH] reconcileFromSnapshot 실패 시 reserved_krw 영구 잠김

**관련 코드**: `MarketEngineManager.cpp` runRecovery_ 터미널 분기, `MarketEngine.cpp` clearPendingState

**현상**: `reconcileFromSnapshot()`이 `false`를 반환해도(OrderStore 미등록, BID 토큰 불일치 등) 터미널 분기에서 `clearPendingState()` + `strategy.syncOnStart()`를 무조건 실행한다.

**영향**: `clearPendingState()` 내부에서 `deactivate()` + `reset()`으로 토큰이 파괴되면, 소멸자의 `releaseWithoutToken()` 호출이 방지되어 `reserved_krw`가 `available_krw`로 복구되지 않는다. 해당 마켓이 영구적으로 매수 불가 상태에 빠질 수 있다.

**수정 방향**: reconcile 실패 + 터미널 조합에서는 토큰을 `deactivate`가 아니라 `AccountManager::release()`로 반환하여 `reserved_krw` → `available_krw` 복구를 보장해야 한다.

### 9.2 [MED] UpbitWebSocketClient::stop() 데이터 레이스 (기존 구조)

**관련 코드**: `UpbitWebSocketClient.cpp` stop() / runReadLoop_

**현상**: `stop()` 스레드가 `ws_`를 dereference/cancel 하는 동안, read loop 스레드가 `doReconnect_()` 내부에서 `ws_.reset()`을 호출할 수 있다. `ws_` (unique_ptr) 접근에 동기화가 없어 UB 가능성이 있다.

**비고**: HYBRID v2 이전부터 존재하는 구조적 문제. §4.1의 `expires_after` 추가로 read가 주기적으로 깨어나면서 레이스 윈도우 도달 확률이 미세하게 달라질 수 있으나 근본 원인은 동일하다.

**수정 방향**: `stop()`에서 `ws_` 직접 접근 대신, `request_stop()` + `join()`만으로 종료하고 read loop 내부에서 자체 정리하는 패턴으로 전환. 또는 `std::mutex`로 `ws_` 접근 보호.

---

본 v2 스펙은 멀티마켓 격리 원칙을 유지하면서,
런타임 재분배 부작용 없이 reconnect/부분체결 복구를 안정화하는 구현안이다.
