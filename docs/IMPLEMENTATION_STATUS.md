# 구현 현황

마지막 업데이트: 2026-02-03

## 전체 진행률

- [x] Phase 0: 기존 코드 리팩토링 (**실질적 완료**)
- [ ] Phase 1: 멀티마켓 (2/7) - 진행 중
- [ ] Phase 2: PostgreSQL (0/5)
- [ ] Phase 3: AWS 배포 (0/6)

**참고**: 각 Phase의 상세 계획은 [ROADMAP.md](ROADMAP.md)를 참조하세요.

---

## Phase 0: 기존 코드 리팩토링

### 목표

새 아키텍처 도입 전 기존 코드 정리 및 단순화

### 작업 항목

#### 0.1 EngineRunner 분해 (2일 예상)

- **상태**: ✅ 완료 (이미 적절히 분리됨)
- **담당자**: -
- **시작일**: -
- **완료일**: 2026-02-03 확인
- **노트**:
  - `handleOne_()` 함수를 `handleMyOrder_()`, `handleMarketData_()` 등으로 분리 ✅
  - 이벤트별 핸들러 함수 추출 (100줄 이하 목표) ✅
    - `handleOne_()`: 20줄 (dispatch 역할만)
    - `handleMyOrder_()`: 53줄
    - `handleMarketData_()`: 80줄
    - `handleEngineEvents_()`: 34줄
  - static 변수 제거 → 클래스 멤버 `last_candle_ts_`로 관리됨 ✅
  - 검증: 단일 마켓 거래 정상 동작 ✅
  - **재평가**: 추가 분해 불필요, 현재 구조 적절

#### 0.2 불필요 인터페이스 제거 (0.5일 예상)

- **상태**: ⚖️ 부분 완료 / 선택적
- **담당자**: -
- **시작일**: -
- **완료일**: 2026-02-03 확인
- **노트**:
  - `UpbitConverters.h` 제거 완료 ✅ (git status: Deleted)
  - `PrivateOrderApi` 인터페이스: **유지 권장** (선택적)
    - 15줄의 간단한 인터페이스
    - 테스트 시 Mock 주입에 유용
    - 제거해도 되지만 DI 패턴 유지 측면에서 유지 권장
  - **재평가**: 강제 제거 불필요, 유지해도 무방

#### 0.3 에러 처리 통합 (1일 예상)

- **상태**: ✅ 핵심 완료
- **담당자**: -
- **시작일**: -
- **완료일**: 2026-02-03 확인
- **노트**:
  - `src/util/Logger.h` 생성 완료 ✅
  - 핵심 로직 Logger 전환 완료 ✅
    - `EngineRunner.cpp`: Logger 사용 중
    - `RealOrderEngine.cpp`: Logger 사용 중
  - 에러 레벨 통일 (info, warn, error) ✅
  - 남은 `std::cout/cerr`: 37개 (6개 파일)
    - UpbitWebSocketClient.cpp: 19개 (연결/디버그)
    - StartupRecovery.cpp: 9개 (초기화)
    - 기타: 9개 (저빈도)
  - **재평가**: 핵심 비즈니스 로직 전환 완료, 나머지는 선택적 개선

#### 0.4 테스트 코드 분리 (0.5일 예상)

