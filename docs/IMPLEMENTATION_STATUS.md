# 구현 현황

마지막 업데이트: 2026-03-10 (Phase 3 문서 상태 동기화)

---

## 1) 전체 진행률

- [x] Phase 0: 기존 코드 리팩토링 (완료)
- [x] Phase 1: 멀티마켓 핵심 기능 구현 (완료)
- [x] Phase 1.7: 장시간 부하/안정화 검증 (완료)
- [x] Phase 2: Streamlit 대시보드 + SQLite 기록 (완료 — Steps 1~9 모두 완료)
- [ ] Phase 3: AWS 운영 자동화 (부분 진행 — HealthChecker/journald/배포 문서 반영, 운영 마감 항목 남음)

참고: 상세 계획은 [ROADMAP.md](ROADMAP.md)

---

## 2) Phase 1 상세 상태

### 2-1. SharedOrderApi

- 상태: ✅ 완료
- 핵심:
  - `IOrderApi` 구현
  - 내부 mutex 직렬화

### 2-2. AccountManager

- 상태: ✅ 완료 (2026-02-18 정책 반영)
- 핵심:
  - reserve/release/finalize 체계
  - 매도 정산 2단계 분리
    - `finalizeFillSell`: 체결 반영
    - `finalizeSellOrder`: 터미널 확정(dust/realized_pnl)
  - 시작 시점 계좌 재구축: `rebuildFromAccount()`
- 관련 파일:
  - `src/trading/allocation/AccountManager.h`
  - `src/trading/allocation/AccountManager.cpp`

### 2-3. MarketEngine

- 상태: ✅ 완료 (2026-02-18 복구 가드 반영)
- 핵심:
  - 마켓 단일 스레드 소유권 보장
  - submit/onMyTrade/onOrderSnapshot 경로 확립
  - reconcile 정책:
    - `delta_volume > 0 && delta_funds <= 0` => `unknown_funds`, 정산 보류
    - 미확정 금액 0원 확정 금지
- 관련 파일:
  - `src/engine/MarketEngine.h`
  - `src/engine/MarketEngine.cpp`

### 2-4. EventRouter

- 상태: ✅ 완료
- 핵심:
  - fast path + fallback 파싱
  - 마켓별 큐 라우팅
- 운영 리스크:
  - `myOrder`/`marketData`가 bounded queue 공유(drop-oldest)
  - 현재 운영 전제(1분봉/3마켓)에서는 위험도 낮음
- 관련 파일:
  - `src/app/EventRouter.h`
  - `src/app/EventRouter.cpp`

### 2-5. MarketEngineManager

- 상태: ✅ 완료 (2026-02-18 recovery 보정)
- 핵심:
  - 마켓별 워커/전략/엔진 관리
  - 시작 시점 2회 계좌 동기화 (`rebuildFromAccount`)
  - 런타임 복구는 주문 단위로만 수행
    - `getOrder` -> `getOpenOrders` fallback
    - `reconciled=false`면 pending 유지
    - done-only 정산 실패 시 강제 snapshot 종료 금지
  - `getOrder` 불완전 응답(`executed_volume>0 && executed_funds<=0`) 재조회
- 관련 파일:
  - `src/app/MarketEngineManager.h`
  - `src/app/MarketEngineManager.cpp`

### 2-6. CoinBot 조립

- 상태: ✅ 완료
- 핵심:
  - `SharedOrderApi -> AccountManager -> MarketEngineManager -> EventRouter`
  - private WS 재연결 시 recovery 트리거 연결
- 관련 파일:
  - `src/app/CoinBot.cpp`

### 2-7. 부하/안정화 검증

- 상태: ✅ 완료 (2026-03-03 기준)
- 현재:
  - 장시간 연속 실행 테스트 통과
  - `unknown_funds` 재시도 시나리오 정상 동작 확인
  - pending 장기 고착 대응 정책 확정 및 구현 완료
- Phase 1.7 기간 주요 수정 내용은 아래 섹션 참조

---

## 2-B) Phase 2 상세 상태

### 2-B-1. DB 인프라 (Steps 1~3)

- 상태: ✅ 완료
- 핵심:
  - `schema.sql`: candles / orders / signals 테이블 + WAL 모드
  - `Database` 클래스: sqlite3 번들 포함, `insertCandle` / `insertOrder` / `insertSignal`
  - `SignalRecord` 구조체 + `SignalCallback` 타입 정의
  - `signals.exit_reason` 컬럼 추가 (2026-03-05, identifier 파싱 의존 제거)
- 관련 파일:
  - `src/database/schema.sql`, `src/database/Database.h/.cpp`
  - `src/trading/strategies/StrategyTypes.h`

