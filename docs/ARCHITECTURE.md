# CoinBot 아키텍처 (현재 코드 기준)

> 이 문서는 **현재 구현과 동일한 실행 구조/복구 흐름**만 다룹니다.  
> 과거 단일 엔진 구조 설명은 `BEFORE_ARCHITECTURE.md`를 참고하세요.
>
> 최종 갱신: 2026-02-18

---

## 1) 시스템 개요

CoinBot은 멀티마켓(예: `KRW-BTC`, `KRW-ETH`, `KRW-XRP`)을 **마켓별 독립 워커 스레드**로 운용합니다.

- 진입점: `src/app/CoinBot.cpp`
- 핵심 실행 관리자: `src/app/MarketEngineManager.h`, `src/app/MarketEngineManager.cpp`
- 마켓 엔진: `src/engine/MarketEngine.h`, `src/engine/MarketEngine.cpp`
- 자산 관리자: `src/trading/allocation/AccountManager.h`, `src/trading/allocation/AccountManager.cpp`
- 이벤트 라우터: `src/app/EventRouter.h`, `src/app/EventRouter.cpp`

---

## 2) 실행 구조

### 2-1. 조립 순서 (`CoinBot.cpp`)

1. REST 계층 구성  
- `UpbitExchangeRestClient`(non-thread-safe) 생성  
- `SharedOrderApi`로 감싸 멀티스레드 안전하게 공유

2. 공유 상태 구성  
- `OrderStore`  
- `AccountManager`

3. `MarketEngineManager` 생성  
- 시작 시점 계좌 동기화(1차)  
- 마켓별 `StartupRecovery` 수행  
- 시작 시점 계좌 동기화(2차)

4. `EventRouter`에 마켓별 큐 등록

5. WebSocket 2개 실행  
- Public: candle 수신  
- Private: myOrder 수신, 재연결 콜백에서 recovery 요청

### 2-2. 스레딩 모델

- `MarketEngineManager`는 마켓마다 `std::jthread` 워커 1개를 생성
- 각 워커는 자신의 `MarketEngine + Strategy + Queue`만 소유
- `MarketEngine`는 `bindToCurrentThread()`로 단일 소유 스레드를 강제
- 공유 객체 동기화
- `SharedOrderApi`: `mutex` 직렬화
- `AccountManager`: `shared_mutex` (읽기 병렬/쓰기 직렬)
- `OrderStore`: `shared_mutex` (읽기 병렬/쓰기 직렬)

---

## 3) 데이터 플로우

### 3-1. 캔들(시장 데이터) 경로

1. Private/Public WS 중 Public WS가 candle JSON 수신
2. `EventRouter::routeMarketData()`가 market 추출 후 해당 마켓 큐에 push
3. 마켓 워커(`workerLoop_`)가 `handleMarketData_()` 실행
4. Candle DTO → 도메인 변환 후 전략 `onCandle()` 호출
5. 전략이 주문 의도(`Decision`)를 내면 `MarketEngine::submit()` 실행
6. `SharedOrderApi::postOrder()`를 통해 업비트 주문 전송
7. 주문 UUID를 `OrderStore`에 pending으로 저장

### 3-2. myOrder(체결/스냅샷) 경로

1. Private WS가 myOrder JSON 수신
2. `EventRouter::routeMyOrder()`가 해당 마켓 큐에 push
3. 마켓 워커 `handleMyOrder_()`에서 DTO 파싱
4. `toEvents()`로 `MyTrade`/`Order` 스냅샷 이벤트 분해
5. `MarketEngine`가 `onMyTrade()`/`onOrderSnapshot()` 처리
6. `EngineEvent`를 전략 이벤트(`onFill`, `onOrderUpdate`)로 전달

---

## 4) 자산/정산 모델

### 4-1. AccountManager 핵심 규칙

- 마켓별 예산(`MarketBudget`) 독립 운영
- 매수 전 `reserve()`로 KRW 예약
- 주문 실패/취소 시 `release()`로 예약 해제
- 매수 체결: `finalizeFillBuy()`로 예약 KRW 차감 + 코인 증가
- 매수 주문 종료: `finalizeOrder()`로 미사용 KRW 복구

### 4-2. 매도 정산 2단계 (현재 정책)

- `finalizeFillSell(market, sold_coin, received_krw)`  
부분 체결 단위 반영(코인 감소, KRW 증가)
- `finalizeSellOrder(market)`  
주문 터미널 시점에만 dust 정리 + `realized_pnl` 확정

