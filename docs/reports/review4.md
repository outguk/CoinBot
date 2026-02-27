# review4 - coin_available 미세 불일치(잠김/더스트) 구조 개선 설계

작성일: 2026-02-18

## 1) 목적

- 큰 오류(오정산)와 별개로, 장기 운용 중 반복적으로 발생하는 **작은 불일치**를 구조적으로 줄인다.
- 대상 이슈 4가지:
1. 매도 시 고정 epsilon 차감으로 더스트가 누적되는 문제
2. `coin_available`가 업비트 free/locked와 의미가 다른 문제
3. 더스트 판정을 `avg_entry_price`로 수행해 오판정이 가능한 문제
4. 런타임 자동 미세 동기화 부재로 드리프트가 누적되는 문제

---

## 2) 현재 구조와 문제 지점

### 2-1. 이슈 1: 고정 epsilon 차감 매도

- 현행:
  - 전략이 매도 수량을 `coin_available - volume_safety_eps`로 계산
  - 코드: `src/trading/strategies/RsiMeanReversionStrategy.cpp:354`
  - epsilon 설정: `src/util/Config.h:14`, `src/util/Config.h:66`
- 문제:
  - 매도할 때마다 의도적으로 잔량이 남음
  - 잔량이 누적되며 local/거래소 표시가 반복적으로 어긋남
- 장기 리스크:
  - 잦은 재진입/재청산 시 “영구 더스트”가 증가
  - 마켓별 자본 회전률 지표가 왜곡

### 2-2. 이슈 2: free/locked 미분리

- 현행:
  - 전략 입력 `coin_available`는 `budget->coin_balance` 단일 필드
  - 코드: `src/app/MarketEngineManager.cpp:580`
  - 로그 출력도 동일 값 사용: `src/app/MarketEngineManager.cpp:491`
- 문제:
  - 업비트는 free/locked를 분리해 보여주지만, 로컬은 단일 값만 관리
  - 주문 대기 중(locked 증가) UI와 로그가 즉시 일치하기 어려움
- 장기 리스크:
  - 운영자가 “오차”로 오해하는 경보가 잦아짐
  - 실제 문제(이벤트 유실)와 정상 차이(잠김) 구분이 어려움

### 2-3. 이슈 3: 더스트 가치 판정 기준

- 현행:
  - 매도 종료 정산에서 `remaining_value = coin_balance * avg_entry_price`
  - 코드: `src/trading/allocation/AccountManager.cpp:399`
  - 기준 미만이면 코인을 0 처리: `src/trading/allocation/AccountManager.cpp:409`
- 문제:
  - 현재 시장가격과 평균단가 괴리가 크면 dust 판정이 왜곡
  - 상승장에서는 실제 가치가 큰 잔량을 dust로 오판정할 가능성 존재
- 장기 리스크:
  - 회계 일관성 저하
  - 잔량 처리 기준의 예측 가능성 저하

### 2-4. 이슈 4: 런타임 미세 동기화 부재

- 현행:
  - 시작 시 `rebuildFromAccount` 2회 수행
  - 코드: `src/app/MarketEngineManager.cpp:93`, `src/app/MarketEngineManager.cpp:127`
  - 런타임은 주문 기반 recovery만 수행
  - 코드: `src/app/MarketEngineManager.cpp:606`
- 문제:
  - 작은 편차(잠김 타이밍/라운딩/미세 유실)가 자동 정리되지 않음
  - 시간이 지날수록 잔차가 누적될 수 있음
- 장기 리스크:
  - 운영 안정성은 유지해도 관측값 신뢰도가 낮아짐
  - 수동 재시작 의존도가 증가

---

## 3) 결론

- 현재 구조는 저빈도(1분봉, 3마켓, 봇 단독)에서 “동작은 가능”하다.
- 그러나 장기 무중단 관점에서는 아래 4개를 함께 개선해야 구조적으로 적절하다.
1. 수량 결정 책임을 전략에서 엔진/거래소 어댑터로 이동
2. 코인 자산 모델을 free/locked/dust로 분리
3. dust 판정에 mark price(현재가) 반영
4. 런타임 경량 동기화(reconcile-lite) 추가

---

## 4) 최종 해결안 (권장 설계)

### 4-1. 해결안 A: 주문 수량 결정 책임 재배치 (이슈 1)

- 원칙:
  - 전략은 “의도(전량 청산)”만 전달
  - 실제 주문 수량은 엔진에서 거래소 제약에 맞춰 계산
- 변경:
1. 전략:
  - `coin_available - eps` 제거
  - `coin_available` 그대로 매도 의도 생성
