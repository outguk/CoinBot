# MarketEngineManager.h 상세 해설

## 1) 파일 역할

`src/app/MarketEngineManager.h`는 멀티마켓 실행의 "제어면(control plane)"을 정의한다.

이 파일이 맡는 범위:

1. 마켓별 실행 컨텍스트(엔진/전략/입력큐/워커) 구조 정의
2. 라이프사이클 API(`registerWith`, `start`, `stop`) 제공
3. 계좌 동기화/시작 복구/이벤트 처리 함수 시그니처 선언

핵심 개념:

- `MarketEngine`는 "마켓 1개 전용 엔진"
- `MarketEngineManager`는 "여러 MarketEngine 묶음 운영자"

---

## 1.2) 이 파일만 읽고 전체 구조를 보는 방법

이 헤더는 아래 4가지만 보면 전체 그림이 잡힌다.

1. 공개 API: `registerWith`, `start`, `stop`
2. 컨텍스트: `MarketContext` (마켓 단위 실행 상태)
3. 워커 진입점: `workerLoop_`
4. 입력/출력 핸들러: `handle*` 계열

빠른 구조도:

```text
EventRouter(JSON 라우팅)
    -> MarketContext.event_queue (마켓별)
        -> workerLoop_
            -> handleMyOrder_ / handleMarketData_
                -> MarketEngine 반영
                -> 전략 실행
                -> EngineEvent 환류(handleEngineEvents_)
```

---

## 1.1) 공개 인터페이스

```cpp
class MarketEngineManager final {
public:
    MarketEngineManager(...);
    ~MarketEngineManager();

    void registerWith(EventRouter& router);
    void start();
    void stop();
};
```

생명주기 계약:

```text
생성자(초기 동기화/복구)
    -> registerWith(EventRouter)
    -> start()
    -> stop()
```

---

## 2) Config: 런타임 정책 묶음

```cpp
struct Config {
    trading::strategies::RsiMeanReversionStrategy::Params strategy_params;
    std::size_t queue_capacity = 5000;
    int sync_retry = 3;
};
```

각 필드 의미:

1. `strategy_params`
   - 마켓별 전략 인스턴스 생성 시 공통 파라미터
2. `queue_capacity`
   - 마켓별 입력 큐 상한
   - 큐 구현(BlockingQueue)의 drop-oldest 정책과 결합됨
3. `sync_retry`
   - 시작 시 계좌 동기화 재시도 횟수

운영 관점:
- 전략 튜닝과 실행 안정성(큐 용량/재시도)을 하나의 설정 구조로 묶어
  실행 파라미터 주입 경로를 단순화한다.

---

## 2.1) Config 필드가 실제 동작에 미치는 영향

1. `strategy_params`
   - `handleMarketData_`에서 전략 실행 결과를 바꾼다.
   - 즉 주문 빈도/타이밍에 직접 영향.
2. `queue_capacity`
   - 라우터 -> 큐 push 단계의 손실 정책에 영향.
   - 과부하 시 오래된 이벤트가 제거될 수 있음(큐 구현 정책).
3. `sync_retry`
   - 시작 단계(`syncAccountWithExchange_`)의 실패 내성에 영향.
   - 값이 작으면 빠르게 실패, 크면 시작 지연이 길어짐.

---

## 3) 핵심 타입: MarketContext

```cpp
struct MarketContext {
    std::string market;
    std::unique_ptr<engine::MarketEngine> engine;
    std::unique_ptr<trading::strategies::RsiMeanReversionStrategy> strategy;
    PrivateQueue event_queue;
    std::jthread worker;
    std::string last_candle_ts;
};
```

### 3.1 필드별 책임

1. `market`
   - 컨텍스트 소유 마켓 키 (`KRW-BTC` 등)
2. `engine`
   - 주문/체결/상태 반영 도메인 엔진
3. `strategy`
   - 캔들 기반 의사결정 로직
4. `event_queue`
   - `EventRouter`가 넣는 입력 버퍼
5. `worker`
   - 해당 마켓 루프 실행 스레드
6. `last_candle_ts`
   - 중복 캔들 무시용 캐시

### 3.2 왜 묶어서 관리하나

- 마켓별 실행 상태를 단일 객체로 유지하면
  생성/시작/종료/디버깅 단위가 동일해진다.
- 멀티마켓에서 "어느 마켓의 어떤 상태가 깨졌는지" 추적이 쉬워진다.

---

## 3.3 MarketContext 생명주기

```text
생성자:
  market 문자열 저장
  event_queue 용량 설정
  engine/strategy는 nullptr

MarketEngineManager 생성자:
  engine 생성
  strategy 생성
  복구 실행

start():
  worker 스레드 생성

stop()/소멸:
  request_stop -> join
```

---

## 4) private 함수 시그니처가 의미하는 설계

```cpp
void syncAccountWithExchange_(bool throw_on_fail);
void recoverMarketState_(MarketContext& ctx);
void workerLoop_(MarketContext& ctx, std::stop_token stoken);
```

설계 포인트:

