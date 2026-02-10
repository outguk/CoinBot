# test_event_router.cpp 매핑 문서

## 목적

이 문서는 `tests/test_event_router.cpp`의 각 테스트가
`src/app/EventRouter.cpp`의 어느 구현 분기와 정책을 검증하는지 연결해서 보여준다.

---

## 1) 전체 흐름 기준 매핑

`EventRouter.cpp` 핵심 검증 축:

1. 마켓 추출
   - fast path: `extractMarketFast_` (`src/app/EventRouter.cpp:57`)
   - slow path: `extractMarketSlow_` (`src/app/EventRouter.cpp:80`)
2. 충돌 처리
   - `code != market` 즉시 실패 (`src/app/EventRouter.cpp:119`, `src/app/EventRouter.cpp:166`)
3. 미등록 마켓 처리
   - `routes_.find(...) == end` (`src/app/EventRouter.cpp:139`, `src/app/EventRouter.cpp:186`)
4. 백프레셔 정책 분리
   - marketData만 드롭 허용 (`src/app/EventRouter.cpp:149`)
   - myOrder는 항상 push (`src/app/EventRouter.cpp:194`)
5. 통계 카운터
   - 증가 지점이 분기별로 다름

---

## 2) 테스트별 상세 매핑

## TEST 1 `testFastPath_CodeKey` (`tests/test_event_router.cpp:43`)

검증 대상:
- `code` 키 fast 추출 성공
- `MarketDataRaw` 타입 push

매핑:
- fast 추출: `src/app/EventRouter.cpp:59`, `src/app/EventRouter.cpp:125`
- fast 카운터: `src/app/EventRouter.cpp:145`
- marketData push: `src/app/EventRouter.cpp:154`

---

## TEST 2 `testFastPath_MarketKey` (`tests/test_event_router.cpp:69`)

검증 대상:
- `market` 키만 있어도 fast 추출 성공

매핑:
- fast 분기: `src/app/EventRouter.cpp:74`, `src/app/EventRouter.cpp:125`
- push/카운터: `src/app/EventRouter.cpp:145`, `src/app/EventRouter.cpp:154`, `src/app/EventRouter.cpp:155`

---

## TEST 3 `testFastPath_BothKeysMatch` (`tests/test_event_router.cpp:89`)

검증 대상:
- `code`와 `market`이 같으면 허용
- conflict 미증가

매핑:
- 일치 허용: `src/app/EventRouter.cpp:63`, `src/app/EventRouter.cpp:65`
- conflict 증가 지점(미실행 확인): `src/app/EventRouter.cpp:121`

---

## TEST 4 `testFallback_UnicodeEscape` (`tests/test_event_router.cpp:113`)

검증 대상:
- fast가 escape 발견 시 포기
- slow 파서로 복구 성공

매핑:
- fast 포기 조건: `src/app/EventRouter.cpp:43`
- fallback 진입: `src/app/EventRouter.cpp:129`
- fallback 카운터: `src/app/EventRouter.cpp:146`
- push: `src/app/EventRouter.cpp:154`

---

## TEST 5 `testUnknownMarket` (`tests/test_event_router.cpp:140`)

검증 대상:
- 미등록 마켓 false
- `unknown_market` 증가

매핑:
- 미등록 체크: `src/app/EventRouter.cpp:139`
- unknown 카운터: `src/app/EventRouter.cpp:141`

---

## TEST 6 `testConflictDetected` (`tests/test_event_router.cpp:159`)

검증 대상:
- `code/market` 불일치 시 즉시 실패
- `conflict_detected` 증가, `parse_failures` 미증가

매핑:
- fast conflict 감지: `src/app/EventRouter.cpp:67`, `src/app/EventRouter.cpp:70`
- route에서 즉시 실패: `src/app/EventRouter.cpp:119`, `src/app/EventRouter.cpp:121`

---

## TEST 7 `testParseFailure_InvalidJson` (`tests/test_event_router.cpp:182`)

검증 대상:
- fast/slow 모두 실패 시 parse_failures 증가

매핑:
- fast 미추출 후 slow 시도: `src/app/EventRouter.cpp:129`
- parse 실패: `src/app/EventRouter.cpp:134`

---

## TEST 8 `testParseFailure_NoMarketKey` (`tests/test_event_router.cpp:202`)

검증 대상:
- 유효 JSON이어도 code/market 없으면 실패

