# Intrabar Cancel PositionEffect 설계안

## 1. 목적

`intrabar` 청산 주문이 `Canceled` 또는 `Rejected`로 끝났을 때,
전략이 주문 상태 이름만 보고 추론하지 않고
"실제 포지션이 어떻게 변했는지"를 기준으로 상태를 확정하도록 구조를 보강한다.

이번 설계의 핵심은 다음과 같다.

- 오버코딩 없이 현재 `engine -> manager -> strategy` 흐름을 유지한다.
- `Canceled`를 `Filled`처럼 취급하지 않는다.
- 대신 엔진이 실제 잔고 기준으로 `PositionEffect`를 계산해 전략에 전달한다.
- 전략은 `status`보다 `position_effect`를 우선 사용해 `PendingEntry` / `PendingExit`를 닫는다.


## 2. 배경 문제

현재 `RsiMeanReversionStrategy::onOrderUpdate()`는 `Canceled` 경로에서
체결 흔적이 있는지만 보고 `PendingExit -> InPosition`으로 복귀한다.

이 방식은 아래 두 케이스를 명확히 구분하지 못한다.

1. 부분 체결 후 취소
2. 실질적으로 전량 체결되었지만 최종 상태가 취소로 들어온 경우

원인은 전략이 주문 상태 이벤트만 보고 판단하기 때문이다.
전략은 실제 남은 코인 수량을 직접 보지 않으며,
현재 주문 결과가 포지션을 줄였는지, 끝냈는지 직접 판정할 정보가 부족하다.


## 3. 설계 목표

이번 수정안은 다음 목표를 만족해야 한다.

- 기존 구조를 최대한 유지할 것
- 수정 범위를 작은 단위로 제한할 것
- 엔진이 이미 알고 있는 실제 잔고 정보를 재사용할 것
- 전략이 상태명(`Filled`, `Canceled`) 해석 책임을 과도하게 가지지 않게 할 것
- `docs/Google_Cpp_Codinguide.md` 기준에 맞게 책임이 명확한 작은 타입을 도입할 것


## 4. 핵심 아이디어

공용 enum `PositionEffect`를 추가한다.

```cpp
enum class PositionEffect {
    Unknown = 0,
    None,
    Opened,
    Reduced,
    Closed,
};
```

의미는 다음과 같다.

- `Unknown`: effect를 확정하지 못함. 기존 로직 fallback 용도
- `None`: 체결이 없어서 포지션 변화가 없음
- `Opened`: 진입 주문이 실제 포지션을 생성함
- `Reduced`: 청산 주문이 포지션을 줄였지만 남은 수량이 있음
- `Closed`: 청산 주문으로 포지션이 사실상 종료됨

핵심 책임 분리는 아래와 같다.

- `MarketEngine`: terminal 주문 처리 후 실제 잔고 기준으로 `PositionEffect` 계산
- `MarketEngineManager`: effect를 전략 이벤트로 그대로 중계
- `RsiMeanReversionStrategy`: `position_effect`를 우선 사용해 상태 확정


## 5. 수정 대상 파일

수정 대상은 아래 파일로 제한한다.

- `src/core/domain/OrderTypes.h`
- `src/engine/EngineEvents.h`
- `src/engine/MarketEngine.h`
- `src/engine/MarketEngine.cpp`
- `src/trading/strategies/StrategyTypes.h`
- `src/app/MarketEngineManager.cpp`
- `src/trading/strategies/RsiMeanReversionStrategy.cpp`

이번 설계에서는 아래 영역은 건드리지 않는다.

- DB 스키마
- AccountManager public API
- WS / REST 파서
- 테스트 코드


## 6. 상세 설계

### 6-1. 공용 enum 추가

위치는 `src/core/domain/OrderTypes.h`로 한다.

이유:

- `engine`, `app`, `strategy` 모두 사용한다.
- 특정 레이어 전용 타입이 아니다.
- 새 헤더를 추가하는 것보다 기존 공용 enum 헤더를 확장하는 편이 단순하다.


### 6-2. 이벤트 타입 확장

`EngineOrderStatusEvent`와 `trading::OrderStatusEvent`에 아래 필드를 추가한다.

```cpp
core::PositionEffect position_effect{ core::PositionEffect::Unknown };
```

생성자에도 기본값이 있는 인자로 추가한다.

설계 의도:

- 기존 호출부를 최대한 유지한다.
- 엔진 구현 전까지는 `Unknown` 기본값으로 회귀를 막는다.


### 6-3. 엔진의 판정 책임

`MarketEngine`에 private helper를 추가한다.

```cpp
core::PositionEffect resolvePositionEffect_(const core::Order& order) const;
```

