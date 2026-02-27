# review3 - 업비트 체결/복구 정산 수정안 재검토

작성일: 2026-02-18

## 1) 결론 요약

- 이전에 제안한 수정 방향은 **대체로 적절**하다.
- 다만 그대로 적용하면 운영 중 `Pending` 장기 고착 가능성이 있어, 아래 보완 조건을 포함해야 실제로 안전하다.
- 최종 권장:  
  1. 주문 단위 복구 유지 (`getOrder` + reconcile)  
  2. 부분체결 정산/주문종료 정산 분리  
  3. `delta_funds=0` 무조건 0원 반영 금지  
  4. 반복 실패 시 제한적 emergency 동기화 허용

## 2) 왜 이 방향이 맞는가

### 2-1. 업비트 주문/체결 특성과 일치

- 시장가 매수: `side=bid`, `ord_type=price`, `price=총액`
- 시장가 매도: `side=ask`, `ord_type=market`, `volume=수량`
- 한 주문에 다수 체결이 발생할 수 있으며, 주문 조회 응답의 `trades`로 체결 목록 확인 가능
- 시장가 매수는 잔량으로 `cancel` 종료가 발생할 수 있음

위 특성 때문에, `done-only` 수신 또는 EOF 이후 복구에서 단일 스냅샷 값만 믿으면 정산 누락/중복이 발생할 수 있다.

## 3) 이전 제안의 적절성 재판정

### 3-1. 유지해야 할 항목 (적절)

1. 런타임에서 `rebuildFromAccount`를 기본 복구 수단으로 쓰지 않는 정책
2. `getOrder` 기반 주문 단위 복구
3. 매도 부분체결 중 dust 정리를 금지하고 주문 종료 시점으로 이관
4. `/v1/order`의 `trades`를 이용해 `executed_funds` 보강

### 3-2. 보완이 필요한 항목!!! (수정 필요)

1. `delta_funds=0`이면 즉시 `reconciled=false`로 끝내는 단일 정책은 부족
- 이유: 일시적 응답 누락 상황에서 `Pending`이 길게 고착될 수 있음
- 보완: 아래 우선순위 체인으로 계산 시도 후 실패 시에만 미확정 처리

2. EOF마다 계좌 전체 동기화는 금지하되, 영구 고착 방지용 emergency 경로는 필요
- 이유: 현재 구조에서 전체 동기화는 타 마켓 KRW 재분배 부작용이 큼
- 보완: 동일 주문 N회 복구 실패 + 장시간 경과 시에만 제한적으로 수행

## 4) 최종 수정안 (권장)

### 4-1. 복구 금액 산정 우선순위

복구 시 `delta_volume > 0`인데 `delta_funds == 0`인 경우:

1. 최신 `getOrder` 재조회(짧은 backoff 포함)
2. `executed_funds`가 유효하면 사용
3. 미유효 시 `trades[].funds` 합으로 계산
4. 그래도 불가하면 `unknown_funds`로 표시하고 정산 확정 보류

원칙: **알 수 없는 금액을 0으로 확정하지 않는다.**

### 4-2. 매도 정산 2단계 분리

1. `applySellFill`: 체결 1건 반영 (coin 차감 + KRW 가산)
2. `finalizeSellOrder`: 주문 터미널에서 dust/realized_pnl 확정

효과: 부분체결 중간에 coin을 0으로 지워 후속 체결 KRW가 누락되는 문제 방지.

### 4-3. 상태 전이 가드

- `reconciled=false`면 `Flat` 확정 금지
- 해당 주문은 `Pending` 유지 + 다음 recovery 주기 재시도
- 실패 카운터/경과시간 임계치 초과 시에만 emergency 동기화

### 4-4. DTO/매퍼 보정

- `/v1/order`의 `trades`를 배열로 파싱
- `executed_funds` 누락 시 0 치환 대신 `unknown` 상태로 보관
- 정산로직은 `unknown`을 명시적으로 분기 처리

## 5) 적용 우선순위

1. `/v1/order` DTO/매퍼 보정 (`trades` 반영)
2. reconcile 금액 산정 체인 도입 (`delta_funds=0` 보강)
3. 매도 정산 2단계 분리
4. `reconciled=false` 상태 전이 가드 + 재시도 정책
5. emergency 동기화 조건부 도입

## 6) 완료 기준 (수용 조건)

1. EOF/재연결 직후에도 `Flat + krw_available 소액 고착`이 재현되지 않는다.
2. 부분체결 2회 이상 주문에서 체결금 누락 없이 KRW가 누적된다.
3. 동일 주문의 복구 재시도는 멱등적으로 동작한다(중복 반영 없음).
4. 운영 로그에서 `unknown_funds` 케이스가 추적 가능하다(원인 파악 가능).

## 7) 참고 (공식 문서)

- 주문 생성: https://docs.upbit.com/kr/kr/reference/new-order
- 개별 주문 조회: https://docs.upbit.com/kr/reference/get-order
- 내 주문/체결 WebSocket: https://docs.upbit.com/kr/reference/websocket-myorder
- FAQ (주문/체결, trades 설명): https://docs.upbit.com/kr/v1.5.9/docs/faq
- KRW 호가/최소주문금액: https://docs.upbit.com/kr/v1.5.9/docs/krw-market-info

