# 프로젝트 구조 개선 검토 (2026-02-12)

## 목적
- 현재 코드베이스에서 운영 안정성/유지보수성 측면의 구조 개선 가능 지점을 정리한다.
- 단순 스타일 이슈보다 실제 장애/확장성에 영향을 줄 항목을 우선한다.

## 요약 (우선순위)
1. 빌드 타깃 분리: 앱 바이너리와 테스트 엔트리 결합 해소
2. 워커 종료 모델 개선: `jthread + stop_token` 1단계 적용, 하위 취소 전파는 미완
3. SharedOrderApi 직렬화 병목 완화: 단일 mutex 구간 재검토
4. WebSocket 루프 구조 개선: 폴링 기반 루프 개선
5. 로깅 경로 통합: `Logger`와 `std::cout` 혼용 제거
6. CMake 유지보수성 개선: 하드코딩/대형 파일 나열식 구조 정리
7. 치명 종료 정책 재검토: 릴리즈 `std::terminate()` 정책 운영성 점검
8. 후속 재동기화 정책 명시: 복구/2차 동기화 soft-fail 이후 정합성 보정 루프 필요

---

## 상세 검토

### 1) 앱/테스트 타깃 결합
- 현황:
  - `CoinBot` 실행 타깃에 테스트 파일이 포함되어 있음
  - 참조: `CMakeLists.txt:58`, `CMakeLists.txt:61`, `tests/real_trading_test.cpp:53`
- 리스크:
  - 운영 바이너리 엔트리와 테스트 엔트리가 섞여 배포/실행 경로 혼선 발생
- 개선:
  - `CoinBot`(운영)과 `real_trading_test`(테스트)를 완전 분리
  - 공통 코드는 라이브러리 타깃(`coinbot_core`)으로 추출 후 양쪽에서 링크

### 2) 워커 종료 제어 패턴 (진행 중)
- 적용 완료(1단계):
  - `MarketEngineManager` 워커가 `std::thread + stop_flag`에서 `std::jthread + stop_token`으로 전환됨
  - 참조: `src/app/MarketEngineManager.h:81`, `src/app/MarketEngineManager.cpp:137`, `src/app/MarketEngineManager.cpp:231`
  - `UpbitWebSocketClient`도 내부 `jthread` 생명주기(`start()/stop()`)로 전환됨
  - 참조: `src/api/ws/UpbitWebSocketClient.h:85`, `src/api/ws/UpbitWebSocketClient.h:160`, `src/api/ws/UpbitWebSocketClient.cpp:55`, `src/api/ws/UpbitWebSocketClient.cpp:64`
- 남은 리스크:
  - `join()`은 여전히 타임아웃 없이 대기하므로, 블로킹 호출 구간에서는 종료 지연 가능
  - 참조: `src/app/MarketEngineManager.cpp:162`, `src/api/ws/UpbitWebSocketClient.cpp:69`
  - `stop_token`은 협조적 취소이므로 하위 API/REST가 빠르게 반환하지 않으면 즉시 종료 보장 불가
- 다음 개선(2단계):
  - 종료 관측 로그/지연 경고 강화
  - 하위 계층(API/REST)까지 취소 신호를 전파하는 협조적 취소 모델 검토

### 3) SharedOrderApi 직렬화 병목
- 현황:
  - REST 호출 전체를 단일 mutex로 직렬화
  - 참조: `src/api/upbit/SharedOrderApi.cpp:39`, `src/api/upbit/SharedOrderApi.cpp:74`
  - 재시도 시 `sleep_for` 포함
  - 참조: `src/api/rest/RestClient.cpp:160`, `src/api/rest/RestClient.cpp:174`
- 리스크:
  - 한 요청 지연이 전체 마켓 주문/취소 지연으로 전파(HOL blocking)
- 개선:
  - 요청 우선순위(취소 우선) 또는 최소한 취소 경로 분리 검토
  - 재시도/백오프 정책을 호출 성격별(조회/주문/취소)로 분리

### 4) WebSocket 루프 구조
- 현황:
  - `cmd_cv_`는 선언/notify는 있으나 `wait` 기반 소비가 없음
  - 참조: `src/api/ws/UpbitWebSocketClient.h:203`, `src/api/ws/UpbitWebSocketClient.cpp:61`, `src/api/ws/UpbitWebSocketClient.cpp:441`
  - idle timeout + 주기 루프로 커맨드/수신 처리
  - 참조: `src/api/ws/UpbitWebSocketClient.cpp:375`, `src/api/ws/UpbitWebSocketClient.cpp:379`
- 리스크:
  - 이벤트 반응 지연 및 폴링 오버헤드 증가
- 개선:
  - 커맨드 소비를 event-driven으로 정리하거나 현재 루프의 책임 분리(읽기/명령/재연결)

