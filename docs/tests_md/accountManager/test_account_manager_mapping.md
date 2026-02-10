# test_account_manager_unified 매핑 문서

## 목적

이 문서는 `tests/test_account_manager_unified.cpp`의 각 테스트가
`src/trading/allocation/AccountManager.cpp`의 어떤 분기/정책을 검증하는지 연결한다.

---

## 1) 핵심 구현 축

`AccountManager.cpp`에서 테스트가 집중 검증하는 축:

1. 생성자 초기 분배 + dust 필터
2. 예약/해제(`reserve`, `release`, RAII)
3. 매수 정산(`finalizeFillBuy`)의 clamp/평균단가
4. 매도 정산(`finalizeFillSell`)의 과매도 보정 + dust 이중 체크
5. 주문 종료(`finalizeOrder`)의 미사용 예약 복구
6. 동기화(`syncWithAccount`)의 리셋 후 재구성
7. 통계 카운터

---

## 2) 테스트별 매핑

## TEST 1 `testInitializationKrwOnly` (`tests/test_account_manager_unified.cpp:37`)

검증:
- KRW-only 초기 자본의 균등 분배

매핑:
- 생성자 1단계 기본 초기화: `src/trading/allocation/AccountManager.cpp:87`
- 코인 없는 마켓 계산/균등 분배: `src/trading/allocation/AccountManager.cpp:143`, `src/trading/allocation/AccountManager.cpp:151`

---

## TEST 2 `testInitializationWithPositions` (`tests/test_account_manager_unified.cpp:72`)

검증:
- 포지션 있는 마켓은 코인 상태, 나머지는 KRW 상태

매핑:
- 포지션 반영: `src/trading/allocation/AccountManager.cpp:106`
- 코인 보유 시 available=0: `src/trading/allocation/AccountManager.cpp:126`, `src/trading/allocation/AccountManager.cpp:129`
- 코인 없는 마켓 KRW 배분: `src/trading/allocation/AccountManager.cpp:153`

---

## TEST 3 `testInitializationDustHandling` (`tests/test_account_manager_unified.cpp:116`)

검증:
- 생성자에서 가치 기준 dust 제거

매핑:
- dust 판단: `src/trading/allocation/AccountManager.cpp:118`
- dust면 코인 0 처리: `src/trading/allocation/AccountManager.cpp:120`

---

## TEST 4 `testReserveRelease` (`tests/test_account_manager_unified.cpp:147`)

검증:
- reserve로 `available -> reserved` 이동
- release로 원복

매핑:
- 예약 이동: `src/trading/allocation/AccountManager.cpp:205`, `src/trading/allocation/AccountManager.cpp:206`
- release 경로: `src/trading/allocation/AccountManager.cpp:215`, `src/trading/allocation/AccountManager.cpp:221`
- 실제 복구 로직: `src/trading/allocation/AccountManager.cpp:236`, `src/trading/allocation/AccountManager.cpp:237`

---

## TEST 5 `testReserveFailures` (`tests/test_account_manager_unified.cpp:192`)

검증:
- 잔액 부족/미등록 마켓 예약 실패
- `reserve_failures` 증가

매핑:
- 미등록 실패: `src/trading/allocation/AccountManager.cpp:184`
- 음수/0 실패: `src/trading/allocation/AccountManager.cpp:190`
- 잔액 부족 실패: `src/trading/allocation/AccountManager.cpp:199`
- 실패 카운터: `src/trading/allocation/AccountManager.cpp:185`, `src/trading/allocation/AccountManager.cpp:192`, `src/trading/allocation/AccountManager.cpp:200`

---

## TEST 6 `testTokenRAII` (`tests/test_account_manager_unified.cpp:219`)

검증:
- 토큰 소멸 시 자동 해제

매핑:
- 소멸자 자동 복구: `src/trading/allocation/AccountManager.cpp:60`, `src/trading/allocation/AccountManager.cpp:63`
- noexcept 해제 경로: `src/trading/allocation/AccountManager.cpp:245`

---

## TEST 7 `testFinalizeFillBuy` (`tests/test_account_manager_unified.cpp:252`)

검증:
- 전량 매수 체결 시 reserved 소진 + coin/avg 업데이트

