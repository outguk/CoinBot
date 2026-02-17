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

---

## 수정 이력

### [2026-02-14] Ctrl+C 종료 지연 버그 수정

#### 원인
`boost::asio::io_context`가 생성되지만 `ioc.run()`을 호출하는 스레드가 없었다.
Beast의 `tcp_stream::expires_after()` 및 WebSocket `idle_timeout`은 모두 io_context 타이머 기반이므로,
io_context가 실행되지 않으면 타이머가 발화하지 않아 `ws_->read()`가 실제 데이터(다음 캔들 ~60초)가 올 때까지 블로킹되었다.

#### 수정 내용 (`src/api/ws/UpbitWebSocketClient.cpp`)

| 위치 | 변경 전 | 변경 후 |
|------|---------|---------|
| `stop()` | `request_stop()` 후 `join()` 대기 | `request_stop()` 후 `socket().cancel()` 호출로 블로킹 즉시 해제 |
| `resetStream()` | `opt.idle_timeout = 200ms` 설정 | `idle_timeout` 제거 (io_context 없이 동작 안 함, `expires_after()`와 타이머 충돌) |
| `runReadLoop_()` | `expires_after(200ms)` + timeout 에러 시 `continue` | `expires_after()` 제거, `operation_aborted` 수신 시 `break`로 루프 탈출 |
| 상수 | `kIdleReadTimeout` 선언 | 제거 (미사용) |

#### 종료 흐름 (수정 후)
```
Ctrl+C → request_stop() + socket.cancel()
       → ws_->read() 즉시 operation_aborted 반환
       → break → thread_.join() 즉시 반환 → 프로그램 종료
```

---

### [2026-02-14] WS 재연결 시 체결 유실 복구 + Pending 타임아웃

#### 문제 현상
1. **Pending 상태 고착**: Private WS가 EOF로 끊긴 후 재연결되면, 끊어진 동안 발생한 체결 이벤트가 유실되어 전략이 `PendingEntry`/`PendingExit` 상태에 영구히 머무름
2. **중복 매수**: 중복 매수 감지 로그가 출력되었음에도 실제 중복 매수가 발생함

#### 근본 원인 분석

**Upbit WS 메시지 흐름**:
```
주문 접수 → trade(부분 체결) × N → done(최종 완료)
```

**MyOrderMapper.h의 이벤트 변환 로직**:
```cpp
bool is_trade = (d.state == "trade" && d.trade_uuid.has_value());
// state=="trade" → MyTrade + OrderSnapshot (MyTrade 먼저)
// state=="done"  → OrderSnapshot만 (MyTrade 없음!)
```

**재연결 중 체결 유실 시나리오**:
```
시점     WS 상태          서버 이벤트        봇 수신
─────────────────────────────────────────────────
T0       연결 중           trade(체결)       ✓ 정상 수신
T1       EOF 발생          -                 -
T2       재연결 중         trade(최종 체결)   ✗ 유실!
T3       재연결 완료       done(완료)        ✓ 수신
```

**T3에서 `done` 메시지 처리 시**:
- `is_trade = false` → `MyTrade` 이벤트 미생성 → `finalizeFillBuy()` 미호출
- `OrderSnapshot(Filled)` 생성 → `finalizeOrder()` 호출
- `finalizeOrder()`에서 `token.remaining() = amount - consumed` 계산
- `consumed = 0` (finalizeFillBuy 미호출) → **전액 KRW 복원** (잘못됨)

**결과 상태**:
```
coin_balance = 0 (체결분 미반영)
available_krw = 전액 복원 (잘못됨)
전략 상태 = InPosition (OrderSnapshot Filled로 전환)
```

**Self-Heal 오작동으로 중복 매수 발생**:
```
InPosition && !hasMeaningfulPos(coin=0) → Flat 전환 → 매수 신호 → 중복 매수
```

#### 해결 방법

두 가지 상호 보완적 메커니즘을 도입했다:

**Solution 1 — WS 재연결 → REST 계좌 동기화**
- Private WS 재연결 성공 시 콜백을 통해 즉시 계좌 동기화 트리거
- 재연결 공백 동안 유실된 체결 이벤트를 REST 조회로 보정

