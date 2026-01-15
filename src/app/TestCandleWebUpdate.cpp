#include <iostream>
#include <vector>
#include <variant>
#include <algorithm>    // sort
#include <cstdint>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>

#include "../src/api/rest/RestClient.h"
#include "../src/api/upbit/UpbitPublicRestClient.h"

// ====== (전략) ======
#include "trading/strategies/RsiMeanReversionStrategy.h"
#include "trading/strategies/StrategyTypes.h"


// 결과 출력 유틸
static void printError(const api::rest::RestError& e)
{
    std::cout << "[RestError]\n";
    std::cout << "  code: " << static_cast<int>(e.code) << "\n";
    std::cout << "  http: " << e.http_status << "\n";
    std::cout << "  msg : " << e.message << "\n";
}

// (start_timestamp 기준) 오래된->최신으로 정렬
static void CandlesOldToNew(std::vector<core::Candle>& src)
{
    // 업비트 응답이 최신 우선일 수 있으니 안전하게 timestamp로 정렬 보장
    std::sort(src.begin(), src.end(),
        [](const core::Candle& a, const core::Candle& b) {
            return a.start_timestamp < b.start_timestamp;
        });
}

int TestCandleWebUpdate()
{
    // 한글 깨짐 방지
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // 1) 네트워크 컨텍스트 준비
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tls_client);

    // (권장) 인증서 검증을 제대로 하려면 verify path 설정이 필요할 수 있음.
    // 지금은 "연동 테스트" 목적이라 기본 설정으로 시작.
    // ssl_ctx.set_default_verify_paths(); 
    // ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);

    // 2) RestClient 생성
    api::rest::RestClient rest{ ioc, ssl_ctx };

    // 3) UpbitPublicRestClient 생성
    api::upbit::UpbitPublicRestClient upbit{ rest };

    // 4) REST로 캔들 N개 받기
    constexpr int kSeedCount = 14;
    const std::string market = "KRW-BTC";
    const int unitMinutes = 15;

    // 4) Candles 호출
    auto rc = upbit.getCandlesMinutes(market, /*unit*/unitMinutes, /*count*/kSeedCount);
    if (std::holds_alternative<api::rest::RestError>(rc))
    {
        printError(std::get<api::rest::RestError>(rc));
        return 1;
    }
    auto& seed = std::get<std::vector<core::Candle>>(rc);

    // 5) seed용 캔들 정렬(오래된 -> 최신)
    CandlesOldToNew(seed);

    std::cout << "\nCandles count: " << seed.size() << " (" << market << ")\n";
    std::cout << "---- candles (first 14) ----\n";
    std::cout << unitMinutes << " period candle" << "\n";
    for (const auto& cw : seed)
    {
        std::cout << cw.market
            << " o=" << cw.open_price
            << " h=" << cw.high_price
            << " l=" << cw.low_price
            << " c=" << cw.close_price
            << " v=" << cw.volume
            << " ts=" << cw.start_timestamp
            << "\n";
    }

    // (4) 전략 warm-up: seed를 전략에 순차 주입
    trading::strategies::RsiMeanReversionStrategy::Params p{};
    p.rsiLength = 5;
    p.trendLookWindow = 14;
    p.volatilityWindow = 14;
    p.maxTrendStrength = 0.03;   // 예시(3%) - 프로젝트에서 쓰던 값으로 맞춰도 됨
    p.minVolatility = 0.01;   // 예시(1%)
    p.oversold = 30.0;
    p.overbought = 70.0;
    p.riskPercent = 10.0;   // seed 단계에서는 계좌를 0으로 줘서 주문이 안 나가게 함
    p.stopLossPct = 1.0;
    p.profitTargetPct = 1.5;

    trading::strategies::RsiMeanReversionStrategy strat{ market, p };

    // seed 단계에서는 주문이 나가면 안 되니까 계좌를 0으로 줌
    trading::AccountSnapshot seedAccount{};
    seedAccount.krw_available = 0.0;  // canBuy() = false
    seedAccount.coin_available = 0.0;  // canSell() = false

    std::cout << "\n[Warm-up] Feeding seed candles into strategy...\n";

    std::size_t orderCount = 0;
    for (std::size_t i = 0; i < seed.size(); ++i)
    {
        const auto& c = seed[i];

        // 전략 업데이트(지표/필터 warm-up 포함)
        const trading::Decision d = strat.onCandle(c, seedAccount);

        // 마지막 캔들 기준 스냅샷 조회
        const auto& snap = strat.lastSnapshot();

        // seedAccount가 0이므로 정상이라면 주문은 절대 나오지 않아야 함
        if (d.hasOrder())
        {
            ++orderCount;
            std::cout << "  [WARN] order generated during seed at i=" << i
                << " ts=" << c.start_timestamp
                << " (this should normally be blocked by seedAccount)\n";
        }

        // 진행 로그(원하면 더 줄여도 됨)
        std::cout << "  i=" << i
            << " ts=" << c.start_timestamp
            << " rsi=" << (snap.rsi.ready ? std::to_string(snap.rsi.v) : "N/A")
            << " close=" << c.close_price
            << " decision=" << (d.hasOrder() ? "ORDER" : (d.is_no_action ? "NO_ACTION" : "NONE"))
            << "\n";
    }

    //

    return 0;
}