# SharedOrderApi.cpp 상세 해설

## 1) 구현 목표 요약

`src/api/upbit/SharedOrderApi.cpp`는 다음 목표를 가진다.

1. 모든 REST 호출을 mutex로 직렬화
2. 내부 클라이언트 호출 자체는 최소 래핑으로 투명하게 위임
3. 테스트에서 직렬화가 실제로 지켜지는지 계측 가능하게 제공

---

## 1.1) 핵심 코드 3줄

실제 핵심은 아래 3단계다.

```cpp
std::lock_guard<std::mutex> lock(mtx_);
InFlightGuard g(in_flight_, max_in_flight_);
return client_->someApi(...);
```

모든 public 메서드가 같은 패턴으로 구현된다.

---

## 2) 구성 요소 상세

## 2.1 `InFlightGuard` (익명 네임스페이스)

원본 코드:

```cpp
struct InFlightGuard {
    std::atomic<int>& in_flight;
    std::atomic<int>& max_in_flight;

    explicit InFlightGuard(std::atomic<int>& f, std::atomic<int>& m) : in_flight(f), max_in_flight(m) {
        const int cur = in_flight.fetch_add(1) + 1;
        int prev = max_in_flight.load();
        while (prev < cur && !max_in_flight.compare_exchange_weak(prev, cur)) {
            // retry
        }
    }
    ~InFlightGuard() { in_flight.fetch_sub(1); }
};
```

역할:
- lock 안에 진입한 호출 수를 자동 증감(RAII)
- 관측된 최대 동시 실행 수를 `max_in_flight_`에 기록

왜 필요한가:
- "mutex가 실제로 직렬화했는지"를 테스트에서 숫자로 증명하기 위해
- 예외가 나도 소멸자에서 감소되어 카운터가 망가지지 않게 하기 위해

---

## 2.2 생성자

원본 코드:

```cpp
SharedOrderApi::SharedOrderApi(std::unique_ptr<api::rest::UpbitExchangeRestClient> client)
    : client_(std::move(client))
{
    if (!client_) {
        throw std::invalid_argument("SharedOrderApi: client cannot be null");
    }
}
```

의미:
- null client를 조기 차단하여 런타임 null dereference 방지
- 객체가 생성된 이후에는 `client_` 유효성이 보장됨

---

## 3) API 메서드별 동작

## 3.1 `getMyAccount()`

원본 코드:

```cpp
std::variant<core::Account, api::rest::RestError>
SharedOrderApi::getMyAccount()
{
    std::lock_guard<std::mutex> lock(mtx_);
    InFlightGuard g(in_flight_, max_in_flight_);
    return client_->getMyAccount();
}
```

순서:
1. mutex 락 획득
2. in-flight 계측 시작
3. 실제 REST 호출 위임
4. 함수 종료 시 가드 소멸 -> in-flight 감소

---

## 3.2 `getOpenOrders(market)`

원본 코드:

```cpp
std::variant<std::vector<core::Order>, api::rest::RestError>
SharedOrderApi::getOpenOrders(std::string_view market)
{
    std::lock_guard<std::mutex> lock(mtx_);
    InFlightGuard g(in_flight_, max_in_flight_);
    return client_->getOpenOrders(market);
}
```

포인트:
- 읽기 API도 동일하게 직렬화한다.
- 현재는 단순성과 안정성을 우선(읽기 병렬화 최적화 미적용).

---

## 3.3 `cancelOrder(uuid, identifier)`

원본 코드:

```cpp
std::variant<bool, api::rest::RestError>
SharedOrderApi::cancelOrder(const std::optional<std::string>& uuid,
                            const std::optional<std::string>& identifier)
{
    std::lock_guard<std::mutex> lock(mtx_);
    InFlightGuard g(in_flight_, max_in_flight_);
    return client_->cancelOrder(uuid, identifier);
}
```

포인트:
- 취소 호출도 동일 패턴
- 성공/실패 불리언은 `variant<bool, RestError>`로 전달

---

## 3.4 `postOrder(req)`

원본 코드:

```cpp
std::variant<std::string, api::rest::RestError>
SharedOrderApi::postOrder(const core::OrderRequest& req)
{
    std::lock_guard<std::mutex> lock(mtx_);
    InFlightGuard g(in_flight_, max_in_flight_);
    return client_->postOrder(req);
}
```

포인트:
- 주문 제출도 동일한 직렬화 규칙 적용
- 성공 시 주문 UUID 문자열 반환

---

## 4) 동작 시퀀스 예시

두 스레드가 동시에 호출해도 내부는 순차 처리된다.

```text
T1: lock 획득 -> InFlightGuard(+1) -> client_->postOrder -> unlock
T2: (대기)    -> lock 획득 -> InFlightGuard(+1) -> client_->getMyAccount -> unlock
```

테스트에서 기대:
- `max_in_flight_ == 1`

---

## 5) 테스트와 연결

`tests/test_shared_order_api_advanced.cpp`와 직접 대응되는 지점:

1. 직렬화 증명 테스트
   - `debugResetInFlight()` 후 다중 스레드 호출
   - `debugMaxInFlight()`가 1인지 확인
2. 예외 안전성 테스트
   - 정상/오류 요청 혼합
   - 한 스레드 실패가 다른 스레드 진행을 막지 않는지 확인
3. 부하 테스트
   - 다수 호출에서 전체 성공/실패 비율 확인

즉, `.cpp`의 lock+guard 패턴이 테스트의 핵심 검증 대상이다.

---

## 6) 왜 이렇게 구현했는가 (설계 판단)

1. 멀티마켓 운영에서 REST 클라이언트 경쟁 접근이 가장 위험한 공유 자원
2. 복잡한 큐/워커 모델보다 mutex 직렬화가 구현/운영 리스크가 낮음
3. 테스트 계측(`InFlightGuard`)을 코드 내부에 넣어 "진짜 직렬화"를 가시화

현재 구현은 Phase 1 목적(안전한 공유 API)에 맞는 최소/명확한 설계다.

---

## 7) 확장 시 고려 포인트

1. 읽기 API 병렬화
   - `getMyAccount`, `getOpenOrders`를 `shared_mutex`로 분리 가능
2. Rate limit 제어
   - 호출 간 최소 간격, 버킷 토큰 방식
3. 우선순위 큐
   - 취소 주문 우선 처리
4. 서킷 브레이커
   - 연속 실패 시 임시 차단/복구

확장 시에도 핵심 원칙은 "호출자 관점 인터페이스(IOrderApi) 유지"다.

