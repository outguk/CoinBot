# Dust 처리 일관성 수정

**날짜**: 2026-02-06
**이슈**: 생성자와 syncWithAccount의 dust 처리 불일치
**심각도**: 🟡 MEDIUM (동작 차이, 예상치 못한 상태)

---

## 📋 문제 요약

### Before (불일치)

| 메서드 | Dust 기준 | 임계값 | 0.00001 BTC @ 100M (1,000원) |
|--------|-----------|--------|------------------------------|
| **생성자** | **가치 기준** | `init_dust_threshold_krw` (5,000원) | ✅ dust 처리 (0으로) |
| **syncWithAccount** | **수량 기준** | `coin_epsilon` (1e-7) | ❌ 정상 코인으로 취급 |

### 문제 시나리오

```cpp
// 1. 프로그램 시작: 생성자로 초기화
Account init;
init.krw_free = 1'000'000.0;
Position dust{.currency="BTC", .free=0.00001, .avg_buy_price=100'000'000.0};
init.positions.push_back(dust);

AccountManager manager(init, {"KRW-BTC", "KRW-ETH"});

// 생성자 결과: dust (1,000원 < 5,000원) → coin_balance = 0
// KRW-BTC: coin=0, krw=500,000
// KRW-ETH: coin=0, krw=500,000

// 2. 외부 거래 후 syncWithAccount()
Account updated;
updated.krw_free = 999'000.0;
updated.positions.push_back(dust);  // 동일한 dust

manager.syncWithAccount(updated);

// syncWithAccount 결과 (수정 전): dust 무시 안 함!
// KRW-BTC: coin=0.00001, krw=0  ❌ 생성자와 다름!
// KRW-ETH: coin=0, krw=999,000
```

**문제:**
- 동일한 dust 코인을 생성자는 무시하지만 syncWithAccount는 정상 코인으로 취급
- 예측 불가능한 동작

---

## ✅ 수정 내용

### After (일관성 확보)

```cpp
void AccountManager::syncWithAccount(const core::Account& account) {
    // Config에서 dust 임계값 로드 (생성자와 동일)
    const auto& cfg = util::AppConfig::instance().account;
    const core::Amount init_dust_threshold = cfg.init_dust_threshold_krw;

    for (const auto& pos : account.positions) {
        // ⭐ 가치 계산 (생성자와 동일)
        core::Amount coin_value = pos.free * pos.avg_buy_price;

        // ⭐ Dust 체크: 가치 기준 (생성자와 동일)
        if (coin_value < init_dust_threshold) {
            // Dust 잔량 → 0으로 처리
            budget.coin_balance = 0;
            budget.avg_entry_price = 0;
        } else {
            // 거래 가능한 코인 보유
            budget.coin_balance = pos.free;
            budget.avg_entry_price = pos.avg_buy_price;
            budget.available_krw = 0.0;
            budget.reserved_krw = 0.0;
        }
    }

    // coin_epsilon은 코인 식별용 (formatDecimalFloor 미세 잔량만)
    for (const auto& [market, budget] : budgets_) {
        if (budget.coin_balance < cfg.coin_epsilon) {
            krw_markets.push_back(market);
        }
    }
}
```

---

## 🔍 Dust 처리 두 단계

### 1. 가치 기준 Dust (init_dust_threshold_krw = 5,000원)

**목적:** 거래소 최소 주문 금액 미만의 무의미한 코인 제거

**적용 시점:**
- 생성자 초기화 (2단계)
- syncWithAccount (1단계) ← 추가

**예시:**
```
0.00001 BTC @ 100M = 1,000원 < 5,000원 → dust
0.0001 BTC @ 100M = 10,000원 > 5,000원 → 정상
```

### 2. 수량 기준 Dust (coin_epsilon = 1e-7)

**목적:** formatDecimalFloor(8자리)로 인한 미세 잔량 제거

**적용 시점:**
- finalizeFillSell (매도 완료 시)
- 코인 보유 마켓 식별 시

**예시:**
```
0.00000001 BTC < 1e-7 → dust (부동소수점 오차)
0.0000001 BTC > 1e-7 → 정상
```

---

## 🧪 추가 테스트

### TEST 14: syncWithAccount Dust 처리

```cpp
// Dust 포지션: 0.00004 BTC @ 100M = 4,000원 < 5,000원
Position dust_pos;
dust_pos.currency = "BTC";
dust_pos.free = 0.00004;
dust_pos.avg_buy_price = 100'000'000.0;

manager.syncWithAccount(updated);

// 검증: Dust는 0으로 처리
assert(btc_budget->coin_balance == 0.0);
assert(btc_budget->avg_entry_price == 0.0);
assert(btc_budget->available_krw > 0.0);  // KRW 배분받음
```

### TEST 15: 생성자 vs syncWithAccount 일관성

```cpp
// 동일한 계좌 상태
Account account;
account.krw_free = 500'000.0;
account.positions = {dust_pos, normal_pos};

// Case 1: 생성자
AccountManager mgr1(account, markets);

// Case 2: syncWithAccount
AccountManager mgr2(empty, markets);
mgr2.syncWithAccount(account);

// 검증: 동일한 결과
assert(mgr1.getBudget("KRW-BTC") == mgr2.getBudget("KRW-BTC"));
```

---

## 📊 영향 범위

### 직접 영향

- ✅ `syncWithAccount()` 동작 일관성 확보
- ✅ 예측 가능한 dust 처리

### 간접 영향

- ⚠️ **StartupRecovery** (Phase 1.5)
  - 재시작 시 dust 코인 자동 제거
  - MarketManager 초기화 시 일관된 상태

- ⚠️ **외부 수동 거래**
  - 사용자가 수동으로 소량 매수 → dust 자동 정리

---

## 📚 설정값 정리

```cpp
struct AccountConfig {
    // 1. 가치 기준 dust (거래소 최소 주문 금액)
    double init_dust_threshold_krw = 5000.0;  // 5,000원 미만

    // 2. 수량 기준 dust (formatDecimalFloor 미세 잔량)
    double coin_epsilon = 1e-7;  // 0.0000001 BTC

    // 3. KRW dust (원 단위 이하 잔량)
    double krw_dust_threshold = 10.0;  // 10원 미만
};
```

**사용 위치:**
- `init_dust_threshold_krw`: 생성자, syncWithAccount
- `coin_epsilon`: finalizeFillSell, 코인 식별
- `krw_dust_threshold`: finalizeOrder

---

## ✅ 변경 파일

1. ✅ **AccountManager.cpp**
   - syncWithAccount(): 가치 기준 dust 처리 추가

2. ✅ **AccountManager.h**
   - 주석 업데이트: dust 처리 일관성 명시

3. ✅ **test_account_manager_improved.cpp**
   - TEST 14: Dust 처리 검증
   - TEST 15: 생성자/syncWithAccount 일관성 검증

---

## 🎯 결론

**일관성 확보 완료!** 이제 생성자와 syncWithAccount가 동일한 방식으로 dust를 처리합니다.

**핵심 개선:**
- ✅ 가치 기준 dust 처리 통일 (init_dust_threshold_krw)
- ✅ 예측 가능한 동작
- ✅ 재시작/복구 시 일관된 상태
- ✅ 테스트 강화

**다음 단계:**
- Phase 1.5 MarketManager에서 StartupRecovery 통합 시 검증
- 실제 거래 환경에서 dust 처리 모니터링
