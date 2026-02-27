# 구현 현황

마지막 업데이트: 2026-02-18

---

## 1) 전체 진행률

- [x] Phase 0: 기존 코드 리팩토링 (완료)
- [x] Phase 1: 멀티마켓 핵심 기능 구현 (완료)
- [ ] Phase 1.7: 장시간 부하/안정화 검증 (미완)
- [ ] Phase 2: PostgreSQL 영속화 (미시작)
- [ ] Phase 3: AWS 운영 자동화 (미시작)

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

- 상태: ⏳ 진행 필요
- 현재:
  - 기능 경로 구현 완료
  - 장시간 부하 기준 검증 미완

---

## 3) 2026-02-18 반영 사항 (코드 기준)

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
- [ ] 장시간 부하 테스트 통과 (최소 1시간)
- [ ] `unknown_funds` 재시도 시나리오 검증
- [ ] pending 장기 고착 대응 확정(정책 또는 구현)

권장:
- [ ] emergency sync 정책 구현 및 검증
- [ ] 큐 포화 관측 지표 강화

### 4-3. Phase 2 -> Phase 3

- [ ] DB 기록/복구/백프레셔 검증 완료

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

---

## 6) 변경 이력

| 날짜 | 구분 | 내용 |
|------|------|------|
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

- 구조 문서: [ARCHITECTURE.md](ARCHITECTURE.md)
- 계획 문서: [ROADMAP.md](ROADMAP.md)
- 정산/복구 검토: [reports/review3.md](reports/review3.md)