### 2-B-2. 봇 통합 (Steps 4~6)

- 상태: ✅ 완료
- 핵심:
  - `RsiMeanReversionStrategy::setSignalCallback` — BUY/SELL 확정 시 signals 기록
  - `pending_exit_reason_` — 청산 사유를 `exit_reason` 컬럼에 직접 저장
  - `MarketEngineManager` — DB 주입 + 콜백 등록
  - `CoinBot.cpp` — `Database::open()` 호출, 수명 보장
  - orders: 터미널 상태(Filled/Canceled/Rejected) 확정 시 1회 INSERT (`ON CONFLICT DO NOTHING`)
- 관련 파일:
  - `src/trading/strategies/RsiMeanReversionStrategy.h/.cpp`
  - `src/app/MarketEngineManager.cpp`, `src/app/CoinBot.cpp`

### 2-B-3. 캔들 수집기 (Step 7)

- 상태: ✅ 완료
- 핵심:
  - `tools/fetch_candles.py` — Upbit `/v1/candles/minutes/15` (15분봉, 백테스트용)
  - Bootstrap(초기 적재) + Incremental(이후 보강) 모드
  - WAL pragma 설정, `ON CONFLICT DO NOTHING`으로 봇과 충돌 없음
- 관련 파일:
  - `tools/fetch_candles.py`

### 2-B-4. Streamlit 대시보드 (Step 8)

- 상태: ✅ 완료 (2026-03-05)
- 핵심:
  - `streamlit/app.py` — 분석 탭(P&L·전략 분석) + 백테스트 탭
  - P&L: identifier 기반 BID↔ASK 페어링 + FIFO fallback
  - 전략 분석: 캔들차트 + BUY/SELL 마커 + RSI 서브플롯, 진입 RSI 분포, 청산 사유 파이차트
  - 백테스트: candle_rsi_backtest 모듈 import, 파라미터 슬라이더, 실거래 signals 오버레이
  - [설계 수정] signals에 paid_fee 없어 P&L은 orders 단독 계산, signals는 전략 컨텍스트 전용
  - [설계 수정] "BID created_at_ms 기준 BUY 창" → identifier 기반 1차 + FIFO 2차 페어링으로 구체화
  - [설계 수정] RSI 서브플롯 기준선 = 파라미터 슬라이더(oversold/overbought) 동적 반영
- 관련 파일:
  - `streamlit/app.py`

### 2-B-5. 백테스트 (Step 9)

- 상태: ✅ 완료 (2026-03-05)
- 관련 파일:
  - `tools/candle_rsi_backtest.py`

---

## 2-C) Phase 3 현재 상태

### 2-C-1. SignalHandler / HealthChecker

- 상태: ✅ 부분 완료
- 핵심:
  - `SIGINT`, `SIGTERM` 처리 구현
  - public/private WS 치명 상태 감지 시 `std::exit(1)`로 systemd 재시작 유도
  - 마켓 워커 비정상 종료도 fatal 조건에 포함
- 관련 파일:
  - `src/app/CoinBot.cpp`
  - `src/app/MarketEngineManager.cpp`
  - `src/api/ws/UpbitWebSocketClient.cpp`

### 2-C-2. journald / 배포 문서

- 상태: ✅ 부분 완료
- 핵심:
  - `deploy/coinbot.service` 기준 `StandardOutput=journal`, `StandardError=journal`
  - `docs/EC2_DEPLOY.md`에 journald 조회와 배포 절차 반영
  - `deploy/deploy.sh`와 systemd 설정에 EBS mountpoint/sentinel 안전 장치 반영
- 관련 파일:
  - `deploy/coinbot.service`
  - `deploy/deploy.sh`
  - `docs/EC2_DEPLOY.md`

### 2-C-3. Graceful shutdown

- 상태: ❌ 미구현
- 현재 판단:
  - 현재 stop 순서는 실행 안정성 관점에서는 동작 가능
  - 그러나 종료 직전 주문 상태 정합성까지 보장하지는 못함
  - 순서 변경만으로는 한계가 있고, pending 주문 REST 최종 확인 경로가 필요
- 관련 파일:
  - `src/app/CoinBot.cpp`
  - `src/app/MarketEngineManager.cpp`

---

## 3) Phase 1.7 반영 사항 (2026-02-23 ~ 2026-03-03)

### 3-1. Self-Heal 제거 — `RsiMeanReversionStrategy` (2026-02-23)