이 함수는 terminal 상태의 주문이 실제 포지션에 어떤 효과를 냈는지 계산한다.

#### 판정 기준

공통 규칙:

- `order.executed_volume <= 0.0` 이면 `PositionEffect::None`
- `account_mgr_.getBudget(market_)` 조회 실패 시 `PositionEffect::Unknown`

BUY 주문:

- terminal 처리 후 `coin_balance >= coin_epsilon` 이면 `PositionEffect::Opened`
- 그렇지 않으면 `PositionEffect::Unknown`

SELL 주문:

- terminal 처리 후 `coin_balance >= coin_epsilon` 이면 `PositionEffect::Reduced`
- 그렇지 않으면 `PositionEffect::Closed`

중요한 점:

- SELL은 `finalizeSellOrder()` 호출 후의 `coin_balance`를 기준으로 본다.
- 즉, dust 정리까지 반영한 실제 포지션 종료 여부를 effect에 담는다.

**dust 기준 불변식**

`PositionEffect::Closed` 판정은 `finalizeSellOrder()` 내부에서 사용하는
`AccountConfig::coin_epsilon`과 `AccountConfig::init_dust_threshold_krw`에 종속된다.

전략 쪽의 포지션 유효성 판단(`hasMeaningfulPos`)은 `StrategyConfig::dust_exit_threshold_krw`를 사용한다.

현재 두 기본값은 모두 `5,000원`으로 동일하며, 이 동일성을 유지해야 한다.

- `AccountConfig::init_dust_threshold_krw` ≡ `StrategyConfig::dust_exit_threshold_krw`

이 값이 서로 다르면 엔진이 `Closed`로 판정한 포지션을 전략이 여전히 유효 포지션으로 간주하거나,
반대로 엔진이 `Reduced`로 판정했는데 전략이 포지션 없음으로 보는 불일치가 발생한다.


### 6-4. `onOrderSnapshot()` 처리 순서

`MarketEngine::onOrderSnapshot()`의 terminal 처리 순서는 아래처럼 **재배치**한다.

1. terminal 상태 감지
2. BUY면 `finalizeBuyToken_()` 호출
3. SELL이면 `finalizeSellOrder()` 호출
4. `resolvePositionEffect_(o)` 호출
5. `EngineOrderStatusEvent` 발행
6. `store_.erase(o.id)`

**주의: 현재 코드와의 차이**

현재 `onOrderSnapshot()`은 `pushEvent_`를 finalize 이전에 호출한다.

```cpp
// 현재 코드 (변경 전)
if (o.status != old_status) {
    pushEvent_(ev);            // 1. 이벤트 먼저 발행
    finalizeBuyToken_(...);    // 2. 토큰 정리
    finalizeSellOrder(...);    // 3. 잔고 확정
}
store_.erase(o.id);
```

이번 구현에서는 `pushEvent_` 호출을 finalize 이후로 이동해야 한다.

```cpp
// 변경 후
if (o.status != old_status) {
    finalizeBuyToken_(...);    // 1. 토큰 정리
    finalizeSellOrder(...);    // 2. 잔고 확정
    ev.position_effect = resolvePositionEffect_(o); // 3. effect 계산
    pushEvent_(ev);            // 4. 이벤트 발행 (post-finalize 상태 기준)
}
store_.erase(o.id);
```

이유: `resolvePositionEffect_`는 `finalizeSellOrder()` 이후의 `coin_balance`를 읽어야
`Reduced` / `Closed`를 정확히 판별할 수 있다.
`pushEvent_` 순서 변경 자체는 기존 이벤트 처리 로직에 영향을 주지 않는다.

**`reconcileFromSnapshot` 경로에서의 보장**

`reconcileFromSnapshot`은 내부적으로 delta 정산(`finalizeFillBuy/Sell`) 후
`onOrderSnapshot`을 호출한다. `onOrderSnapshot`이 위 순서로 수정되면
reconcile 경로도 자동으로 올바른 post-finalize 상태 기준으로 effect를 계산한다.
별도 수정 불필요.


### 6-5. 매니저의 책임

`MarketEngineManager`는 새 판단 로직을 넣지 않는다.

역할은 단순 중계다.

```cpp
trading::OrderStatusEvent out{
    e.identifier,
    e.status,
    e.position,
    e.executed_volume,
    e.remaining_volume,
    e.executed_funds,
    e.position_effect
};
```

이 레이어에 별도 해석 로직을 넣지 않는 이유는,
판정 책임이 다시 분산되면 구조가 흐려지기 때문이다.


### 6-6. 전략의 상태 확정 규칙

`RsiMeanReversionStrategy::onOrderUpdate()`는 `position_effect`를 우선 사용한다.

#### `PendingEntry`