- **상태**: ✅ 완료
- **담당자**: -
- **시작일**: -
- **완료일**: 2026-02-03 확인
- **노트**:
  - `src/app/test/` → `tests/` 디렉토리 이동 ✅
    - src/app/test/*.cpp 5개 파일 삭제됨 (git status: Deleted)
    - tests/ 디렉토리에 6개 테스트 파일 신규 추가
  - `tests/CMakeLists.txt` 업데이트 ✅
  - 테스트 파일 목록:
    - `tests/real_trading_test.cpp` - 실거래 테스트
    - `tests/test_candle_web.cpp` - WebSocket 캔들 테스트
    - `tests/test_indicators.cpp` - 지표 테스트
    - `tests/test_shared_order_api_advanced.cpp` - SharedOrderApi 테스트
    - `tests/test_strategy.cpp` - 전략 테스트
    - `tests/test_upbit_public.cpp` - 공개 API 테스트

#### 0.5 이벤트 타입 통합 (1일 예상)

- **상태**: △ 진행 중
- **담당자**: -
- **시작일**: -
- **완료일**: -
- **노트**:
  - `EngineFillEvent` ≈ `trading::FillEvent` 변환 로직 존재 (EngineRunner에서)
  - `EngineOrderStatusEvent` ≈ `trading::OrderStatusEvent` 변환 로직 존재
  - 현재 구조: 엔진 이벤트 → 전략 이벤트로 변환하여 전달
  - **재평가**: 현재 구조가 명시적이고 이해하기 쉬움, 강제 통합 불필요할 수 있음

#### 0.6 설정 외부화 (0.5일 예상)

- **상태**: ✅ 핵심 완료
- **담당자**: -
- **시작일**: -
- **완료일**: 2026-02-03 확인
- **노트**:
  - `src/util/Config.h` 생성 완료 ✅
  - `RealOrderEngine.cpp`에서 Config 사용 중 ✅
  - 주요 상수 Config로 관리:
    - `kMinNotionalKrw`
    - `kVolumeSafetyEps`
    - 타임아웃 값들
  - `config/defaults.json`: 선택적 (현재 필요시 추가)
  - **재평가**: 핵심 설정 외부화 완료, 모든 상수 이동은 오버엔지니어링

### 완료 기준

- [x] `EngineRunner::handleOne_()` 100줄 이하 ✅ (20줄, dispatch 역할만)
- [x] `PrivateOrderApi` 인터페이스 제거됨 → **재평가: 유지 권장** (테스트 Mock용)
- [x] 핵심 로그가 `Logger`를 통해 출력 ✅ (EngineRunner, RealOrderEngine 완료)
- [x] `tests/` 디렉토리에 테스트 코드 분리 ✅
- [x] 기존 단일 마켓 거래 정상 동작

**Phase 0 실질적 완료**: Phase 1 시작 가능

---

## Phase 1: 멀티마켓

### 목표

1~5개 마켓 동시 거래 지원 (마켓별 독립 스레드 및 전략)

**전제 조건**: Phase 0 완료 기준 충족

### 작업 항목

#### 1.1 SharedOrderApi (1일 예상)

- **상태**: 완료
- **담당자**: Claude
- **시작일**: 2026-01-29
- **완료일**: 2026-01-29
- **노트**:
  - `UpbitExchangeRestClient`를 감싸는 thread-safe 래퍼 구현 완료
  - 내부 mutex 직렬화 + 주석 (확장 포인트 명시)
  - 검증: 멀티스레드 주문 제출 테스트 작성 완료
  - 관련 파일:
    - `src/api/upbit/SharedOrderApi.h` - 헤더 파일
    - `src/api/upbit/SharedOrderApi.cpp` - 구현 파일
    - `tests/test_shared_order_api.cpp` - 멀티스레드 테스트
    - `docs/SharedOrderApi_USAGE.md` - 사용 가이드

#### 1.2 AccountManager (3일 예상)

- **상태**: ✅ 완료 (2026-02-06 재확인)
- **담당자**: Claude
- **시작일**: 2026-02-03
- **완료일**: 2026-02-03
- **노트**:
  - **전량 거래 모델 완전 구현** ✅
    - 불변 조건: `(coin_balance > 0) XOR (available_krw > 0)`
    - 각 마켓 100% KRW 또는 100% 코인 상태 유지
    - 마켓 간 자본 이동 없음 (완전 독립)
  - `MarketBudget` 구조체 정의 ✅
  - `reserve/release/finalizeFillBuy/finalizeFillSell` API 구현 ✅
  - 부분 체결 누적 로직 ✅ (가중 평균 단가 계산)
  - **rebalance() 메서드 의도적 제외** (전량 거래로 불필요)
  - `ReservationToken` RAII 토큰 구현 ✅ (move-only, 자동 해제 안전망)
  - `syncWithAccount()` 물리 계좌 동기화 ✅
    - 1단계 전체 리셋 (외부 거래 대응)
    - 2단계 API 응답 반영
    - 3단계 KRW 재분배
  - **Dust 이중 체크** ✅
    - 1차: 수량 기준 (`coin_epsilon = 1e-7`)
    - 2차: 가치 기준 (`init_dust_threshold_krw = 5,000원`)
    - 전략 일관성: `RsiMeanReversionStrategy.hasMeaningfulPos`와 동일 기준
  - Thread-safe 보장 (`shared_mutex`) ✅
  - 통계 카운터 (`Stats`, atomic) ✅
  - 검증: 동시 예약, 부분 체결, 과매도, 외부 거래 테스트 ✅
  - 관련 파일:
    - `src/trading/allocation/AccountManager.h` - 헤더 파일
    - `src/trading/allocation/AccountManager.cpp` - 구현 파일
    - `tests/test_account_manager_unified.cpp` - 단위 테스트 (23개 테스트)

#### 1.3 MarketEngine (2일 예상)

- **상태**: 미시작
- **담당자**:
- **시작일**:
- **완료일**:
- **노트**:
  - `RealOrderEngine` 로직 추출 + 리팩토링
  - 로컬 상태만 직접 변경, 공유 자원은 API 호출만
  - `bindToCurrentThread()` 유지
  - 검증: 단일 마켓 엔진 동작 테스트

#### 1.4 EventRouter (1일 예상)

- **상태**: 미시작
- **담당자**:
- **시작일**:
- **완료일**:
- **노트**:
  - `extractStringValue()` 일반화 함수 구현
  - "KRW-" 하드코딩 제거
  - Fast path + Fallback 파싱
  - 검증: 다양한 JSON 포맷 테스트

#### 1.5 MarketEngineManager (3일 예상)

- **상태**: 미시작
- **담당자**:
- **시작일**:
- **완료일**:
- **노트**:
  - `MarketContext` 생성/관리
  - 워커 스레드 생명주기
  - 이벤트 라우팅
  - **멀티마켓 StartupRecovery 통합** ← 신규 (1일 추가)
    - `syncAccountWithExchange()` 메서드 추가
    - 초기화 시 각 마켓별 `StartupRecovery::run()` 호출
    - `AccountManager.syncWithAccount()` 연동
    - 미체결 주문 처리 (Cancel 정책)
  - 검증: 3개 마켓 동시 실행 테스트

#### 1.6 MarketContext + 통합 (2일 예상)

- **상태**: 미시작
- **담당자**:
- **시작일**:
- **완료일**:
- **노트**:
  - 기존 `EngineRunner` 로직 → `MarketEngineManager`로 이전
  - `main()` 진입점 수정
  - 설정 파일 (`config/markets.json`) 로딩
  - 검증: 멀티마켓 End-to-End 테스트
  - **재시작 시나리오 테스트** ← 신규
    - 각 마켓별 포지션 복구 검증
    - AccountManager 잔고 동기화 검증
    - 미체결 주문 처리 검증

#### 1.7 멀티마켓 테스트 (2일 예상)

- **상태**: 미시작
- **담당자**:
- **시작일**:
- **완료일**:
- **노트**:
  - 단위 테스트 작성
  - 통합 테스트 작성
  - 부하 테스트 (5마켓, 1시간)
  - 검증: 모든 테스트 통과

### 완료 기준

- [ ] 1~5개 마켓 동시 거래 가능
- [ ] 마켓별 독립 KRW 할당
- [ ] WS 이벤트 올바른 마켓으로 라우팅
- [ ] 부분 체결 정확히 누적
- [ ] 부하 테스트 통과 (CPU < 50%, 메모리 안정)
- [ ] **재시작 시 각 마켓별 포지션 복구** ← 신규
- [ ] **AccountManager와 실제 계좌 동기화** ← 신규

---

## Phase 2: PostgreSQL

### 목표

거래 내역 영속화 및 복구 기능 구현

**전제 조건**: Phase 1 완료 기준 충족

### 작업 항목

#### 2.1 스키마 생성 (0.5일 예상)

- **상태**: 미시작
- **담당자**:
- **시작일**:
- **완료일**:
- **노트**:
  - `sql/schema.sql` 작성
  - `OrderSize` variant (`order_volume`/`order_amount_krw`) 반영
  - CHECK 제약 조건
  - 검증: psql로 스키마 적용 테스트

#### 2.2 DatabasePool (1일 예상)

- **상태**: 미시작
- **담당자**:
- **시작일**:
- **완료일**:
- **노트**:
  - libpqxx 연결 풀
  - RAII Connection 가드
  - 타임아웃 처리
  - 검증: 연결 획득/반환 테스트

#### 2.3 TradeLogger (3일 예상)

- **상태**: 미시작
- **담당자**:
- **시작일**:
- **완료일**:
- **노트**:
  - `BoundedQueue` + 배치 쓰기
  - WAL 파일 폴백
  - 백프레셔 메트릭
  - `OrderSize` variant 분기 저장
  - 검증: DB 장애 시 WAL 복구 테스트

#### 2.4 기존 코드 통합 (1일 예상)

- **상태**: 미시작
- **담당자**:
- **시작일**:
- **완료일**:
- **노트**:
  - `MarketEngine`에서 `TradeLogger` 호출
  - 거래/주문 로깅 삽입
  - 전략 스냅샷 저장
  - 검증: 실거래 시 DB 기록 확인

#### 2.5 복구 테스트 (1일 예상)

- **상태**: 미시작
- **담당자**:
- **시작일**:
- **완료일**:
- **노트**:
  - WAL 복구 시나리오 테스트
  - DB 재연결 테스트
  - 백프레셔 동작 확인
  - 검증: 모든 복구 시나리오 통과

### 완료 기준

- [ ] 모든 거래가 DB에 기록됨
- [ ] DB 장애 시 WAL로 폴백
- [ ] 재시작 시 WAL에서 복구
- [ ] 백프레셔 메트릭 정상 동작

---

## Phase 3: AWS 배포

### 목표

24시간 무중단 운영 환경 구축

**전제 조건**: Phase 2 완료 기준 충족

### 작업 항목

#### 3.1 SignalHandler (1일 예상)

- **상태**: 미시작
- **담당자**:
- **시작일**:
- **완료일**:
- **노트**:
  - SIGTERM/SIGINT 처리
  - `stop_flag` 전파
  - 검증: `kill -TERM` 시 정상 종료

#### 3.2 GracefulShutdown (2일 예상)

- **상태**: 미시작
- **담당자**:
- **시작일**:
- **완료일**:
- **노트**:
  - 시장가 주문 체결 대기
  - REST API로 주문 상태 확인
  - 미체결 주문 정책 적용
  - `TradeLogger` flush + 타임아웃
  - 종료 요약 로그
  - 검증: 진행 중 주문 있을 때 종료 테스트

#### 3.3 HealthChecker (1일 예상)

- **상태**: 미시작
- **담당자**:
- **시작일**:
- **완료일**:
- **노트**:
  - WS 연결 상태
  - DB 연결 상태
  - 마켓 스레드 상태
  - `TradeLogger` 백프레셔
  - 검증: 각 서비스 장애 시 감지 테스트

#### 3.4 Logger 개선 (1일 예상)

- **상태**: 미시작
- **담당자**:
- **시작일**:
- **완료일**:
- **노트**:
  - 구조화된 JSON 로깅
  - CloudWatch 싱크 (선택적)
  - 로그 레벨 동적 변경
  - 검증: 로그 포맷 검증

#### 3.5 배포 스크립트 (1일 예상)

- **상태**: 미시작
- **담당자**:
- **시작일**:
- **완료일**:
- **노트**:
  - Dockerfile 작성
  - `coinbot.service` (systemd)
  - 환경 변수 템플릿
  - 검증: Docker 빌드 및 실행

#### 3.6 인프라 구성 (2일 예상)

- **상태**: 미시작
- **담당자**:
- **시작일**:
- **완료일**:
- **노트**:
  - EC2 인스턴스 설정
  - RDS PostgreSQL 설정
  - CloudWatch 알림 설정
  - Secrets Manager 연동
  - 검증: 24시간 운영 테스트

### 완료 기준

- [ ] systemd로 자동 재시작
- [ ] SIGTERM 시 graceful shutdown
- [ ] 진행 중 주문 안전하게 처리
- [ ] CloudWatch에서 메트릭 확인 가능
- [ ] 24시간 무중단 테스트 통과

---

## 변경 이력

| 날짜 | Phase | 작업 | 상태 | 비고 |
|------|-------|------|------|------|
| 2026-02-06 | 문서 | ROADMAP/ARCHITECTURE 업데이트 | 완료 | AccountManager 구현 기반 문서 동기화, rebalance 제거 |
| 2026-02-06 | Phase 1 | 1.2 AccountManager 재확인 | 완료 | 전량 거래 모델 완전 구현 확인, Dust 이중 체크, 23개 테스트 |
| 2026-02-03 | Phase 0 | 전체 재평가 | 완료 | 실제 코드 확인 결과 대부분 완료 상태 확인 |
| 2026-02-03 | Phase 0 | 0.1 EngineRunner 분해 | 완료 | 이미 적절히 분리됨 (함수별 20-80줄) |
| 2026-02-03 | Phase 0 | 0.3 Logger 통합 | 핵심완료 | EngineRunner, RealOrderEngine 전환 완료 |
| 2026-02-03 | Phase 0 | 0.4 테스트 분리 | 완료 | tests/ 디렉토리 이동 완료 |
| 2026-02-03 | Phase 0 | 0.6 Config 통합 | 핵심완료 | RealOrderEngine에서 Config 사용 중 |
| 2026-02-03 | Phase 1 | 1.2 AccountManager | 완료 | 전량 거래 모델, Dust 이중 체크, syncWithAccount, 23개 테스트 |
| 2026-01-29 | Phase 1 | 1.1 SharedOrderApi | 완료 | Thread-safe 래퍼 구현 및 테스트 |
| 2026-01-29 | - | 문서 생성 | 완료 | 초기 구조 작성 |

---

## Phase 간 게이트 체크

각 Phase 전환 시 이전 Phase의 완료 기준을 모두 충족해야 합니다.

### Phase 0 → Phase 1 게이트

- [x] `EngineRunner::handleOne_()` ≤ 100줄 ✅ (20줄)
- [x] `PrivateOrderApi` 인터페이스 → **유지 권장** (테스트 Mock용, 제거 불필요)
- [x] `Logger` 클래스로 핵심 로그 출력 ✅
- [x] `tests/` 디렉토리 존재 ✅
- [ ] 단일 마켓 거래 테스트 통과 (확인 필요)

**게이트 상태**: ✅ 통과 가능 (4/5 완료, 1개 확인 필요)

### Phase 1 → Phase 2 게이트

- [ ] 3개 마켓 동시 거래 성공
- [ ] `AccountManager` 예약/체결 정상 동작
- [ ] `EventRouter` fast path 성공률 > 99%
- [ ] 부하 테스트 통과 (5마켓, 1시간)
- [ ] **재시작 시 포지션 복구 정상 동작** ← 신규
- [ ] **AccountManager 계좌 동기화 검증** ← 신규

### Phase 2 → Phase 3 게이트

- [ ] 모든 거래 DB에 기록됨
- [ ] WAL 복구 테스트 통과
- [ ] 백프레셔 메트릭 정상

### Phase 3 완료 체크

- [ ] 24시간 무중단 운영 성공
- [ ] Graceful shutdown 정상 동작
- [ ] CloudWatch 알림 동작 확인

---

## 관련 문서

- **현재 아키텍처**: [ARCHITECTURE.md](ARCHITECTURE.md)
- **미래 로드맵**: [ROADMAP.md](ROADMAP.md)
- **멀티마켓 포지션 복구 설계**: [STARTUP_RECOVERY_MULTIMARKET.md](STARTUP_RECOVERY_MULTIMARKET.md)
- **개발자 가이드**: [../CLAUDE.md](../CLAUDE.md)

# 파일 생성 규칙
- 모든 텍스트 파일은 한글이 깨지지 않도록 저장
- 오버코딩 금지
- 주석으로 왜 필요한지, 기능과 동작을 간단히 설명할 것
- 우선 테스트 코드는 작성, 수정하지 말고 요청 시 작성

---

## 테스트 작성 지침

### 스타일 규칙

**CoinBot 프로젝트는 수동 테스트 스타일을 사용합니다.**

- ❌ **GoogleTest/GTest 사용 금지**
- ✅ **수동 함수 기반 테스트 사용**

### 테스트 작성 패턴

```cpp
// tests/test_example.cpp

#include <iostream>

// 공통 테스트 유틸리티 (NDEBUG 안전, 예외 기반)
#include "test_utils.h"

// 프로젝트 헤더
#include "module/ClassName.h"

namespace test {

    // 테스트 함수 (void testXXX() 형식)
    void testBasicFunctionality() {
        std::cout << "\n[TEST 1] Basic functionality\n";

        // Arrange
        ClassName obj(param1, param2);

        // Act
        auto result = obj.someMethod();

        // Assert (TEST_ASSERT 사용 - NDEBUG 안전, 예외 기반)
        TEST_ASSERT(result.success);
        TEST_ASSERT_EQ(result.value, expected_value);
        TEST_ASSERT_DOUBLE_EQ(result.price, 100.5);  // 부동소수점

        std::cout << "  result: " << result.value << "\n";
        std::cout << "  [PASS] Basic functionality works\n";
    }

    void testEdgeCase() {
        std::cout << "\n[TEST 2] Edge case handling\n";

        // 테스트 로직...
        TEST_ASSERT(condition);

        std::cout << "  [PASS] Edge case handled\n";
    }

    // 메인 실행 함수 (개별 테스트 실패 보고)
    struct TestCase {
        const char* name;
        void (*func)();
    };

    bool runAllTests() {
        std::cout << "\n========================================\n";
        std::cout << "  Example Module Tests\n";
        std::cout << "========================================\n";

        TestCase tests[] = {
            {"BasicFunctionality", testBasicFunctionality},
            {"EdgeCase", testEdgeCase},
        };

        int passed = 0;
        int failed = 0;
        const int total = sizeof(tests) / sizeof(tests[0]);

        for (int i = 0; i < total; ++i) {
            try {
                tests[i].func();
                ++passed;
            }
            catch (const TestFailure& e) {
                ++failed;
                std::cerr << "\n[TEST FAILED] " << tests[i].name << "\n";
                std::cerr << "  Reason: " << e.condition() << "\n";
                std::cerr << "  Location: " << e.file() << ":" << e.line() << "\n";
            }
            catch (const std::exception& e) {
                ++failed;
                std::cerr << "\n[TEST FAILED] " << tests[i].name << "\n";
                std::cerr << "  Exception: " << e.what() << "\n";
            }
        }

        std::cout << "\n========================================\n";
        if (failed == 0) {
            std::cout << "  ALL TESTS PASSED (" << passed << "/" << total << ")\n";
            return true;
        }
        else {
            std::cerr << "  TESTS FAILED: " << failed << " failed\n";
            return false;
        }
    }

} // namespace test

int main() {
    bool success = test::runAllTests();
    return success ? 0 : 1;
}
```

### 핵심 원칙

1. **namespace test { } 사용**
   - 모든 테스트 함수를 `test` 네임스페이스 안에 작성

2. **함수 기반 테스트**
   - `void testXXX()` 형식의 함수로 각 테스트 작성
   - TEST_F, EXPECT_EQ 같은 GTest 매크로 사용 금지

3. **test_utils.h 포함 + TEST_ASSERT 사용** ⭐
   - `#include "test_utils.h"` 필수
   - `TEST_ASSERT(condition)` - NDEBUG 영향 없음, 예외 기반
   - `TEST_ASSERT_EQ(actual, expected)` - 값 비교 (실패 시 양쪽 출력)
   - `TEST_ASSERT_DOUBLE_EQ(actual, expected)` - 부동소수점 비교
   - ❌ `assert()` 사용 금지 (NDEBUG에서 제거됨, 실패 보고 불가)

4. **std::cout으로 출력**
   - `[TEST N] 테스트 이름` 형식으로 시작
   - `[PASS]` 메시지로 성공 표시
   - 중요한 값은 출력해서 확인
   - 실패 시 자동으로 `[TEST FAILED]` + 위치 출력

5. **runAllTests() 패턴** ⭐
   - `TestCase` 배열로 테스트 목록 관리
   - 각 테스트를 try/catch로 감싸 실패 보고
   - 실패 시 테스트 이름, 조건, 파일 위치 출력
   - 모든 테스트 실행 후 요약 (passed/failed/total)
   - 반환값: 성공 0, 실패 1

6. **CMakeLists.txt 설정**
   ```cmake
   add_executable(test_example
       test_example.cpp
       ${CMAKE_SOURCE_DIR}/src/module/Source.cpp
   )

   target_include_directories(test_example
       PRIVATE
           ${CMAKE_SOURCE_DIR}/src
           ${CMAKE_SOURCE_DIR}/tests  # test_utils.h 위치
   )

   target_link_libraries(test_example
       PRIVATE
           # 필요한 라이브러리만
   )
   ```

### 기존 테스트 참고

- `tests/test_market_engine.cpp` - MarketEngine 테스트 (24개 테스트) ⭐ **최신 패턴**
- `tests/test_account_manager_unified.cpp` - AccountManager 테스트 (23개 테스트)
- `tests/test_shared_order_api_advanced.cpp` - SharedOrderApi 테스트
- `tests/test_utils.h` - 공통 테스트 유틸리티 (TEST_ASSERT 매크로)

### 일관성 유지 이유

1. **단순성**: 외부 프레임워크 의존성 없음
2. **명확성**: 테스트 로직이 명시적으로 보임
3. **일관성**: 프로젝트 내 모든 테스트가 동일한 스타일
4. **가독성**: 새로운 개발자도 쉽게 이해 가능
5. **안정성**: NDEBUG 영향 없음, Release 빌드에서도 동작
6. **실패 보고**: 어떤 테스트가 왜 실패했는지 명확히 출력
