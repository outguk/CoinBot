// src/trading/allocation/AccountManager.cpp

#include "AccountManager.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util/Config.h"

namespace trading::allocation {

    // ============================================================
    // ReservationToken 구현
    // ============================================================

    ReservationToken::ReservationToken(AccountManager* mng, std::string market,
                                       core::Amount amount, uint64_t id)
        : manager_(mng)
        , market_(std::move(market))
        , amount_(amount)
        , consumed_(0)
        , id_(id)
        , active_(true)
    {}

    ReservationToken::ReservationToken(ReservationToken&& other) noexcept
        : manager_(other.manager_)
        , market_(std::move(other.market_))
        , amount_(other.amount_)
        , consumed_(other.consumed_)
        , id_(other.id_)
        , active_(other.active_)
    {
        // 이동 후 원본 비활성화 (소멸자에서 release 방지)
        other.manager_ = nullptr;
        other.active_ = false;
    }

    // = 도 오버라이딩으로 move 처리
    ReservationToken& ReservationToken::operator=(ReservationToken&& other) noexcept {
        if (this != &other) {
            // 기존 토큰이 active면 먼저 해제 (토큰 객체 없이)
            if (active_ && manager_ != nullptr) {
                manager_->releaseWithoutToken(market_, amount_ - consumed_);
            }

            manager_ = other.manager_;
            market_ = std::move(other.market_);
            amount_ = other.amount_;
            consumed_ = other.consumed_;
            id_ = other.id_;
            active_ = other.active_;

            other.manager_ = nullptr;
            other.active_ = false;
        }
        return *this;
    }

    ReservationToken::~ReservationToken() {
        // 안전망: active 상태로 파괴되면 자동 해제 (토큰 객체 없이)
        if (active_ && manager_ != nullptr) {
            manager_->releaseWithoutToken(market_, amount_ - consumed_);
        }
    }

    void ReservationToken::addConsumed(core::Amount executed_krw) {
        consumed_ += executed_krw;
        // 체결 금액이 예약 금액을 초과할 수 없음
        if (consumed_ > amount_) {
            consumed_ = amount_;
        }
    }

    // ============================================================
    // AccountManager 구현
    // ============================================================

    AccountManager::AccountManager(const core::Account& account,
                                   const std::vector<std::string>& markets)
    {
        // 생성자에서 throw는 중간 상태를 만들지 않기 위해 catch를 놓지 않고 만들지 않거나 프로그램 진입부에서 호출
        if (markets.empty()) {
            throw std::invalid_argument("AccountManager: markets cannot be empty");
        }

        // 1단계: 마켓별 예산 초기화 (0으로)
        for (const auto& market : markets) {
            MarketBudget budget;
            budget.market = market;
            budget.available_krw = 0;
            budget.reserved_krw = 0;
            budget.coin_balance = 0;
            budget.avg_entry_price = 0;
            budget.initial_capital = 0;
            budget.realized_pnl = 0;

            budgets_[market] = std::move(budget);
        }

        // 2단계: 실제 계좌의 코인 포지션 반영 및 initial_capital 설정
        // dust 임계값: Config에서 로드 (formatDecimalFloor로 인한 잔량 처리)
        const auto& cfg = util::AppConfig::instance().account;
        const core::Amount init_dust_threshold = cfg.init_dust_threshold_krw;

        for (const auto& pos : account.positions) {
            // 마켓 코드 구성: "KRW-" + currency (예: "BTC" -> "KRW-BTC")
            std::string market = "KRW-" + pos.currency;

            auto it = budgets_.find(market);
            if (it != budgets_.end()) {
                MarketBudget& budget = it->second;

                // 코인 가치 계산
                core::Amount coin_value = pos.free * pos.avg_buy_price;

                // dust 체크: 설정값 미만은 무시
                if (coin_value < init_dust_threshold) {
                    // dust 잔량 → 0으로 처리, KRW로 거래 가능 상태 유지
                    budget.coin_balance = 0;
                    budget.avg_entry_price = 0;
                    budget.initial_capital = 0;  // 다음 단계에서 KRW로 설정됨
                    // available_krw는 0 유지 (3단계에서 배분)
                } else {
					// 거래 가능한 코인 보유 시 평단가와 초기 자본 설정
                    budget.coin_balance = pos.free;
                    budget.avg_entry_price = pos.avg_buy_price;
                    budget.initial_capital = coin_value;
                    budget.available_krw = 0;  // 전량 매수 상태
                }
            }
        }

        // 3단계: 남은 KRW를 코인이 없는 마켓에 균등 배분
        core::Amount remaining_krw = account.krw_free;

        if (remaining_krw <= 0 && budgets_.size() > 0) {
            // 모든 자산이 코인으로 전환된 상태 (정상)
            return;
        }

        // 코인이 없는 마켓 카운트
        int markets_without_coin = 0;
        for (const auto& [_, budget] : budgets_) {
            if (budget.coin_balance == 0) {
                markets_without_coin++;
            }
        }

        if (markets_without_coin > 0) {
            // 남은 KRW를 코인 없는 마켓에 전액 균등 분배
            core::Amount per_market = remaining_krw / static_cast<double>(markets_without_coin);

            for (auto& [_, budget] : budgets_) {
                if (budget.coin_balance == 0) {
                    budget.available_krw = per_market;
                    budget.initial_capital = per_market;
                }
            }
        }
    }

