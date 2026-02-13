# MarketEngineManager.cpp 상세 해설

## 1) 구현 목표 요약

`src/app/MarketEngineManager.cpp`는 멀티마켓 실행을 "초기화 -> 실행 -> 종료" 3단계로 구현한다.

핵심 목표:

1. 시작 전 상태 정합성 확보(계좌 동기화 + 복구)
2. 마켓별 워커 루프 실행
3. WS 입력을 엔진/전략으로 연결
4. 종료 시 모든 워커 안전 정리

---

## 1.1) 전체 실행 타임라인(한눈에 보기)

```text
[프로세스 시작]
  -> MarketEngineManager 생성
      -> 계좌 동기화(강제)
      -> 마켓별 엔진/전략/복구
      -> 계좌 동기화(완화)
  -> registerWith(router)
  -> start()
      -> 마켓별 workerLoop_ 진입
  -> 런타임
      -> routeMarketData/routeMyOrder
      -> handleMarketData_/handleMyOrder_
      -> engine->pollEvents() -> handleEngineEvents_
  -> stop()
      -> request_stop + join
[종료]
```

---

## 2) 파일 상단 유틸 함수

익명 네임스페이스에는 로그 가독성을 위한 보조 함수가 있다.

```cpp
const char* toStringState(...);
std::string orderSizeToLog(const core::OrderSize& size);
```

역할:
- 전략 상태 enum을 사람이 읽을 문자열로 변환
- `OrderSize` variant를 로그 친화적으로 변환

의도:
- 운영 로그를 통해 의사결정/주문 흐름을 빠르게 추적 가능하게 한다.

---

## 3) 생성자: 시작 전 정합성 단계

생성자는 단순 멤버 초기화가 아니라 "실행 전 준비 절차"를 수행한다.

핵심 순서:

1. `syncAccountWithExchange_(true)`
   - 실패 시 예외로 생성 중단
2. 마켓별 `MarketContext` 생성
3. 마켓별 `MarketEngine`, 전략 인스턴스 생성
4. `recoverMarketState_(*ctx)` 실행
5. `syncAccountWithExchange_(false)` 최종 동기화

### 3.1 중복 마켓 방어

```cpp
if (contexts_.count(market) > 0) {
    logger.warn(...);
    continue;
}
```

왜 필요한가:
- 같은 key를 덮어쓰면 기존 컨텍스트의 큐 포인터를 EventRouter가 계속 들고 있을 수 있다.
- 이 경우 라우팅 경로에 dangling 위험이 생긴다.

---

## 3.2 생성자 단계별 입출력 관점

1. 입력:
   - `api_`, `store_`, `account_mgr_`, `markets`, `cfg`
2. 내부 변화:
   - `contexts_` 구성
   - 마켓별 `engine/strategy` 생성
3. 출력:
   - 실행 가능한 멀티마켓 런타임 객체 완성
4. 실패:
   - 계좌 동기화 실패 시 예외로 생성 실패

---

## 4) 소멸자

```cpp
MarketEngineManager::~MarketEngineManager() { stop(); }
```

의미:
- 호출자가 `stop()`을 잊어도 종료 경로를 강제해 워커 누수를 막는다.

---

## 5) registerWith: 라우터와 큐 연결

```cpp
for (auto& [market, ctx] : contexts_)
    router.registerMarket(market, ctx->event_queue);
```

핵심:
- 라우터가 마켓 문자열로 바로 해당 큐를 찾도록 매핑을 설정한다.
- 워커 시작 전에 호출되어야 정상 동작한다.

---

## 6) start/stop 라이프사이클

## 6.1 start

```cpp
if (started_) return;
...
ctx->worker = std::jthread([this, &ctx_ref = *ctx](std::stop_token stoken) {
    workerLoop_(ctx_ref, stoken);
});
...
started_ = true;
```

핵심 의도:

1. `started_`로 중복 시작 방어
2. 마켓당 워커 1개 생성
3. `std::stop_token` 전달 경로 표준화

## 6.2 stop

```cpp
if (!started_) return;
for (auto& [market, ctx] : contexts_) ctx->worker.request_stop();
for (auto& [market, ctx] : contexts_) if (ctx->worker.joinable()) ctx->worker.join();
started_ = false;
```

핵심 의도:

1. 모든 워커에 일괄 종료 신호
2. 전부 join 후 종료 완료
3. 종료 순서를 명확히 하여 데드락/유실 가능성 축소

---

## 6.3 start/stop에서 주의할 동작 포인트

1. `start()`는 워커 생성만 수행하고 라우터 등록은 하지 않는다.
   - 즉 `registerWith()`를 먼저 호출해야 이벤트가 들어온다.
2. `stop()`는 `started_`가 false면 바로 반환한다.
   - 라이프사이클 꼬임이 있으면 정지 요청이 누락될 수 있으므로 호출 순서가 중요하다.
