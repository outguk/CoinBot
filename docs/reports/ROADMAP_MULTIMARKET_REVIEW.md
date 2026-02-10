# ROADMAP 멀티마켓 구조/Phase 타당성 검토 보고서

작성일: 2026-02-09  
검토 대상: `docs/ROADMAP.md` (교차 검증: `docs/ARCHITECTURE.md`, `docs/IMPLEMENTATION_STATUS.md`, `docs/reports/STARTUP_RECOVERY_MULTIMARKET.md`)

## 체크리스트
- 멀티마켓 구조의 책임 분리와 동시성 모델 타당성 점검
- 폴더/모듈/관계 설계가 현재 코드베이스와 정합적인지 확인
- Phase 0~3의 의존성과 게이트가 논리적으로 이어지는지 검증
- 문서 불일치/누락/정책 충돌을 식별하고 우선순위화
- 각 평가/제안 후 목표 충족 여부를 짧게 자체 검증

## 1. 총평
- 멀티마켓의 핵심 구조(공유 계층 + 마켓별 엔진 분리)는 방향이 적절합니다.
- 다만, 문서 정합성 문제(링크/경로/섹션 누락), 테스트 정책 충돌, 게이트 정의의 측정 불명확성 때문에 실행 로드맵으로는 보완이 필요합니다.
- 결론: **구조는 적절(조건부), 실행 계획 문서 완성도는 미흡**.

목표 부합성 검증: 멀티마켓 구조 적절성 + Phase 타당성 + 수정 필요 항목 식별이라는 목표를 모두 직접 다뤘습니다.

---

## 2. 멀티마켓 구조 평가

### 2.1 Shared Layer + MarketEngine 분리
- 평가:
  - `SharedOrderApi`/`OrderStore`/`AccountManager`를 공유하고, `MarketEngine`이 로컬 상태만 변경하는 구조는 스레드 안전성과 책임 분리에 유리합니다 (`docs/ROADMAP.md:39`, `docs/ROADMAP.md:646`).
  - 실제 코드에도 `MarketEngine` 단일 소유권(`bindToCurrentThread`, `assertOwner_`)이 반영되어 설계 방향과 일치합니다 (`src/engine/MarketEngine.h:43`, `src/engine/MarketEngine.cpp:33`).
- 보완 제안:
  - `OrderStore` 공유 시 크로스마켓 이벤트 오염 방지를 문서상 불변조건으로 명시(“모든 상태 업데이트는 market 일치 검증 필수”)하면 운영 리스크를 더 낮출 수 있습니다.

목표 부합성 검증: 구조 적절성 점검 목표를 충분히 충족합니다.

### 2.2 SharedOrderApi 직렬화 전략
- 평가:
  - HTTP/1.1 + 거래소 rate limit 가정에서 mutex 직렬화 선택은 현실적입니다 (`docs/ROADMAP.md:79`).
  - 다만 5마켓 동시 운영에서 주문/취소/조회가 동일 락 경로를 공유하면 취소 지연이 생길 수 있습니다.
- 보완 제안:
  - 최소한 우선순위(취소 > 신규 주문 > 조회) 정책을 문서에 추가하고, Phase 1 완료 기준에 “취소 요청 p95 latency”를 넣는 것이 좋습니다.

목표 부합성 검증: 구조 점검에 더해 실운영 병목 리스크까지 보완했습니다.

### 2.3 AccountManager (사전 분배 + 예약 기반)
- 평가:
  - 경쟁 없는 독립 운영이라는 목적에는 부합합니다 (`docs/ROADMAP.md:260`).
  - 그러나 “재분배 없음”은 마켓별 성과 편차가 커질 때 자본 활용 비효율이 누적될 수 있습니다.
- 보완 제안:
  - 현재 정책은 유지하되, Phase 2 이후 선택적 “운영자 트리거형 재할당(수동)” 확장 포인트를 문서에 남기는 것이 안전합니다.

목표 부합성 검증: 독립성 목표를 유지하면서 장기 운용 리스크를 보완했습니다.

