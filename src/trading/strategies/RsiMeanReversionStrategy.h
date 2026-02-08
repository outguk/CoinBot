#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/domain/Candle.h"
#include "StrategyTypes.h"

#include "trading/indicators/Sma.h"
#include "trading/indicators/ClosePriceWindow.h"
#include "trading/indicators/ChangeVolatilityIndicator.h"
#include "trading/indicators/RsiWilder.h"


namespace trading::strategies {

    /*
        RSI Mean Reversion (관점 A)
        - market_ 는 생성 시 고정 (단일 종목 전용 인스턴스)
        - 상태 머신 확정:
            Flat(미보유) -> PendingEntry(매수 주문 넣고 대기) -> InPosition(보유 중)
            InPosition -> PendingExit(매도 주문 넣고 대기) -> Flat
        - entry(진입가)/stop(손절가)/target(익절가) 은 체결 시점(onFill)에서 확정
        - SMA는 추후 추가하자
    */
    class RsiMeanReversionStrategy final {
    public:
        // 전략 파라미터(“숫자 튜닝”은 이 구조체에서만)
        struct Params final {
            // RSI
            std::size_t rsiLength{ 3 };
            double oversold{ 80 };
            double overbought{ 70 };
            
            // SMA 추가
            int smaLength{ 20 };
            double smaBand{ 0.0 };

            // 추세 강도(trendStrength) 계산용: close[N]
            // trendStrength = abs(close - closeN) / closeN
            std::size_t trendLookWindow{ 3 };
            double maxTrendStrength{ 1 }; // 3% 이상 한 방향으로 벌어졌으면 “추세 강함 → 평균회귀 부적합” 같은 필터

            // 변동성(최근 수익률 표준편차)
            std::size_t volatilityWindow{ 3 };
            double minVolatility{ 0 };    // 1% 이상이면 거래하기 적당

            // 포지션/리스크
            double riskPercent{ 10.0 };       // 매수에 사용할 KRW 비율(계좌의 krw_available 기준)
            double stopLossPct{ 1.0 };        // 진입가 대비 손절 %
            double profitTargetPct{ 1 };    // 진입가 대비 익절 %
        };

        enum class State : std::uint8_t {
            Flat = 0,
            PendingEntry =1,
            InPosition =2,
            PendingExit =3
        };

        

    public:
        RsiMeanReversionStrategy(std::string market, Params p);

        [[nodiscard]] StrategyId id() const noexcept { return "rsi_mean_reversion"; }
        [[nodiscard]] const std::string& market() const noexcept { return market_; }

        [[nodiscard]] State state() const noexcept { return state_; }
        [[nodiscard]] double entryPrice() const noexcept { return entry_price_.value_or(0.0); }
        [[nodiscard]] double stopPrice() const noexcept { return stop_price_.value_or(0.0); }
        [[nodiscard]] double targetPrice() const noexcept { return target_price_.value_or(0.0); }

        // (2) last snapshot getter (public)
    public:
        // 테스트용, 현재 사용x
        [[nodiscard]] const Snapshot& lastSnapshot() const noexcept { return last_snapshot_; }

        // 메인 진입점: “봉 1개” 들어오면, 주문 의도가 있으면 Decision::submit 반환
        [[nodiscard]] Decision onCandle(const core::Candle& c, const AccountSnapshot& account);

        // 체결 이벤트(부분/복수 체결 누적)
        void onFill(const FillEvent& fill);

        // 주문 상태 이벤트(최종 확정/롤백 기준)
        void onOrderUpdate(const trading::OrderStatusEvent& ev);

        // [필수] 엔진 submit(=주문 POST) 실패 시, Pending 상태 즉시 롤백(WS 이벤트가 절대 오지 않음)
        void onSubmitFailed();

        // - 미체결 주문은 상위(앱/엔진)에서 전부 취소 후 호출(프로그램 시작 시 작동)
        void syncOnStart(const trading::PositionSnapshot& pos);

        // 테스트/리셋
        void reset();

    private:
        // 1) 지표 업데이트 + 필터 계산을 한 번에 끝내는 단계
        [[nodiscard]] Snapshot buildSnapshot(const core::Candle& c);

        // 2) 스냅샷을 보고 “진입 주문 의도”를 만들지 결정
        [[nodiscard]] Decision maybeEnter(const Snapshot& s, const AccountSnapshot& account);

        // 3) 스냅샷을 보고 “청산 주문 의도”를 만들지 결정
        [[nodiscard]] Decision maybeExit(const Snapshot& s, const AccountSnapshot& account);

        // client_order_id 생성 (전략 내부 시퀀스)
        [[nodiscard]] std::string makeIdentifier(std::string_view tag);

        // 주문 생성 헬퍼(시장가 매수/매도), (전략 의도 → 엔진 제출용)
        [[nodiscard]] core::OrderRequest makeMarketBuyByAmount(double krw_amount, std::string_view tag);
        [[nodiscard]] core::OrderRequest makeMarketSellByVolume(double volume, std::string_view tag);

        // stop/target 계산(체결가 기준)
        void setStopsFromEntry(double entry);
        // 진입 시 손절, 익절가 확인용 로그 함수
        void logEntryConfirmed_(std::string_view reason, double entry);


    private:
        std::string market_;
        Params params_{};

        // 상태 + 주문 추적
        State state_{ State::Flat };
        std::optional<std::string> pending_client_id_{};

        // 부분 체결 누적용
        double pending_filled_volume_{ 0.0 }; // Σ filled_volume 지금까지 체결된 수량
        double pending_cost_sum_{ 0.0 };      // Σ (fill_price * filled_volume) 지금까지 체결된 총 비용
        double pending_last_price_{ 0.0 };    // 접수된 주문에서 가장 마지막 체결 가격(filled_volume이 0으로 올 때 폴백)


        // 포지션 정보(확정은 onFill에서)
        std::optional<double> entry_price_{};
        std::optional<double> stop_price_{};
        std::optional<double> target_price_{};

        // 지표들
        trading::indicators::RsiWilder rsi_{};
        //trading::indicators::Sma sma_{};
        trading::indicators::ClosePriceWindow closeN_{};
        trading::indicators::ChangeVolatilityIndicator vol_{};

        // client_order_id 시퀀스
        std::uint64_t seq_{ 0 };

        // 마지막 스냅샷 저장용 멤버 (private)
        Snapshot last_snapshot_{};

        // 같은 1분 캔들이 여러 번(업데이트 형태로) 들어오는 경우 중복 누적 방지용
        std::optional<std::string> last_candle_ts_{};
    };

} // namespace trading::strategies
