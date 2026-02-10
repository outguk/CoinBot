# test_shared_order_api 매핑 문서

## 목적

이 문서는 `SharedOrderApi` 관련 테스트가
`src/api/upbit/SharedOrderApi.cpp`의 어떤 동작을 검증하는지 연결해 보여준다.

---

## 1) 테스트 소스 현황

현재 저장소 기준:

- 실제 테스트 구현 파일:
  - `tests/test_shared_order_api_advanced.cpp`
- CMake 타겟:
  - `test_shared_order_api` (별도 테스트 소스 미포함, 현재 실질 검증 없음)
  - `test_shared_order_api_advanced` (실제 검증 수행)

즉, 매핑은 `test_shared_order_api_advanced.cpp` 기준으로 작성한다.

---

## 2) SharedOrderApi.cpp 핵심 검증 포인트

테스트가 겨냥하는 구현 포인트:

1. 모든 public 메서드 mutex 직렬화
   - `std::lock_guard<std::mutex> lock(mtx_);`
2. lock 내부 실행 동시성 계측
   - `InFlightGuard g(in_flight_, max_in_flight_);`
3. 내부 클라이언트 위임
   - `return client_->...`
4. 생성자 null 방어
   - `if (!client_) throw std::invalid_argument(...)`

---

## 3) 테스트별 상세 매핑

## TEST A `testSerializationProof` (`tests/test_shared_order_api_advanced.cpp:37`)

검증 의도:
- 여러 스레드가 동시에 `getMyAccount()` 호출해도
  내부 lock 기준 동시 실행 수가 1을 넘지 않는지 확인

테스트 핵심 코드:
- 계측 리셋: `api->debugResetInFlight()` (`tests/test_shared_order_api_advanced.cpp:40`)
- 다중 호출: `api->getMyAccount()` (`tests/test_shared_order_api_advanced.cpp:54`)
- 결과 확인: `api->debugMaxInFlight()` (`tests/test_shared_order_api_advanced.cpp:61`)

구현 매핑:
- `getMyAccount` lock: `src/api/upbit/SharedOrderApi.cpp:39`
- lock 안 계측: `src/api/upbit/SharedOrderApi.cpp:43`
- `InFlightGuard` 증가/최대치 갱신: `src/api/upbit/SharedOrderApi.cpp:16`, `src/api/upbit/SharedOrderApi.cpp:18`
- `InFlightGuard` 감소(RAII): `src/api/upbit/SharedOrderApi.cpp:22`

검증되는 불변식:
- `max_in_flight_ == 1`이어야 직렬화 성공

---

## TEST B `testExceptionSafety` (`tests/test_shared_order_api_advanced.cpp:74`)

검증 의도:
- 정상 호출(`getMyAccount`)과 의도적 오류 호출(`postOrder` with bad request)을 섞었을 때,
  한쪽 실패가 다른 호출 흐름을 망가뜨리지 않는지 확인

테스트 핵심 코드:
- 정상 워커: `api->getMyAccount()` (`tests/test_shared_order_api_advanced.cpp:85`)
- 오류 워커: `api->postOrder(bad_req)` (`tests/test_shared_order_api_advanced.cpp:102`)
- 성공/오류 카운터 점검 (`tests/test_shared_order_api_advanced.cpp:121`, `tests/test_shared_order_api_advanced.cpp:122`)

구현 매핑:
- `getMyAccount` 직렬화/위임: `src/api/upbit/SharedOrderApi.cpp:39`, `src/api/upbit/SharedOrderApi.cpp:45`
- `postOrder` 직렬화/위임: `src/api/upbit/SharedOrderApi.cpp:74`, `src/api/upbit/SharedOrderApi.cpp:79`
- 계측 RAII로 호출 종료 정리: `src/api/upbit/SharedOrderApi.cpp:22`

해석:
- 이 테스트는 "에러 처리 정확성"보다는 "혼합 트래픽에서도 wrapper 안정성 유지"를 본다.

---

## TEST C `testLoadTest` (`tests/test_shared_order_api_advanced.cpp:136`)

검증 의도:
- 다수 스레드/반복 호출에서 wrapper가 지속적으로 동작하는지 부하 관점 확인

테스트 핵심 코드:
- worker 내 반복 호출: `api->getMyAccount()` (`tests/test_shared_order_api_advanced.cpp:147`)
- 총 성공/실패 통계 출력 (`tests/test_shared_order_api_advanced.cpp:177`)

구현 매핑:
- `getMyAccount` lock + 계측 + 위임: `src/api/upbit/SharedOrderApi.cpp:39`, `src/api/upbit/SharedOrderApi.cpp:43`, `src/api/upbit/SharedOrderApi.cpp:45`

주의:
- 네트워크/실서버 상태에 영향 받는 통합 성격 테스트라 결과는 환경 의존적이다.

---

## TEST DRIVER `runAdvancedTests` (`tests/test_shared_order_api_advanced.cpp:194`)

검증 의도:
- 실제 `UpbitExchangeRestClient`를 생성해 `SharedOrderApi` 실동작 확인

테스트 핵심 코드:
- `shared_api` 생성: `tests/test_shared_order_api_advanced.cpp:225`
- 시나리오 3종 실행: `tests/test_shared_order_api_advanced.cpp:228`, `tests/test_shared_order_api_advanced.cpp:229`, `tests/test_shared_order_api_advanced.cpp:230`

구현 매핑:
- 생성자 null 방어는 간접 전제(여기서는 정상 포인터 전달)
- public API 4종 중 실제로는 `getMyAccount`, `postOrder` 중심 검증

---

## 4) 커버리지 요약 (무엇을 검증/미검증하는가)

검증됨:
- mutex 기반 직렬화 동작
- InFlightGuard 계측의 일관성(최대 동시 실행 1)
- 혼합 성공/오류 호출에서 wrapper 생존성
- 반복 호출 부하 상황에서 기본 동작

상대적으로 약한 부분:
- `getOpenOrders`, `cancelOrder` 전용 시나리오의 개별 검증 부족
- 생성자 null 입력 시 예외 테스트 없음
- 완전 오프라인 단위 테스트(모의 client 기반)보다는 실환경 의존도가 높음

---

## 5) 장애 역추적 가이드

1. `max_in_flight > 1`이면
   - 먼저 `SharedOrderApi.cpp`의 lock 위치 확인: `src/api/upbit/SharedOrderApi.cpp:39`, `src/api/upbit/SharedOrderApi.cpp:51`, `src/api/upbit/SharedOrderApi.cpp:63`, `src/api/upbit/SharedOrderApi.cpp:74`
2. 카운터 불일치면
   - `InFlightGuard` ctor/dtor 점검: `src/api/upbit/SharedOrderApi.cpp:16`, `src/api/upbit/SharedOrderApi.cpp:22`
3. 호출 성공률 저하면
   - wrapper 자체보다 네트워크/API key/rate-limit 원인 여부를 먼저 분리

