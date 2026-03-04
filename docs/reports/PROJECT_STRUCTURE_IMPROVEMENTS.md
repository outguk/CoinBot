# 프로젝트 구조 개선 검토 (2026-02-19 / 2026-02-23 self-heal 추가 / 2026-02-27 OrderStore erase 전환 / 2026-02-28 WS 유실 진입가 폴백 추가 / 2026-02-28 dust 밴드 제거 + 드리프트 로그 추가 / 2026-03-02 #4 Recovery 트리거 해결 + #24 큐/이벤트 구조 개선 추가 + #25 Asio Strand 장기 검토 추가)

## 목적
- 현재 코드베이스에서 운영 안정성/유지보수성 측면의 구조 개선 가능 지점을 정리한다.
- 단순 스타일 이슈보다 실제 장애/확장성에 영향을 줄 항목을 우선한다.

## 우선순위 등급
| 등급 | 기준 |
|------|------|
| P1 | 실제 장애로 직결 가능. 즉시 개선 필요 |
| P2 | 운영 안정성 저하. 단기 내 개선 권장 |
| P3 | 장기 운용 시 리스크. 중기 개선 |
| P4 | 유지보수성/확장성. 여유 있을 때 개선 |

---

## 모듈별 항목 인덱스

| 모듈 | 관련 항목 |
|------|---------|
| `src/app/EventRouter` | #1, #3 |
| `src/app/MarketEngineManager` | #4, #6, #7, #12, #18, #20, #24, #25 |
| `src/app/CoinBot` | #4, #8 |
| `src/app/StartupRecovery` | #5 |
| `src/core/BlockingQueue` | #1, #3, #25 |
| `src/engine/MarketEngine` | #6, #10, #14, #15, #23, #24 |
| `src/engine/OrderStore` | #10 |
| `src/api/upbit/SharedOrderApi` | #11 |
| `src/api/rest/RestClient` | #11 |
| `src/api/ws/UpbitWebSocketClient` | #5, #12, #13 |
| `src/trading/strategies/RsiMeanReversionStrategy` | #15, #16, #17, #18, #19, #20, #21, #23 |
| `src/trading/allocation/AccountManager` | #7, #16, #17, #20, #22 |
| `src/util/Logger` | #2, #5 |
| `CMakeLists.txt` | #9 |

---

## 상세 검토

### 0) 2026-02-19 장기 운용 로그 기반 추가 관찰
- 관찰 범위:
  - 마켓별 로그 약 5.16시간 운영 구간 분석
  - 예시: `market_logs/KRW-BTC.log:1` ~ `market_logs/KRW-BTC.log:1406`
- 확인된 패턴:
  - 마켓별 `Running recovery`가 다수 반복되지만, 대부분 `No pending orders, recovery skipped`
  - 예시: `market_logs/KRW-BTC.log:8`, `market_logs/KRW-BTC.log:9`
  - `done-only detected` 이벤트는 세 마켓 모두 반복 관찰
  - 예시: `market_logs/KRW-BTC.log:29`, `market_logs/KRW-ETH.log:137`, `market_logs/KRW-XRP.log:29`
- 해석:
  - 현재 복구 경로 자체는 동작하지만, 장기 운용 기준으로는
    - 불필요 복구 루프 호출
    - 큐 유실 가능성
    - 로깅 I/O 병목
    - 관측성 부족
    을 우선 개선해야 함

---

## P1 — 즉시 개선

### 1) [P1] 이벤트 큐 유실 위험 — `EventRouter` / `BlockingQueue`
하지만 현재의 구조에서는 일어날 가능성이 없음. (3마켓 1분봉, 5000의 큐 크기)
- **발생 모듈**: `src/app/EventRouter.h`, `src/app/EventRouter.cpp`, `src/core/BlockingQueue.h`
- 현황:
  - 마켓별 단일 큐를 `marketData`와 `myOrder`가 공유
  - 참조: `src/app/EventRouter.h:37`, `src/app/EventRouter.cpp:157`, `src/app/EventRouter.cpp:209`
  - 큐 포화 시 `drop-oldest`로 조용히 제거
  - 참조: `src/core/BlockingQueue.h:32`
  - 설계자 주석: `EventRouter.h:37`에 "실운영에서 큐 분리 검토" 이미 명시
- 리스크:
  - 트래픽 증가 시 `myOrder` 이벤트 유실로 정산 지연/누락 가능
  - Pending 고착 빈도 증가 시 복구 의존도가 급격히 상승
- 개선:
  - `myOrder` 전용 큐 분리(우선순위 높음)
  - 최소한 `myOrder` 경로는 drop 금지 또는 별도 백프레셔 정책 적용
- 구체적 구현 방향 (2026-03-02 검토):
  - `MarketContext`에 큐 2개로 분리: `candle_queue`(bounded, drop-oldest), `order_queue`(unbounded 또는 대용량 bounded, drop 금지)
  - `workerLoop_`: `order_queue.try_pop()` 우선 처리 후 `candle_queue.pop_for(50ms)` 대기
  - `EventRouter`: `routeMarketData()`는 `candle_queue`로, `routeMyOrder()`는 `order_queue`로 라우팅
  - 변경 범위: `MarketContext`, `EventRouter::registerMarket()` 시그니처, `workerLoop_` 순서 제어

