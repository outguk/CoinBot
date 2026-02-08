# SharedOrderApi 코드 검토 보고서

## 검토 일자
2026-01-29

## 1. SharedOrderApi 구현 검토

### ✅ 적절한 부분

#### 1.1 Thread-Safety 구현
```cpp
std::variant<core::Account, api::rest::RestError>
SharedOrderApi::getMyAccount()
{
    std::lock_guard<std::mutex> lock(mtx_);  // ✅ 올바른 mutex 보호
    return client_->getMyAccount();
}
```
- 모든 public 메서드에서 `std::lock_guard` 사용
- RAII 패턴으로 예외 안전성 보장
- 하나의 mutex로 모든 API 호출 직렬화

#### 1.2 Move/Copy 금지 (SharedOrderApi.h:51-57)
```cpp
// Copy 금지 (mutex는 복사 불가)
SharedOrderApi(const SharedOrderApi&) = delete;
SharedOrderApi& operator=(const SharedOrderApi&) = delete;

// Move 금지(필요없음)
SharedOrderApi(SharedOrderApi&&) noexcept = delete;
SharedOrderApi& operator=(SharedOrderApi&&) noexcept = delete;
```
- ✅ **적절함**: `std::mutex`는 복사도 이동도 불가능
- ✅ **shared_ptr로만 공유**하는 설계이므로 move 불필요
- 원래 코드에서 `= default`였던 것을 `= delete`로 수정한 것이 올바름

#### 1.3 Null 검증 (SharedOrderApi.cpp:11-13)
```cpp
if (!client_) {
    throw std::invalid_argument("SharedOrderApi: client cannot be null");
}
```
- ✅ 생성자에서 null 체크
- 명확한 에러 메시지

---

## 2. 테스트 코드 검토

### ❌ 치명적 오류

#### 2.1 RestClient 생성자 호출 오류 (test_shared_order_api.cpp:118)

**현재 코드 (잘못됨)**:
```cpp
boost::asio::io_context ioc;
boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tls_client);

// ❌ 컴파일 에러: ioc에는 ssl_ctx 멤버가 없음
api::rest::RestClient rest_client(ioc.ssl_ctx);
```

**RestClient 생성자 시그니처 (RestClient.h:28-29)**:
```cpp
RestClient(boost::asio::io_context& ioc,
           boost::asio::ssl::context& ssl_ctx);
```

**올바른 수정**:
```cpp
boost::asio::io_context ioc;
boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tls_client);

// ✅ 두 개의 인자를 모두 전달
api::rest::RestClient rest_client(ioc, ssl_ctx);
```

---

### ⚠️ 테스트 검증 항목의 문제점

테스트 코드 상단 주석 (5-8줄):
```cpp
// 검증 항목:
// 1. 여러 스레드에서 동시 호출 시 데이터 경쟁(data race) 없음
// 2. 모든 호출이 순차적으로 직렬화됨
// 3. 예외 안전성 (한 스레드의 예외가 다른 스레드에 영향 없음)
```

#### 문제 1: Data Race 검증 안됨

**현재 테스트가 하는 것**:
- 3개 스레드에서 `getMyAccount()` 호출
- 성공/실패 카운트만 확인
- 크래시가 나지 않으면 통과

**실제로 검증되는 것**:
- ❌ Data race 없음을 증명하지 **못함**
- ❌ 단순히 "프로그램이 죽지 않았다"만 확인
- ❌ Race condition은 간헐적으로 발생할 수 있음

**진짜 Data Race 검증 방법**:
```cpp
// 방법 1: Thread Sanitizer 사용
// 컴파일: g++ -fsanitize=thread -g test.cpp

// 방법 2: 공유 상태 변경 및 검증
std::atomic<int> call_order{0};
std::vector<int> order_log;  // mutex 없이 접근하면 race

// 각 스레드에서:
int my_order = call_order.fetch_add(1);
// SharedOrderApi 호출
order_log.push_back(my_order);  // 여기서 race 발생 가능

// 검증: order_log 크기와 call_order 비교
```

#### 문제 2: 직렬화 검증 안됨

**현재 테스트**:
```cpp
auto start = std::chrono::steady_clock::now();
// 3개 스레드 실행
auto end = std::chrono::steady_clock::now();
std::cout << "소요 시간: " << duration.count() << "ms\n";
```

**문제점**:
- 시간만 출력하고 **검증하지 않음**
- 직렬화되었는지 병렬 실행되었는지 판단 불가

**올바른 검증**:
```cpp
// 각 호출이 약 50ms + API 응답시간 걸린다고 가정
// 직렬화 시: 15 * (50ms + API_time)
// 병렬 시:   5 * (50ms + API_time)

// 예상 최소 시간 계산
auto expected_min = 15 * 50ms;  // 직렬화 가정

if (duration < expected_min) {
    std::cerr << "[FAIL] 직렬화되지 않음! 병렬 실행 의심\n";
}
```

#### 문제 3: 예외 안전성 테스트 없음

**현재 테스트**:
- 예외를 발생시키는 시나리오가 **전혀 없음**
- 정상 케이스만 테스트