1. `syncAccountWithExchange_`
   - 실행 전 실제 계좌 상태를 메모리 모델(`AccountManager`)에 반영
2. `recoverMarketState_`
   - 시작 시 미체결/포지션 복구 책임 분리
3. `workerLoop_`
   - 워커 진입점 통합
   - `stop_token` 기반 종료 신호 표준화

---

## 4.1 함수 분리 기준(왜 이 이름/경계인지)

1. `syncAccountWithExchange_`
   - "거래소 외부 상태 -> 로컬 모델" 동기화 책임만 가짐
2. `recoverMarketState_`
   - 시작 시점 복구 정책만 담당
3. `workerLoop_`
   - 런타임 처리만 담당(이벤트 소비/환류)
4. `handleOne_`
   - 입력 타입 분기만 담당
5. `handleMyOrder_`, `handleMarketData_`
   - 입력 종류별 파싱/도메인 반영 책임 분리
6. `handleEngineEvents_`
   - 엔진 출력을 전략으로 전달하는 환류 단계 분리

이 분리 덕분에 "문제 위치"를 기능별로 좁혀 디버깅하기 쉽다.

---

## 5) 이벤트 처리 함수 분리

```cpp
void handleOne_(MarketContext& ctx, const engine::input::EngineInput& in);
void handleMyOrder_(MarketContext& ctx, const engine::input::MyOrderRaw& raw);
void handleMarketData_(MarketContext& ctx, const engine::input::MarketDataRaw& raw);
void handleEngineEvents_(MarketContext& ctx, const std::vector<engine::EngineEvent>& evs);
```

분리 의도:

1. variant 분기(`handleOne_`)
2. 주문 이벤트 경로(`handleMyOrder_`)
3. 시장 데이터 경로(`handleMarketData_`)
4. 엔진 출력 이벤트를 전략으로 환류(`handleEngineEvents_`)

이 구조로 인해 디버깅 시 "입력 경로"와 "출력 경로"를 분리해서 볼 수 있다.

---

## 6) 멤버 상태 설계

```cpp
api::upbit::IOrderApi& api_;
engine::OrderStore& store_;
trading::allocation::AccountManager& account_mgr_;
Config cfg_;
std::unordered_map<std::string, std::unique_ptr<MarketContext>> contexts_;
bool started_{false};
```

### 6.1 공유 의존성과 경계

1. `api_`: 외부 거래소 I/O
2. `store_`: 마켓들이 공유하는 주문 저장소
3. `account_mgr_`: 마켓별 자산 예산/정산

### 6.2 실행 상태

1. `contexts_`: 마켓별 장기 수명 컨테이너
2. `started_`: 라이프사이클 재진입 방지 플래그

---

## 6.1 스레드 관점에서 본 멤버 접근 경계

1. 주 스레드(초기화/종료):
   - `contexts_` 생성
   - `registerWith`, `start`, `stop` 호출
2. 워커 스레드(마켓별):
   - 해당 `MarketContext`의 `engine`, `strategy`, `event_queue` 소비
   - `last_candle_ts` 갱신
3. 공유 객체:
   - `api_`, `store_`, `account_mgr_`는 멀티워커에서 공유
   - thread-safe 가정은 각 클래스 구현이 보장해야 함

---

## 7) 스레드 모델 요약

이 헤더에서 확정되는 스레드 모델:

1. 마켓당 워커 1개 (`std::jthread`)
2. 종료는 `request_stop`/`stop_token` 계약 사용
3. `MarketEngine`는 워커 스레드에 바인딩되어 단일 스레드 호출 규칙 유지

즉 "멀티마켓 = 멀티워커"지만, 각 엔진 내부는 단일 스레드 안전 모델이다.

---

## 8) 학습 시 체크해야 할 질문

1. `registerWith`를 빼먹으면 어떤 일이 생기는가
2. `queue_capacity`가 작으면 어떤 이벤트가 먼저 손실되는가
3. 시작 복구/계좌 동기화 실패 정책이 어떻게 정의되어 있는가
4. 워커 예외가 프로세스 전체에 미치는 영향은 무엇인가

이 질문은 구현 파일(`.cpp`) 읽을 때 바로 대응된다.

---

## 8.1 체크리스트(문서 독자용)

아래를 순서대로 보면 구조 파악이 빠르다.

1. `MarketContext`를 보고 "마켓 1개 실행 단위"를 이해한다.
2. `start/stop` 시그니처를 보고 "워커 관리 모델"을 이해한다.
3. `workerLoop_`/`handle*` 선언을 보고 "입력 -> 엔진 -> 전략" 흐름을 그린다.
4. `syncAccountWithExchange_`/`recoverMarketState_`를 보고 "시작 전 정합성 단계"를 확인한다.

---

## 9) 함께 보면 좋은 파일

- `src/app/MarketEngineManager.cpp`
- `src/app/EventRouter.h`
- `src/app/EventRouter.cpp`
- `src/engine/MarketEngine.h`
- `src/engine/MarketEngine.cpp`
- `tests/test_market_engine_manager.cpp`