매핑:
- slow에서 키 없음 반환: `src/app/EventRouter.cpp:102`, `src/app/EventRouter.cpp:103`, `src/app/EventRouter.cpp:105`
- parse_failures 증가: `src/app/EventRouter.cpp:134`

---

## TEST 9 `testBackpressure_MarketDataDropped` (`tests/test_event_router.cpp:227`)

검증 대상:
- marketData는 큐 포화 시 드롭
- 반환 true, `route_queue_full` 증가, `total_routed` 미증가

매핑:
- 백프레셔 체크: `src/app/EventRouter.cpp:149`
- 드롭 카운터: `src/app/EventRouter.cpp:150`
- true 반환(드롭): `src/app/EventRouter.cpp:151`

---

## TEST 10 `testNoBackpressure_MyOrderAlwaysPushed` (`tests/test_event_router.cpp:254`)

검증 대상:
- myOrder는 포화와 무관하게 push
- `MyOrderRaw` 타입 확인

매핑:
- myOrder push: `src/app/EventRouter.cpp:194`
- total_routed 증가: `src/app/EventRouter.cpp:195`

---

## TEST 11 `testMultiMarket_CorrectRouting` (`tests/test_event_router.cpp:291`)

검증 대상:
- 마켓별 큐 격리(오배달 없음)
- `MarketDataRaw` 타입 유지

매핑:
- 큐 선택 분기: `src/app/EventRouter.cpp:139`
- marketData push: `src/app/EventRouter.cpp:154`

---

## TEST 12 `testMultiMarket_BackpressureIsolation` (`tests/test_event_router.cpp:323`)

검증 대상:
- 한 마켓 큐 포화가 다른 마켓 라우팅에 영향 없음

매핑:
- 큐별 독립 size 체크: `src/app/EventRouter.cpp:149`
- 포화 마켓 드롭 + 다른 마켓 push 공존:
  - 드롭: `src/app/EventRouter.cpp:150`
  - 정상 push: `src/app/EventRouter.cpp:154`

---

## TEST 13 `testStats_MixedScenario` (`tests/test_event_router.cpp:353`)

검증 대상:
- 복합 시나리오에서 통계 카운터 조합 검증

매핑:
- fast 카운터: `src/app/EventRouter.cpp:145`
- fallback 카운터: `src/app/EventRouter.cpp:146`
- unknown 카운터: `src/app/EventRouter.cpp:141`
- conflict 카운터: `src/app/EventRouter.cpp:121`
- parse_failures 카운터: `src/app/EventRouter.cpp:134`
- total_routed 카운터: `src/app/EventRouter.cpp:155`

주의:
- 현재 구현에서 `routeMarketData`는 unknown_market이면 fast/fallback 카운터를 올리지 않는다.
  - 카운터 증가가 unknown 체크 뒤에 있기 때문 (`src/app/EventRouter.cpp:145`, `src/app/EventRouter.cpp:146`)

---

## TEST 14 `testMyOrder_NormalRouting` (`tests/test_event_router.cpp:395`)

검증 대상:
- myOrder 정상 라우팅 시 `MyOrderRaw` push

매핑:
- fast 추출: `src/app/EventRouter.cpp:173`
- myOrder push: `src/app/EventRouter.cpp:194`

---

## TEST 15 `testMyOrder_ParseFailure` (`tests/test_event_router.cpp:418`)

검증 대상:
- myOrder 파싱 실패 시 false + parse_failures 증가

매핑:
- parse_failures 증가: `src/app/EventRouter.cpp:180`

---

## TEST 16 `testMultiMarket_MyOrderIsolation` (`tests/test_event_router.cpp:439`)

검증 대상:
- myOrder의 멀티마켓 큐 격리
- 타입 `MyOrderRaw` 유지

매핑:
- 큐 선택 분기: `src/app/EventRouter.cpp:186`
- myOrder push: `src/app/EventRouter.cpp:194`

---

## 3) 빠른 추적 가이드

문제 발생 시 아래 순서로 역추적하면 빠르다.

1. 테스트 실패 위치 확인 (`tests/test_event_router.cpp`)
2. 대응 카운터/분기를 이 문서에서 찾기
3. 실제 증가/분기 라인 확인 (`src/app/EventRouter.cpp`)
4. 정책 문제인지(설계) 기대값 문제인지(테스트) 분리