3. `jthread`를 사용하므로 stop 신호 전달 경로가 명확하다.

---

## 7) syncAccountWithExchange_: 시작 계좌 동기화

동작:

1. `api_.getMyAccount()` 호출
2. 성공 시 `account_mgr_.syncWithAccount(...)`
3. 실패 시 `sync_retry` 횟수만큼 재시도
4. `throw_on_fail`이면 예외, 아니면 경고 후 진행

의미:
- 시작 시점의 잔고/포지션 불일치를 최소화한다.
- 생성자 1차 동기화는 강제, 2차 동기화는 완화 정책으로 구분된다.

---

## 7.1 실패 정책 표

| 호출 위치 | throw_on_fail | 실패 시 동작 |
|---|---:|---|
| 생성자 초반 | true | 예외 발생, Manager 생성 실패 |
| 생성자 후반 | false | 경고 로그 후 진행 |

의도:
- 시작 자체가 불가능한 상태는 빠르게 실패
- 복구 이후 재동기화 실패는 가용성을 위해 허용

---

## 8) recoverMarketState_: 마켓별 시작 복구

핵심:

1. 전략 ID + 마켓 기반 prefix 구성
2. `StartupRecovery::run(...)` 호출
3. 실패는 해당 마켓 경고로만 처리(전체 중단 안 함)

이 설계는 "가용성 우선" 정책이다.

---

## 9) workerLoop_: 실행 핵심 루프

구조:

1. 엔진 소유 스레드 바인딩
2. 시작 로그
3. `while (!stoken.stop_requested())`
4. 입력 이벤트 1건 처리
5. 엔진 출력 이벤트 배치 처리
6. 종료 로그

### 9.1 바인딩 실패 방어

```cpp
try { ctx.engine->bindToCurrentThread(); }
catch (const std::exception& e) { ... return; }
```

의도:
- 마켓 워커 하나의 실패가 프로세스 전체 종료로 전파되지 않게 한다.

### 9.2 루프 내부 예외 격리

```cpp
try {
    auto maybe = ctx.event_queue.pop_for(200ms);
    ...
} catch (const std::exception& e) {
    logger.error(...);
}
```

의도:
- 나쁜 이벤트 1건으로 워커가 죽지 않도록 보호
- "해당 이벤트 skip 후 다음 이벤트" 전략 유지

### 9.3 `pop_for(200ms)`의 의미

1. 종료 신호 polling 간격
2. 이벤트 없을 때 무한 대기 회피
3. CPU busy-spin 방지

trade-off:
- 종료/반응 지연 상한이 생긴다(최대 대기 구간).

---

## 9.4 workerLoop 상세 순서(반복 1회 기준)

```text
1) stop_token 확인
2) 큐에서 이벤트 1건 대기(pop_for 200ms)
3) 이벤트가 있으면 handleOne_ 실행
4) 엔진 내부 누적 이벤트 pollEvents()
5) 이벤트 있으면 handleEngineEvents_ 실행
6) 루프 상단으로 복귀
```

실패 시:
- 2~5 단계에서 `std::exception` 발생 -> 로그 후 다음 반복
- 워커 스레드 자체는 유지

---

## 10) handleOne_: 입력 분기점

`EngineInput` variant를 분기한다.

```cpp
if constexpr (std::is_same_v<T, MyOrderRaw>) ...
else if constexpr (std::is_same_v<T, MarketDataRaw>) ...
```

이 함수는 "입력 디스패처" 역할만 수행하고,
실제 파싱/도메인 처리는 하위 함수로 넘긴다.

---

## 10.1 입력 타입별 처리 책임 요약

| 입력 타입 | 담당 함수 | 핵심 동작 |
|---|---|---|
| `MyOrderRaw` | `handleMyOrder_` | 주문/체결 이벤트를 엔진에 반영 |
| `MarketDataRaw` | `handleMarketData_` | 캔들 기반 전략 실행 및 주문 제출 |

---

## 11) handleMyOrder_: 주문 이벤트 경로

처리 순서:

1. JSON parse
2. DTO 변환
3. mapper로 이벤트 분해(`Order` / `MyTrade`)
4. 엔진 반영
   - `onOrderSnapshot`
   - `onMyTrade`

핵심 포인트:

- 파싱/DTO 실패는 로그 후 return
- 엔진은 단일 스레드 소유권 규칙으로 호출됨(worker thread)

---

## 11.1 handleMyOrder 실패 지점과 결과

1. JSON parse 실패
   - 로그 후 해당 이벤트 드롭
2. DTO 변환 실패
   - 로그 후 해당 이벤트 드롭
3. mapper 결과 처리 중 엔진 예외
   - workerLoop의 상위 try-catch에서 격리됨