매핑:
- reserved 차감: `src/trading/allocation/AccountManager.cpp:302`
- 평균단가 갱신: `src/trading/allocation/AccountManager.cpp:309`, `src/trading/allocation/AccountManager.cpp:314`
- 코인 증가: `src/trading/allocation/AccountManager.cpp:318`
- consumed 누적: `src/trading/allocation/AccountManager.cpp:321`

---

## TEST 8 `testPartialFillBuyWithAvgPrice` (`tests/test_account_manager_unified.cpp:288`)

검증:
- 부분 체결 누적과 가중 평균 단가 계산
- finalizeOrder 시 잔여 예약 복구

매핑:
- 누적 매수 정산: `src/trading/allocation/AccountManager.cpp:266`
- finalizeOrder 잔여 복구: `src/trading/allocation/AccountManager.cpp:426`, `src/trading/allocation/AccountManager.cpp:428`

---

## TEST 9 `testFinalizeFillSell` (`tests/test_account_manager_unified.cpp:332`)

검증:
- 전량 매도 시 coin=0, avg=0, realized_pnl 계산

매핑:
- 매도 반영: `src/trading/allocation/AccountManager.cpp:350`, `src/trading/allocation/AccountManager.cpp:376`
- dust 정리 후 avg=0: `src/trading/allocation/AccountManager.cpp:399`, `src/trading/allocation/AccountManager.cpp:401`
- realized_pnl 계산: `src/trading/allocation/AccountManager.cpp:404`

---

## TEST 10 `testPartialFillSell` (`tests/test_account_manager_unified.cpp:381`)

검증:
- 부분 매도 중간 상태 유지, 최종 매도 후 pnl 반영

매핑:
- 부분 매도 누적: `src/trading/allocation/AccountManager.cpp:376`
- dust 기준에 따라 최종 청산: `src/trading/allocation/AccountManager.cpp:385`, `src/trading/allocation/AccountManager.cpp:393`, `src/trading/allocation/AccountManager.cpp:399`

---

## TEST 11 `testSyncWithAccountStateModel` (`tests/test_account_manager_unified.cpp:425`)

검증:
- sync 후 코인 보유 마켓 KRW=0
- flat 마켓만 KRW 보유(XOR 모델)

매핑:
- 전체 코인 리셋: `src/trading/allocation/AccountManager.cpp:454`
- 코인 보유 반영 + KRW 0 강제: `src/trading/allocation/AccountManager.cpp:483`, `src/trading/allocation/AccountManager.cpp:487`
- 코인 없는 마켓 KRW 분배: `src/trading/allocation/AccountManager.cpp:494`, `src/trading/allocation/AccountManager.cpp:509`

---

## TEST 12 `testSyncWithMultiplePositions` (`tests/test_account_manager_unified.cpp:480`)

검증:
- 다중 코인 포지션 반영 + 나머지 마켓 KRW 배분

매핑:
- positions 순회 반영: `src/trading/allocation/AccountManager.cpp:464`
- flat 마켓 식별/배분: `src/trading/allocation/AccountManager.cpp:497`, `src/trading/allocation/AccountManager.cpp:511`

---

## TEST 13 `testEquityAndROI` (`tests/test_account_manager_unified.cpp:536`)

검증:
- `MarketBudget::getCurrentEquity`, `getROI`, `getRealizedROI`
- 매수/매도 정산과 계산 함수 결합 동작

매핑:
- 계산 함수 선언: `src/trading/allocation/AccountManager.h:52`, `src/trading/allocation/AccountManager.h:57`, `src/trading/allocation/AccountManager.h:63`
- realized_pnl 갱신 지점: `src/trading/allocation/AccountManager.cpp:404`

---

## TEST 14 `testSyncWithAccountDustHandling` (`tests/test_account_manager_unified.cpp:585`)

검증:
- syncWithAccount에서도 생성자와 같은 가치 기준 dust 적용

매핑:
- sync dust 판단: `src/trading/allocation/AccountManager.cpp:475`
- dust면 코인 0 처리: `src/trading/allocation/AccountManager.cpp:476`, `src/trading/allocation/AccountManager.cpp:478`

---

## TEST 15 `testFinalizeFillSellLowPriceCoin` (`tests/test_account_manager_unified.cpp:632`)

검증:
- 저가 코인 소액 잔량을 가치 기준 dust로 제거