**Solution 2 — Pending 상태 타임아웃 (120초)**
- 전략이 `PendingEntry`/`PendingExit`에 진입한 후 120초 경과 시 자동 복구
- WS 재연결 외 다른 원인(API 응답 유실 등)으로 인한 Pending 고착도 커버

#### 수정 파일 및 변경 내용

##### 1. `src/api/ws/UpbitWebSocketClient.h`
| 변경 | 내용 |
|------|------|
| 추가 | `ReconnectCallback` 타입 별칭 (`std::function<void()>`) |
| 추가 | `setReconnectCallback(ReconnectCallback cb)` 선언 |
| 추가 | `on_reconnect_` 멤버 변수 |

##### 2. `src/api/ws/UpbitWebSocketClient.cpp`
| 위치 | 내용 |
|------|------|
| `setReconnectCallback()` | 콜백 setter 구현 |
| `doReconnect` 람다 내부 | `resubscribeAll()` 직후 `on_reconnect_()` 호출 |

재연결 성공 흐름:
```
reconnectOnce_() 성공 → resubscribeAll() → on_reconnect_() 호출
                                              ↓
                                   MarketEngineManager::requestAccountSync()
```

##### 3. `src/engine/input/EngineInput.h`
| 변경 | 내용 |
|------|------|
| 추가 | `AccountSyncRequest` 구조체 (빈 마커 타입) |
| 변경 | `EngineInput` variant에 `AccountSyncRequest` 추가 |

기존 `BlockingQueue<EngineInput>` 인프라를 활용하여 스레드 간 동기화 요청을 전달한다.

##### 4. `src/engine/MarketEngine.h` / `MarketEngine.cpp`
| 변경 | 내용 |
|------|------|
| 추가 | `clearPendingState()` — 활성 토큰/주문 ID 안전 정리 |

```cpp
void MarketEngine::clearPendingState()
{
    assertOwner_();
    if (active_buy_token_.has_value())
    {
        active_buy_token_->deactivate();   // 소멸자의 releaseWithoutToken 방지
        active_buy_token_.reset();
        active_buy_order_id_.clear();
    }
    if (!active_sell_order_id_.empty())
        active_sell_order_id_.clear();
}
```

핵심: `deactivate()` → `reset()` 순서. `reset()`만 하면 소멸자에서 `releaseWithoutToken()`이
AccountManager 상태를 오염시킨다.

##### 5. `src/app/MarketEngineManager.h`
| 변경 | 내용 |
|------|------|
| 설정 추가 | `pending_timeout{120}` (MarketManagerConfig) |
| 메서드 추가 | `requestAccountSync()` (public) |
| 메서드 추가 | `runRecovery_()`, `checkPendingTimeout_()` (private) |
| 멤버 추가 | `sync_in_progress_`, `last_sync_ok_` (atomic) |
| MarketContext 추가 | `tracking_pending`, `pending_entered_at`, `pending_timeout_fired` |
| 반환 타입 변경 | `syncAccountWithExchange_()`: `void` → `bool` |

##### 6. `src/app/MarketEngineManager.cpp`

**`requestAccountSync()`** — 모든 마켓 큐에 동기화 요청 전파:
```cpp
void MarketEngineManager::requestAccountSync()
{
    for (auto& [market, ctx] : contexts_)
        ctx->event_queue.push(engine::input::AccountSyncRequest{});
}
```

**`handleOne_()` — AccountSyncRequest 분기 추가**:
```
EngineInput 수신 → variant visit
  ├─ MyOrderRaw      → handleMyOrder_()
  ├─ MarketDataRaw   → handleMarketData_()
  └─ AccountSyncRequest → runRecovery_()   ← NEW
```

**`workerLoop_()` — 타임아웃 체크 추가**:
```
while (!stoken.stop_requested()) {
    auto input = event_queue.pop(100ms);
    if (input) handleOne_(ctx, *input);
    checkPendingTimeout_(ctx);              ← NEW (매 반복마다)
}
```