즉 "나쁜 myOrder 메시지 1건"이 워커 전체 중단으로 이어지지 않도록 설계되어 있다.

---

## 12) handleMarketData_: 시장 데이터 경로

처리 순서:

1. JSON parse
2. `type`이 `candle` prefix인지 확인
3. DTO 변환 후 도메인 캔들 변환
4. 동일 timestamp 중복 제거
5. 계좌 스냅샷 생성
6. 전략 실행
7. 주문 의도 존재 시 `engine->submit`
8. submit 실패면 `strategy->onSubmitFailed()`

### 12.1 중복 캔들 제거

```cpp
if (!ctx.last_candle_ts.empty() && ctx.last_candle_ts == candle.start_timestamp)
    return;
ctx.last_candle_ts = candle.start_timestamp;
```

의도:
- 같은 캔들 시각의 반복 업데이트로 중복 의사결정이 발생하는 것을 방지

---

## 12.2 handleMarketData 단계별 데이터 흐름

```text
Raw JSON
  -> nlohmann::json 파싱
  -> Candle DTO 변환
  -> core::Candle 변환
  -> 중복 timestamp 필터
  -> AccountSnapshot 생성
  -> strategy->onCandle(candle, snapshot)
  -> (주문 의도 있으면) engine->submit
  -> submit 실패면 strategy rollback(onSubmitFailed)
```

핵심:
- 전략과 엔진 사이에서 "의도(Decision)"를 경계 타입으로 사용한다.

---

## 13) handleEngineEvents_: 엔진 출력 환류

엔진 이벤트를 전략 이벤트로 변환한다.

1. `EngineFillEvent` -> `FillEvent` -> `strategy->onFill`
2. `EngineOrderStatusEvent` -> `OrderStatusEvent` -> `strategy->onOrderUpdate`

의미:
- 전략은 거래소/엔진 세부 타입을 몰라도 상태를 갱신할 수 있다.

---

## 13.1 왜 환류 단계가 필요한가

`handleMarketData_`만으로는 전략 상태가 완성되지 않는다.

예:
- 주문 제출 성공 후 실제 체결
- 주문 상태 변화(Filled/Canceled/Rejected)

이 정보는 엔진에서 이벤트로 나오고, 이를 전략으로 다시 넣어야
전략 상태와 실제 주문 상태가 일치한다.

---

## 14) buildAccountSnapshot_: 전략 입력 축약

```cpp
auto budget = account_mgr_.getBudget(market);
snap.krw_available = budget->available_krw;
snap.coin_available = budget->coin_balance;
```

의도:
- 전략에 필요한 최소 상태만 전달
- 계좌 구조 변경의 영향을 전략 인터페이스에서 완화

---

## 15) 운영 관점에서 읽어야 할 포인트

1. 시작 시 강한 동기화 + 복구로 정합성 확보
2. 워커 예외를 로컬 격리해 프로세스 중단 위험 완화
3. 종료는 `jthread` 기반 일괄 stop/join
4. 마켓별 컨텍스트 분리로 멀티마켓 간 기본 격리 유지

주의:
- 큐 정책(용량/드롭)과 이벤트 우선순위는 라우터/큐 설계와 함께 봐야 한다.

---

## 15.1 장애 관점 체크포인트

1. 계좌 동기화 실패
   - 시작 중단인지, 경고 후 진행인지 위치별로 다름
2. 라우팅 누락
   - `registerWith()` 미호출 시 이벤트가 워커에 전달되지 않음
3. 입력 데이터 품질
   - JSON/DTO 오류는 이벤트 단위로 스킵됨
4. 워커 종료
   - `stop_token` 기반이며 최대 poll 간격만큼 지연 가능

---

## 16) 학습 순서 추천

1. 생성자 + `syncAccountWithExchange_`
2. `start`/`stop`
3. `workerLoop_`
4. `handleMarketData_` -> `handleEngineEvents_` 흐름
5. 마지막으로 테스트 매핑(`tests/test_market_engine_manager.cpp`) 대조

---

## 16.1 문서 기반 빠른 디버깅 가이드

문제 현상별로 볼 위치:

1. "시작 직후 종료됨"
   - `syncAccountWithExchange_`, 생성자 예외 경로
2. "이벤트가 안 먹음"
   - `registerWith`, `start`, `handleOne_` 진입 여부
3. "캔들은 오는데 주문이 안 나감"
   - `handleMarketData_`의 type 필터/중복 필터/strategy decision
4. "전략 상태가 안 맞음"
   - `handleEngineEvents_` 환류 경로

---

## 17) 함께 보면 좋은 파일

- `src/app/MarketEngineManager.h`
- `src/app/EventRouter.cpp`
- `src/app/StartupRecovery.cpp`
- `src/engine/MarketEngine.cpp`
- `src/trading/allocation/AccountManager.cpp`
- `tests/test_market_engine_manager.cpp`