### 2.4 StartupRecovery 통합
- 평가:
  - MarketEngineManager에서 통합 처리하는 방향은 책임 경계 측면에서 타당합니다 (`docs/ROADMAP.md:145`).
  - 다만 ROADMAP 링크 경로가 실제와 불일치합니다: `docs/STARTUP_RECOVERY_MULTIMARKET.md`로 표기했지만 실제 파일은 `docs/reports/STARTUP_RECOVERY_MULTIMARKET.md`입니다 (`docs/ROADMAP.md:231`, `docs/ROADMAP.md:1289`).
- 보완 제안:
  - 링크 경로 수정 + 복구 정책(Cancel/KeepOpen)별 AccountManager 반영 규칙을 ROADMAP 본문에 요약해 문서 의존성을 줄이십시오.

목표 부합성 검증: 구조 적합성과 수정 필요 사항을 동시에 충족합니다.

---

## 3. Phase별 타당성 평가

### 3.1 Phase 0
- 평가:
  - 완료 판정 논리는 일부 충돌이 있습니다.
  - 예: `PrivateOrderApi 제거` 작업을 명시했지만(`docs/ROADMAP.md:856`), 게이트에는 “유지 권장”으로 표기(`docs/ROADMAP.md:1144`)되어 기준이 이중화되었습니다.
  - 예: “단일 마켓 거래 정상 동작 확인 필요”(`docs/ROADMAP.md:890`)인데 결론은 “실질 완료”(`docs/ROADMAP.md:892`)로 단정됩니다.
- 보완 제안:
  - 완료 기준을 “필수/권장”으로 분리하고, 미확인 항목이 남아 있으면 ‘조건부 완료’로 표기해야 합니다.

목표 부합성 검증: Phase 타당성 및 논리 연속성 검증 목표를 충족합니다.

### 3.2 Phase 1 (멀티마켓)
- 평가:
  - 작업 분해 자체는 합리적입니다.
  - 하지만 문서 정합성 문제가 큽니다:
    - `SharedOrderApi` 위치를 `src/engine/SharedOrderApi.h`로 표기했지만 실제는 `src/api/upbit/SharedOrderApi.h` (`docs/ROADMAP.md:75`, `src/api/upbit/SharedOrderApi.h:1`)
    - `신규 파일 목록`에 `MarketEngine`을 신규로 표기했으나 이미 존재 (`docs/ROADMAP.md:1227`, `src/engine/MarketEngine.h:1`)
- 보완 제안:
  - Phase 1 항목을 “신규 구현”과 “통합/마이그레이션”으로 재분류해야 일정 추정이 현실화됩니다.

목표 부합성 검증: 구조와 상세 구현 타당성 점검 목표를 충족합니다.

### 3.3 Phase 2 (PostgreSQL)
- 평가:
  - DDL 기본 방향은 적절하나, 운영 제약이 더 필요합니다.
  - 현재 체크 제약은 `order_volume XOR order_amount_krw` 중심이며, `order_type/position` 조합 제약까지는 명시가 약합니다 (`docs/ROADMAP.md:493`).
- 보완 제안:
  - `market buy -> amount only`, `market sell/limit -> volume only`, `limit -> price not null` 같은 규칙을 SQL CHECK로 강화하십시오.
  - 24시간 운영 목표를 고려하면 보존 기간/파티셔닝/인덱스 유지비도 Phase 2 범위에 최소 설계가 필요합니다.

목표 부합성 검증: 세부 구현 타당성 점검과 보완 제안을 충분히 제시했습니다.

### 3.4 Phase 3 (AWS 운영)
- 평가:
  - Graceful shutdown 절차 정의는 상세하고 실무적입니다 (`docs/ROADMAP.md:674`).
  - 그러나 `AskUser` 정책은 systemd/비대화형 운영과 충돌합니다 (`docs/ROADMAP.md:797`).
- 보완 제안:
  - 서버 모드에서는 `AskUser`를 금지하고 `Cancel` 또는 `KeepOpen`만 허용하도록 명시하십시오.
  - 완료 기준에 기능뿐 아니라 SLO(예: 복구 시간, shutdown timeout 준수율)를 넣어야 검증 가능성이 올라갑니다.

