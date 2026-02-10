# AccountManager.h 상세 해설

## 1) 파일 역할

`src/trading/allocation/AccountManager.h`는 멀티마켓 환경에서
마켓별 자산을 독립적으로 관리하기 위한 계약(데이터 구조 + API)을 정의한다.

핵심 기능:

1. 마켓별 예산(`MarketBudget`) 관리
2. 주문 전 KRW 예약(`reserve`)
3. 체결 후 상태 정산(`finalizeFillBuy`, `finalizeFillSell`, `finalizeOrder`)
4. 실제 계좌와 동기화(`syncWithAccount`)

---

## 1.1) 먼저 코드로 보는 핵심 인터페이스

```cpp
class AccountManager {
public:
    std::optional<MarketBudget> getBudget(std::string_view market) const;
    std::map<std::string, MarketBudget> snapshot() const;

    std::optional<ReservationToken> reserve(std::string_view market, core::Amount krw_amount);
    void release(ReservationToken&& token);

    void finalizeFillBuy(ReservationToken& token, core::Amount executed_krw,
                         core::Volume received_coin, core::Price fill_price);
    void finalizeFillSell(std::string_view market, core::Volume sold_coin, core::Amount received_krw);
    void finalizeOrder(ReservationToken&& token);

    void syncWithAccount(const core::Account& account);
};
```

---

## 2) 핵심 데이터 구조

## 2.1 `MarketBudget`

원본 핵심 필드:

```cpp
struct MarketBudget {
    std::string market;
    core::Amount available_krw{0};
    core::Amount reserved_krw{0};
    core::Volume coin_balance{0};
    core::Price avg_entry_price{0};
    core::Amount initial_capital{0};
    core::Amount realized_pnl{0};
};
```

의미:
- `available_krw`: 즉시 주문 가능한 KRW
- `reserved_krw`: 주문 제출 후 체결 대기 금액
- `coin_balance`: 보유 코인 수량
- `avg_entry_price`: 평균 매수 단가
- `initial_capital`: 기준 자본(ROI 계산 기준점)
- `realized_pnl`: 실현 손익 누적

계산 헬퍼:

```cpp
core::Amount getCurrentEquity(core::Price current_price) const;
double getROI(core::Price current_price) const;
double getRealizedROI() const;
```

---

## 2.2 `ReservationToken` (RAII)

핵심 선언:

```cpp
class ReservationToken {
public:
    ReservationToken(const ReservationToken&) = delete;
    ReservationToken& operator=(const ReservationToken&) = delete;
    ReservationToken(ReservationToken&& other) noexcept;
    ReservationToken& operator=(ReservationToken&& other) noexcept;
    ~ReservationToken();
};
```

의도:
- 예약 성공 시 토큰 발급
- 토큰이 active인 채로 사라지면 자동 복구(release)
- move-only로 소유권 흐름 명확화

주문 흐름:

```text
reserve() -> token 생성
partial fill 반복 -> finalizeFillBuy(token, ...)
주문 종료(완체결/취소) -> finalizeOrder(std::move(token))
```

---

## 3) 전량 거래 모델 (핵심 설계)

문서/주석 기준 모델:

```text
Flat 상태: KRW 중심 (available_krw > 0, coin_balance == 0)
InPosition 상태: 코인 중심 (coin_balance > 0, available_krw ~= 0)
```

즉 한 마켓은 "KRW 또는 코인" 중심으로 운영된다.

---

## 4) 동시성 설계

동기화 방식:

```cpp
mutable std::shared_mutex mtx_;
```

- 조회 API: `shared_lock` (읽기 병렬)
- 변경 API: `unique_lock` (쓰기 직렬)

원칙:
- 외부 스레드는 락을 직접 다루지 않음
- AccountManager public 메서드 호출만으로 thread-safe 사용

---

## 5) 왜 이렇게 구현했는가

1. 멀티마켓에서 잔고를 직접 수정하면 race condition 발생
2. 주문 전 예약이 없으면 초과 주문/잔고 부족 에러 가능
3. 체결은 부분 체결이 흔해 토큰 단위 누적 정산이 필요
4. 재시작/외부 수동 거래를 위해 `syncWithAccount`가 필수

결론:
- 이 헤더는 "예약 기반 자산 관리 + 부분 체결 정산 + 동기화 복구"를
  하나의 안전한 계약으로 묶은 구조다.

---

## 6) 빠른 사용 예시

```cpp
AccountManager mgr(account, {"KRW-BTC", "KRW-ETH"});

auto token = mgr.reserve("KRW-BTC", 100'000.0);
if (token) {
    mgr.finalizeFillBuy(*token, 60'000.0, 0.0012, 50'000'000.0);
    mgr.finalizeFillBuy(*token, 40'000.0, 0.0008, 50'000'000.0);
    mgr.finalizeOrder(std::move(*token));  // 미사용 예약 정리 + 토큰 비활성화
}

mgr.finalizeFillSell("KRW-BTC", 0.0020, 110'000.0);
```

---

## 7) 함께 보면 좋은 파일

- `src/trading/allocation/AccountManager.cpp`
  - 실제 정산/보정/동기화 로직
- `tests/test_account_manager_unified.cpp`
  - reserve/finalize/sync/dust/oversell 검증 시나리오
- `docs/tests_md/accountManager/CHANGELOG_syncWithAccount_fix.md`
  - syncWithAccount 관련 수정 배경

