// src/trading/allocation/AccountManager.h
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/domain/Account.h"
#include "core/domain/Types.h"

namespace trading::allocation {

    /*
     * MarketBudget
     *
     * 마켓별 자산 현황 (전량 매수/매도 방식 최적화)
     * AccountManager에서 관리하며, 읽기 전용으로 외부에 제공
     *
     * [전량 거래 방식]
     * - Flat 상태: available_krw만 존재, coin_balance = 0
     * - InPosition 상태: coin_balance만 존재, available_krw ≈ 0
     * - 평가자산은 저장하지 않고 필요시 계산 (getCurrentEquity)
     *
     * [필드 설명]
     * - available_krw: 거래 가능 KRW (Flat 상태에서만 > 0)
     * - reserved_krw: 주문 대기 중인 예약 KRW (PendingEntry 상태)
     * - coin_balance: 보유 코인 수량 (InPosition 상태에서만 > 0)
     * - avg_entry_price: 매수 평균 단가
     * - initial_capital: 초기 자본 (고정값, 수익률 계산용)
     * - realized_pnl: 실현 손익 누적 (매도 완료 시 갱신)
     */
    struct MarketBudget {
        std::string market;             // 마켓 코드 (예: "KRW-BTC")

        // 현재 상태 (전량 거래 시 둘 중 하나만 0이 아님)
        core::Amount available_krw{0};  // 거래 가능 KRW
        core::Amount reserved_krw{0};   // 예약된 KRW (주문 대기)
        core::Volume coin_balance{0};   // 보유 코인 수량
        core::Price avg_entry_price{0}; // 평균 매수 단가

        // 통계 추적
        core::Amount initial_capital{0}; // 초기 자본 (고정)
        core::Amount realized_pnl{0};    // 실현 손익 누적

        // 현재 평가자산 계산 (저장하지 않고 필요시 계산)
        core::Amount getCurrentEquity(core::Price current_price) const {
            return available_krw + reserved_krw + (coin_balance * current_price);
        }

        // 수익률 계산 (%)
        double getROI(core::Price current_price) const {
            if (initial_capital == 0) return 0.0;
            return (getCurrentEquity(current_price) - initial_capital) / initial_capital * 100.0;
        }

        // 실현 수익률 (%)
        double getRealizedROI() const {
            if (initial_capital == 0) return 0.0;
            return realized_pnl / initial_capital * 100.0;
        }
    };

    // Forward declaration
    class AccountManager;

    /*
     * ReservationToken
     *
     * KRW 예약을 나타내는 RAII 토큰 (move-only, 복사 금지)
     * - 객체가 만들어질 때 자원을 얻고 객체가 사라질 때 자원을 자동으로 반환하는 설계 방식
     *
     * [생명주기] 토큰은 각 마켓별 엔진이 마켓 토큰을 소유한다.
     * 1. reserve() 성공 시 생성
     * 2. 주문 제출 후 체결 이벤트 수신
     * 3. finalizeFillBuy()로 체결 금액 정산
     * 4. finalizeOrder()로 토큰 비활성화
     *
     * [안전망]
     * - 소멸자에서 active 상태면 자동으로 release() 호출
     * - 체결 없이 토큰 파괴 시 예약 KRW 복구
     *
     * [Thread-Safety]
     * - 토큰 자체는 단일 스레드(소유 마켓 스레드)에서만 사용
     * - AccountManager 호출은 내부적으로 락으로 보호됨
     */
    class ReservationToken {
    public:
        // AccountManager::reserve()에서만 생성 가능
        friend class AccountManager;

        // 복사 금지
        ReservationToken(const ReservationToken&) = delete;
        ReservationToken& operator=(const ReservationToken&) = delete;

        // Move 가능
        ReservationToken(ReservationToken&& other) noexcept;
        ReservationToken& operator=(ReservationToken&& other) noexcept;

