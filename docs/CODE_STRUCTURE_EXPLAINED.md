# CoinBot 구조 설명 메모

최종 갱신: 2026-03-12

이 문서는 실제 질문이 많이 나왔던 구조 요소를 기준으로 CoinBot 코드를 읽는 방법을 정리한 학습용 문서다.

다루는 주제는 아래와 같다.

- 포인터(`*`)와 참조(`&`)를 왜 구분해서 쓰는지
- `AccountManager::rebuildFromAccount()`가 의존하는 마켓 예산이 어디서 초기화되는지
- `std::function`, 람다, 콜백 등록 구조가 이 프로젝트에서 어떻게 연결되는지
- `std::jthread`와 `[this]` 캡처가 왜 필요한지
- WebSocket의 `ping`이 무엇인지
- `BlockingQueue`와 `std::deque`를 왜 섞어 쓰는지
- `pop_for(50ms)`가 실제로 어떻게 동작하는지
- `std::variant`와 `std::visit`가 프로젝트에서 어떻게 쓰이는지

---

## 1. 이 프로젝트에서 참조와 포인터를 읽는 기준

이 프로젝트는 의도가 비교적 분명하다.

- 참조(`T&`): 반드시 있어야 하는 의존성
- 포인터(`T*`): 없을 수도 있는 선택 의존성, 또는 C API 핸들
- 스마트 포인터(`std::unique_ptr<T>`): 소유권 이전 또는 단일 소유

### 1-1. 반드시 필요한 객체는 참조로 받는다

`MarketEngineManager`는 핵심 의존성을 참조로 받는다.

- `api::upbit::IOrderApi&`
- `engine::OrderStore&`
- `trading::allocation::AccountManager&`

이 셋은 없으면 매니저가 동작할 수 없기 때문이다.

관련 코드:

- `src/app/MarketEngineManager.h`
- `src/app/MarketEngineManager.cpp`

```cpp
MarketEngineManager(api::upbit::IOrderApi& api,
                    engine::OrderStore& store,
                    trading::allocation::AccountManager& account_mgr,
                    const std::vector<std::string>& markets,
                    MarketManagerConfig cfg = MarketManagerConfig{},
                    db::Database* db = nullptr);
```

이 생성자는 내부 멤버도 참조로 보관한다.

```cpp
api::upbit::IOrderApi& api_;
engine::OrderStore& store_;
trading::allocation::AccountManager& account_mgr_;
```

즉 생성 시점부터 "반드시 유효해야 한다"는 계약이 코드에 드러난다.

### 1-2. 선택 기능은 포인터로 받는다

반대로 `db::Database* db = nullptr`는 포인터다.

이유는 DB 기록이 선택 기능이기 때문이다. DB가 없어도 봇 핵심 로직은 돌 수 있고, 단지 기록만 생략하면 된다.

관련 코드:

- `src/app/MarketEngineManager.h`
- `src/app/MarketEngineManager.cpp`

```cpp
db::Database* db_{ nullptr };
```

실사용도 항상 null 체크를 거친다.

```cpp
if (db_) {
    db_->insertSignal(sig);
}
```

즉 이 프로젝트에서 포인터는 "없을 수도 있음"이라는 의미를 자주 가진다.

### 1-3. 참조로 받고 포인터로 저장하는 경우도 있다

`EventRouter::registerMarket()`는 큐를 참조로 받지만, 내부 맵에는 포인터로 저장한다.

관련 코드:

- `src/app/EventRouter.h`
- `src/app/EventRouter.cpp`

```cpp
void registerMarket(const std::string& market, PrivateQueue& queue);
std::unordered_map<std::string, PrivateQueue*> routes_;
```

이 경우의 의미는 다음과 같다.

- 등록 시점에는 큐가 반드시 있어야 하므로 입력은 참조
- 하지만 컨테이너에 참조를 직접 저장하기 불편하므로 내부 보관은 포인터

즉 "입력 계약"과 "내부 보관 방식"은 다를 수 있다.

### 1-4. 스마트 포인터는 소유권 의미가 있다

`SharedOrderApi`는 `UpbitExchangeRestClient`를 `std::unique_ptr`로 받는다.

관련 코드:

- `src/api/upbit/SharedOrderApi.h`
- `src/api/upbit/SharedOrderApi.cpp`

