# test_market_engine 매핑 문서

## 목적

이 문서는 `tests/test_market_engine.cpp`의 각 테스트가
`src/engine/MarketEngine.cpp`의 어떤 로직을 검증하는지 매핑한다.

---

## 1) 핵심 분기 축

매핑의 중심이 되는 구현 축:

1. `submit` 사전 검증/마켓검증/중복방지/reserve/API 실패 롤백
2. `onMyTrade` 중복 체결 방지 + fill 정산
3. `onOrderStatus` 터미널 처리 + 토큰/활성 ID 정리
4. `onOrderSnapshot` 스냅샷 병합 + 상태 이벤트 발행
5. `computeReserveAmount`의 reserve_margin 반영
6. 이벤트 큐(`pushEvent_`, `pollEvents`)

---

## 2) 테스트별 매핑

## TEST 1 `testConstruction` (`tests/test_market_engine.cpp:68`)

검증:
- 엔진 생성 시 market 값 보존

매핑:
- ctor 초기화: `src/engine/MarketEngine.cpp:17`
- `market()` 접근자: `src/engine/MarketEngine.h:64`

---

## TEST 2 `testSubmitBuySuccess` (`tests/test_market_engine.cpp:88`)

검증:
- BUY submit 성공
- reserve 수행
- API 호출

매핑:
- BUY 경로 분기: `src/engine/MarketEngine.cpp:63`
- reserve 호출: `src/engine/MarketEngine.cpp:76`
- API postOrder: `src/engine/MarketEngine.cpp:98`
- store upsert: `src/engine/MarketEngine.cpp:155`

---

## TEST 3 `testSubmitSellSuccess` (`tests/test_market_engine.cpp:123`)

검증:
- SELL submit 성공

매핑:
- ASK 경로 분기: `src/engine/MarketEngine.cpp:84`
- active sell id 설정: `src/engine/MarketEngine.cpp:133`

---

## TEST 4 `testDuplicateBuyRejection` (`tests/test_market_engine.cpp:154`)

검증:
- 활성 BUY 존재 시 추가 BUY 거부

매핑:
- 중복 BUY 거부: `src/engine/MarketEngine.cpp:66`

---

## TEST 5 `testDuplicateSellRejection` (`tests/test_market_engine.cpp:184`)

검증:
- 활성 SELL 존재 시 추가 SELL 거부

매핑:
- 중복 SELL 거부: `src/engine/MarketEngine.cpp:87`

---

## TEST 6 `testOppositeSellBlocksBuy` (`tests/test_market_engine.cpp:217`)

검증:
- SELL 활성 시 BUY 차단

매핑:
- 반대 포지션 차단(BUY 시): `src/engine/MarketEngine.cpp:71`

---

## TEST 7 `testOppositeBuyBlocksSell` (`tests/test_market_engine.cpp:251`)

검증:
- BUY 활성 시 SELL 차단

매핑:
- 반대 포지션 차단(SELL 시): `src/engine/MarketEngine.cpp:92`

---

## TEST 8 `testRejectWrongMarket` (`tests/test_market_engine.cpp:288`)

검증:
- 엔진 마켓 불일치 주문 거부
- API 호출 전 차단

매핑:
- 마켓 검증: `src/engine/MarketEngine.cpp:58`

---

## TEST 9 `testInsufficientBalance` (`tests/test_market_engine.cpp:319`)

검증:
- reserve 실패 시 `InsufficientFunds`

매핑:
- reserve 실패 분기: `src/engine/MarketEngine.cpp:77`

---

## TEST 10 `testPostOrderFailure_ShouldReleaseReservation` (`tests/test_market_engine.cpp:348`)

검증:
- postOrder 실패 시 BUY 예약 해제

매핑:
- API 실패 분기: `src/engine/MarketEngine.cpp:100`
- 예약 롤백: `src/engine/MarketEngine.cpp:105`

---

## TEST 11 `testOnMyTradeDuplicatePrevention` (`tests/test_market_engine.cpp:384`)

검증:
- 같은 trade 중복 수신 1회만 처리

매핑:
- dedupe key: `src/engine/MarketEngine.cpp:170`
- once 체크: `src/engine/MarketEngine.cpp:171`
- set/fifo dedupe 구현: `src/engine/MarketEngine.cpp:544`

---

## TEST 12 `testOnMyTradeBuyFill` (`tests/test_market_engine.cpp:428`)

검증:
- BUY 체결 시 `finalizeFillBuy` 반영

매핑:
- BUY fill 분기: `src/engine/MarketEngine.cpp:204`
- 토큰+order_id 일치 조건: `src/engine/MarketEngine.cpp:208`
- finalizeFillBuy 호출: `src/engine/MarketEngine.cpp:211`

---

## TEST 13 `testOnMyTradeSellFill` (`tests/test_market_engine.cpp:472`)

검증:
- SELL 체결 시 `finalizeFillSell` 반영
- 수수료 차감 반영

매핑:
- SELL fill 분기: `src/engine/MarketEngine.cpp:223`
- received_krw 계산: `src/engine/MarketEngine.cpp:226`
- finalizeFillSell 호출: `src/engine/MarketEngine.cpp:227`