        // 소멸자: active 상태면 자동 release (소멸자를 명시한다는건 수명 종료에 의미가 있다는 의미)
        ~ReservationToken();

        // 조회 메서드
        const std::string& market() const noexcept { return market_; }
        core::Amount amount() const noexcept { return amount_; }
        core::Amount consumed() const noexcept { return consumed_; }
        core::Amount remaining() const noexcept { return amount_ - consumed_; }
        bool isActive() const noexcept { return active_; }
        uint64_t id() const noexcept { return id_; }

        // 체결 금액 누적 (finalizeFillBuy 호출 시 내부에서 사용)
        void addConsumed(core::Amount executed_krw);

        // 토큰 비활성화 (finalizeOrder 호출 시 내부에서 사용)
        void deactivate() noexcept { active_ = false; }

    private:
        // AccountManager에서만 생성
        ReservationToken(AccountManager* mgr, std::string market,
                        core::Amount amount, uint64_t id);

        AccountManager* manager_{nullptr};  // 소유자 참조
        std::string market_;                // 예약 마켓
        core::Amount amount_{0};            // 총 예약 금액
        core::Amount consumed_{0};          // 사용(체결)된 금액
        uint64_t id_{0};                    // 고유 ID (디버깅용)
        bool active_{true};                 // 활성 상태
    };

    /*
     * AccountManager
     *
     * 멀티마켓 전량 거래 환경에서 자산을 관리하는 thread-safe 클래스
     *
     * [역할]
     * 1. 마켓별 독립적 자산 관리 (초기화 시 균등 배분)
     * 2. 주문 전 KRW 예약 (reserve)
     * 3. 체결 시 잔고 업데이트 (finalizeFillBuy/Sell)
     * 4. 실현 손익 추적
     *
     * [전량 거래 방식]
     * - 각 마켓은 할당된 금액 전부로 매수 → 전부 매도 반복
     * - 마켓 간 자금 이동 없음 (rebalance 제거)
     * - 수익/손실은 각 마켓에서 독립적으로 누적
     *
     * [Thread-Safety]
     * - 모든 public 메서드는 shared_mutex로 보호
     * - 읽기 전용 메서드(getBudget, snapshot)는 shared_lock
     * - 쓰기 메서드(reserve, release, finalize*)는 unique_lock
     *
     * [주문 흐름]
     *   reserve(available_krw) ──► submitBuyOrder() ──► finalizeFillBuy() ──► finalizeOrder()
     *       │                            │                  (부분 체결 반복)
     *       │                       [실패/취소]
     *       └────────────────────────► release()
     *
     * [설계 원칙]
     * - 마켓별 독립 운영 (예비금 없음)
     * - 예약 기반으로 잔고 부족 주문 방지
     * - 전량 체결 지원
     */
    class AccountManager {
    public:
        /*
         * 생성자
         * @param account: 실제 계좌 정보 (REST API getMyAccount() 결과)
         * @param markets: 거래할 마켓 목록 (예: {"KRW-BTC", "KRW-ETH"})
         *
         * 초기화:
         * - account.krw_free를 마켓에 균등 배분 (예비금 없음)
         * - account.positions에서 각 마켓의 코인 잔고 및 평단가 반영
         * - initial_capital 설정 (수익률 계산용)
         */
        AccountManager(const core::Account& account,
                      const std::vector<std::string>& markets);

        // 복사/이동 금지
        AccountManager(const AccountManager&) = delete;
        AccountManager& operator=(const AccountManager&) = delete;
        AccountManager(AccountManager&&) = delete;
        AccountManager& operator=(AccountManager&&) = delete;

        ~AccountManager() = default;

        // --- 조회 메서드 (shared_lock) ---

        /*
         * 특정 마켓의 예산 정보 조회
         * @return 복사본 (thread-safe)
         */
        std::optional<MarketBudget> getBudget(std::string_view market) const;

