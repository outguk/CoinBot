# AccountManager.cpp 상세 해설

## 1) 구현 목표 요약

`src/trading/allocation/AccountManager.cpp`는 아래 목적을 구현한다.

1. 예약 기반 KRW 관리(초과 주문 방지)
2. 부분 체결 누적 정산
3. 과매도/미세 잔량(dust) 보정
4. 실제 계좌 상태와의 동기화 복구

---

## 1.1) 핵심 흐름 한 줄 요약

```text
reserve -> (fill buy/sell 반복) -> finalizeOrder -> 필요시 syncWithAccount
```

---

## 2) ReservationToken 구현 포인트

## 2.1 move/소멸자 안전망

원본 핵심:

```cpp
ReservationToken::~ReservationToken() {
    if (active_ && manager_ != nullptr) {
        manager_->releaseWithoutToken(market_, amount_ - consumed_);
    }
}
```

의미:
- 토큰이 비정상 경로로 파괴되어도 예약 미사용 금액 자동 복구

move 대입 시에도 기존 active 토큰은 먼저 복구:

```cpp
if (active_ && manager_ != nullptr) {
    manager_->releaseWithoutToken(market_, amount_ - consumed_);
}
```

---

## 3) 생성자 초기화 로직

원본 흐름:

1. markets 비어있으면 예외
2. `budgets_`를 마켓별 0 상태로 생성
3. `account.positions` 반영(코인 가치가 dust 이상이면 코인 보유로 설정)
4. 남은 KRW를 코인 없는 마켓에 균등 배분

핵심 코드:

```cpp
if (markets.empty()) {
    throw std::invalid_argument("AccountManager: markets cannot be empty");
}
...
core::Amount coin_value = pos.free * pos.avg_buy_price;
if (coin_value < init_dust_threshold) { ... } else { ... }
...
core::Amount per_market = remaining_krw / static_cast<double>(markets_without_coin);
```

의도:
- 시작 시점부터 마켓별 초기 상태를 일관되게 맞춤
- dust는 초기부터 제거해 전략/엔진 판단 불일치 방지

---

## 4) 조회 API

## `getBudget`, `snapshot`

원본:

```cpp
std::shared_lock lock(mtx_);
```

특징:
- 읽기 동시성 허용
- 반환은 복사본이라 외부에서 내부 상태를 직접 훼손할 수 없음

---

## 5) 예약/해제 로직

## 5.1 `reserve(market, krw_amount)`

핵심 코드:

```cpp
if (it == budgets_.end()) return std::nullopt;
if (krw_amount <= 0) return std::nullopt;
if (budget.available_krw < krw_amount) return std::nullopt;

budget.available_krw -= krw_amount;
budget.reserved_krw += krw_amount;
return ReservationToken(this, std::string(market), krw_amount, token_id);
```

의미:
- 예약 성공 시점에 KRW를 `available -> reserved`로 이동
- 이후 주문 체결/취소 결과에 따라 reserved를 정산

## 5.2 `release(token)` / `releaseInternal` / `releaseWithoutToken`

핵심 동작:

```cpp
budget.reserved_krw -= remaining_amount;
budget.available_krw += remaining_amount;
if (budget.reserved_krw < 0) budget.reserved_krw = 0;
```

구조 이유:
- `releaseInternal`: 락 없는 핵심 로직(중복 제거)
- `releaseWithoutToken`: 소멸자/move 연산용 noexcept 경로

---

## 6) 매수 정산 `finalizeFillBuy`

원본 처리 순서:

1. 토큰 active 확인
2. 입력값 검증(0/음수 차단)
3. 예약 잔액 초과 시 clamp
4. `reserved_krw` 차감
5. 평균 매수가 재계산(가중 평균)
6. `coin_balance` 증가
7. 토큰 consumed 누적

핵심 코드:

```cpp
if (executed_krw > token.remaining()) {
    executed_krw = token.remaining();
}
...
core::Amount old_total = budget.coin_balance * budget.avg_entry_price;
core::Amount new_total = received_coin * fill_price;
core::Volume new_balance = budget.coin_balance + received_coin;
budget.avg_entry_price = (old_total + new_total) / new_balance;
budget.coin_balance = new_balance;
token.addConsumed(executed_krw);
```

포인트:
- 부분 체결 여러 번 호출해도 평균단가/잔고가 누적 일관성 유지

---

## 7) 매도 정산 `finalizeFillSell`