---

## TEST 14 `testOnOrderStatusTerminalClearsBuyToken` (`tests/test_market_engine.cpp:520`)

검증:
- BUY 주문 터미널 상태에서 buy token 정리

매핑:
- 터미널 판정: `src/engine/MarketEngine.cpp:259`
- BID active 주문이면 token finalize: `src/engine/MarketEngine.cpp:266`

---

## TEST 15 `testOnOrderStatusTerminalClearsSellId` (`tests/test_market_engine.cpp:553`)

검증:
- SELL 주문 터미널 상태에서 active sell id 정리

매핑:
- ASK active 주문이면 id clear: `src/engine/MarketEngine.cpp:270`

---

## TEST 16 `testOnOrderStatusMarketIsolation` (`tests/test_market_engine.cpp:590`)

검증:
- OrderStore 공유 환경에서 다른 마켓 주문 상태 갱신 무시

매핑:
- 마켓 격리 분기: `src/engine/MarketEngine.cpp:242`

---

## TEST 17 `testReserveAmountWithMargin_AmountSize` (`tests/test_market_engine.cpp:640`)

검증:
- AmountSize BUY에서 reserve_margin 적용

매핑:
- `computeReserveAmount` Amount 경로: `src/engine/MarketEngine.cpp:471`

---

## TEST 18 `testReserveAmountWithMargin_VolumeSize` (`tests/test_market_engine.cpp:676`)

검증:
- Volume+price BUY에서 reserve_margin 적용

매핑:
- `computeReserveAmount` Volume 경로: `src/engine/MarketEngine.cpp:477`

---

## TEST 19 `testSubmitDoesNotGenerateImmediateEvent` (`tests/test_market_engine.cpp:722`)

검증:
- submit 자체로는 이벤트 생성 안 됨

매핑:
- submit 경로엔 `pushEvent_` 호출 없음 (`src/engine/MarketEngine.cpp:48` ~ `src/engine/MarketEngine.cpp:158`)
- 이벤트 배출은 `onMyTrade`/`onOrderSnapshot`에서만 수행

---

## TEST 20 `testOnMyTradeGeneratesFillEvent` (`tests/test_market_engine.cpp:751`)

검증:
- onMyTrade 시 identifier 기반 `EngineFillEvent` 생성

매핑:
- fill event 생성 분기: `src/engine/MarketEngine.cpp:191`
- 이벤트 push: `src/engine/MarketEngine.cpp:200`

---

## TEST 21 `testOnOrderSnapshotGeneratesStatusEvent` (`tests/test_market_engine.cpp:798`)

검증:
- snapshot 터미널 전환 시 `EngineOrderStatusEvent` 생성

매핑:
- snapshot 터미널 분기: `src/engine/MarketEngine.cpp:339`
- status event 생성: `src/engine/MarketEngine.cpp:343`
- 이벤트 push: `src/engine/MarketEngine.cpp:350`

---

## TEST 22 `testOnOrderSnapshotIgnoresWrongMarket` (`tests/test_market_engine.cpp:842`)

검증:
- 다른 마켓 snapshot 무시

매핑:
- snapshot 마켓 필터: `src/engine/MarketEngine.cpp:292`

---

## TEST 23 `testBuyFilledStatusRestoredReservedKrw` (`tests/test_market_engine.cpp:881`)

검증:
- BUY Filled에서 finalizeOrder 경유 예약 KRW 해제

매핑:
- onOrderStatus 터미널 BID -> finalizeBuyToken_: `src/engine/MarketEngine.cpp:266`
- finalizeBuyToken_ 구현: `src/engine/MarketEngine.cpp:484`
- AccountManager finalizeOrder 호출: `src/engine/MarketEngine.cpp:499`

---

## TEST 24 `testBuyCanceledStatusRestoredReservedKrw` (`tests/test_market_engine.cpp:924`)

검증:
- BUY Canceled에서도 동일하게 예약 KRW 해제

매핑:
- 터미널 상태 판정(Canceled 포함): `src/engine/MarketEngine.cpp:260`
- 이후 정리 경로는 TEST 23과 동일

---

## 3) 보조 검증 포인트

여러 테스트가 간접적으로 검증하는 공통 로직:

- 오너 스레드 강제: `src/engine/MarketEngine.cpp:50`, `src/engine/MarketEngine.cpp:163`, `src/engine/MarketEngine.cpp:234`, `src/engine/MarketEngine.cpp:289`
- 외부 주문 체결 무시: `src/engine/MarketEngine.cpp:176`
- 주기적 cleanup: `src/engine/MarketEngine.cpp:274`

---

## 4) 빠른 역추적 가이드

1. 실패한 테스트 번호 확인 (`tests/test_market_engine.cpp`)
2. 이 문서에서 대응 구현 라인 확인
3. 실패가 정책 차이인지(의도) 구현 결함인지(버그) 분리
   - 정책 예: 반대 포지션 차단, 마켓 격리, 이벤트 생성 시점
   - 버그 후보: 토큰 정리 누락, dedupe 누락, reserve 롤백 누락