매핑:
- 가치 기준 dust: `src/trading/allocation/AccountManager.cpp:393`, `src/trading/allocation/AccountManager.cpp:394`
- dust 제거: `src/trading/allocation/AccountManager.cpp:399`

---

## TEST 16 `testFinalizeFillSellHighPriceCoin` (`tests/test_account_manager_unified.cpp:682`)

검증:
- 고가 코인 유의미 잔량은 유지

매핑:
- dust 조건 불충족 시 잔량 유지(else 경로): `src/trading/allocation/AccountManager.cpp:392` ~ `src/trading/allocation/AccountManager.cpp:397`

---

## TEST 17 `testReserveInputValidation` (`tests/test_account_manager_unified.cpp:723`)

검증:
- reserve 0/음수 거부

매핑:
- 입력 검증: `src/trading/allocation/AccountManager.cpp:190`

---

## TEST 18 `testFinalizeFillBuyInputValidation` (`tests/test_account_manager_unified.cpp:754`)

검증:
- 0/음수 입력 무시
- 예약 초과 체결 clamp

매핑:
- 입력 검증 무시: `src/trading/allocation/AccountManager.cpp:275`
- 초과 clamp: `src/trading/allocation/AccountManager.cpp:282`, `src/trading/allocation/AccountManager.cpp:286`

---

## TEST 19 `testFinalizeFillSellOversell` (`tests/test_account_manager_unified.cpp:813`)

검증:
- 과매도 시 coin 0 clamp
- KRW를 실제 보유분 비율로만 반영

매핑:
- 과매도 감지: `src/trading/allocation/AccountManager.cpp:355`
- 실제 반영 KRW 계산: `src/trading/allocation/AccountManager.cpp:366`
- coin 0 강제: `src/trading/allocation/AccountManager.cpp:372`

참고:
- 테스트 출력 문자열은 `[TEST 18]`로 오타가 있으나 함수/실행 순서는 TEST 19다.

---

## TEST 20 `testFinalizeFillSellInputValidation` (`tests/test_account_manager_unified.cpp:854`)

검증:
- sold_coin/received_krw 0/음수 입력 무시

매핑:
- 입력 검증 무시: `src/trading/allocation/AccountManager.cpp:330`

---

## TEST 21 `testSyncWithAccountPositionDisappears` (`tests/test_account_manager_unified.cpp:907`)

검증:
- account.positions에서 사라진 포지션이 sync 후 0으로 복구

매핑:
- sync 1단계 전체 리셋: `src/trading/allocation/AccountManager.cpp:454`, `src/trading/allocation/AccountManager.cpp:457`
- 이후 positions 재구성: `src/trading/allocation/AccountManager.cpp:464`

---

## TEST 22 `testThreadSafety` (`tests/test_account_manager_unified.cpp:959`)

검증:
- 멀티스레드 reserve/release 반복에도 상태 훼손 없음

매핑:
- reserve unique_lock: `src/trading/allocation/AccountManager.cpp:181`
- releaseWithoutToken unique_lock: `src/trading/allocation/AccountManager.cpp:255`
- 조회 shared_lock: `src/trading/allocation/AccountManager.cpp:164`

---

## TEST 23 `testStatistics` (`tests/test_account_manager_unified.cpp:1023`)

검증:
- stats 카운터 증가 경로

매핑:
- total_reserves: `src/trading/allocation/AccountManager.cpp:209`
- total_releases: `src/trading/allocation/AccountManager.cpp:257`
- total_fills_buy: `src/trading/allocation/AccountManager.cpp:323`
- total_fills_sell: `src/trading/allocation/AccountManager.cpp:407`
- reserve_failures: `src/trading/allocation/AccountManager.cpp:185`, `src/trading/allocation/AccountManager.cpp:192`, `src/trading/allocation/AccountManager.cpp:200`

---

## 3) 빠른 역추적 가이드

1. 테스트 실패 번호 확인 (`tests/test_account_manager_unified.cpp`)
2. 이 문서에서 대응 구현 라인 확인
3. 구현 분기가 "정책 의도"인지 "버그"인지 분리
   - 정책: dust 기준, 과매도 보정, sync 재분배
   - 버그 가능: 카운터 증가 시점, clamp 누락, lock 누락