2. 엔진/REST 어댑터:
  - 주문 직전 `volume`를 거래소 자릿수로 floor
  - 0 이하이면 주문 생성 중단
  - 최소 주문 금액(5,000 KRW) 미만이면 주문 중단
- 기대효과:
  - 인위적 더스트 누적 제거
  - 수량 정책 단일화(전략/엔진 이중 정책 제거)

### 4-2. 해결안 B: 코인 3버킷 모델 도입 (이슈 2)

- 신규 모델:
  - `coin_free`: 주문 가능 코인
  - `coin_locked`: 주문에 묶인 코인
  - `coin_dust`: 거래 불가 잔량(보관용)
- 규칙:
1. 매도 주문 제출 성공 시 `coin_free -> coin_locked`
2. 체결 시 `coin_locked` 감소 + KRW 증가
3. 주문 종료 시 미체결 잔량 `coin_locked -> coin_free` 반환
4. 전략 입력 `coin_available`는 `coin_free`만 전달
- DTO/도메인 보강:
  - `Position`에 `locked` 필드 추가 권장
  - 현재 `AccountMapper`는 코인 `locked`를 유실
  - 관련 코드: `src/api/upbit/mappers/AccountMapper.h:42`
- 기대효과:
  - 업비트 free/locked 표시와 의미 정합
  - 운영 로그 해석 난이도 대폭 감소

### 4-3. 해결안 C: mark price 기반 dust 판정 (이슈 3)

- 원칙:
  - dust 판정은 “현재 가치” 기준이어야 함
- 변경:
1. `finalizeSellOrder(market, mark_price)` 시그니처 확장
2. 가치 계산: `coin_balance * mark_price`
3. mark price 부재 시 fallback으로만 `avg_entry_price` 사용
4. dust는 즉시 소거 대신 `coin_dust`로 이동
- 기대효과:
  - 급등/급락 구간 오판정 감소
  - 장부 추적 가능성 개선(삭제 대신 분리)

### 4-4. 해결안 D: reconcile-lite 주기 동기화 (이슈 4)

- 목적:
  - 전체 재분배 없이 코인 미세 편차만 교정
- 동작:
1. 5~10분 주기로 `getMyAccount()` 조회
2. 마켓별 비교:
  - exchange_total = free + locked
  - local_total = coin_free + coin_locked + coin_dust
3. 편차가 임계치 초과 && 연속 N회 && pending 주문 없음일 때만 보정
4. 보정 범위는 코인 버킷만 (KRW 재분배 금지)
- 제약:
  - 기존 정책 유지: 런타임 기본 경로는 주문 기반 recovery
  - 전체 `rebuildFromAccount()`를 주기적으로 돌리지 않음
- 기대효과:
  - 작은 드리프트의 자동 수렴
  - 기존 멀티마켓 KRW 배분 불변식 보존

---

## 5) 설계 상세 (데이터 모델 / API)

### 5-1. AccountManager 데이터 구조 변경

- 기존:
  - `coin_balance` 단일 필드
- 변경:
  - `coin_free`, `coin_locked`, `coin_dust`
- 파생 값:
  - `coin_total = coin_free + coin_locked + coin_dust`
- 전략 입력:
  - `coin_available = coin_free`

### 5-2. AccountManager 인터페이스 확장

- 신규/변경 함수 제안:
1. `reserveSell(market, volume)`  
2. `releaseSell(market, volume)`  
3. `applySellFill(market, filled_volume, received_krw)`  
4. `finalizeSellOrder(market, mark_price)`  
5. `reconcileCoinLite(market, exchange_free, exchange_locked)`

### 5-3. 엔진 연동 변경

- `MarketEngine::submit(ASK)` 성공 시 `reserveSell` 호출
- `onMyTrade(ASK)` / `reconcileFromSnapshot(ASK)`에서 `applySellFill` 호출
- 터미널 스냅샷에서 `finalizeSellOrder(market, latest_price)` 호출

---

## 6) 적용 순서 (안전 배포 순서)

1. 1차: 수량 정책 정리
- 전략의 epsilon 차감 제거
- 엔진/REST 수량 floor + min notional check 통일

2. 2차: 3버킷 도입
- `coin_free/locked/dust` 모델 적용
- 전략 입력을 `coin_free`로 전환

3. 3차: dust 판정 교체
- `finalizeSellOrder(market, mark_price)` 적용
- `coin_dust` 이동 정책 적용

4. 4차: reconcile-lite 추가
- 주기 비교 + 조건부 보정
- 지표/로그/알람 포함

---

## 7) 검증 기준 (수용 조건)