## 8) 실제 적용된 수정 사항 (2026-02-18)

### 8-1. 매도 정산 2단계 분리 적용

- `finalizeFillSell`에서 부분체결 반영(coin/KRW)만 수행하도록 정리
- 주문 터미널 시점 dust/손익 확정용 `finalizeSellOrder` 추가
- 부분체결 중 조기 dust 정리로 후속 체결 KRW가 누락되는 문제를 차단
- 적용 파일:
  - `src/trading/allocation/AccountManager.h`
  - `src/trading/allocation/AccountManager.cpp`
  - `src/engine/MarketEngine.cpp` (터미널 snapshot 경로에서 호출)

### 8-2. `delta_funds=0` 미확정 정산 가드 적용

- `reconcileFromSnapshot`에서 `delta_volume > 0 && delta_funds <= 0`이면
  정산을 확정하지 않고 `false` 반환 (`unknown_funds` 로그)
- 미확정 금액을 0원으로 강제 반영하던 경로 제거
- 적용 파일:
  - `src/engine/MarketEngine.h`
  - `src/engine/MarketEngine.cpp`

### 8-3. done-only / 복구 상태 전이 보정

- done-only에서 reconcile 실패 시 `onOrderSnapshot` fallback으로 닫지 않고
  pending 유지 + recovery 재시도 플래그 설정
- recovery에서도 터미널 주문이라도 `reconciled=false`면 상태를 닫지 않고 유지
- `getOrder` 성공이더라도 `executed_volume>0 && executed_funds<=0`인 불완전 응답은
  짧은 backoff 후 재조회
- 적용 파일:
  - `src/app/MarketEngineManager.cpp`

### 8-4. `/v1/order` trades 기반 funds 보강

- `OrderResponseDto.trades`를 단일 객체에서 배열로 수정하고 파싱 추가
- `executed_funds` 누락 시 `trades[].funds` 합으로 보강
- 적용 파일:
  - `src/api/upbit/dto/UpbitAssetOrderDtos.h`
  - `src/api/upbit/mappers/OpenOrdersMapper.h`

### 8-5. 영향 범위/리스크 검토 결과

- 런타임 전체 계좌 재분배(`rebuildFromAccount`) 경로는 건드리지 않음
- 변경 범위는 정산/복구 및 `/v1/order` 매핑 경로로 한정
- 현재 작업 환경에는 `cmake/ninja/msbuild/cl`이 없어 빌드 실행 검증은 미수행

## 9) 문제를 일으킨 구체 지점 (코드/로그 기준)

### 9-1. 초기 자산 32137원 → 27527원 축소 현상

- 재현 흐름(로그):
  - 1차 매수 후 `krw_available`가 소액으로 감소
  - 1회 매도 주문에서 체결이 2건 이상으로 나뉘어 들어옴(부분체결)
  - 주문 종료 후 `krw_available`가 기대값보다 작게 남음
- 핵심 원인(수정 전):
  - 매도 체결 함수(`finalizeFillSell`) 내부에서 부분체결 중에도 dust 정리를 수행해
    `coin_balance`를 먼저 0으로 만드는 경로가 존재
  - 이후 후속 체결이 들어오면 과매도 보정 분기에서 실제 수령 KRW가 0으로 보정되어
    체결금 일부가 누락될 수 있음
- 문제 지점:
  - `src/trading/allocation/AccountManager.cpp` (기존 `finalizeFillSell` 내부 dust/과매도 연쇄)
- 조치:
  - 체결 반영(`finalizeFillSell`)과 주문 종료 확정(`finalizeSellOrder`)을 분리

### 9-2. Flat 상태인데 `krw_available`가 16~17원대에 고착되는 현상

- 재현 흐름(로그):
  - EOF/재연결 직후 recovery 실행
  - `delta_volume > 0`인데 `delta_funds = 0` 관측
  - 주문은 터미널로 보이지만 KRW 정산값이 확정되지 않아 재진입 금액이 5,000원 미만으로 고착
- 핵심 원인(기존 경로):
  - 복구 스냅샷의 `executed_funds`가 누락/지연/0으로 들어오는 순간이 있음
  - 이 상태를 0원으로 정산 확정하면(또는 코인/상태만 먼저 닫으면) 로컬 잔고가 실제와 벌어짐
- 문제 지점:
  - `src/engine/MarketEngine.cpp`의 reconcile 금액 산정 분기
  - `src/app/MarketEngineManager.cpp`의 터미널 처리 분기
- 조치:
  - `delta_volume > 0 && delta_funds <= 0`를 `unknown_funds`로 처리해 정산 보류
  - pending 유지 + 재조회 재시도 (`getOrder` 불완전 응답 재시도 포함)

### 9-3. 장기적으로 불일치를 만들 수 있는 상수 리스크 (현 구조상)

- `myOrder`와 `marketData`가 같은 bounded queue를 공유하며, 포화 시 drop-oldest 정책을 사용
  - 파일: `src/app/EventRouter.h`, `src/core/BlockingQueue.h`
- 봇이 생성하지 않은 외부 주문 체결은 엔진에서 무시
  - 파일: `src/engine/MarketEngine.cpp` (`Ignoring external trade`)
- 현재 운영 전제(봇 단독, 저빈도 3마켓)에서는 발생확률이 낮지만,
  운영 조건이 바뀌면 재검토 필요