```cpp
explicit SharedOrderApi(std::unique_ptr<api::rest::UpbitExchangeRestClient> client);
```

이 뜻은 단순 전달이 아니라 "이제 `SharedOrderApi`가 이 객체의 단일 소유자가 된다"는 뜻이다.

---

## 2. `budgets_`는 어디서 처음 채워지는가

질문 포인트는 `rebuildFromAccount()`는 기존 예산을 재구성하는데, 마켓 키는 언제 들어가느냐는 것이었다.

정답은 `AccountManager` 생성자다.

관련 코드:

- `src/app/CoinBot.cpp`
- `src/trading/allocation/AccountManager.cpp`

### 2-1. 마켓 목록은 `CoinBot.cpp`에서 준비된다

`loadMarkets()`가 환경 변수 또는 기본 설정에서 마켓 목록을 만든다.

그 다음 `AccountManager`를 이렇게 생성한다.

```cpp
trading::allocation::AccountManager account_mgr(core::Account{}, markets);
```

### 2-2. 생성자에서 `budgets_`의 틀을 먼저 만든다

`AccountManager` 생성자는 `markets`를 순회하면서 먼저 모든 마켓 예산을 0으로 초기화한다.

```cpp
for (const auto& market : markets) {
    MarketBudget budget;
    budget.market = market;
    ...
    budgets_[market] = std::move(budget);
}
```

즉 이 단계에서 `KRW-BTC`, `KRW-ETH` 같은 마켓 키가 `budgets_`에 들어간다.

### 2-3. `rebuildFromAccount()`는 새 마켓을 추가하지 않는다

그 이후 `MarketEngineManager`가 계좌를 동기화하면서 `rebuildFromAccount()`를 호출한다.

```cpp
account_mgr_.rebuildFromAccount(std::get<core::Account>(result));
```

이 함수는 `budgets_`를 새로 만드는 것이 아니라, 이미 존재하는 마켓 예산의 값만 실제 계좌 기준으로 다시 채운다.

즉 역할 분리는 아래와 같다.

- 생성자: 마켓 등록
- `rebuildFromAccount()`: 실제 자산 상태 재반영

---

## 3. `std::function`, 람다, 콜백은 어떻게 연결되는가

이 프로젝트에서 콜백 구조는 매우 중요하다.

핵심 개념은 아래 한 줄로 정리된다.

- `std::function`: 나중에 호출할 함수를 담아두는 슬롯
- 람다: 그 슬롯에 넣는 실제 동작
- 콜백: 이벤트가 생겼을 때 저장해 둔 함수를 호출하는 구조

### 3-1. WebSocket 메시지 처리 예시

`UpbitWebSocketClient`는 메시지를 받았을 때 무엇을 할지 스스로 결정하지 않는다. 대신 외부에서 함수 하나를 등록받는다.

관련 코드:

- `src/api/ws/UpbitWebSocketClient.h`
- `src/api/ws/UpbitWebSocketClient.cpp`

```cpp
using MessageHandler = std::function<void(std::string_view)>;
MessageHandler on_msg_;

void setMessageHandler(MessageHandler cb) {
    on_msg_ = std::move(cb);
}
```

이 슬롯에 `CoinBot.cpp`가 람다를 등록한다.

```cpp
ws_public.setMessageHandler([&router](std::string_view json) {
    (void)router.routeMarketData(json);
});
```

이 람다의 뜻은 아래와 같다.

- `json` 문자열을 입력으로 받고
- `router.routeMarketData(json)`를 호출한다
- 반환값 `bool`은 `(void)`로 버린다

실제 메시지가 도착하면 WebSocket 클라이언트가 나중에 이 함수를 호출한다.

```cpp
if (on_msg_)
    on_msg_(std::string_view(msg));
```

즉 흐름은 다음과 같다.

1. 외부에서 메시지 처리 함수를 등록
2. WebSocket 내부에 저장
3. 수신 시점에 저장된 함수를 호출

### 3-2. 전략 신호 저장도 같은 구조다

전략은 "신호가 발생했다"는 사실만 알고, 그걸 DB에 저장할지는 모른다. 그래서 `SignalCallback`을 둔다.

관련 코드:

- `src/trading/strategies/StrategyTypes.h`
- `src/trading/strategies/RsiMeanReversionStrategy.h`
- `src/trading/strategies/RsiMeanReversionStrategy.cpp`
- `src/app/MarketEngineManager.cpp`