- `onCandle`에서 Pending 상태를 강제 전이하던 Block 1/2 완전 제거
- Pending 전이 허용 경로를 `onOrderUpdate` / `onSubmitFailed` / `syncOnStart`로 단일화
- 해결된 문제: PendingEntry dust 오검출(#16), PendingExit 가격 하락 오검출(#17), 주문 근거 없는 조기 해제(#15)

### 3-2. Engine Token 기반 Timeout 추적 — `MarketEngineManager` (2026-02-23)

- `checkPendingTimeout_` 추적 기준을 `strategy->state()`에서 `engine->activePendingIds()`로 교체
- self-heal로 전략 상태가 바뀌어도 engine token이 남아있는 한 추적 지속 → timeout 정상 발동
- timeout → `runRecovery_` → `reconcileFromSnapshot` → token 해제 경로 확립 (#18)

### 3-3. dust/히스테리시스 정리 — `RsiMeanReversionStrategy` / `Config.h` (2026-02-23 / 2026-02-28)

- Block 3/4 임계값 분리(진입 5,000원 / 이탈 1,000원) 히스테리시스 적용 (#19, 2026-02-23)
- `dust_exit_threshold_krw` 1,000원 → 5,000원으로 상향 (#21, 2026-02-28)
  - `min_notional_krw`와 동일 기준 정렬 → 히스테리시스 밴드 완전 제거
  - 1,000~5,000원 구간 InPosition 고착 차단

### 3-4. OrderStore erase 즉시 전환 — `MarketEngine` / `OrderStore` (2026-02-27)

- `cleanup()` 방식 제거 → `onOrderSnapshot()` 터미널 처리 직후 `erase()` 즉시 호출 (#10)
- 불변 조건 확립: `orders_`에 존재 = 활성(New/Open/Pending) 주문

### 3-5. WS 유실 진입가 폴백 — `MarketEngine` / `RsiMeanReversionStrategy` (2026-02-28)

- `EngineOrderStatusEvent.executed_funds` 누락 수정 (#23)
- Canceled/Filled 폴백 체인 3단계로 통일
  1. WS 누적 VWAP (`pending_cost_sum_ / pending_filled_volume_`)
  2. REST VWAP (`ev.executed_funds / ev.executed_volume`)
  3. 마지막 체결가 (`pending_last_price_`)
- WS 유실 재연결 경로에서 `entry_price_` 미설정으로 인한 손절·익절 불가 상태 차단

### 3-6. 자본 드리프트 관측 로그 — `MarketEngineManager` (2026-02-28)

- `rebuildFromAccount` 직후 마켓별 equity/drift 계산 및 로그 추가 (#20 부분 해결)
- `|drift| > target * 20%`이면 WARN 출력

### 3-7. Recovery 트리거 조건화 — `MarketEngineManager` (2026-03-02)

- `MarketContext::has_active_pending` (atomic bool) 도입 (#4)
- `requestReconnectRecovery()`에서 active pending 없는 마켓은 recovery 요청 skip
- 정상 구간 불필요 복구 루프/로그 오버헤드 제거

---

## 4) 2026-02-18 반영 사항 (코드 기준)

1. 매도 정산 2단계 분리 적용
- 부분체결 중 조기 dust 정리 제거
- 주문 종료 시점에만 최종 확정

2. `unknown_funds` 가드 적용
- `delta_volume > 0 && delta_funds <= 0` 정산 보류
- pending 유지 후 recovery 재시도

3. done-only/터미널 처리 보정
- reconcile 실패 시 `onOrderSnapshot` fallback 종료 제거
- 터미널이더라도 `reconciled=false`면 상태 유지

4. `/v1/order` funds 보강
- `trades` 배열 파싱
- `executed_funds` 누락 시 `trades[].funds` 합 사용

5. 미구현 항목
- emergency sync(조건부 `rebuildFromAccount`)는 아직 미구현

---

## 4) Phase 게이트 체크

### 4-1. Phase 0 -> Phase 1

- [x] 멀티마켓 기본 구조 완성
- [x] 마켓별 워커/엔진/전략 연결 완료

### 4-2. Phase 1 -> Phase 2

필수:
- [x] 장시간 부하 테스트 통과 (최소 1시간)
- [x] `unknown_funds` 재시도 시나리오 검증
- [x] pending 장기 고착 대응 확정 — self-heal 제거 + engine token timeout (#15/#18)

권장:
- [ ] emergency sync 정책 구현 및 검증
- [ ] 큐 포화 관측 지표 강화

### 4-3. Phase 2 -> Phase 3

- [x] DB (candles/orders/signals) 기록 정상 동작 확인 (Steps 1~6 완료)
- [ ] Streamlit 대시보드 분석 탭(P&L·전략 분석) + 백테스트 탭 동작 확인
- [ ] 백테스트 스크립트로 전략 손익 시뮬레이션 가능

---

## 5) 알려진 리스크

1. `unknown_funds` 반복 시 pending 장기 유지 가능
- 현재: 보류 + 재시도
- 필요: 조건부 emergency sync

2. 큐 포화 시 이벤트 유실 가능성(drop-oldest)
- 현재 운영 조건에서는 가능성 낮음
- 처리량 증가 시 큐 분리 필요

3. 봇 외부 거래와 로컬 상태 불일치 가능성
- 현재 정책: 외부 주문 체결은 무시
- 운영 전제: 봇 단독 계좌 사용

4. Graceful shutdown 미구현으로 종료 직전 주문 상태 정합성 리스크 존재
- 현재 종료 순서: `engine_mgr.stop() -> ws_private.stop() -> ws_public.stop()`
- 실행 안정성 관점에서는 큰 문제 없으나, 워커 종료 후 도착한 마지막 `myOrder`는 처리 보장이 없음
- stop 순서만 바꾸는 것으로는 근본 해결이 어렵고, 종료 시 pending 주문 REST 확인 경로가 필요

---

## 6) 변경 이력

| 날짜 | 구분 | 내용 |
|------|------|------|
| 2026-03-10 | Phase 3/문서 | HealthChecker/journald 완료 상태 반영, graceful shutdown(3.6) 미해결 리스크와 현재 stop 순서 평가 문서화 |
| 2026-03-05 | Phase 2 | streamlit/app.py 구현 완료 — 분석 탭(P&L·전략 분석) + 백테스트 탭, Step 8·9 완료 |
| 2026-03-05 | Phase 2 | signals.exit_reason 컬럼 추가 — identifier 파싱 의존 제거, pending_exit_reason_ 전략 멤버 추가 |
| 2026-03-05 | Phase 2 | fetch_candles.py 캔들 주기 15분봉 확정, 일별 P&L 집계 설계 추가 |
| 2026-03-05 | Phase 2 | Steps 4~7 완료 반영, IMPLEMENTATION_STATUS 동기화 |
| 2026-03-05 | 문서 | ROADMAP 완료 기준 orders upsert → terminal INSERT 수정, 실시간 탭 참조 제거 |
| 2026-03-03 | 문서 | Phase 2 재설계: Streamlit+API(실시간) / SQLite(분석·백테스트) 분리 구조로 변경 |
| 2026-03-03 | 문서 | Phase 1.7 완료 반영. 전체 진행 상태 및 게이트 체크 업데이트 |
| 2026-03-02 | Phase 1.7 | Recovery 트리거 조건화 — `has_active_pending` flag 도입 (#4) |
| 2026-02-28 | Phase 1.7 | WS 유실 진입가 폴백 3단계 통일 (#23), 드리프트 관측 로그 (#20), 히스테리시스 밴드 제거 (#21) |
| 2026-02-27 | Phase 1.7 | OrderStore erase 즉시 전환 (#10), AccountManager 중복 로직 등록 (#22) |
| 2026-02-23 | Phase 1.7 | Self-Heal Block 1/2 제거 (#15/#16/#17), engine token 추적 전환 (#18), dust 히스테리시스 (#19) |
| 2026-02-18 | 문서 | ROADMAP/IMPLEMENTATION_STATUS를 현재 코드 기준으로 전면 동기화 |
| 2026-02-18 | Phase 1 | 매도 정산 2단계 분리, `unknown_funds` 가드, done-only 종료 보정, `/v1/order` funds 보강 |
| 2026-02-14 | 문서 | 기존 Phase 1 완료 항목 반영 |
| 2026-02-13 | Phase 1 | MarketEngineManager/EventRouter/CoinBot 멀티마켓 통합 |
| 2026-02-08 | Phase 1 | MarketEngine 구현 |
| 2026-02-03 | Phase 1 | AccountManager 구현 |
| 2026-01-29 | Phase 1 | SharedOrderApi 구현 |

---

## 7) 테스트 작성 지침

CoinBot 프로젝트 테스트 정책:
- ❌ GoogleTest/GTest 사용 금지
- ✅ 수동 함수 기반 테스트 (`namespace test`, `void testXxx()`, `TEST_ASSERT`)
- 참고 파일:
  - `tests/test_market_engine.cpp`
  - `tests/test_account_manager_unified.cpp`
  - `tests/test_utils.h`

---

## 8) 관련 문서

- 학습 문서: [PROJECT_FLOW_STUDY.md](PROJECT_FLOW_STUDY.md)
- 계획 문서: [ROADMAP.md](ROADMAP.md)
- 정산/복구 검토: [reports/review3.md](reports/review3.md)
