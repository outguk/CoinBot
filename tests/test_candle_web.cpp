#include <iostream>
#include <vector>
#include <variant>
#include <algorithm>    // sort
#include <cstdint>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>

#include "../src/api/rest/RestClient.h"
#include "../src/api/upbit/UpbitPublicRestClient.h"

// ====== (����) ======
#include "trading/strategies/RsiMeanReversionStrategy.h"
#include "trading/strategies/StrategyTypes.h"


// ��� ��� ��ƿ
static void printError(const api::rest::RestError& e)
{
    std::cout << "[RestError]\n";
    std::cout << "  code: " << static_cast<int>(e.code) << "\n";
    std::cout << "  http: " << e.http_status << "\n";
    std::cout << "  msg : " << e.message << "\n";
}

// (start_timestamp ����) ������->�ֽ����� ����
static void CandlesOldToNew(std::vector<core::Candle>& src)
{
    // ����Ʈ ������ �ֽ� �켱�� �� ������ �����ϰ� timestamp�� ���� ����
    std::sort(src.begin(), src.end(),
        [](const core::Candle& a, const core::Candle& b) {
            return a.start_timestamp < b.start_timestamp;
        });
}

int TestCandleWebUpdate()
{
    // �ѱ� ���� ����
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // 1) ��Ʈ��ũ ���ؽ�Ʈ �غ�
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tls_client);

    // (����) ������ ������ ����� �Ϸ��� verify path ������ �ʿ��� �� ����.
    // ������ "���� �׽�Ʈ" �����̶� �⺻ �������� ����.
    // ssl_ctx.set_default_verify_paths(); 
    // ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);

    // 2) RestClient ����
    api::rest::RestClient rest{ ioc, ssl_ctx };

    // 3) UpbitPublicRestClient ����
    api::upbit::UpbitPublicRestClient upbit{ rest };

    // 4) REST�� ĵ�� N�� �ޱ�
    constexpr int kSeedCount = 14;
    const std::string market = "KRW-BTC";
    const int unitMinutes = 15;

    // 4) Candles ȣ��
    auto rc = upbit.getCandlesMinutes(market, /*unit*/unitMinutes, /*count*/kSeedCount);
    if (std::holds_alternative<api::rest::RestError>(rc))
    {
        printError(std::get<api::rest::RestError>(rc));
        return 1;
    }
    auto& seed = std::get<std::vector<core::Candle>>(rc);

    // 5) seed�� ĵ�� ����(������ -> �ֽ�)
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

    // (4) ���� warm-up: seed�� ������ ���� ����
    trading::strategies::RsiMeanReversionStrategy::Params p{};
    p.rsiLength = 5;
    p.trendLookWindow = 14;
    p.volatilityWindow = 14;
    p.maxTrendStrength = 0.03;   // ����(3%) - ������Ʈ���� ���� ������ ���絵 ��
    p.minVolatility = 0.01;   // ����(1%)
    p.oversold = 30.0;
    p.overbought = 70.0;
    p.utilization = 0.1;    // seed �ܰ迡���� ���¸� 0���� �༭ �ֹ��� �� ������ ��
    p.stopLossPct = 1.0;
    p.profitTargetPct = 1.5;

    trading::strategies::RsiMeanReversionStrategy strat{ market, p };

    // seed �ܰ迡���� �ֹ��� ������ �� �Ǵϱ� ���¸� 0���� ��
    trading::AccountSnapshot seedAccount{};
    seedAccount.krw_available = 0.0;  // canBuy() = false
    seedAccount.coin_available = 0.0;  // canSell() = false

    std::cout << "\n[Warm-up] Feeding seed candles into strategy...\n";

    std::size_t orderCount = 0;
    for (std::size_t i = 0; i < seed.size(); ++i)
    {
        const auto& c = seed[i];

        // ���� ������Ʈ(��ǥ/���� warm-up ����)
        const trading::Decision d = strat.onCandle(c, seedAccount);

        // ������ ĵ�� ���� ������ ��ȸ
        const auto& snap = strat.lastSnapshot();

        // seedAccount�� 0�̹Ƿ� �����̶�� �ֹ��� ���� ������ �ʾƾ� ��
        if (d.hasOrder())
        {
            ++orderCount;
            std::cout << "  [WARN] order generated during seed at i=" << i
                << " ts=" << c.start_timestamp
                << " (this should normally be blocked by seedAccount)\n";
        }

        // ���� �α�(���ϸ� �� �ٿ��� ��)
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