```cpp
using SignalCallback = std::function<void(const SignalRecord&)>;
```

전략 객체에 콜백을 등록하는 쪽은 `MarketEngineManager`다.

```cpp
ctx->strategy->setSignalCallback([this](const trading::SignalRecord& sig) {
    db_->insertSignal(sig);
});
```

그리고 전략 내부에서는 신호 확정 시 아래처럼 호출한다.

```cpp
if (signal_callback_) {
    signal_callback_(sig);
}
```

즉 전략은 DB를 직접 모르고도 신호를 저장할 수 있다.

이 구조의 장점은 계층 분리다.

- 전략: 신호 발생 판단
- 매니저: 그 신호를 DB에 기록할지 결정

### 3-3. 람다의 `()`는 반환형이 아니라 입력 인자다

초보자가 자주 헷갈리는 부분이 `std::function<void(std::string_view)>` 같은 표기다.

이 표기는 아래 의미다.

- 반환형: `void`
- 인자: `std::string_view`

즉

```cpp
std::function<void(std::string_view)>
```

는 아래 함수 모양과 같다.

```cpp
void handler(std::string_view json);
```

따라서 아래 람다는 타입이 맞다.

```cpp
[&router](std::string_view json) {
    (void)router.routeMarketData(json);
}
```

여기서 `std::string_view json`은 반환값이 아니라 입력 매개변수다.

---

## 4. `std::jthread`, 람다, `[this]` 캡처

`UpbitWebSocketClient::start()`는 내부 수신 루프를 별도 스레드에서 실행한다.

관련 코드:

- `src/api/ws/UpbitWebSocketClient.cpp`
- `src/api/ws/UpbitWebSocketClient.h`

```cpp
thread_ = std::jthread([this](std::stop_token stoken) {
    runReadLoop_(stoken);
});
```

### 4-1. 왜 `jthread`의 인자로 람다가 들어가는가

`std::jthread`는 "새 스레드에서 실행할 callable"을 받는다.

람다는 callable이므로 바로 넣을 수 있다.

즉 이 코드는 사실상 아래 뜻이다.

- 새 스레드를 하나 만들고
- 그 스레드에서 이 람다를 실행하고
- 람다 안에서 `runReadLoop_(stoken)`를 호출한다

### 4-2. `[this]`는 무엇인가

`this`는 현재 객체 자신을 가리키는 포인터다.

여기서는 현재 `UpbitWebSocketClient` 객체를 뜻한다.

`[this]`가 필요한 이유는 `runReadLoop_()`가 멤버 함수이기 때문이다. 멤버 함수는 어느 객체에 대해 호출할지 알아야 하므로, 현재 객체 포인터가 필요하다.

즉 람다 안 코드는 사실상 아래와 같다.

```cpp
this->runReadLoop_(stoken);
```

### 4-3. `stop_token`은 무엇에 쓰는가

`runReadLoop_()`는 `std::stop_token`을 인자로 받는다.

```cpp
void runReadLoop_(std::stop_token stoken);
```

그래서 외부에서 `thread_.request_stop()`을 호출하면, 루프 안에서 종료 요청을 감지하고 빠져나올 수 있다.

---

## 5. WebSocket의 `ping`이 의미하는 것

이 프로젝트의 WebSocket 클라이언트는 ping 주기를 멤버로 가진다.

관련 코드:

- `src/api/ws/UpbitWebSocketClient.h`

```cpp
std::chrono::seconds ping_interval_{ 25 };
```

WebSocket의 `ping`은 연결 생존 확인용 제어 프레임이다.

쉽게 말하면:

- 클라이언트가 "아직 살아 있냐"는 작은 신호를 보냄
- 서버는 보통 `pong`으로 응답
- 이를 통해 연결이 죽었는지, 유휴 연결이 끊겼는지 빠르게 감지

주요 목적은 아래와 같다.

- 연결 생존 확인
- 유휴 연결 유지
- 죽은 연결 조기 탐지

즉 비즈니스 데이터가 아니라 프로토콜 차원의 헬스체크다.

---

## 6. `BlockingQueue`와 `std::deque`는 왜 같이 쓰는가

둘은 겉모습이 비슷하지만, 역할이 다르다.

- `std::deque`: 그냥 자료구조
- `BlockingQueue`: `deque` + 락 + 조건변수 + timeout 대기

