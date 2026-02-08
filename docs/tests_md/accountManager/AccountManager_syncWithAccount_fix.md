# AccountManager::syncWithAccount() 버그 수정

## 문제 요약

현재 `syncWithAccount()` 구현은 **전량 거래 모델을 위반**합니다.

### 버그 시나리오

```
초기 상태:
  KRW-BTC: available_krw=500,000, coin_balance=0
  KRW-ETH: available_krw=500,000, coin_balance=0

외부 거래 (AccountManager 외부에서):
  - 500,000원으로 BTC 0.01 매수

syncWithAccount() 호출 후 (현재 구현):
  KRW-BTC: coin_balance=0.01, available_krw=250,000  ❌ 위반!
  KRW-ETH: coin_balance=0, available_krw=250,000     ✓ 정상

전량 거래 모델:
  - coin_balance > 0 → available_krw = 0 (전량 보유)
  - available_krw > 0 → coin_balance = 0 (전량 KRW)
  - 둘 다 > 0은 상태 위반!
```

---

## 수정 방안

### Option 1: 코인 보유 마켓의 KRW를 0으로 설정

```cpp
void AccountManager::syncWithAccount(const core::Account& account) {
    std::unique_lock lock(mtx_);

    core::Amount actual_free_krw = account.krw_free;

    // 1. 코인 잔고 갱신 + 코인 보유 마켓의 KRW를 0으로
    for (const auto& pos : account.positions) {
        std::string market = "KRW-" + pos.currency;
        auto it = budgets_.find(market);
        if (it != budgets_.end()) {
            MarketBudget& budget = it->second;
            budget.coin_balance = pos.free;
            budget.avg_entry_price = pos.avg_buy_price;

            // ⭐ 전량 거래 모델: 코인 보유 → KRW = 0
            budget.available_krw = 0.0;
            budget.reserved_krw = 0.0;
        }
    }

    // 2. 코인이 없는 마켓에만 KRW 분배
    std::vector<std::string> krw_markets;
    for (const auto& [market, budget] : budgets_) {
        if (budget.coin_balance < cfg.coin_epsilon) {
            krw_markets.push_back(market);
        }
    }

    if (krw_markets.empty()) {
        // 모든 마켓이 코인 보유 → 정상 (전량 매수 상태)
        return;
    }

    // 3. 남은 KRW를 코인 없는 마켓에 균등 배분
    core::Amount per_market = actual_free_krw / static_cast<double>(krw_markets.size());
    for (const auto& market : krw_markets) {
        auto it = budgets_.find(market);
        if (it != budgets_.end()) {
            it->second.available_krw = per_market;
        }
    }
}
```

**장점:**
- 전량 거래 모델 완벽 준수
- 로직 명확

**단점:**
- 기존 비율 무시 (균등 분배로 변경)

---

### Option 2: 코인 없는 마켓의 기존 비율 유지

```cpp
void AccountManager::syncWithAccount(const core::Account& account) {
    std::unique_lock lock(mtx_);

    core::Amount actual_free_krw = account.krw_free;

    // 1. 코인 잔고 갱신 + 코인 보유 마켓의 KRW를 0으로
    for (const auto& pos : account.positions) {
        std::string market = "KRW-" + pos.currency;
        auto it = budgets_.find(market);
        if (it != budgets_.end()) {
            MarketBudget& budget = it->second;
            budget.coin_balance = pos.free;
            budget.avg_entry_price = pos.avg_buy_price;
            budget.available_krw = 0.0;
            budget.reserved_krw = 0.0;
        }
    }

    // 2. 코인 없는 마켓의 총 KRW 계산
    core::Amount krw_markets_total = 0.0;
    for (const auto& [_, budget] : budgets_) {
        if (budget.coin_balance < cfg.coin_epsilon) {
            krw_markets_total += budget.available_krw;
        }
    }

    // 3. 실제 KRW를 코인 없는 마켓에 기존 비율로 재분배
    if (krw_markets_total > 0.01) {
        for (auto& [_, budget] : budgets_) {
            if (budget.coin_balance < cfg.coin_epsilon) {
                double ratio = budget.available_krw / krw_markets_total;
                budget.available_krw = actual_free_krw * ratio;
            }
        }
    } else {
        // 모든 마켓이 KRW=0 → 균등 분배
        std::vector<std::string> krw_markets;
        for (const auto& [market, budget] : budgets_) {
            if (budget.coin_balance < cfg.coin_epsilon) {
                krw_markets.push_back(market);
            }
        }
        if (!krw_markets.empty()) {
            core::Amount per_market = actual_free_krw / krw_markets.size();
            for (const auto& market : krw_markets) {
                budgets_[market].available_krw = per_market;
            }
        }
    }
}
```

**장점:**
- 기존 비율 유지 (전략 의도 반영)
- 전량 거래 모델 준수

**단점:**
- 로직 복잡

---

## 추천: Option 1 (균등 분배)

**이유:**
1. **syncWithAccount()는 비정상 복구 시나리오**
   - 프로그램 재시작
   - 외부 수동 거래
   - AccountManager와 실제 계좌 불일치

2. **균등 분배가 더 안전**
   - 예측 가능
   - 복구 후 공정한 시작

3. **로직 단순**
   - 버그 가능성 감소
   - 유지보수 용이

---

## 테스트 추가

```cpp
// 전량 거래 모델 검증
for (const auto& [market, budget] : manager.snapshot()) {
    bool has_coin = budget.coin_balance > 1e-9;
    bool has_krw = budget.available_krw > 1.0;

    // 상태 불변 조건: coin > 0 XOR krw > 0
    assert(!(has_coin && has_krw));  // 둘 다 true는 위반
}
```

---

## 관련 이슈

- **초기화**: `AccountManager` 생성자에서도 동일한 로직 적용 필요
  - 현재는 코인 보유 마켓의 `available_krw = 0` 설정 완료 (line 129)
  - 일관성 유지

- **rebalance()**: 주기적 재분배 시에도 전량 거래 모델 준수 필요