    // --- 조회 메서드 ---
    std::optional<MarketBudget> AccountManager::getBudget(std::string_view market) const {
        std::shared_lock lock(mtx_);

        auto it = budgets_.find(std::string(market));
        if (it == budgets_.end()) {
            return std::nullopt;
        }
        return it->second;  // 복사본 반환
    }

    std::map<std::string, MarketBudget> AccountManager::snapshot() const {
        std::shared_lock lock(mtx_);
        return budgets_;  // 복사본 반환
    }

    // --- 예약 메서드 ---
    std::optional<ReservationToken> AccountManager::reserve(std::string_view market,
                                                            core::Amount krw_amount) {
        std::unique_lock lock(mtx_);

        auto it = budgets_.find(std::string(market));
		if (it == budgets_.end()) { // .end()는 마지막 요소 다음 위치를 가리킴
            stats_.reserve_failures.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;  // 마켓 미등록
        }

        // 입력 검증: 0 또는 음수 금액
        if (krw_amount <= 0) {
            // 경고: 0 이하 예약 시도는 로직 오류 가능성 높음
            stats_.reserve_failures.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        MarketBudget& budget = it->second;

        // 잔액 확인
        if (budget.available_krw < krw_amount) {
            stats_.reserve_failures.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;  // 잔액 부족
        }

        // 예약 적용
        budget.available_krw -= krw_amount;
        budget.reserved_krw += krw_amount;

        uint64_t token_id = next_token_id_.fetch_add(1, std::memory_order_relaxed);
        stats_.total_reserves.fetch_add(1, std::memory_order_relaxed);

        // 토큰 생성 (private 생성자 호출)
        return ReservationToken(this, std::string(market), krw_amount, token_id);
    }

    void AccountManager::release(ReservationToken&& token) {
        if (!token.isActive()) {
            return;  // 이미 비활성화된 토큰
        }

        // releaseWithoutToken 재사용으로 코드 중복 제거
        releaseWithoutToken(token.market(), token.remaining());
        token.deactivate();
    }

    void AccountManager::releaseInternal(const std::string& market,
                                         core::Amount remaining_amount) {
        // 호출자가 락을 보유해야 함
        auto it = budgets_.find(market);
        if (it == budgets_.end()) {
            return;
        }

        MarketBudget& budget = it->second;

        // 미사용 금액 복구
        budget.reserved_krw -= remaining_amount;
        budget.available_krw += remaining_amount;

        // 음수 방지 (안전망)
        if (budget.reserved_krw < 0) {
            budget.reserved_krw = 0;
        }
    }

    void AccountManager::releaseWithoutToken(const std::string& market,
                                             core::Amount remaining_amount) noexcept {
        // 토큰 객체 없이 예약 해제 (락 포함, noexcept 보장)
        // ReservationToken의 operator= 및 소멸자에서만 사용
        //
        // 설계 의도:
        // - operator=와 소멸자에서는 토큰 객체를 release()에 전달할 수 없음
        // - 따라서 마켓과 금액만으로 해제하는 별도 경로 필요
        // - noexcept 보장으로 소멸자와 move 연산 안전성 확보
        try {
            std::unique_lock lock(mtx_);
            releaseInternal(market, remaining_amount);
            stats_.total_releases.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            // noexcept 보장: 예외 발생 시 무시 (최선의 노력)
            // 실전에서는 Logger로 기록 권장
        }
    }

    // --- 체결 정산 메서드 ---

    void AccountManager::finalizeFillBuy(ReservationToken& token,
                                         core::Amount executed_krw,
                                         core::Volume received_coin,
                                         core::Price fill_price) {
        if (!token.isActive()) {
            return;  // 비활성화된 토큰
        }

        // 입력 검증: 0 이하 값 방어
        if (executed_krw <= 0 || received_coin <= 0 || fill_price <= 0) {
            // 경고: 잘못된 입력 (0 또는 음수)
            // 실전: Logger로 기록 권장
            return;
        }

        // 예약 초과 체크
        if (executed_krw > token.remaining()) {
            // 경고: 예약 금액 초과 체결 시도
            // 실전: Logger로 기록 권장
            // 최선의 노력: 예약 잔액만큼만 처리
            executed_krw = token.remaining();
            if (executed_krw <= 0) {
                return;
            }
        }

        std::unique_lock lock(mtx_);

        auto it = budgets_.find(token.market());
        if (it == budgets_.end()) {
            return;
        }

        MarketBudget& budget = it->second;

        // reserved_krw 차감 (체결 완료된 금액)
        budget.reserved_krw -= executed_krw;
        if (budget.reserved_krw < 0) {
            // 경고 로그 필요 (로직 오류 가능성)
            budget.reserved_krw = 0;
        }

        // 평균 매수가 재계산 (가중 평균)
        // new_avg = (old_total + new_total) / (old_qty + new_qty)
        core::Amount old_total = budget.coin_balance * budget.avg_entry_price;
        core::Amount new_total = received_coin * fill_price;
        core::Volume new_balance = budget.coin_balance + received_coin;

        if (new_balance > 0) {
            budget.avg_entry_price = (old_total + new_total) / new_balance;
        }

        // 코인 잔고 증가
        budget.coin_balance = new_balance;

        // 토큰에 체결 금액 누적
        token.addConsumed(executed_krw);

        stats_.total_fills_buy.fetch_add(1, std::memory_order_relaxed);
    }

    void AccountManager::finalizeFillSell(std::string_view market,
                                          core::Volume sold_coin,
                                          core::Amount received_krw) {
        // 입력 검증
        // sold_coin <= 0: 매도량 없음 (로직 오류)
        // received_krw < 0: 음수 금액 (데이터 오류)
        // received_krw == 0: 허용 — delta_funds 누락 시에도 코인 차감 필요
        if (sold_coin <= 0 || received_krw < 0) {
            return;
        }

        std::unique_lock lock(mtx_);

        auto it = budgets_.find(std::string(market));
        if (it == budgets_.end()) {
            return;
        }

        MarketBudget& budget = it->second;

        // 코인 잔고 감소
        const core::Volume balance_before = budget.coin_balance;
        budget.coin_balance -= sold_coin;

        // 과매도 감지 및 보정이 필요??

        // KRW 잔고 증가 (실제 매도량에 대한 금액만)
        budget.available_krw += received_krw;

        stats_.total_fills_sell.fetch_add(1, std::memory_order_relaxed);
    }

    void AccountManager::finalizeSellOrder(std::string_view market,
                                           std::optional<core::Price> mark_price) {
        std::unique_lock lock(mtx_);

        auto it = budgets_.find(std::string(market));
        if (it == budgets_.end()) {
            return;
        }

        MarketBudget& budget = it->second;
        const auto& cfg = util::AppConfig::instance().account;

        bool should_clear_coin = false;

        // 1) 수량 기준 dust: 부동소수점 잔량을 정리한다.
        if (budget.coin_balance < cfg.coin_epsilon) {
            should_clear_coin = true;
        }
        // 2) 가치 기준 dust: mark_price(현재가) 기준으로 판정한다.
        //    mark_price가 없거나 비정상(<=0)이면 가치 판정 생략 — avg_entry_price fallback 없음.
        //    (avg_entry_price로 fallback하면 가격 급등 시 오판 문제가 재발한다)
        else if (mark_price.has_value() && *mark_price > 0.0) {
            const core::Amount remaining_value = budget.coin_balance * (*mark_price);
            if (remaining_value < cfg.init_dust_threshold_krw) {
                should_clear_coin = true;
            }
        }

        if (!should_clear_coin) {
            return;
        }

        budget.coin_balance = 0;
        budget.avg_entry_price = 0;

        // 주문 종료 시점에만 실현 손익을 확정한다.
        budget.realized_pnl = budget.available_krw - budget.initial_capital;
    }

    void AccountManager::finalizeOrder(ReservationToken&& token) {
        if (!token.isActive()) {
            return;
        }

        std::unique_lock lock(mtx_);

        auto it = budgets_.find(token.market());
        if (it == budgets_.end()) {
            token.deactivate();
            return;
        }

        MarketBudget& budget = it->second;

        // 미사용 잔액을 available로 복구 (releaseInternal 재사용으로 코드 중복 제거)
        core::Amount remaining = token.remaining();
        if (remaining > 0) {
            releaseInternal(token.market(), remaining);
        }

        // formatDecimalFloor로 인한 reserved_krw 미세 잔량 정리
        // 매수 주문 후 원 단위 이하 잔량이 reserved에 남을 수 있음
        const auto& cfg = util::AppConfig::instance().account;
        if (budget.reserved_krw > 0 && budget.reserved_krw < cfg.krw_dust_threshold) {
            budget.available_krw += budget.reserved_krw;
            budget.reserved_krw = 0;
        }

        token.deactivate();
    }

    // --- 동기화 메서드 ---

    // 시작/수동점검 전용 전체 재구축
    // 런타임 복구에서는 호출 금지
    void AccountManager::rebuildFromAccount(const core::Account& account) {
        std::unique_lock lock(mtx_);

        // 실제 KRW 잔고
        core::Amount actual_free_krw = account.krw_free;

        // Config에서 dust 임계값 로드 (생성자와 동일)
        const auto& cfg = util::AppConfig::instance().account;
        const core::Amount init_dust_threshold = cfg.init_dust_threshold_krw;

        // 1단계: 모든 마켓의 코인 잔고를 먼저 0으로 리셋
        // 중요: account.positions에 없는 마켓(외부 거래로 전량 매도 등)을 처리하기 위함
        for (auto& [market, budget] : budgets_) {
            budget.coin_balance = 0;
            budget.avg_entry_price = 0;
            // available_krw와 reserved_krw는 아래에서 재설정
        }

        // 2단계: account.positions에 있는 코인만 설정 + 전량 거래 모델 적용
        // 생성자와 동일하게 가치 기준 dust 처리
        for (const auto& pos : account.positions) {
            // 마켓 코드 구성: "KRW-" + currency
            std::string market = "KRW-" + pos.currency;

            auto it = budgets_.find(market);
            if (it != budgets_.end()) {
                MarketBudget& budget = it->second;

                // 코인 가치 계산 (생성자와 동일)
                core::Amount coin_value = pos.free * pos.avg_buy_price;

                // dust 체크: 가치 기준 (생성자와 동일)
                if (coin_value < init_dust_threshold) {
                    // dust 잔량 → 0으로 처리
                    budget.coin_balance = 0;
                    budget.avg_entry_price = 0;
                    // available_krw는 3단계에서 배분됨
                } else {
                    // 거래 가능한 코인 보유
                    budget.coin_balance = pos.free;
                    budget.avg_entry_price = pos.avg_buy_price;

                    // 전량 거래 모델: 코인 보유 → KRW = 0
                    budget.available_krw = 0.0;
                    // 추후 복구 시 미체결 주문을 가지고 있는 경우 수정이 필요??????????
                    budget.reserved_krw = 0.0;
                }
            }
        }

        // 3단계: 코인이 없는 마켓 식별 (KRW 보유 가능 마켓)
        // coin_epsilon은 formatDecimalFloor로 인한 미세 잔량만 체크
        std::vector<std::string> krw_markets;
        for (const auto& [market, budget] : budgets_) {
            if (budget.coin_balance < cfg.coin_epsilon) {
                krw_markets.push_back(market);
            }
        }

        // 모든 마켓이 코인 보유 중 → 정상 상태 (전량 매수 완료)
        if (krw_markets.empty()) {
            return;
        }

        // 4단계: 실제 free KRW를 코인 없는 마켓에 전액 균등 분배
        core::Amount per_market = actual_free_krw / static_cast<double>(krw_markets.size());

        for (const auto& market : krw_markets) {
            auto it = budgets_.find(market);
            if (it != budgets_.end()) {
                it->second.available_krw = per_market;
                // reserved는 복구 시 0으로 리셋 (미체결 주문은 이미 취소되었다고 가정)
                it->second.reserved_krw = 0.0;
            }
        }
    }

} // namespace trading::allocation
