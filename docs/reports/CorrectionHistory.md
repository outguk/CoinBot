## 수정 이력

### [2026-02-19] 장기 운용 로그 기반 구조 개선 항목 반영

#### 배경
- `market_logs` 장기 운용 로그를 기준으로 현재 구조의 운영 리스크를 점검했다.
- 코드 경로와 로그 패턴을 대조해 개선 우선순위를 재정렬했다.

#### 반영 내용
- 요약 우선순위를 최신 운영 리스크 기준으로 갱신
- 상세 검토에 아래 항목 추가:
  - `marketData`/`myOrder` 공유 큐 유실 위험
  - Recovery 트리거 과다 및 조건 부족
  - 로깅 경합/flush 비용
  - 큐 드롭 관측성 부족
  - 운영 경로 하드코딩
- 권장 실행 순서를 운영 안정성 중심으로 재정렬

#### 핵심 메시지
- 현재 코드는 기능적으로 동작하나, 장기 운용 안정성의 주요 병목은
  1) 큐 유실 가능 구조
  2) 복구 트리거 제어 부재
  3) 동기식 고빈도 로깅
  이며, 해당 3개를 우선 해소해야 한다.

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