이 분리로 부분체결 중 조기 dust 정리로 인한 후속 체결 누락을 방지합니다.

---

## 5) 복구(Recovery) 아키텍처

### 5-1. 시작 시점 복구 (계좌 기준 재구축)

`MarketEngineManager` 생성자에서:

1. `rebuildAccountOnStartup_(true)`  
`getMyAccount()` 기반 `AccountManager::rebuildFromAccount()`
2. 마켓별 `StartupRecovery::run()`  
봇 prefix 주문 취소 + 전략 `syncOnStart()`
3. `rebuildAccountOnStartup_(false)`  
최종 계좌 동기화

### 5-2. 런타임 복구 (주문 기준, 계좌 재분배 금지)

트리거:

- Private WS 재연결 성공 콜백
- Pending timeout(`cfg_.pending_timeout`)
- done-only 정산 실패 시 재시도 플래그

흐름(`runRecovery_`):

1. `activePendingIds()`로 pending 주문 식별
2. `getOrder(uuid)` 재시도 조회
3. 실패 시 `getOpenOrders(market)` fallback
4. 조회 성공 시 `reconcileFromSnapshot()`로 delta 정산
5. 터미널 + 정산 성공 시 `clearPendingState(true)` 후 전략 상태 동기화
6. 정산 실패(`reconciled=false`)면 pending 유지 후 다음 recovery에서 재시도

중요:

- 런타임 recovery는 `rebuildFromAccount()`를 기본 경로로 사용하지 않습니다.
- `emergency sync`(조건부 계좌 재동기화)는 아직 구현되지 않았습니다.

---

## 6) reconcile 정책 (핵심)

`MarketEngine::reconcileFromSnapshot()`는 `OrderStore` 누적값 대비 delta를 계산해 멱등 정산합니다.

- `delta_volume = max(0, snapshot.executed_volume - prev.executed_volume)`
- `delta_funds = max(0, snapshot.executed_funds - prev.executed_funds)`
- `delta_paid_fee = max(0, snapshot.paid_fee - prev.paid_fee)`

가드:

- `delta_volume > 0 && delta_funds <= 0`이면 `unknown_funds`로 보고 `false` 반환
- 금액을 0으로 확정하지 않고 pending을 유지해 오정산을 방지

---

## 7) DTO/매핑 정책

- `/v1/order` DTO(`OrderResponseDto`)는 `trades`를 배열로 파싱
- `executed_funds`가 누락되면 `trades[].funds` 합으로 보강
- 매퍼: `src/api/upbit/mappers/OpenOrdersMapper.h`

---

## 8) 큐/유실 리스크

- `BlockingQueue`는 bounded 모드에서 drop-oldest 정책
- `EventRouter`는 현재 `marketData`와 `myOrder`를 동일한 마켓 큐로 전달
- 저빈도(1분봉, 소수 마켓)에서는 위험이 낮지만, 처리량 증가 시 `myOrder` 전용 큐 분리가 필요할 수 있습니다.

---

## 9) 파일 맵

- 진입점: `src/app/CoinBot.cpp`
- 코디네이터: `src/app/MarketEngineManager.h`, `src/app/MarketEngineManager.cpp`
- 라우팅: `src/app/EventRouter.h`, `src/app/EventRouter.cpp`
- 시작 복구: `src/app/StartupRecovery.h`, `src/app/StartupRecovery.cpp`
- 엔진: `src/engine/MarketEngine.h`, `src/engine/MarketEngine.cpp`
- 주문 저장소: `src/engine/OrderStore.h`, `src/engine/OrderStore.cpp`
- 자산 관리: `src/trading/allocation/AccountManager.h`, `src/trading/allocation/AccountManager.cpp`
- 전략: `src/trading/strategies/RsiMeanReversionStrategy.h`, `src/trading/strategies/RsiMeanReversionStrategy.cpp`
- 업비트 API 인터페이스/공유 래퍼: `src/api/upbit/IOrderApi.h`, `src/api/upbit/SharedOrderApi.h`
- WS 클라이언트: `src/api/ws/UpbitWebSocketClient.h`, `src/api/ws/UpbitWebSocketClient.cpp`
- 구현 현황: `docs/IMPLEMENTATION_STATUS.md`
- 계획: `docs/ROADMAP.md`
