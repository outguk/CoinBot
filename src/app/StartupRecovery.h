#pragma once

#include <string>
#include <string_view>

#include "api/upbit/UpbitExchangeRestClient.h"
#include "api/upbit/IOrderApi.h"
#include "trading/strategies/StrategyTypes.h"

namespace app {
    // StartupRecovery:
    // - 프로그램 재시작 시점에 "미체결 주문 정리 + 포지션 복구"를 수행하는 유틸리티.
    // - 케이스 A 정책:
    //   1) 미체결 주문 중 "내 봇이 낸 주문"만 전부 취소한다.
    //   2) 계정 잔고에서 해당 market의 보유 포지션(수량/평단)을 읽어 PositionSnapshot을 만든다.
    //   3) 전략에 syncOnStart(PositionSnapshot)로 주입하여 상태를 InPosition/Flat로 복구한다.
    //
    // - 재시작 후에도 전략이 "이미 보유 중인 코인"을 모른 채 재진입(중복 매수)하는 사고를 방지.
    // - 재시작 시 남아있는 봇 주문을 정리하지 않으면, 새 주문과 충돌/중복 주문 위험이 커짐.

    // - 추후 주문 취소가 아닌 기존 주문 복원 기능을 추가!!!!!!!!!!!!!!
    class StartupRecovery final {
    public:
        struct Options {
            // 봇 주문만 취소하기 위한 prefix
            // 예) "rsi_mean_reversion:KRW-BTC:"
            // strategy_id + ":" + market + ":" 형태로 고정
            std::string bot_identifier_prefix;

            int cancel_retry = 3;
            int verify_retry = 3;

            // (보완 포인트) verify_retry 사이에 잠깐 sleep/backoff가 있으면
            // Upbit 반영 지연/레이트리밋에 더 안정적
        };

        // 기존 UpbitExchangeRestClient 오버로드 (하위 호환)
        template <class StrategyT>
        static void run(api::rest::UpbitExchangeRestClient& api,
            std::string_view market,
            const Options& opt,
            StrategyT& strategy)
        {
            cancelBotOpenOrders(api, market, opt);
            const trading::PositionSnapshot pos = buildPositionSnapshot(api, market);
            strategy.syncOnStart(pos);
        }

        // IOrderApi 오버로드 (MarketEngineManager용)
        template <class StrategyT>
        static void run(api::upbit::IOrderApi& api,
            std::string_view market,
            const Options& opt,
            StrategyT& strategy)
        {
            cancelBotOpenOrders(api, market, opt);
            const trading::PositionSnapshot pos = buildPositionSnapshot(api, market);
            strategy.syncOnStart(pos);
        }

    private:
        // 미체결 주문 중 "봇 prefix"가 붙은 주문만 취소한다
        static void cancelBotOpenOrders(api::rest::UpbitExchangeRestClient& api,
            std::string_view market,
            const Options& opt);
        static void cancelBotOpenOrders(api::upbit::IOrderApi& api,
            std::string_view market,
            const Options& opt);

        // 계정(core::Account)에서 해당 market의 base currency 포지션을 찾아 PositionSnapshot을 만든다
        static trading::PositionSnapshot buildPositionSnapshot(api::rest::UpbitExchangeRestClient& api,
            std::string_view market);
        static trading::PositionSnapshot buildPositionSnapshot(api::upbit::IOrderApi& api,
            std::string_view market);

        // market 파싱 유틸:
        //  - "KRW-BTC"에서 BTC / KRW 추출
        static std::string_view baseCurrency(std::string_view market); // "KRW-BTC" -> "BTC"
        static std::string_view unitCurrency(std::string_view market); // "KRW-BTC" -> "KRW"

        // prefix 검사 (오취소 방지 핵심)
        static bool startsWith(std::string_view s, std::string_view prefix) noexcept;
    };

} // namespace app