### 2) [P1] 로깅 경합 및 flush 비용 — `Logger` / `MarketEngineManager`
- **발생 모듈**: `src/util/Logger.h`, `src/app/MarketEngineManager.cpp`
- 현황:
  - `Logger`가 전역 mutex 기반 동기 출력
  - 참조: `src/util/Logger.h:178`
  - 콘솔/파일/마켓파일 모두 매줄 flush
  - 참조: `src/util/Logger.h:193`, `src/util/Logger.h:199`, `src/util/Logger.h:219`
  - 캔들 확정마다 `Candle/Account/Strategy` 3개 INFO 로그 고정 출력
  - 참조: `src/app/MarketEngineManager.cpp:486`, `src/app/MarketEngineManager.cpp:491`, `src/app/MarketEngineManager.cpp:499`
- 리스크:
  - 로그 락 대기와 I/O flush 비용이 이벤트 처리 지연으로 전파
  - 처리 지연이 큐 적체를 만들고, 결국 드롭 정책과 결합해 이벤트 유실 위험 증가
- 개선:
  - INFO 배치 flush, WARN/ERROR 즉시 flush로 정책 분리
  - 반복 로그(예: recovery skip) rate-limit
  - 장기적으로 비동기 로거(전용 logging thread + queue) 검토

---

## P2 — 단기 내 개선

### 3) [P2] 큐 드롭 관측성 부족 — `BlockingQueue` / `EventRouter`
- **발생 모듈**: `src/core/BlockingQueue.h`, `src/app/EventRouter.h`
- 현황:
  - `BlockingQueue`는 drop 시 카운트/로그가 없음
  - 참조: `src/core/BlockingQueue.h:32`
  - `EventRouter::Stats`에도 drop 지표가 없음
  - 참조: `src/app/EventRouter.h:43`
- 리스크:
  - 운영 중 유실 발생 시 선제 감지 불가
  - 사후 분석 시 원인 규명이 어려움
- 개선:
  - 마켓별 dropped count, high-watermark, queue-lag 지표 추가
  - 임계치 초과 시 WARN 알림 연동

### ~~4) [P2] Recovery 트리거 과다 및 조건 부족~~ ✅ 해결 (2026-03-02) — `MarketEngineManager`
- **발생 모듈**: `src/app/MarketEngineManager.cpp`, `src/app/CoinBot.cpp`
- 기존 문제:
  - Private WS 재연결 콜백에서 전 마켓 복구 요청 (pending 유무 무관)
  - 정상 구간에서도 복구 루프/로그 오버헤드 누적
  - 참조 (구): `src/app/CoinBot.cpp:157`, `src/app/MarketEngineManager.cpp:589`
- 수정 내용:
  - `MarketContext::has_active_pending` (atomic bool) 추가 — `checkPendingTimeout_`에서 매 루프마다 `activePendingIds()` 기반으로 갱신
  - `requestReconnectRecovery()`에서 `has_active_pending`이 false인 마켓은 `recovery_requested` 설정 자체를 skip → pending 없는 마켓은 복구 루프 진입 차단
  - `runRecovery_()` 진입 시 `activePendingIds()` 재확인 후 조기 반환 (레이스 컨디션 안전망)
  - atomic flag 방식으로 중복 WS 재연결 요청이 자동 병합 → cooldown과 동등한 효과
  - 참조: `src/app/MarketEngineManager.cpp:618-636`, `src/app/MarketEngineManager.cpp:648-655`
- 잔존 한계:
  - trigger reason별 계측(`reconnect` / `pending_timeout` / `reconcile_fail`) 미추가 — 운영 로그에서 컨텍스트로 구분 가능하므로 현 단계에서는 허용

### 5) [P2] 로깅 경로 혼용 — `Logger` / `UpbitWebSocketClient` / `StartupRecovery`
- **발생 모듈**: `src/util/Logger.h`, `src/api/ws/UpbitWebSocketClient.cpp`, `src/app/StartupRecovery.cpp`, `src/api/upbit/UpbitExchangeRestClient.cpp`, `src/engine/upbit/UpbitPrivateOrderApi.cpp`
- 현황:
  - `Logger`와 `std::cout`가 혼재
  - 주요 위치: `src/api/ws/UpbitWebSocketClient.cpp` (25곳), `src/app/StartupRecovery.cpp` (10곳),
    `src/api/upbit/UpbitExchangeRestClient.cpp` (4곳), `src/engine/upbit/UpbitPrivateOrderApi.cpp` (2곳)
- 리스크:
  - WS 연결/재연결 이벤트가 Logger를 거치지 않아 마켓 파일에 미기록
  - 멀티마켓 동시 출력 시 콘솔 로그가 시간순으로 섞여 마켓별 원인 추적 난이도 증가
- 개선:
  - 운영 코드에서는 `Logger` 단일 경로 사용
  - 디버그 출력은 레벨 기반으로 제어하고 고빈도 로그는 샘플링 적용
  - **우선 적용안**: 마켓별 분리 로그 파일(`market_logs/KRW-BTC.log`, `market_logs/KRW-ETH.log` 등) 중심 운영
  - 콘솔은 요약 로그만 유지하고, 상세 원인 분석은 마켓별 파일 기준으로 수행