        /*
         * 전체 마켓 스냅샷 조회
         * @return 모든 마켓의 MarketBudget 맵
         */
        std::map<std::string, MarketBudget> snapshot() const;

        // --- 예약 메서드 (unique_lock) ---

        /*
         * KRW 예약 요청
         * @param market: 마켓 코드
         * @param krw_amount: 예약할 KRW 금액
         * @return 예약 토큰 (실패 시 nullopt)
         *
         * 실패 조건:
         * - 해당 마켓이 등록되지 않음
         * - available_krw < krw_amount
         */
        std::optional<ReservationToken> reserve(std::string_view market,
                                                core::Amount krw_amount);

        /*
         * 예약 해제 (주문 실패/취소 시), 비정상 종료 시
         * @param token: 예약 토큰 (move)
         *
         * unique_lock(mtx_)를 스스로 잡아 thread-safe
         * 미사용 금액(amount - consumed)을 available_krw로 복구
         */
        void release(ReservationToken&& token);

        // --- 체결 정산 메서드 (unique_lock) ---

        /*
         * 매수 체결 정산 (부분 체결 시 여러 번 호출 가능)
         * @param token: 예약 토큰 참조
         * @param executed_krw: 이번 체결에 사용된 KRW
         * @param received_coin: 이번 체결로 받은 코인 수량
         * @param fill_price: 체결 단가
         *
         * [입력 검증]
         * - executed_krw, received_coin, fill_price <= 0 → 무시
         * - executed_krw > token.remaining() → 예약 잔액만큼만 처리 (clamp)
         *
         * [동작]
         * 1. reserved_krw -= executed_krw
         * 2. coin_balance += received_coin
         * 3. avg_entry_price 재계산 (가중 평균)
         * 4. token.consumed += executed_krw
         */
        void finalizeFillBuy(ReservationToken& token,
                            core::Amount executed_krw,
                            core::Volume received_coin,
                            core::Price fill_price);

        /*
         * 매도 체결 정산 (부분 체결 시 여러 번 호출 가능)
         * @param market: 마켓 코드
         * @param sold_coin: 판매한 코인 수량
         * @param received_krw: 수령한 KRW (수수료 차감 후)
         *
         * [입력 검증]
         * - sold_coin <= 0 또는 received_krw <= 0 → 무시
         *
         * [동작]
         * 1. coin_balance -= sold_coin
         * 2. 과매도 감지 및 보정:
         *    - coin_balance < 0 → 실제 보유량만큼만 매도된 것으로 간주
         *    - KRW도 비율에 맞게 조정 (과매도분 KRW는 받지 않음)
         * 3. available_krw += actual_received_krw
         * 4. 잔량 dust 처리 (이중 체크)
         *
         * [과매도 예시]
         * - 보유: 0.001 BTC
         * - 매도 시도: 0.002 BTC → 200,000 KRW
         * - 실제 처리: 0.001 BTC → 100,000 KRW만 반영
         *
         * [Dust 처리 - 이중 체크]
         * 1차: 수량 기준 (coin_epsilon = 1e-7)
         *   - formatDecimalFloor(8자리)로 인한 부동소수점 오차 제거
         *   - 예: 0.00000001 BTC → dust
         *
         * 2차: 가치 기준 (init_dust_threshold_krw = 5,000원)
         *   - 거래 불가능한 저가 코인 잔량 제거
         *   - RsiMeanReversionStrategy의 hasMeaningfulPos와 동일 기준
         *   - 예: 1,000 DOGE @ 0.003원 = 3원 → dust
         *
         * [전략 일관성]
         * - 전략: posNotional >= min_notional_krw → "의미 있는 포지션"
         * - AccountManager: remaining_value < init_dust_threshold_krw → "dust"
         * - 동일한 기준(5,000원)으로 상태 불일치 방지
         */
        void finalizeFillSell(std::string_view market,
                             core::Volume sold_coin,
                             core::Amount received_krw);