### 5) 로깅 경로 혼용
- 현황:
  - `Logger`와 `std::cout`가 혼재
  - 참조: `src/api/ws/UpbitWebSocketClient.cpp:416`, `src/app/StartupRecovery.cpp:38`, `src/app/MarketDataEventBridge.cpp:42`
- 리스크:
  - 로그 레벨/포맷/수집 경로 불일치, 운영 분석성 저하
  - 멀티마켓 동시 출력 시 콘솔 로그가 시간순으로 섞여 마켓별 원인 추적 난이도 증가
- 개선:
  - 운영 코드에서는 `Logger` 단일 경로 사용
  - 디버그 출력은 레벨 기반으로 제어하고 고빈도 로그는 샘플링 적용
  - **우선 적용안**: 마켓별 분리 로그 파일(`logs/KRW-BTC.log`, `logs/KRW-ETH.log` 등) 도입
  - 콘솔은 요약 로그만 유지하고, 상세 원인 분석은 마켓별 파일 기준으로 수행

### 6) CMake 구조 개선
- 현황:
  - 환경 의존 절대 경로 및 대형 파일 나열식 타깃 정의
  - 참조: `CMakeLists.txt:16`, `CMakeLists.txt:22`, `CMakeLists.txt:61`
- 리스크:
  - 환경 이식성/변경 추적성 저하, 신규 파일 추가 시 누락 위험
- 개선:
  - 타깃 단위 모듈화(`core`, `api`, `app` 등)
  - 경로는 캐시 변수/툴체인 파일/패키지 매니저 기반으로 정리

### 7) 릴리즈 치명 종료 정책
- 현황:
  - 스레드 소유권 위반 시 릴리즈에서도 `std::terminate()` 호출
  - 참조: `src/engine/MarketEngine.cpp:39`, `src/engine/RealOrderEngine.cpp:33`
- 리스크:
  - 오용 탐지에는 유리하지만 프로세스 즉시 종료로 가용성 저하 가능
- 개선:
  - 정책 자체는 유지 가능하나, 운영 모드에서 진단 정보(마켓/호출 경로/최근 이벤트) 강화 필요

### 8) 후속 재동기화 필요성
- 현황:
  - `recoverMarketState_`는 마켓 단위 best-effort로 실패 시 경고 후 진행
  - 참조: `src/app/MarketEngineManager.cpp:223`, `src/app/MarketEngineManager.cpp:226`
  - 생성자 2차 `syncAccountWithExchange_`는 `throw_on_fail=false`로 실패 시 경고 후 진행
  - 참조: `src/app/MarketEngineManager.cpp:103`, `src/app/MarketEngineManager.cpp:203`
  - Private WS 재연결 구간에서 발생한 체결 이벤트를 놓치면, 런타임 중 즉시 보정 루프가 거의 없음
  - `myOrder`는 실시간 구독(`is_only_realtime=true`) 기반이라 재연결 공백 이벤트 재수신이 제한됨
  - 참조: `src/app/CoinBot.cpp:131`, `src/api/ws/UpbitWebSocketClient.cpp:342`, `src/api/ws/UpbitWebSocketClient.cpp:344`
  - `AccountManager::syncWithAccount`는 봇 내부 모델(전량 거래/균등 재배분) 기준으로 상태를 재구성
  - 참조: `src/trading/allocation/AccountManager.cpp:444`, `src/trading/allocation/AccountManager.cpp:509`
- 리스크:
  - 시작 시점에 복구/동기화 일부 실패가 발생해도 프로세스는 계속 실행되므로, 초기 불일치가 런타임에 남을 수 있음
  - 외부 주문 개입/이벤트 누락/일시적 API 실패가 겹치면 `available/reserved/coin` 내부 상태와 실계좌 간 drift 누적 가능
  - 재연결 사이 체결 유실 시 전략/엔진 상태(`PendingEntry/Exit`)와 실계좌 상태가 벌어져 주문 정체 또는 중복 리스크 증가
- 개선:
  - 주기적 후속 재동기화(예: N분 주기) 정책을 명시하고 운영 기본값 설정
  - 재동기화 트리거를 규정(주문 실패 급증, myOrder gap 감지, unknown market 급증 등)
  - 재동기화 결과를 지표화(동기화 성공률, drift 감지 횟수)하여 경고/알림 연동
  - 재연결 직후 강제 보정 루틴(계좌 조회 + 오픈오더 재조회 + 상태 재매핑) 도입 검토

---

## 권장 실행 순서
1. 빌드/타깃 분리 (`CoinBot` vs 테스트)  
2. 종료 경로 안정화 (2단계: 종료 관측 강화 + 하위 취소 전파)  
3. SharedOrderApi 병목 완화 (취소 우선/재시도 정책 분리)  
4. WebSocket 루프 책임 분리  
5. 후속 재동기화 정책/지표 도입  
6. 로깅 경로 통합  
7. CMake 모듈화 정리  

## 비고
- 본 문서는 정적 코드 검토 기준이며, 성능 수치/장애 재현은 별도 실험으로 보강 필요.
