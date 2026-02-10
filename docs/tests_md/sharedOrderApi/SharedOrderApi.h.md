# SharedOrderApi.h 상세 해설

## 1) 파일 역할

`src/api/upbit/SharedOrderApi.h`는 멀티마켓 환경에서 여러 스레드가 주문 REST API를 호출할 때,
하나의 `UpbitExchangeRestClient`를 안전하게 공유하기 위한 thread-safe 래퍼의 공개 계약이다.

핵심 아이디어:

```text
여러 마켓 스레드 -> SharedOrderApi(mutex) -> UpbitExchangeRestClient(단일 진입)
```

---

## 1.1) 먼저 코드로 보는 핵심 인터페이스

```cpp
class SharedOrderApi : public IOrderApi {
public:
    explicit SharedOrderApi(std::unique_ptr<api::rest::UpbitExchangeRestClient> client);

    std::variant<core::Account, api::rest::RestError> getMyAccount() override;
    std::variant<std::vector<core::Order>, api::rest::RestError> getOpenOrders(std::string_view market) override;
    std::variant<bool, api::rest::RestError> cancelOrder(const std::optional<std::string>& uuid,
                                                         const std::optional<std::string>& identifier) override;
    std::variant<std::string, api::rest::RestError> postOrder(const core::OrderRequest& req) override;
};
```

인터페이스만 보면 "IOrderApi 계약을 그대로 제공하는 직렬화 래퍼"라는 성격이 명확하다.

---

## 2) 어디에 연결되는가 (의존/연결 구조)

### 상위 의존성

- `IOrderApi.h`
  - 엔진은 구체 구현 대신 인터페이스에 의존
- `core/domain/*`
  - 계좌/주문/주문요청 도메인 타입 사용
- `api/rest/RestError.h`
  - 네트워크/거래소 실패를 예외 대신 값으로 반환

### 내부 연결점

- `std::unique_ptr<api::rest::UpbitExchangeRestClient> client_`
  - 실 REST 호출 주체
- `std::mutex mtx_`
  - 모든 public API 직렬화

### 테스트 연결점

- `debugMaxInFlight()`, `debugResetInFlight()`
  - 내부 직렬화가 깨졌는지 테스트에서 측정하는 디버그 계측 API

---

## 3) 클래스 설계와 의도

## 3.1 생성자와 소유권

```cpp
explicit SharedOrderApi(std::unique_ptr<api::rest::UpbitExchangeRestClient> client);
```

의도:
- `client_`의 단일 소유권을 명확히 보장
- 외부에서 임의로 동일 인스턴스를 동시에 조작하지 못하게 차단

생성자에서 null 포인터는 허용하지 않는다(구현에서 `invalid_argument` throw).

## 3.2 복사/이동 금지

```cpp
SharedOrderApi(const SharedOrderApi&) = delete;
SharedOrderApi& operator=(const SharedOrderApi&) = delete;
SharedOrderApi(SharedOrderApi&&) noexcept = delete;
SharedOrderApi& operator=(SharedOrderApi&&) noexcept = delete;
```

이유:
- `mutex` 포함 객체의 복사 의미가 불명확하고 위험
- 실사용은 `std::shared_ptr<SharedOrderApi>` 공유로 충분

## 3.3 반환 타입 정책 (`std::variant`)

모든 API가 `variant<성공값, RestError>`를 반환한다.

예시:

```cpp
std::variant<std::string, api::rest::RestError> postOrder(const core::OrderRequest& req);
```

의도:
- 예외 기반 흐름 대신 호출자에게 명시적 분기 강제
- 엔진에서 실패 원인을 일관되게 처리 가능

---

## 4) thread-safety 설계 포인트

헤더 주석 기준 정책:

1. 모든 public 메서드는 mutex로 보호
2. 내부 `UpbitExchangeRestClient`는 결과적으로 단일 스레드처럼 호출됨
3. 멀티마켓 스레드는 `shared_ptr<SharedOrderApi>` 공유

디버그 계측 멤버:

```cpp
std::atomic<int> in_flight_{0};
std::atomic<int> max_in_flight_{0};
```

의도:
- lock 안쪽에서 실행 중인 호출 수를 기록
- 정상이라면 `max_in_flight_ == 1`

---

## 5) 실제 사용 예시

```cpp
auto upbit_client = std::make_unique<api::rest::UpbitExchangeRestClient>(rest_client, std::move(signer));
auto shared_api = std::make_shared<api::upbit::SharedOrderApi>(std::move(upbit_client));

// 여러 스레드/엔진이 같은 객체 공유
std::thread t1([&]{ (void)shared_api->getMyAccount(); });
std::thread t2([&]{ (void)shared_api->getOpenOrders("KRW-BTC"); });
t1.join();
t2.join();
```

외부에서는 동시 호출하지만, 내부에서는 mutex로 직렬 처리된다.

---

## 6) 왜 이런 구조로 구현했는가

1. 멀티마켓 전환 시 가장 먼저 깨지기 쉬운 지점이 REST 호출 동시성
2. 거래소 API는 rate limit가 있어 무제한 병렬 호출이 오히려 실패율을 키움
3. Phase 1에서는 단순하고 확실한 직렬화가 운영 안정성에 유리

즉, 현재 설계는 "최적 성능"보다 "정합성과 예측 가능성"을 우선한 선택이다.

---

## 7) 함께 보면 좋은 파일

- `src/api/upbit/SharedOrderApi.cpp`
  - 실제 lock/계측/위임 구현
- `src/api/upbit/IOrderApi.h`
  - SharedOrderApi가 구현하는 계약
- `tests/test_shared_order_api_advanced.cpp`
  - 직렬화/예외 안전/부하 검증 시나리오