### ~~15) [P2] Self-Heal Pending 조기 해제 → 주문 추적 유실 + engine token 잠금~~ ✅ 해결 (2026-02-23) — `RsiMeanReversionStrategy`
- **발생 모듈**: `src/trading/strategies/RsiMeanReversionStrategy.cpp`, `src/engine/MarketEngine.cpp`
- 기존 문제:
  - self-heal Block 1/2가 상태 전환과 동시에 `pending_client_id_`를 reset
  - 이후 `onFill`/`onOrderUpdate` 이벤트가 도착해도 early return으로 무시
  - 전략 state는 InPosition/Flat으로 전환됐지만 엔진의 `active_buy_token_` / `active_sell_order_uuid_`는 그대로 유지
- 수정 내용:
  - `onCandle`에서 Pending 상태 전이를 담당하던 Block 1/2를 **완전 제거**
  - Pending 상태 전이 허용 경로를 단일화: `onOrderUpdate` / `onSubmitFailed` / `syncOnStart`
  - WS 누락 시: `checkPendingTimeout_` (engine token 기준, #18) → `runRecovery_` → `syncOnStart` 경로로 수렴
  - 근거: "주문 근거 없는 상태 전이 금지" 원칙 적용. 조건 정교화보다 발생 경로 자체를 차단.

### ~~16) [P2] Self-Heal PendingEntry dust 오검출 → 잘못된 InPosition 전환~~ ✅ 해결 (2026-02-23) — `RsiMeanReversionStrategy`
- **발생 모듈**: `src/trading/strategies/RsiMeanReversionStrategy.cpp`
- 기존 문제:
  - Block 1 트리거 조건(`coin_available * close >= min_notional_krw`)이 이번 주문 체결과 기존 dust 가격 상승을 구분 불가
  - dust 가치 상승만으로 Block 1 발동 → InPosition 오전환 + `entry_price_` 오설정
- 수정 내용:
  - Block 1 제거 (#15와 동일 수정)로 자동 해결
  - dust 오검출 시나리오 자체가 발생하지 않음

### ~~17) [P2] Self-Heal PendingExit 가격 하락 오검출 → Flat 오전환 + 재진입 가능~~ ✅ 해결 (2026-02-23) — `RsiMeanReversionStrategy`
- **발생 모듈**: `src/trading/strategies/RsiMeanReversionStrategy.cpp`
- 기존 문제:
  - Block 2 트리거 조건(`coin_available * close < min_notional_krw`)이 가격 하락만으로 발동
  - 매도 주문이 거래소에 살아있는 상태에서 전략만 Flat 전환 → `canBuy() = true` → 재진입 가능
- 수정 내용:
  - Block 2 제거 (#15와 동일 수정)로 자동 해결
  - PendingExit 상태 이탈은 `onOrderUpdate(Filled/Canceled)` 경로에서만 발생

### ~~23) [P2] WS 유실 시 진입가 미설정 + Filled/Canceled 폴백 비대칭~~ ✅ 해결 (2026-02-28) — `MarketEngine` / `RsiMeanReversionStrategy`
- **발생 모듈**: `src/engine/MarketEngine.cpp`, `src/trading/strategies/RsiMeanReversionStrategy.cpp`
- 기존 문제 1 — `EngineOrderStatusEvent.executed_funds` 미설정:
  - `onOrderSnapshot`에서 `EngineOrderStatusEvent` 발행 시 `executed_funds`를 채우지 않아 항상 0.0으로 전달
  - 참조: `src/engine/MarketEngine.cpp:365-373`
  - WS trade 이벤트 유실(재연결 등) 시 `pending_filled_volume_ = 0`, `pending_last_price_ = 0`으로 두 전략 폴백이 모두 무력화
  - 결과: `PendingEntry → InPosition` 전환되지만 `entry_price_ / stop_price_ / target_price_` 미설정 → 손절·익절 불가 상태
- 기존 문제 2 — Filled/Canceled 경로 폴백 비대칭:
  - Canceled 경로: `WS 누적 VWAP → 마지막 체결가 → (없음)` 순서로 `executed_funds` 폴백 없음
  - Filled 경로: `WS 누적 VWAP → executed_funds/volume → (없음)` 순서로 `pending_last_price_` 폴백 없음
  - 참조: `src/trading/strategies/RsiMeanReversionStrategy.cpp:452`
  - `filled_volume=0`으로 WS fill이 도착한 경우(`pending_filled_volume_=0, pending_last_price_>0`)에 Filled 경로는 `pending_last_price_`를 사용할 수 없어 `final_price=0` 도달
- 수정 내용:
  - `MarketEngine.cpp`: `ev.executed_funds = o.executed_funds` 추가 → 파이프라인 전 구간에 누적 체결 금액 전달
  - `RsiMeanReversionStrategy.cpp`: Canceled/Filled 양쪽 폴백 체인을 동일한 3단계로 통일
    1. WS 누적 VWAP (`pending_cost_sum_ / pending_filled_volume_`)
    2. REST VWAP (`ev.executed_funds / ev.executed_volume`)
    3. 마지막 체결가 (`pending_last_price_`)
  - 참조: `src/engine/MarketEngine.cpp:372`, `src/trading/strategies/RsiMeanReversionStrategy.cpp:411-416`, `src/trading/strategies/RsiMeanReversionStrategy.cpp:448-453`
- 부수 효과:
  - Filled done-only WS 유실 케이스에서도 `final_price` 폴백이 정상 작동 (기존엔 `executed_funds=0`으로 0 반환)

### 18) ~~[P2] Self-Heal → timeout recovery 사각지대~~ ✅ 해결 (2026-02-23) — `MarketEngineManager`
- **발생 모듈**: `src/app/MarketEngineManager.cpp`
- 기존 문제:
  - `checkPendingTimeout_`이 `strategy->state()`를 기준으로 추적하여, self-heal이 전략 state를 먼저 변경하면 추적이 종료되고 engine token이 영구 잠금되는 사각지대 존재
- 수정 내용 (`src/app/MarketEngineManager.cpp:773-776`):
  - 추적 기준을 `strategy->state()`에서 `engine->activePendingIds()`로 교체
  - self-heal이 전략 state를 InPosition으로 바꿔도 `active_buy_order_uuid_`가 남아있는 한 추적 지속
  - timeout 발동 → `runRecovery_` → `reconcileFromSnapshot` → `finalizeBuyToken_` → token 해제
  - `runRecovery_` 내 `syncOnStart(pos)`에서 `entry_price_`도 `avg_entry_price`(실제 평단가)로 교정
- 이중계상 안전성 확인:
  - self-heal은 `coin_balance > 0`일 때만 발동하며, `coin_balance`는 WS 메시지에서 MyTrade와 OrderSnapshot이 동일 JSON으로 함께 처리되므로 AccountManager와 OrderStore가 항상 함께 업데이트됨
  - `reconcileFromSnapshot` delta = REST.executed_volume - store.executed_volume = 미처리 잔량만 정산 → 이중계상 없음

---

## P3 — 중기 개선

### 8) [P3] 운영 경로 하드코딩 — `CoinBot`
- **발생 모듈**: `src/app/CoinBot.cpp`
- 현황:
  - 마켓 로그 경로가 절대경로로 고정
  - 참조: `src/app/CoinBot.cpp:103`
- 리스크:
  - 배포 경로/계정 변경 시 이식성 저하
  - 컨테이너/서비스 환경에서 운영 설정 유연성 부족
- 개선:
  - 설정 파일 또는 환경변수 기반 로그 경로 주입
  - 기본값은 상대경로(`market_logs`)로 유지하고, 운영 환경에서 오버라이드

### 9) [P3] CMake 구조 개선 — `CMakeLists.txt`
- **발생 모듈**: `CMakeLists.txt`
- 현황:
  - 환경 의존 절대 경로 및 대형 파일 나열식 타깃 정의
  - 참조: `CMakeLists.txt:16`, `CMakeLists.txt:22`, `CMakeLists.txt:61`
- 리스크:
  - 환경 이식성/변경 추적성 저하, 신규 파일 추가 시 누락 위험
- 개선:
  - 타깃 단위 모듈화(`core`, `api`, `app` 등)
  - 경로는 캐시 변수/툴체인 파일/패키지 매니저 기반으로 정리

### ~~10) [P3] OrderStore cleanup 트리거 경로 불일치~~ ✅ 해결 (2026-02-27) — `MarketEngine` / `OrderStore`
- **발생 모듈**: `src/engine/MarketEngine.cpp`, `src/engine/OrderStore.h`, `src/engine/OrderStore.cpp`
- 기존 문제:
  - `OrderStore::cleanup()` 호출은 `MarketEngine::onOrderStatus()` 경로에만 존재
  - `onOrderSnapshot()`에서는 cleanup을 트리거하지 않아 장기 누적 가능성 존재
  - DB 저장 도입 예정으로, 메모리 내 완료 주문 보관 모델 자체가 불필요해짐
- 수정 내용:
  - `OrderStore::cleanup()`, `completed_order_uuids_` 데크, `max_completed_orders_` 상수 전부 제거
  - `MarketEngine`의 `completed_count_` 카운터 및 cleanup 호출 블록 제거
  - `onOrderSnapshot()` 터미널 처리 완료 후 `store_.erase(o.id)` 즉시 호출로 대체
  - early-return 경로(store에 없는 터미널 주문)는 upsert 자체를 skip
- 불변 조건 성립:
  - `orders_`에 존재 = 활성(New/Open/Pending) 주문
  - `onOrderStatus()` → snapshot 사이 과도기에는 `getOpenOrdersByMarket()`의 `isOpenStatus` 필터가 안전망 역할
- 추후 연계:
  - DB 저장 시 erase 전 영속화 호출을 `onOrderSnapshot()` 터미널 분기에 추가하면 됨

### ~~19) [P3] Self-Heal dust 경계 오실레이션~~ ✅ 해결 (2026-02-23) — `RsiMeanReversionStrategy`
- **발생 모듈**: `src/trading/strategies/RsiMeanReversionStrategy.cpp`
- 기존 문제:
  - Block 3의 진입(Flat→InPosition)과 이탈(InPosition→Flat) 기준이 모두 `min_notional_krw = 5,000원`으로 동일
  - 경계 근처에서 매 캔들마다 상태가 뒤집히는 oscillation 발생
- 수정 내용:
  - `StrategyConfig`에 `dust_exit_threshold_krw = 1,000원` 추가 (`src/util/Config.h`)
  - Block 3 진입 기준: `min_notional_krw` (5,000원) — 기존 유지
   - Block 4 이탈 기준: `dust_exit_threshold_krw` (1,000원) — 분리 적용
   - 1,000~5,000원 히스테리시스 밴드: 이 구간에서는 어느 상태든 전이 없음 → 경계 진동 차단

### ~~20) [P3] 재시작 시 소액 포지션으로 인한 마켓별 자본 드리프트 고착~~ 부분 해결 (2026-02-28) — `AccountManager` / `RsiMeanReversionStrategy` / `MarketEngineManager`
- **발생 모듈**: `src/trading/allocation/AccountManager.cpp`, `src/trading/strategies/RsiMeanReversionStrategy.cpp`, `src/app/MarketEngineManager.cpp`
- 현황:
  - 초기화/재구축 시 `coin_value >= init_dust_threshold_krw(5,000원)`이면 해당 마켓을 코인 보유 마켓으로 간주
  - 코인 보유 마켓은 `available_krw = 0`으로 두고, `krw_free`는 코인이 없는 마켓에만 균등 배분
  - 참조: `src/trading/allocation/AccountManager.cpp:119`, `src/trading/allocation/AccountManager.cpp:130`, `src/trading/allocation/AccountManager.cpp:153`, `src/trading/allocation/AccountManager.cpp:497`, `src/trading/allocation/AccountManager.cpp:519`
  - 전략은 `min_notional_krw(5,000원)` 이상일 때만 매도 주문을 생성하므로, 임계값 근처 소액 포지션은 상태/자본 분배 왜곡을 길게 유지할 수 있음
  - 참조: `src/trading/strategies/RsiMeanReversionStrategy.cpp:332`, `src/app/MarketEngineManager.cpp:580`
- 시나리오(리뷰 사례):
  - 2마켓(KRW-BTC, KRW-ETH) 기준, 초기 균등 배분 자본 각 50,000원
  - 여러 사이클 반복 후 BTC 마켓에 floor 잔량 dust가 누적 → dust BTC 가치 6,000원
  - dust가 발생할 때마다 나머지 KRW는 매도 체결로 이미 `krw_free`에 반환됨
    → 재시작 시점: `krw_free ≈ 94,000원` (ETH 몫 50,000 + BTC 회수분 ~44,000), BTC dust 6,000원
  - 재구축 시 BTC 마켓은 코인 보유로 간주 → KRW 배분 제외, ETH 마켓이 krw_free 94,000원 전액 수령
  - BTC는 dust 6,000원짜리 포지션으로 재시작, 매도 후에도 ~6,000원만 회수 → 원래 자본 50,000원의 대부분이 ETH로 이전된 상태 고착
- 리스크:
  - 총자산이 사라지는 문제는 아니지만, 마켓별 예산이 한쪽으로 치우친 상태(자본 드리프트)가 고착될 수 있음
  - 이후 매도 체결로 KRW를 회수해도, 재시작 직후의 마켓별 운용 한도/진입 타이밍이 왜곡되어 전략 일관성 저하
- 리뷰 타당성 정리:
  - "임계값 초과 소액 포지션이 재시작 자본 배분을 비틀 수 있다"는 문제 제기는 타당
  - 다만 표현은 "자본 손실"보다 "마켓별 자본 드리프트 고착"이 정확
- 수정 내용 (2026-02-28):
  - `MarketEngineManager.cpp`: `rebuildFromAccount` 직후 마켓별 equity/drift 계산 및 로그 추가
    - `equity = available_krw + coin_balance * avg_entry_price` (재구축 시점 추정값)
    - `target = total_equity / num_markets`
    - `|drift| > target * 20%`이면 WARN 출력
    - 참조: `src/app/MarketEngineManager.cpp` (rebuildAccountOnStartup_ 내)
- 잔존 한계:
  - `coin_value >= 5,000원`인 정상 포지션으로 인한 드리프트는 KRW 배분 로직 변경 없이 유지됨
  - 단, 해당 포지션은 매도 가능(`>= min_notional_krw`)하므로 청산 후 다음 재시작 시 자동 정정됨
  - #21 수정으로 1,000~5,000원 구간 dust의 드리프트 원인은 함께 제거됨

### ~~21) [P3] 히스테리시스 밴드(1,000~5,000원) 장기 고착 가능성~~ ✅ 해결 (2026-02-28) — `RsiMeanReversionStrategy`
- **발생 모듈**: `src/trading/strategies/RsiMeanReversionStrategy.cpp`, `src/util/Config.h`
- 기존 문제:
  - 상태 전이 기준이 `enter >= min_notional_krw(5,000원)`, `exit < dust_exit_threshold_krw(1,000원)`으로 분리
  - 결과적으로 1,000~5,000원 구간은 "상태 전이 없음 + 매도 주문 불가" 밴드 → InPosition 고착
- 수정 내용:
  - `Config.h`: `dust_exit_threshold_krw` 1,000원 → 5,000원으로 변경 (`min_notional_krw`와 동일)
  - `hasMeaningfulPos`와 `isTrueDust`가 상호 배타적 → 히스테리시스 밴드 제거
  - 경계(5,000원) 진동 가능하지만 `canBuy()/canSell()` 가드로 실제 주문 발생 없음
  - `RsiMeanReversionStrategy.cpp` 주석 업데이트 (151~154줄)
  - 참조: `src/util/Config.h:18`

---

## P4 — 중장기 개선

### 11) [P4] SharedOrderApi 직렬화 병목 — `SharedOrderApi` / `RestClient`
- **발생 모듈**: `src/api/upbit/SharedOrderApi.cpp`, `src/api/rest/RestClient.cpp`
- 현황:
  - REST 호출 전체를 단일 mutex로 직렬화
  - 참조: `src/api/upbit/SharedOrderApi.cpp:39`, `src/api/upbit/SharedOrderApi.cpp:74`
  - 재시도 시 `sleep_for` 포함
  - 참조: `src/api/rest/RestClient.cpp:160`, `src/api/rest/RestClient.cpp:174`
- 리스크:
  - 한 요청 지연이 전체 마켓 주문/취소 지연으로 전파(HOL blocking)
  - 현재 3마켓 수준에서는 제한적이나 마켓 수 증가 시 실질 병목
- 개선:
  - 요청 우선순위(취소 우선) 또는 최소한 취소 경로 분리 검토
  - 재시도/백오프 정책을 호출 성격별(조회/주문/취소)로 분리

### 12) [P4] 워커 종료 제어 패턴 (진행 중) — `MarketEngineManager` / `UpbitWebSocketClient`
- **발생 모듈**: `src/app/MarketEngineManager.h`, `src/app/MarketEngineManager.cpp`, `src/api/ws/UpbitWebSocketClient.h`, `src/api/ws/UpbitWebSocketClient.cpp`
- 적용 완료(1단계):
  - `MarketEngineManager` 워커가 `std::thread + stop_flag`에서 `std::jthread + stop_token`으로 전환됨
  - 참조: `src/app/MarketEngineManager.h:81`, `src/app/MarketEngineManager.cpp:137`, `src/app/MarketEngineManager.cpp:231`
  - `UpbitWebSocketClient`도 내부 `jthread` 생명주기(`start()/stop()`)로 전환됨
  - 참조: `src/api/ws/UpbitWebSocketClient.h:85`, `src/api/ws/UpbitWebSocketClient.h:160`, `src/api/ws/UpbitWebSocketClient.cpp:55`, `src/api/ws/UpbitWebSocketClient.cpp:64`
- 남은 리스크:
  - `join()`은 여전히 타임아웃 없이 대기하므로, 블로킹 호출 구간에서는 종료 지연 가능
  - 참조: `src/app/MarketEngineManager.cpp:162`, `src/api/ws/UpbitWebSocketClient.cpp:69`
  - `stop_token`은 협조적 취소이므로 하위 API/REST가 빠르게 반환하지 않으면 즉시 종료 보장 불가
  - 실제 영향: 워커 50ms 루프, WS idle_timeout 1s 기준으로 종료 지연은 수 초 이내로 제한적
- 다음 개선(2단계):
  - 종료 관측 로그/지연 경고 강화
  - 하위 계층(API/REST)까지 취소 신호를 전파하는 협조적 취소 모델 검토

### 13) [P4] WebSocket 루프 구조 — `UpbitWebSocketClient`
- **발생 모듈**: `src/api/ws/UpbitWebSocketClient.h`, `src/api/ws/UpbitWebSocketClient.cpp`
- 현황:
  - 커맨드 큐(`cmd_q_`)를 루프에서 주기적으로 swap/poll하여 처리
  - 참조: `src/api/ws/UpbitWebSocketClient.h:188`, `src/api/ws/UpbitWebSocketClient.cpp:311`, `src/api/ws/UpbitWebSocketClient.cpp:317`
  - read는 idle timeout 기반 반복 루프에서 처리
  - 참조: `src/api/ws/UpbitWebSocketClient.cpp:376`, `src/api/ws/UpbitWebSocketClient.cpp:383`
- 리스크:
  - 커맨드 반영 지연이 루프 주기/timeout에 종속
  - 재연결/수신/명령 처리 책임이 단일 루프에 몰려 분석 난이도 상승
- 개선:
  - 커맨드 소비를 event-driven으로 전환하거나 루프 책임 분리(읽기/명령/재연결)

### 14) [P4] 릴리즈 치명 종료 정책 — `MarketEngine`
- **발생 모듈**: `src/engine/MarketEngine.cpp`
- 현황:
  - 스레드 소유권 위반 시 릴리즈에서도 `std::terminate()` 호출
  - 참조: `src/engine/MarketEngine.cpp:39`
- 평가:
  - 스레드 소유권 위반은 프로그래밍 오류이므로 즉시 종료는 의도적 설계로 합리적
  - 단, 종료 전 진단 정보(마켓/주문 상태/잔량)가 충분히 로그에 남는지 확인 필요
- 개선:
  - 정책 자체는 유지 가능하나, 운영 모드에서 진단 정보(마켓/호출 경로/최근 이벤트) 강화 필요

### 24) [P4] Engine 이벤트 출력 방식 개선 — `MarketEngine` / `MarketEngineManager`
- **발생 모듈**: `src/engine/MarketEngine.h`, `src/app/MarketEngineManager.cpp`
- 현황:
  - `engine->pollEvents()`를 `workerLoop_` 매 반복 끝에서 호출하는 pull 방식
  - 참조: `src/app/MarketEngineManager.cpp:323`
  - 이벤트가 없어도 매 루프마다 빈 벡터 조회 발생
- 리스크:
  - 실질 성능 영향은 미미 (1분봉, 소수 마켓)
  - `handleOne_()` → `pollEvents()` 사이에 다른 이벤트가 끼어들 여지가 없으므로 처리 순서 문제는 없음
- 개선 (선택적):
  - `MarketEngine`에 `setEventCallback(std::function<void(const EngineEvent&)>)` 추가
  - `onMyTrade()` / `onOrderSnapshot()` 내부에서 이벤트 발생 시 즉시 콜백 호출
  - `pollEvents()` 메서드 및 내부 이벤트 버퍼(`pending_events_`) 제거
  - 효과: 처리 즉시성 향상, 불필요한 벡터 할당 제거
  - 주의: 콜백은 엔진과 동일 스레드에서만 호출되므로 스레드 안전 문제 없음 (`bindToCurrentThread` 불변 조건 유지)

### 25) [P4] Boost.Asio Strand 기반 아키텍처 — 장기 확장 고려
- **발생 모듈**: `src/app/MarketEngineManager.cpp`, `src/app/EventRouter.cpp`, `src/core/BlockingQueue.h`
- 현황:
  - 마켓당 전용 `jthread` 1개 + `BlockingQueue` 구조
  - WS IO thread → `EventRouter` → `BlockingQueue.push()` → 워커 `pop_for(50ms)`
  - 50ms timeout으로 stop token / recovery flag / pending timeout을 주기적으로 확인하는 timer-poll 패턴
- 대안 구조 (Strand 방식):
  - 마켓당 `boost::asio::strand<io_context::executor_type>` 1개
  - WS 수신 콜백에서 `boost::asio::post(market_strand, handler)` 직접 디스패치
  - io_context 스레드 풀이 스케줄링 담당 → 마켓 수와 스레드 수 독립적 조정 가능
  - BlockingQueue 불필요 → drop-oldest 리스크 구조적 제거
- 트레이드오프:
  - ✅ 마켓 수십 개 이상으로 확장 시 스레드 수 절감 효과
  - ✅ 이벤트 발생 즉시 처리 (50ms polling 제거)
  - ⚠️ Recovery REST 호출(blocking)이 io_context thread를 점유 → `co_spawn` 또는 별도 executor 분리 필요
  - ⚠️ 아키텍처 전면 재작성 수준 — `bindToCurrentThread` 불변 조건 재설계 포함
- 결론:
  - 현재(3~5마켓, 1분봉)에서는 오버엔지니어링
  - 마켓 수가 수십 개 이상으로 늘어나거나 tick 단위 고빈도로 전환 시 재검토

### 22) [P4] AccountManager 생성자/재구축 로직 중복 — `AccountManager` / `MarketEngineManager`
- **발생 모듈**: `src/trading/allocation/AccountManager.cpp`, `src/app/MarketEngineManager.cpp`, `src/app/CoinBot.cpp`
- 현황:
  - `AccountManager` 생성자와 `rebuildFromAccount()`의 핵심 로직이 대부분 유사함
  - 다만 두 엔트리포인트 자체는 생명주기상 필요함
    - `AccountManager`는 초기 조립 시 더미 계좌로 먼저 생성됨 (`CoinBot.cpp`)
    - 실제 계좌 반영은 이후 REST 조회 결과로 `rebuildFromAccount()`에서 수행됨
  - 시작 시점의 2회 동기화도 역할이 다름
    - 1차: 초기 계좌 반영
    - 복구/취소 반영 후 2차: 최종 계좌 재반영
- 리스크:
  - 유사 로직이 두 군데 유지되면 정책 변경 시 누락/불일치 가능성 증가
  - 실제로 `initial_capital` 처리, 코인 없음 판정 기준 등 세부 차이가 있어 추적 난이도 상승
- 개선:
  - 엔트리포인트(생성자, `rebuildFromAccount`)는 유지
  - 내부 계좌 반영 알고리즘은 공통 private 함수로 단계적 통합 검토
  - 추가 메모: AccountManager 외 다른 모듈도 추후 공통 함수 리팩토링 검토 필요

---

## 권장 실행 순서
1. [P1] 이벤트 큐 분리 (`candle_queue` / `order_queue` 이원화, #1)
2. [P1] 로깅 병목 완화 (flush 정책 분리 / rate-limit)
3. [P2] 큐 드롭 관측성 지표 도입
4. ~~[P2] Recovery 트리거 제어 (조건부 실행 + cooldown, #4)~~ ✅ 완료
5. [P2] 로깅 경로 통합 (`std::cout` → `Logger`)
8. ~~[P2] Self-Heal PendingExit 가격 하락 오검출 수정 (#17)~~ ✅ 완료
9. ~~[P2] Self-Heal PendingEntry dust 오검출 수정 (#16)~~ ✅ 완료
10. ~~[P2] Self-Heal Pending 조기 해제 개선 (#15)~~ ✅ 완료
11. ~~[P2] Self-Heal → timeout recovery 사각지대 해소 (#18)~~ ✅ 완료
12. ~~[P2] WS 유실 진입가 미설정 + 폴백 비대칭 (#23)~~ ✅ 완료
12. ~~[P3] dust 경계 히스테리시스 적용 (#19)~~ ✅ 완료
13. [P3] 운영 경로 환경변수화
14. [P3] CMake 모듈화 정리
15. ~~[P3] OrderStore cleanup 트리거 정합화 (#10)~~ ✅ 완료
16. ~~[P3] 재시작 자본 드리프트 관측 로그 추가 (#20)~~ 부분 해결 (2026-02-28)
17. ~~[P3] 히스테리시스 밴드 제거 — dust_exit_threshold_krw 상향 (#21)~~ ✅ 완료 (2026-02-28)
18. [P4] SharedOrderApi 병목 완화
19. [P4] 종료 경로 안정화 (2단계: 하위 취소 전파)
20. [P4] WebSocket 루프 책임 분리
21. [P4] 치명 종료 진단 강화
22. [P4] AccountManager 내부 공통화 및 타 모듈 공통 함수 리팩토링 검토 (#22)
23. [P4] Engine 이벤트 출력 콜백 전환 (pollEvents → callback, #24)
24. [P4] Asio Strand 기반 아키텍처 검토 — 마켓 수 대폭 증가 시 (#25)

## 비고
- 본 문서는 정적 코드 검토 기준이며, 성능 수치/장애 재현은 별도 실험으로 보강 필요.
- Self-heal 관련 항목(#15~#19)은 2026-02-23 추가. 코드 리뷰 기반 정적 분석 결과임.
- #15/#16/#17 해결 (2026-02-23): `onCandle`에서 Pending 상태 전이 Block 1/2 제거. Pending 전이를 주문 이벤트 경로로 단일화.
- #19 해결 (2026-02-23): Block 3/4 임계값 분리(진입 5,000원 / 이탈 1,000원)로 히스테리시스 적용.
- #21 추가 (2026-02-27): 히스테리시스 밴드(1,000~5,000원) 장기 체류에 대한 운영 리스크/계측 항목 추가.
- #21 해결 (2026-02-28): `dust_exit_threshold_krw` 1,000원 → 5,000원으로 상향. 히스테리시스 밴드 제거. `min_notional_krw`와 동일 기준으로 정렬되어 1,000~5,000원 구간 고착 차단.
- #20 부분 해결 (2026-02-28): `rebuildFromAccount` 직후 마켓별 equity/drift 로그 추가. 1,000~5,000원 구간 드리프트 원인은 #21 수정으로 함께 제거됨. 5,000원 이상 정상 포지션의 드리프트는 청산 후 자동 정정 구조 유지.
- #22 추가 (2026-02-27): AccountManager 생성자/재구축 중복 검토 및 공통 함수 리팩토링 과제 등록.
- #10 해결 (2026-02-27): cleanup() 방식 → erase() 즉시 제거 방식으로 전환. `orders_`에 존재 = 활성 주문 불변 조건 성립. DB 연동 시 erase 전 영속화 호출 지점 확보.
- #23 해결 (2026-02-28): `EngineOrderStatusEvent.executed_funds` 누락 수정 + Canceled/Filled 폴백 체인 3단계 통일. WS 유실 경로에서 `entry_price_` 미설정으로 인한 손절·익절 불가 상태 차단.
- #4 해결 (2026-03-02): `has_active_pending` atomic flag 도입으로 `requestReconnectRecovery()`에서 active pending이 없는 마켓은 recovery 요청 자체를 skip. `runRecovery_()` 진입 시 이중 확인으로 레이스 컨디션 방어. 불필요한 복구 루프/로그 오버헤드 제거.
- #24 추가 (2026-03-02): Engine pollEvents() pull 방식을 callback 방식으로 전환하는 개선 방향 등록. P4 — 현 규모에서 체감 효과 미미하나 구조 단순화 목적.
- #25 추가 (2026-03-02): Boost.Asio Strand 기반 아키텍처 장기 검토 항목 등록. 마켓 수 수십 개 이상 또는 고빈도 전환 시 재검토. 현 단계에서는 오버엔지니어링.
- #1 업데이트 (2026-03-02): 큐 이원화 구체적 구현 방향 추가 (candle_queue/order_queue 분리, workerLoop_ 우선순위 처리, EventRouter 라우팅 타깃 분리).

---