## 7.1 기본 처리

```cpp
budget.coin_balance -= sold_coin;
budget.available_krw += actual_received_krw;
```

## 7.2 과매도 보정

핵심 코드:

```cpp
if (budget.coin_balance < 0) {
    core::Volume actual_sold = balance_before;
    if (actual_sold > 0 && sold_coin > 0) {
        actual_received_krw = (received_krw / sold_coin) * actual_sold;
    } else {
        actual_received_krw = 0;
    }
    budget.coin_balance = 0;
}
```

의미:
- 보유량보다 많이 매도된 입력이 들어와도
  실제 보유분까지만 KRW 반영하고 잔고를 0으로 고정

## 7.3 dust 이중 체크

1. 수량 기준: `coin_epsilon`
2. 가치 기준: `init_dust_threshold_krw`

핵심 코드:

```cpp
if (budget.coin_balance < cfg.coin_epsilon) {
    should_clear_coin = true;
} else {
    core::Amount remaining_value = budget.coin_balance * budget.avg_entry_price;
    if (remaining_value < cfg.init_dust_threshold_krw) {
        should_clear_coin = true;
    }
}
```

dust면:

```cpp
budget.coin_balance = 0;
budget.avg_entry_price = 0;
budget.realized_pnl = budget.available_krw - budget.initial_capital;
```

---

## 8) 주문 종료 `finalizeOrder`

핵심 동작:

1. 토큰 remaining을 `releaseInternal`로 복구
2. `reserved_krw`의 KRW dust 정리(`krw_dust_threshold`)
3. 토큰 비활성화

코드:

```cpp
core::Amount remaining = token.remaining();
if (remaining > 0) {
    releaseInternal(token.market(), remaining);
}
...
if (budget.reserved_krw > 0 && budget.reserved_krw < cfg.krw_dust_threshold) {
    budget.available_krw += budget.reserved_krw;
    budget.reserved_krw = 0;
}
token.deactivate();
```

포인트:
- 주문 생명주기 종료 시 reserved 잔여 찌꺼기를 강제로 정리

---

## 9) 동기화 `syncWithAccount`

복구 중심 4단계:

1. 모든 마켓 코인 상태 초기화
2. `account.positions`만 다시 반영(가치 기준 dust 동일 적용)
3. 코인 없는 마켓 목록 산출
4. `account.krw_free`를 해당 마켓에 균등 분배

핵심 코드:

```cpp
for (auto& [market, budget] : budgets_) {
    budget.coin_balance = 0;
    budget.avg_entry_price = 0;
}
...
if (coin_value < init_dust_threshold) { ... } else { ... }
...
core::Amount per_market = actual_free_krw / static_cast<double>(krw_markets.size());
it->second.available_krw = per_market;
it->second.reserved_krw = 0.0;
```

의도:
- 외부 수동 거래/재시작 후 상태 불일치를 한 번에 재정렬
- "코인 보유 마켓은 KRW 0" 전량 모델 유지

---

## 10) 통계 카운터 해석

`Stats`는 운영/테스트에서 경로를 확인하는 용도:

- `total_reserves`: 예약 성공 수
- `reserve_failures`: 예약 실패 수
- `total_releases`: 예약 복구 수
- `total_fills_buy`: 매수 정산 호출 수
- `total_fills_sell`: 매도 정산 호출 수

주의:
- 비정상 입력을 조용히 무시하는 분기들이 있어, 호출 횟수와 카운터가 항상 1:1은 아니다.

---

## 11) 빠른 실행 예시

```cpp
auto token = manager.reserve("KRW-BTC", 100'000.0);
if (!token) return;

manager.finalizeFillBuy(*token, 40'000.0, 0.0008, 50'000'000.0);
manager.finalizeFillBuy(*token, 60'000.0, 0.0012, 50'000'000.0);
manager.finalizeOrder(std::move(*token));   // buy 주문 종료

manager.finalizeFillSell("KRW-BTC", 0.0020, 110'000.0);  // sell 정산
```

---

## 12) 함께 보면 좋은 파일

- `src/trading/allocation/AccountManager.h`
  - 타입/계약/주석 기반 설계 의도
- `tests/test_account_manager_unified.cpp`
  - reserve/fill/sell/oversell/sync/dust 시나리오 검증
- `docs/tests_md/accountManager/CHANGELOG_dust_handling_fix.md`
  - dust 기준 변경 배경