        /*
         * 주문 완료 처리 (체결 완료 또는 취소 후)
         * @param token: 예약 토큰 (move)
         *
         * [동작]
         * 1. 미사용 잔액(remaining) → available_krw로 복구
         * 2. 토큰 비활성화
         */
        void finalizeOrder(ReservationToken&& token);

        // --- 동기화 메서드 (unique_lock) ---

        /*
         * 물리 계좌와 동기화 (API 조회 결과 반영)
         * @param account: REST API로 조회한 실제 계좌 정보
         *
         * [동작]
         * 1. 모든 마켓의 코인 잔고를 0으로 리셋 (외부 거래 대응)
         * 2. account.positions에 있는 코인만 설정
         * 3. Dust 처리: 가치 기준 (init_dust_threshold_krw 미만은 0으로)
         * 4. 코인을 보유한 마켓의 available_krw를 0으로 설정 (전량 거래 모델)
         * 5. 코인이 없는 마켓에 실제 KRW를 균등 분배
         *
         * [외부 거래 대응]
         * - 1단계 전체 리셋으로 "포지션이 사라진 마켓" 자동 감지
         * - 예: 외부 앱에서 전량 매도 → positions 없음 → coin_balance = 0
         * - 리셋 없이는 기존 coin_balance가 유지되어 상태 불일치 발생
         *
         * [전량 거래 모델 준수]
         * - coin_balance > 0 → available_krw = 0 (전량 코인 보유)
         * - coin_balance = 0 → available_krw > 0 (전량 KRW 보유)
         * - 상태 불변 조건: coin > 0 XOR krw > 0
         *
         * [Dust 처리 일관성]
         * - 생성자와 동일한 가치 기준 (init_dust_threshold_krw = 5,000원)
         * - 예: 0.00001 BTC @ 100M = 1,000원 → dust 처리
         * - coin_epsilon은 코인 식별용 (formatDecimalFloor 미세 잔량)
         *
         * [사용 시나리오]
         * - 프로그램 재시작 시 실제 계좌 상태 복구
         * - 외부 수동 거래 후 동기화
         * - AccountManager와 실제 계좌 불일치 해소
         */
        void syncWithAccount(const core::Account& account);

        // --- 통계/디버깅 ---

        struct Stats {
            std::atomic<uint64_t> total_reserves{0};     // 총 예약 횟수
            std::atomic<uint64_t> total_releases{0};     // 총 해제 횟수
            std::atomic<uint64_t> total_fills_buy{0};    // 총 매수 체결 횟수
            std::atomic<uint64_t> total_fills_sell{0};   // 총 매도 체결 횟수
            std::atomic<uint64_t> reserve_failures{0};   // 예약 실패 횟수
        };

        const Stats& stats() const noexcept { return stats_; }

    private:
		// 핵심 예약 해제 로직 (락 없음, 호출자가 락 보유) 중복 락을 잡지 않도록 락 없이 설계
        // budgets_ 상태만 변경, 통계나 토큰 상태는 변경하지 않음
        void releaseInternal(const std::string& market,
                            core::Amount remaining_amount);

        // 토큰 없이 예약 해제 (락 포함, noexcept 보장)
        // ReservationToken의 operator= 및 소멸자에서만 사용
        // 토큰 객체를 release()에 넘길 수 없는 상황을 위한 경로
        void releaseWithoutToken(const std::string& market,
                                core::Amount remaining_amount) noexcept;

        // ReservationToken에서 접근
        friend class ReservationToken;

        mutable std::shared_mutex mtx_;                 // 읽기/쓰기 동기화
        std::map<std::string, MarketBudget> budgets_;   // 마켓별 예산
        std::atomic<uint64_t> next_token_id_{1};        // 토큰 ID 생성기
        Stats stats_;                                   // 통계
    };

} // namespace trading::allocation