목표 부합성 검증: Phase 실현 가능성 검증 목표를 충족합니다.

---

## 4. 핵심 문제점 (우선순위)

### High
1. 문서 링크/경로 불일치
   - `docs/STARTUP_RECOVERY_MULTIMARKET.md` 링크가 실제 파일 위치와 다름 (`docs/ROADMAP.md:231`, 실제 `docs/reports/STARTUP_RECOVERY_MULTIMARKET.md`).
2. 핵심 섹션 내용 누락
   - `1.2 구현 단계`, `3.1 배포 아키텍처`, `3.2 Systemd 서비스 파일`이 “기존 내용 유지”로 남아 실행 문서 역할이 약함 (`docs/ROADMAP.md:660`, `docs/ROADMAP.md:668`, `docs/ROADMAP.md:672`).
3. 테스트 전략 충돌
   - ROADMAP 예시는 GTest 스타일(`TEST(...)`)인데 (`docs/ROADMAP.md:1189`), 프로젝트 지침은 GTest 금지 (`docs/IMPLEMENTATION_STATUS.md:519`).

목표 부합성 검증: 수정·보완이 필요한 핵심 항목을 식별했습니다.

### Medium
1. Phase 번호/구조 가독성 저하
   - `## Phase 1` 다음에 곧바로 `## Phase 3`가 나타나고, 실제 구현 계획은 `## Phase 4`에서 다시 0~3을 설명 (`docs/ROADMAP.md:594`, `docs/ROADMAP.md:664`, `docs/ROADMAP.md:816`).
2. 게이트 기준의 측정 불명확성
   - 예: `CPU < 50%`는 하드웨어/환경 기준이 없어 재현성이 낮음 (`docs/ROADMAP.md:969`).
3. 완료 상태 표현의 이중성
   - 확인 필요 항목이 남았는데 완료 결론을 단정하는 구간 존재 (`docs/ROADMAP.md:890`, `docs/ROADMAP.md:892`).

목표 부합성 검증: Phase 타당성/연계성 점검 목표를 충족합니다.

### Low
1. 용어 혼용
   - `MarketEngineManager`/`MarketManager`가 문서별로 혼재되어 추적성이 떨어짐.
2. 운영 정책 설명의 분산
   - StartupRecovery 상세가 별도 문서 의존적이라 ROADMAP 단독 가독성이 낮음.

목표 부합성 검증: 구조적 개선 여지를 추가로 식별했습니다.

---

## 5. 수정 권고안 (실행 순서)

1. 문서 정합성 1차 정리 (즉시)
   - 링크/경로/파일 상태(신규 vs 완료) 정정
   - “기존 내용 유지” placeholder 제거
2. Phase 구조 재편 (단기)
   - 상위 Phase 섹션(1~3)과 상세 구현 Phase(0~3)를 단일 체계로 통합
   - 각 완료 기준을 정량 지표 + 측정 환경과 함께 명시
3. 멀티마켓 운영 리스크 보강 (단기)
   - SharedOrderApi 우선순위 정책, StartupRecovery 정책 매트릭스(Cancel/KeepOpen) 명문화
4. 테스트 전략 통일 (즉시)
   - ROADMAP 예시를 수동 테스트 스타일로 변환해 정책 충돌 제거

목표 부합성 검증: “문제점 식별 + 수정 제안” 목표를 직접적으로 충족합니다.

---

## 6. 최종 판정
- 멀티마켓 구조 자체는 충분히 타당합니다.
- 다만 현재 ROADMAP은 실행 문서로서의 정합성과 검증 가능성 보완이 필요합니다.
- 위 High 항목을 먼저 정리하면, 이후 Phase 진행의 예측 가능성과 팀 내 의사결정 품질이 크게 개선됩니다.

목표 부합성 검증: 요청한 구조 평가, Phase 검증, 보완점 제안을 모두 완료했습니다.
