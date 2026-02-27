# AccountManager 생성자 조건 검토 (small review)

대상:
- `src/trading/allocation/AccountManager.cpp:80`
- `src/trading/allocation/AccountManager.cpp:138`
- `src/trading/allocation/AccountManager.cpp:143`
- `src/trading/allocation/AccountManager.cpp:453`

## 1) 질문 포인트 검증

질문한 조건:

```cpp
if (remaining_krw <= 0 && budgets_.size() > 0) {
    return;
}
```

해석:
- 이 분기는 `remaining_krw <= 0`일 때만 실행된다.
- 따라서 `dust/eps`로 KRW가 **양수로 남아있는 경우**에는 이 분기로 "전부 코인 상태"가 확정되지는 않는다.

즉, 질문의 직접 우려(양수 KRW인데 위 if로 확정)는 코드상 그대로는 성립하지 않는다.

## 2) 실제 리스크 (더 중요한 지점)

### A. KRW 소실 가능 케이스

생성자 3단계는 KRW를 `coin_balance == 0`인 마켓에만 분배한다 (`AccountManager.cpp:145`).

문제 시나리오:
1. 모든 마켓이 코인 보유(`coin_balance > 0`)
2. 그런데 `account.krw_free`가 양수(작은 dust 포함)

결과:
- `markets_without_coin == 0`이 되어 분배 루프가 실행되지 않음
- `remaining_krw`가 어떤 `budget.available_krw`에도 반영되지 않고 종료됨

동일 패턴이 `rebuildFromAccount`에도 존재:
- `krw_markets.empty()`면 즉시 return (`AccountManager.cpp:513`)
- `actual_free_krw`가 양수여도 버짓에 미반영 가능

### B. 판정 기준 불일치

- 생성자: `budget.coin_balance == 0` (정확 비교)
- rebuildFromAccount: `budget.coin_balance < coin_epsilon` (epsilon 기준)

같은 "코인 없음" 의미를 서로 다른 기준으로 판정해, 초기화/재동기화 간 결과가 달라질 수 있다.

## 3) 영향도

- 단기 장애로 즉시 터지는 유형보다는, 장기 운용 시 자산 스냅샷 드리프트(특히 소액 KRW) 위험이 누적되는 타입
- 내부 예산과 거래소 `krw_free` 불일치가 커지면 디버깅 난이도 상승

우선순위 판단:
- 운영 안정성 관점에서 **P3(중기 개선)**가 타당

## 4) 개선 제안

1. KRW 보존 보장 분기 추가
- `remaining_krw > 0 && markets_without_coin == 0`이면 정책적으로 한 마켓(예: 사전 정의된 기준 마켓)에 귀속하거나 별도 `unassigned_krw`로 관리

2. 코인 없음 판정 기준 통일
- 생성자/`rebuildFromAccount` 모두 `coin_epsilon` + 가치 기준(`init_dust_threshold_krw`) 조합으로 동일하게 판단

3. 주석 정확화
- `remaining_krw <= 0` 분기 주석을
  - 기존: "모든 자산이 코인으로 전환된 상태"
  - 권장: "분배 가능한 free KRW가 없는 상태"
  로 수정

## 5) 결론

질문한 if 문 자체는 양수 KRW를 직접 오판정하지 않지만,  
현재 구조에는 "모든 마켓 코인 보유 + 양수 KRW 잔존"에서 KRW가 버짓에 반영되지 않는 경계 케이스가 있다.

핵심 보완점은 `if` 한 줄보다, **KRW 보존 정책과 코인 없음 판정 기준 통일**이다.
