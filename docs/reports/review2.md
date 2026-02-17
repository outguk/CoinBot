# review2 - `insufficient_funds_ask` 원인 분석 및 대응안

작성일: 2026-02-18

## 1) 문제 현상

- 업비트 매도 주문에서 반복적으로 아래 오류가 발생함.
  - `insufficient_funds_ask` (`주문 가능한 금액(BTC/ETH)이 부족합니다.`)
- 내부 상태상 `coin_available`가 남아 있다고 판단해 매도를 시도하지만, 거래소 실제 가용 수량은 부족한 상태가 반복됨.

## 2) 근본 원인 요약

### 원인 A. WS `myOrder`가 `state="done"`만 오는 케이스 누락

- 현재 매퍼는 `state=="trade" && trade_uuid 존재`일 때만 `MyTrade`를 생성함.
- `state="done"`만 오면 `MyTrade`가 생성되지 않음.
- 자산 정산(`finalizeFillBuy`/`finalizeFillSell`)은 `onMyTrade()` 경로에서만 수행되므로, 체결 정산이 누락될 수 있음.

### 원인 B. 복구(`reconcile`) 경로에서 `delta_volume>0, delta_funds=0` 처리 결함

- 복구 로그에서 `delta_vol > 0`인데 `delta_funds = 0`이 관측됨.
- 현재 ASK 정산은 `received_krw > 0`일 때만 실행되어, 이 경우 코인 차감이 수행되지 않음.
- 결과적으로 내부 코인 잔고가 실제보다 크게 남고, 이후 동일 수량 매도 재시도 시 거래소에서 `insufficient_funds_ask`가 발생함.

## 3) 이번 장애의 성격

- 단일 원인이 아니라, 아래가 결합된 복합 장애로 판단됨.
  - `done-only` 이벤트 정산 누락 가능성
  - 복구 경로 `delta_funds=0` 시 코인 미차감
- 특히 실제 로그에서는 복구 직후 `delta_vol>0, delta_funds=0`이 찍히고, 이후 부족 오류가 반복되어 원인 B의 영향이 명확함.

## 4) 가장 간단하고 적절한 해결 방안 (최소 변경)

### 4-1. `done-only`도 정산 경로로 포함

- `handleMyOrder_`에서 `MyTrade` 이벤트가 하나도 생성되지 않은 경우,
  `Order snapshot`을 `onOrderSnapshot()`만 호출하지 말고 `reconcileFromSnapshot()`으로 처리.
- 기존 `trade` 이벤트가 있는 경우에는 현재 순서(`onMyTrade -> onOrderSnapshot`) 유지.

### 4-2. ASK 복구 정산의 핵심 정책 변경

- `reconcileFromSnapshot()`에서 ASK 처리 시:
  - `delta_volume > 0`이면 `delta_funds`가 0이어도 **코인 수량 차감은 반드시 수행**.
  - KRW는 0으로 반영하고 경고 로그 남김.
- 원칙: KRW 과소반영보다, 코인 과대보유(오버셀 유발)를 우선 차단.

### 4-3. `finalizeFillSell()` 입력 허용 범위 조정

- `received_krw == 0`을 허용하도록 검증 완화.
- `sold_coin <= 0` 또는 `received_krw < 0`만 거부.

### 4-4. `/v1/order` 파싱 fallback 보강

- `executed_funds`가 누락되면 `trades[].funds` 합으로 보완.
- 보완 불가 시에만 0으로 처리하고 진단 로그 강화.

### 4-5. BID 복구도 코인 반영 (ASK 4-2와 대칭)

- BID에서 `delta_volume>0`이면 코인이 들어온 것이 확정이므로 반드시 `coin_balance`에 반영.
- 미반영 시 이중 매수 유발 가능 (내부: "KRW 있고 코인 없음" / 실제: "코인 있고 KRW 없음").
- 가격 추정 fallback 체인:
  1. `delta_funds > 0`: 정확한 단가 (`delta_funds / delta_volume`), KRW = `delta_funds + delta_paid_fee`
  2. `delta_funds == 0 && 지정가 && snapshot.price 존재`: limit price를 단가로 추정, KRW = `price * volume + delta_paid_fee` + 경고 로그
  3. 둘 다 불가 (시장가이거나 price 없음): 에러 로그, 코인 미반영 상태로 터미널 진행 (rebuildFromAccount에서 교정)
- 시장가의 `price`는 총 KRW 금액이므로 단가로 사용 불가 → 지정가만 fallback 허용
- 단가 오차는 다음 `rebuildFromAccount`에서 교정 가능하므로 코인 미반영보다 안전.