- `None` -> `Flat`
- `Opened` -> 진입 확정 후 `InPosition`

#### `PendingExit`

- `None` -> 체결 없음. `InPosition` 롤백
- `Reduced` -> 부분 청산 확정 후 `InPosition`
- `Closed` -> 완전 청산 확정 후 `Flat`

#### `Unknown`

`position_effect == Unknown`이면 기존 로직으로 fallback 한다.

fallback 규칙:

- `Filled` -> BUY는 `Opened`, SELL은 `Closed`
- `Canceled/Rejected + 체결 흔적 있음` -> BUY는 `Opened`, SELL은 `Reduced`
- `Canceled/Rejected + 체결 흔적 없음` -> `None`

이 fallback은 엔진 보강 이전/복구 실패 상황에서도 회귀를 막기 위한 안전망이다.


### 6-7. SELL signal 기록 규칙

현재 구현에서 SELL 신호 기록 경로는 두 곳으로 분리되어 있다.

- **완전청산**: `Filled` 경로 (`is_partial=0`, L594)
- **부분청산**: `Canceled + 체결 흔적` 경로 (`is_partial=1`, L520)

이 설계로 `Canceled + PositionEffect::Closed`가 추가되면 완전청산이
`Canceled` 경로에서도 발생할 수 있다. 따라서 다음 규칙을 명시한다.

**신호 기록 규칙**

| position_effect | is_partial | 기록 경로 |
|---|---|---|
| `Closed` (Filled 경유) | 0 | 기존 Filled 경로 (변경 없음) |
| `Closed` (Canceled 경유) | 0 | Canceled 경로에 추가 |
| `Reduced` | 1 | 기존 Canceled 경로 (변경 없음) |

구현 시 `PendingExit + Canceled` 분기에서 `position_effect == Closed`인 경우
`is_partial=0`으로 SELL 신호를 기록해야 한다.
`position_effect == Reduced`인 경우는 기존과 동일하게 `is_partial=1`로 기록한다.

**partial signal volume 폴백 보강**

현재 SELL partial 기록은 `pending_filled_volume_ > 0.0`에만 의존하는 부분이 있다.

- `pending_filled_volume_ > 0.0` 우선 사용
- 없으면 `ev.executed_volume`을 폴백으로 사용

이유: WS fill 유실 후 snapshot/recovery 경로에서도 partial SELL 기록이 누락되지 않게 하기 위함


## 7. 구현 순서

작업 순서는 아래와 같이 가져간다.

1. `OrderTypes.h`에 `PositionEffect` 추가
2. `EngineEvents.h`, `StrategyTypes.h`에 `position_effect` 필드 추가
3. `MarketEngineManager.cpp`에서 중계 연결
4. `RsiMeanReversionStrategy.cpp`에서 `position_effect` 우선 사용 + fallback 유지
5. `MarketEngine.h/.cpp`에 `resolvePositionEffect_` 구현
   + `onOrderSnapshot` 내 `pushEvent_` 호출을 finalize 이후로 재배치 (§6-4 참조)

이 순서를 따르는 이유:

- 중간 단계에서도 컴파일/논리 회귀를 최소화할 수 있다.
- 엔진 구현 전까지 `Unknown` 기본값으로 기존 동작을 유지할 수 있다.


## 8. 기대 효과

이 설계 적용 후 기대되는 효과는 다음과 같다.

- `Canceled + 체결 없음`은 기존과 동일하게 안전하게 롤백
- `Canceled + 부분 체결`은 부분 청산으로 명확히 처리
- `Canceled + 실질 전량 청산`은 다음 캔들을 기다리지 않고 즉시 `Flat` 처리
- 전략이 상태 문자열 해석보다 실제 포지션 결과를 기준으로 움직이게 됨
- `engine -> manager -> strategy` 책임 분리가 더 선명해짐


## 9. 비목표

이번 설계에서 해결 범위에 포함하지 않는 항목은 아래와 같다.

- 주문/체결 DB 모델 전면 개편
- `AccountManager` 인터페이스 재설계
- `Filled` / `Canceled` 외 추가 주문 상태 체계 재정의
- 테스트 인프라 확장
- 문서 외 별도 대규모 리팩터링


## 10. 결론

이번 설계는 `Canceled`를 `Filled`처럼 취급하는 우회책이 아니라,
엔진이 실제 잔고 기준으로 포지션 효과를 계산해 전략에 전달하는 방식이다.

이 접근은 현재 구조를 유지하면서도
`intrabar` 청산의 `cancel-after-trade` 해석 문제를 가장 작은 변경으로 줄일 수 있다.

특히 SELL terminal 처리 후 실제 `coin_balance`를 기준으로
`Reduced` 와 `Closed`를 구분한다는 점이 핵심이다.