관련 코드:

- `src/core/BlockingQueue.h`

### 6-1. `BlockingQueue`는 스레드 간 전달 채널이다

`BlockingQueue` 내부 구현은 `std::deque<T>`지만, 여기에 아래 기능이 붙어 있다.

- `std::mutex`
- `std::condition_variable`
- `try_pop()`
- `pop_for(timeout)`
- 최대 크기 초과 시 `drop-oldest`

즉 일반 컨테이너가 아니라 "대기 가능한 스레드 안전 큐"다.

이 프로젝트에서 마켓별 `event_queue`가 바로 이 타입이다.

관련 코드:

- `src/app/MarketEngineManager.h`
- `src/app/EventRouter.h`

```cpp
using PrivateQueue = core::BlockingQueue<engine::input::EngineInput>;
```

이 큐는 아래처럼 스레드 간 이벤트 전달에 쓰인다.

- WS/EventRouter 쪽이 push
- 마켓 워커 스레드가 pop

따라서 blocking 기능이 필요하다.

### 6-2. `std::deque`는 내부 버퍼나 FIFO 기록용이다

프로젝트에서 `deque`는 주로 같은 스레드 안의 내부 버퍼로 사용된다.

예시:

- `src/api/ws/UpbitWebSocketClient.h`의 `cmd_q_`
- `src/engine/MarketEngine.h`의 `events_`
- `src/engine/MarketEngine.h`의 `seen_trade_uuid_fifo_`

각각 의미는 다르지만 공통점은 아래와 같다.

- 단순 FIFO 순서만 필요
- 직접 락을 제어하거나
- 같은 owner thread 안에서만 사용
- `BlockingQueue`처럼 wait/notify 기능이 필요하지 않음

예를 들어 `MarketEngine::events_`는 엔진 내부에서 이벤트를 쌓았다가 `pollEvents()` 호출 시 한 번에 비운다.

```cpp
while (!events_.empty())
{
    out.emplace_back(std::move(events_.front()));
    events_.pop_front();
}
```

이건 스레드 간 blocking 채널이 아니라 내부 임시 버퍼이므로 그냥 `deque`면 충분하다.

### 6-3. 왜 `cmd_q_`도 `BlockingQueue`가 아닌가

`UpbitWebSocketClient`의 `cmd_q_`는 하나씩 blocking pop 하지 않고, 락을 잠깐 잡은 뒤 `swap`으로 통째로 로컬 큐에 옮겨 처리한다.

```cpp
std::deque<Command> local;
{
    std::lock_guard lk(cmd_mu_);
    local.swap(cmd_q_);
}
```

즉 이쪽은 "대기형 소비자 큐"보다 "배치로 가져와 처리하는 커맨드 버퍼"에 가깝다.

---

## 7. `event_queue.pop_for(50ms)`는 실제로 어떻게 동작하는가

관련 코드:

- `src/core/BlockingQueue.h`
- `src/app/MarketEngineManager.cpp`

`pop_for()`는 한 번에 여러 개를 꺼내지 않는다. 최대 1개만 꺼낸다.

```cpp
T v = std::move(q_.front());
q_.pop_front();
return v;
```

`MarketEngineManager::workerLoop_()`도 그 전제로 작성되어 있다.

```cpp
auto maybe = ctx.event_queue.pop_for(50ms);

if (maybe.has_value())
    handleOne_(ctx, *maybe);
```

즉 워커 루프의 한 사이클은 다음과 같다.

1. 최대 50ms 동안 이벤트를 기다림
2. 이벤트가 오면 1개만 꺼냄
3. 그 1개를 처리
4. 엔진 이벤트 후처리와 타임아웃 체크 실행
5. 다시 다음 루프로 감

### 7-1. 50ms가 지나면 실패하는가

아니다. 타임아웃일 뿐이다.

큐가 비어 있으면 `pop_for(50ms)`는 `std::nullopt`를 반환한다.

그 경우:

- `handleOne_()`는 호출되지 않음
- 하지만 루프는 계속 진행
- `pollEvents()`, `checkPendingTimeout_()` 등은 계속 수행

즉 이 50ms는 "이벤트가 없더라도 주기적으로 깨어나서 점검하라"는 의미다.

---

## 8. `std::variant`와 `std::visit`를 어떻게 읽어야 하는가