1. 반복 매매 1,000회 후
- `coin_dust`가 비정상적으로 단조 증가하지 않는다.

2. 매도 대기/부분체결 구간에서
- 업비트 `locked`와 로컬 `coin_locked` 차이가 임계치 내 유지된다.

3. 급등/급락 시나리오에서
- mark price 기준 dust 판정이 avg 기준보다 오판정이 낮다.

4. 장시간(>= 24h) 운용에서
- reconcile-lite가 편차를 자동 수렴시키고
- `rebuildFromAccount` 없는 런타임에서도 편차가 임계치 밖으로 장기 잔류하지 않는다.

---

## 8) 운영 지표/로그 권장

- 마켓별:
  - `coin_free`, `coin_locked`, `coin_dust`, `coin_total`
  - `exchange_coin_total`, `coin_delta`
  - `reconcile_lite_trigger_count`
- 경고 조건:
  - `abs(coin_delta) > delta_threshold` 연속 N회
  - `coin_dust / coin_total` 비율 급증

---

## 9) 영향 범위 및 리스크

- 영향 파일(예상):
  - `src/trading/allocation/AccountManager.h`
  - `src/trading/allocation/AccountManager.cpp`
  - `src/trading/strategies/RsiMeanReversionStrategy.cpp`
  - `src/engine/MarketEngine.cpp`
  - `src/app/MarketEngineManager.cpp`
  - `src/core/domain/Position.h`
  - `src/api/upbit/mappers/AccountMapper.h`

- 주요 리스크:
  - 데이터 모델 변경 시 기존 경로 누락 가능
  - free/locked 전환 타이밍 버그 시 매도 거부/중복 잠김 가능

- 완화:
  - 단계적 배포(6장 순서)
  - 각 단계별 회귀 로그 지표 확인

---

## 10) 최종 판단

- 1~4번 이슈는 개별 패치보다 **통합 모델 변경(수량 책임 정리 + 3버킷 + mark dust + reconcile-lite)**로 해결하는 것이 가장 적절하다.
- 이 방식은 현재 멀티마켓 구조(`주문 기반 복구`, `런타임 KRW 재분배 금지`)를 유지하면서 장기 운용 신뢰도를 가장 크게 높인다.

---

## 11) 이슈 1 적용 결과 (2026-02-18)

### 결정 근거

`UpbitExchangeRestClient::postOrder()`가 이미 `formatDecimalFloor(vol, 8)`로 주문 수량을 소수점 8자리 floor 처리한다.
floor는 수학적으로 항상 입력값 이하를 보장하므로 `coin_available`을 그대로 전달해도 과매도가 불가능하다.
따라서 전략의 `volume_safety_eps` 차감은 이 보장을 모르고 추가된 중복 방어였으며, 매도마다 의도적 잔량을 생성하는 부작용이 있었다.

### 변경 내용

| 파일 | 변경 전 | 변경 후 |
|------|---------|---------|
| `src/trading/strategies/RsiMeanReversionStrategy.cpp:354` | `coin_available - volume_safety_eps` | `coin_available` |
| `src/util/Config.h` | `StrategyConfig::volume_safety_eps = 1e-7` 존재 | 필드 삭제 |
| `src/util/Config.h` | `AccountConfig::coin_epsilon = 1e-7` | `1e-8` (업비트 최소 수량 단위) |

### coin_epsilon 조정 이유

`coin_epsilon`은 `volume_safety_eps`와 값을 맞추기 위해 `1e-7`로 설정되어 있었다.
`volume_safety_eps` 제거 후 이 근거가 사라졌으며, 실제로 포착해야 할 잔량(floor 오차)의 최대값은 `1e-8` 미만이다.
업비트 최소 수량 단위(소수점 8자리 = `1e-8`)에 맞춰 `1e-8`로 변경한다.

### 변경하지 않은 것

- `formatDecimalFloor(vol, 8)`: 과매도 방지의 실제 담당자, 그대로 유지
- `min_notional_krw` 체크: dust 수량 매도 시도 방지, 그대로 유지
- `finalizeSellOrder()` dust 정리: float 누적 오차로 남는 1 satoshi 수준 잔량 대응, 그대로 유지

### 한계

float 누적 오차로 `coin_balance`가 업비트 실잔고보다 `1e-8` 미만으로 낮게 계산될 수 있다.
이 경우 `formatDecimalFloor` 결과가 실잔고보다 1 satoshi 적어 극소량의 잔량이 남을 수 있으나,
`finalizeSellOrder()`가 이를 정리하므로 실질적 문제는 없다.