**필요한 테스트**:
```cpp
// 의도적으로 잘못된 요청 생성
core::OrderRequest bad_req;
bad_req.market = "";  // 빈 마켓 (에러 발생)

// 여러 스레드에서 동시 호출
// - 한 스레드는 예외 발생
// - 다른 스레드는 정상 동작
// - 모든 스레드가 안전하게 종료되는지 확인
```

---

## 3. 개선된 테스트 제안

### 3.1 Thread Sanitizer 활용

**CMakeLists.txt 수정**:
```cmake
# Thread sanitizer 옵션 추가
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_shared_order_api PRIVATE
        -fsanitize=thread
        -g
    )
    target_link_options(test_shared_order_api PRIVATE
        -fsanitize=thread
    )
endif()
```

### 3.2 직렬화 검증 강화

```cpp
void testSerializationProof(std::shared_ptr<api::upbit::SharedOrderApi> api)
{
    std::atomic<int> concurrent_count{0};
    std::atomic<int> max_concurrent{0};

    auto worker = [&]() {
        for (int i = 0; i < 10; ++i) {
            // 진입 시 카운트 증가
            int current = concurrent_count.fetch_add(1) + 1;

            // 최대 동시 실행 수 추적
            int prev_max = max_concurrent.load();
            while (prev_max < current &&
                   !max_concurrent.compare_exchange_weak(prev_max, current));

            // API 호출
            api->getMyAccount();

            // 퇴출 시 카운트 감소
            concurrent_count.fetch_sub(1);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 5; ++i) {
        threads.emplace_back(worker);
    }

    for (auto& t : threads) {
        t.join();
    }

    // 검증: 최대 동시 실행 수가 1이어야 함 (직렬화)
    if (max_concurrent.load() == 1) {
        std::cout << "[PASS] 직렬화 검증 성공\n";
    } else {
        std::cout << "[FAIL] 최대 동시 실행: " << max_concurrent.load()
                  << " (예상: 1)\n";
    }
}
```

### 3.3 예외 안전성 테스트 추가

```cpp
void testExceptionSafety(std::shared_ptr<api::upbit::SharedOrderApi> api)
{
    std::atomic<int> success{0};
    std::atomic<int> expected_errors{0};

    auto goodWorker = [&]() {
        for (int i = 0; i < 10; ++i) {
            auto result = api->getMyAccount();
            if (std::holds_alternative<core::Account>(result)) {
                success.fetch_add(1);
            }
        }
    };

    auto badWorker = [&]() {
        for (int i = 0; i < 10; ++i) {
            core::OrderRequest bad_req;
            bad_req.market = "";  // 의도적 오류

            auto result = api->postOrder(bad_req);
            if (std::holds_alternative<api::rest::RestError>(result)) {
                expected_errors.fetch_add(1);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.emplace_back(goodWorker);
    threads.emplace_back(badWorker);
    threads.emplace_back(goodWorker);

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "성공: " << success.load()
              << ", 예상 에러: " << expected_errors.load() << "\n";

    if (success > 0 && expected_errors > 0) {
        std::cout << "[PASS] 예외 안전성 검증 성공\n";
    }
}
```

---

## 4. 종합 평가

### SharedOrderApi 구현: ✅ **적절함**

- Thread-safe 구현 올바름
- Copy/Move 금지 적절함
- Null 체크 존재
- 확장 포인트 주석 명시

### 테스트 코드: ❌ **부적절함**

#### 즉시 수정 필요
- **컴파일 에러**: `rest_client(ioc.ssl_ctx)` → `rest_client(ioc, ssl_ctx)`

#### 개선 필요
1. **검증 항목 거짓 주장**: 주석에 명시된 검증이 실제로 이루어지지 않음
2. **Data Race 미검증**: 단순히 크래시 안 나는 것만 확인
3. **직렬화 미검증**: 시간 출력만 하고 검증 안함
4. **예외 안전성 미검증**: 예외 발생 시나리오 없음

---

## 5. 권장 사항

### 단기 (즉시)
1. **테스트 코드 컴파일 에러 수정** (test_shared_order_api.cpp:118)
2. **테스트 주석 수정**: 실제 검증되는 것만 명시

### 중기 (1-2일)
1. Thread Sanitizer 통합
2. 직렬화 검증 로직 추가
3. 예외 안전성 테스트 추가

### 장기 (Phase 1 완료 전)
1. Stress Test (100+ 스레드, 장시간 실행)
2. 부하 테스트 (초당 수백 요청)
3. Rate Limit 검증 (Upbit API 제한 준수)

---

## 6. 수정된 테스트 코드 (최소 수정)

```cpp
// line 118: 컴파일 에러 수정
api::rest::RestClient rest_client(ioc, ssl_ctx);  // ✅ 수정됨
```

```cpp
// line 189-190: 과장된 주장 제거
std::cout << "\n=== 테스트 완료 ===\n";
std::cout << "결과: 모든 호출이 정상 완료되었습니다.\n";
// "순차적으로 직렬화되어 data race 없이" 삭제 (검증 안됨)
```
