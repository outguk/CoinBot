#pragma once


#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "trading/indicators/IndicatorTypes.h"
#include "core/domain/OrderRequest.h"
#include "core/domain/OrderTypes.h"
#include "core/domain/Types.h"

namespace trading {

    // 실거래/데모 공통으로 전략이 "의사결정"에 필요한 최소 계좌 스냅샷
    // - 현물(Long-only) 기준: 매수는 KRW, 매도(청산)는 보유 코인 수량이 핵심
    struct AccountSnapshot final {
        double krw_available{ 0.0 };   // 주문 가능 KRW
        double coin_available{ 0.0 }; // 해당 market의 보유 코인 수량(청산에 사용)

        [[nodiscard]] constexpr bool canBuy() const noexcept { return krw_available > 0.0; }
        [[nodiscard]] constexpr bool canSell() const noexcept { return coin_available > 0.0; }
    };

    
    // 케이스 A(포지션만 복구)용 최소 타입
    // - 미체결 주문은 시작 루틴에서 전부 취소하는 정책이므로,
    //  “미체결 복구용 데이터(OpenOrderSnapshot 등)”는 여기서 다루지 않는다.
    struct PositionSnapshot final {
        double coin{ 0.0 };              // 해당 마켓 보유 수량
        double avg_entry_price{ 0.0 };   // 평균 매수가(없거나 신뢰 불가면 0)
        [[nodiscard]] constexpr bool hasPosition() const noexcept { return coin > 0.0; }
    };

    // 체결 이벤트(전략이 entryPrice 확정 / pending 해제에 사용 / 
    // 이 체결이 내가 낸 주문인지? 진입인지? 청산인지? / 이제 다음 캔들에서 어떤 판단을 해야 하는지 ? )
    // - identifier로 전략이 "내가 낸 주문"인지 매칭
    struct FillEvent final {
        std::string identifier;
        core::OrderPosition position{ core::OrderPosition::BID };
        core::Price fill_price{ 0.0 };
        core::Volume filled_volume{ 0.0 };

        FillEvent() = default;

        explicit FillEvent(std::string_view cid,
            core::OrderPosition pos,
            double price,
            double vol = 0.0)
            : identifier(cid), position(pos), fill_price(price), filled_volume(vol) {
        }

        explicit FillEvent(std::string cid,
            core::OrderPosition pos,
            double price,
            double vol = 0.0) noexcept
            : identifier(std::move(cid)), position(pos), fill_price(price), filled_volume(vol) {
        }
    };
    // 주문 상태 이벤트(실거래 필수)
    // - FillEvent는 "부분 체결"로 여러 번 올 수 있고, 그 자체로 "완전 체결"을 보장하지 않는다.
    // - 따라서 Pending 해제 / 롤백(취소·거절) / 최종 확정(Filled)은 OrderStatusEvent를 기준으로 처리하는 게 안전하다.
    // - client_order_id로 전략이 "내가 낸 주문"인지 매칭
    struct OrderStatusEvent final {
        std::string identifier;
        core::OrderStatus status{ core::OrderStatus::Pending };
        core::OrderPosition position{ core::OrderPosition::BID };

        // (선택) WS/엔진이 제공할 수 있으면 채워서 전달
        // - 시장가 매수(Amount)처럼 원주문 수량을 알기 어려운 경우가 있어, 전략이 이 값에 의존하지 않는 설계를 권장
        double executed_volume{ 0.0 };
        double remaining_volume{ 0.0 };

        OrderStatusEvent() = default;

        OrderStatusEvent(std::string cid,
            core::OrderStatus st,
            core::OrderPosition pos,
            double execVol = 0.0,
            double remVol = 0.0) noexcept
            : identifier(std::move(cid)),
            status(st),
            position(pos),
            executed_volume(execVol),
            remaining_volume(remVol) {
        }
    };

    struct Decision final {
        // order가 있으면 SubmitOrder, 없으면 None/NoAction
        std::optional<core::OrderRequest> order{}; 

        [[nodiscard]] static constexpr Decision none() noexcept { return {}; }

        // 로깅 목적의 "NoAction"을 유지하고 싶다면 플래그만 별도로 둬도 됨
        bool is_no_action{false};

        [[nodiscard]] static constexpr Decision noAction() noexcept {
            Decision d;
            d.is_no_action = true;
            return d;
        }

        [[nodiscard]] static Decision submit(core::OrderRequest req) {
            Decision d;
            d.order = std::move(req);
            d.is_no_action = false;
            return d;
        }

        [[nodiscard]] constexpr bool hasOrder() const noexcept { return order.has_value(); }
    };

    // “지표/필터 결과를 한 번에” 들고 다니는 스냅샷
    struct Snapshot final {
        // 입력(현재 봉)
        double close{ 0.0 };

        // 지표(준비 여부 포함)
        Value<double> rsi{};
        Value<double> closeN{};
        Value<double> volatility{}; // ChangeVolatilityIndicator 결과(수익률 stdev)

        // 파생 필터 값
        bool trendReady{ false };
        double trendStrength{ 0.0 };

        // 최종 “시장 적합성”
        bool marketOk{ false };
    };


    // (선택) 전략 식별자: 문자열 복사 없이 string_view로 사용 가능
    using StrategyId = std::string_view;

} // namespace trading