## 5) 기대 효과

- `state="done"` 단독 수신 케이스에서도 체결 반영 누락 방지.
- 복구 시 `delta_funds` 누락/지연 상황에서도 코인 과대보유가 제거되어 `insufficient_funds_ask` 반복 차단.
- 기존 구조를 크게 바꾸지 않고, 핵심 실패 경로만 보강 가능.

## 6) 구현 내역 (2026-02-18 적용)

### 4-1. `handleMyOrder_` done-only reconcile 경로 추가

**파일**: `src/app/MarketEngineManager.cpp` — `handleMyOrder_`

- events에서 MyTrade 존재 여부를 사전 확인 (`has_trade` 플래그)
- MyTrade가 없고 Order가 터미널 상태(Filled/Canceled/Rejected)인 경우,
  `onOrderSnapshot()` 대신 `reconcileFromSnapshot()`으로 처리
- `reconcileFromSnapshot`이 내부에서 delta 정산 + `onOrderSnapshot`을 모두 수행하므로 이중 호출 없음
- reconcile 실패 시(store 미등록 등) `onOrderSnapshot`으로 fallback하여 snapshot 유실 방지
- MyTrade가 있는 정상 경로(`trade` → `done`)는 기존 동작 그대로 유지

### 4-2. ASK reconcile에서 `delta_funds=0`이어도 코인 차감

**파일**: `src/engine/MarketEngine.cpp` — `reconcileFromSnapshot()` ASK 분기

- `if (received_krw > 0.0)` 조건 제거 → `delta_volume > 0`이면 무조건 `finalizeFillSell` 호출
- `received_krw == 0`일 때 경고 로그 출력 (진단용)

### 4-3. `finalizeFillSell()` 입력 검증 완화

**파일**: `src/trading/allocation/AccountManager.cpp` — `finalizeFillSell()`

- `sold_coin <= 0 || received_krw <= 0` → `sold_coin <= 0 || received_krw < 0` 변경
- `received_krw == 0` 허용: 코인 차감만 단독 수행 가능 (4-2의 전제조건)

### 4-5. BID reconcile에서 `delta_funds=0` 시 지정가 price fallback

**파일**: `src/engine/MarketEngine.cpp` — `reconcileFromSnapshot()` BID 분기

- `delta_funds > 0`: 기존 로직 (정확한 단가 계산, KRW = `delta_funds + delta_paid_fee`)
- `delta_funds == 0 && 지정가 && snapshot.price > 0`: limit price를 fallback 단가로 `finalizeFillBuy` 호출 + 경고 로그, KRW = `price * volume + delta_paid_fee`
- 시장가이거나 price 없음: 에러 로그 출력, 코인 미반영 (최후의 방어선, rebuildFromAccount에서 교정)
- 시장가의 `price`는 총 KRW 금액(per-unit 아님)이므로 단가로 사용 불가 → 지정가만 fallback 허용

### 기존 흐름 영향 없음 확인

| 시나리오 | 변경 전 | 변경 후 |
|---------|--------|--------|
| `trade` → `done` (정상) | `onMyTrade` + `onOrderSnapshot` | 동일 (has_trade=true) |
| `done` 단독 (빠른 체결) | `onOrderSnapshot`만 → 정산 누락 | `reconcileFromSnapshot` → delta 정산 |
| `cancel` (미체결 취소) | `onOrderSnapshot` → 토큰 정리 | delta_volume=0 → finalize 미호출 → 동일 |
| `wait`/`watch` (비터미널) | `onOrderSnapshot` | isTerminal=false → 동일 |
| reconcile BID, delta_funds>0 | `finalizeFillBuy` 호출 | 동일 |
| reconcile BID, delta_funds=0 | `finalizeFillBuy` 미호출 → 코인 미반영 | snapshot.price fallback → 코인 반영 |
| reconcile ASK, delta_funds>0 | `finalizeFillSell` 호출 | 동일 |
| reconcile ASK, delta_funds=0 | `finalizeFillSell` 미호출 → 코인 잔류 | 코인 차감 수행 + 경고 로그 |

## 7) 검증 포인트

- 복구 로그에서 `delta_vol>0, delta_funds=0` 발생 시에도 이후 `coin_available`가 감소하는지 확인.
- 동일 마켓에서 매도 성공 후 곧바로 같은 수량 매도 재시도가 사라지는지 확인.
- `done-only` 메시지 수신 시 내부 자산 상태가 거래소 상태와 일치하는지 확인.