**`runRecovery_()`** — 3단계 복구 절차:
```
[1단계] REST 계좌 동기화
    ├─ CAS(sync_in_progress_) 성공 → syncAccountWithExchange_() 실행
    │   └─ RAII SyncGuard로 예외 안전성 보장
    │   └─ 결과를 last_sync_ok_에 저장 (대기 워커 전파용)
    └─ CAS 실패 (다른 워커가 실행 중) → 10초 대기 후 결과 수신
        └─ 타임아웃 시 sync_in_progress_ 재확인 후 실패 판정

    ※ 동기화 실패 → 복구 중단 (clearPendingState 미실행으로 안전)

[2단계] 미체결 주문 확인
    ├─ api_.getOpenOrders() 호출
    ├─ 봇 식별자(identifier prefix) 매칭으로 해당 마켓 미체결 필터링
    └─ REST 실패 시 "미체결 있음" 가정 (안전 기본값)

[3단계] 상태 복구
    ├─ 미체결 있음 → WS 이벤트 대기 (상태 변경 없음)
    └─ 미체결 없음 →
        ├─ engine.clearPendingState()  (토큰/주문 ID 정리)
        ├─ AccountManager에서 포지션 조회
        └─ strategy.syncOnStart(pos)   (Flat 또는 InPosition으로 리셋)
```

CAS 기반 중복 방지와 RAII 가드:
```cpp
bool expected = false;
if (sync_in_progress_.compare_exchange_strong(expected, true))
{
    struct SyncGuard {
        std::atomic<bool>& flag;
        ~SyncGuard() { flag.store(false, std::memory_order_release); }
    } guard{sync_in_progress_};

    sync_ok = syncAccountWithExchange_(false);
    last_sync_ok_.store(sync_ok, std::memory_order_release);
}
else { /* 대기 후 last_sync_ok_ 참조 */ }
```

**`checkPendingTimeout_()`** — Pending 상태 감시:
```
전략 상태 확인 (매 100ms 루프마다)
  ├─ PendingEntry 또는 PendingExit →
  │   ├─ 미추적 → 추적 시작 (시각 기록)
  │   └─ 추적 중 + 120초 초과 + 미발화 →
  │       └─ runRecovery_() 실행 (1회만)
  └─ 그 외 상태 → 추적 리셋
```

##### 7. `src/app/CoinBot.cpp`
| 위치 | 내용 |
|------|------|
| Private WS 설정부 | `setReconnectCallback()` 연결 |

```cpp
ws_private.setReconnectCallback([&engine_mgr]() {
    engine_mgr.requestAccountSync();
});
```

#### 전체 복구 흐름도

```
[트리거 A] Private WS EOF → 재연결 성공 → on_reconnect_()
                                              ↓
                                   requestAccountSync()
                                              ↓
                              모든 마켓 큐에 AccountSyncRequest push
                                              ↓
                                   각 워커에서 runRecovery_()

[트리거 B] workerLoop_ 반복 → checkPendingTimeout_()
                                              ↓
                              Pending 상태 120초 초과 감지
                                              ↓
                                   runRecovery_() (1회)
```

#### 코드 리뷰 반영 사항

| 리뷰 | 심각도 | 문제 | 수정 |
|------|--------|------|------|
| sync 실패 미체크 | High | `runRecovery_`가 sync 실패 후에도 `clearPendingState()` 진행 | `syncAccountWithExchange_` 반환타입 `bool` 변경, 실패 시 조기 복귀 |
| 예외 안전성 | Medium | CAS~store(false) 사이 예외 시 `sync_in_progress_` 영구 true | RAII `SyncGuard` 도입 |
| sync 결과 전파 | High | 대기 워커가 sync 성공 여부를 알 수 없음 | `last_sync_ok_` atomic 추가, sync 실행 워커가 결과 저장 |
| 타임아웃 시 stale 읽기 | Medium | 10초 대기 후 이전 cycle의 `last_sync_ok_` 참조 가능 | 타임아웃 후 `sync_in_progress_` 재확인, true면 실패 판정 |

#### 개선 항목 #8 해소
본 수정은 "후속 재동기화 정책" (상세 검토 #8)의 핵심 요구사항을 해소한다:
- **재연결 직후 강제 보정 루틴**: Solution 1로 구현 완료
- **Pending 고착 자동 복구**: Solution 2로 구현 완료
- **REST 계좌 조회 + 오픈오더 재조회 + 상태 재매핑**: `runRecovery_()` 3단계로 구현 완료