이 프로젝트는 타입별 분기를 문자열이 아니라 `std::variant`로 표현하는 부분이 있다.

초보자 관점에서는 아래처럼 이해하면 된다.

- `variant`: 여러 타입 중 하나를 담는 상자
- `visit`: 그 상자 안에 실제로 뭐가 들었는지 보고 맞는 코드를 실행

### 8-1. `EngineInput` 예시

관련 코드:

- `src/engine/input/EngineInput.h`
- `src/app/MarketEngineManager.cpp`

```cpp
using EngineInput = std::variant<MyOrderRaw, MarketDataRaw, AccountSyncRequest>;
```

즉 `EngineInput` 하나에는 아래 셋 중 하나만 들어 있다.

- `MyOrderRaw`
- `MarketDataRaw`
- `AccountSyncRequest`

이걸 처리하는 코드가 `handleOne_()`다.

```cpp
std::visit([&](const auto& x)
{
    using T = std::decay_t<decltype(x)>;

    if constexpr (std::is_same_v<T, engine::input::MyOrderRaw>)
        handleMyOrder_(ctx, x);
    else if constexpr (std::is_same_v<T, engine::input::MarketDataRaw>)
        handleMarketData_(ctx, x);
    else if constexpr (std::is_same_v<T, engine::input::AccountSyncRequest>)
        runRecovery_(ctx);
}, in);
```

이걸 사람 말로 바꾸면 아래와 같다.

- `in` 안에 실제로 `MyOrderRaw`가 들어 있으면 `handleMyOrder_`
- `MarketDataRaw`가 들어 있으면 `handleMarketData_`
- `AccountSyncRequest`면 `runRecovery_`

즉 `std::visit`는 variant 전용 switch 문처럼 읽으면 된다.

### 8-2. `OrderSize` 예시

관련 코드:

- `src/core/domain/OrderRequest.h`
- `src/app/MarketEngineManager.cpp`

```cpp
using OrderSize = std::variant<VolumeSize, AmountSize>;
```

즉 주문 크기는 아래 둘 중 하나다.

- 코인 수량 기반
- KRW 금액 기반

로그 문자열로 바꾸는 코드가 아래다.

```cpp
return std::visit([](const auto& s) -> std::string {
    using T = std::decay_t<decltype(s)>;
    std::ostringstream oss;
    if constexpr (std::is_same_v<T, core::VolumeSize>)
        oss << "VOL=" << s.value;
    else if constexpr (std::is_same_v<T, core::AmountSize>)
        oss << "AMOUNT=" << s.value;
    else
        oss << "UNKNOWN";
    return oss.str();
}, size);
```

이 의미는 단순하다.

- 수량형이면 `VOL=...`
- 금액형이면 `AMOUNT=...`

즉 `visit`는 현재 들어 있는 실제 타입에 맞는 문자열 변환 로직을 실행한다.

---

## 9. 이 문서를 읽고 나서 코드를 어디서 보면 좋은가

학습 순서는 아래가 가장 효율적이다.

1. `src/app/CoinBot.cpp`
2. `src/app/MarketEngineManager.h`
3. `src/app/MarketEngineManager.cpp`
4. `src/app/EventRouter.h`
5. `src/core/BlockingQueue.h`
6. `src/engine/input/EngineInput.h`
7. `src/engine/MarketEngine.h`
8. `src/trading/allocation/AccountManager.h`
9. `src/trading/strategies/RsiMeanReversionStrategy.h`
10. `src/api/ws/UpbitWebSocketClient.h`

위 순서로 보면 아래 흐름이 자연스럽게 연결된다.

- 프로그램 조립
- 마켓별 워커 구조
- 이벤트 전달
- 큐와 스레드
- 엔진/전략/계좌/DB 연결

---

## 10. 한 줄 요약

이 프로젝트의 구조를 읽을 때는 아래 네 가지 기준만 먼저 잡아도 훨씬 수월해진다.

- 참조는 "반드시 있어야 하는 객체", 포인터는 "없을 수도 있는 객체"
- `std::function`은 함수 슬롯, 람다는 그 슬롯에 넣는 실제 동작
- `BlockingQueue`는 스레드 간 전달 채널, `deque`는 내부 FIFO 버퍼
- `std::visit`는 `variant` 안의 실제 타입을 보고 맞는 코드를 실행하는 도구
