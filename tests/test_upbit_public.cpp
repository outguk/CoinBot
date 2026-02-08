#include <iostream>
#include <vector>
#include <variant>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>

#include "../src/api/rest/RestClient.h"
#include "../src/api/upbit/UpbitPublicRestClient.h"
#include "../src/api/upbit/UpbitExchangeRestClient.h"
#include "../src/api/auth/UpbitJwtSigner.h"

// 결과 출력 유틸
static void printError(const api::rest::RestError& e)
{
    std::cout << "[RestError]\n";
    std::cout << "  code: " << e.code << "\n";
    std::cout << "  http: " << e.http_status << "\n";
    std::cout << "  msg : " << e.message << "\n";
}

static void printAccount(const core::Account& a)
{
    std::cout << "[Account]\n";
    std::cout << "  krw_free  : " << a.krw_free << "\n";
    std::cout << "  krw_locked: " << a.krw_locked << "\n";
    std::cout << "  positions : " << a.positions.size()-1 << "\n";

    // KRW 제외하고 보유 코인만 간단히 출력
    for (const auto& p : a.positions)
    {
        if (p.currency == "KRW") continue;
        std::cout << "  - " << p.currency
            << " | balance=" << p.free
            << " | avg_buy=" << p.avg_buy_price
            << " | unit=" << p.unit_currency << "\n";
    }
}

int TestUpbitPublic()
{
    // 한글 깨짐 방지
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    const char* access = std::getenv("UPBIT_ACCESS_KEY");
    const char* secret = std::getenv("UPBIT_SECRET_KEY");

    if (!access || !secret)
    {
        std::cout << "[Fatal] env missing: UPBIT_ACCESS_KEY / UPBIT_SECRET_KEY\n";
        return 2;
    }

    try
    {
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
        api::auth::UpbitJwtSigner signer{ std::string(access), std::string(secret) };

        api::rest::UpbitExchangeRestClient ex{ rest, std::move(signer) };
        // 계좌 조회
        auto a = ex.getMyAccount();

        if (std::holds_alternative<api::rest::RestError>(a))
        {
            printError(std::get<api::rest::RestError>(a));
            return 1;
        }

        const auto& acc = std::get<core::Account>(a);
        printAccount(acc);

        return 0; // 임시 종료

        // 4) 호출: 마켓 전체 조회 (isDetails=false)
        auto r = upbit.getMarkets(false);

        if (std::holds_alternative<api::rest::RestError>(r))
        {
            printError(std::get<api::rest::RestError>(r));
            return 1;
        }

        const auto& markets = std::get<std::vector<core::MarketInfo>>(r);

        // 5) 출력
        std::cout << "Markets count: " << markets.size() << "\n";
        std::cout << "---- first 5 ----\n";
        for (std::size_t i = 0; i < markets.size() && i < 5; ++i)
        {
            const auto& m = markets[i];
            std::cout << m.market << " | " << m.ko_name << " | " << m.en_name << " | " << m.is_warning << "\n";
        }

        // 테스트용: 앞에서 5개 market code만 뽑기
        std::vector<std::string> top;
        for (std::size_t i = 0; i < markets.size() && top.size() < 5; ++i)
            top.push_back(markets[i].market);

        std::cout << "\n[Test markets]\n";
        for (auto& m : top) std::cout << "  " << m << "\n";

        // 2) Ticker
        auto rt = upbit.getTickers(top);
        if (std::holds_alternative<api::rest::RestError>(rt))
        {
            printError(std::get<api::rest::RestError>(rt));
            return 1;
        }
        const auto& tickers = std::get<std::vector<core::Ticker>>(rt);

        std::cout << "\nTickers count: " << tickers.size() << "\n";
        std::cout << "---- tickers ----\n";
        for (const auto& t : tickers)
        {
            // core::Ticker 필드명은 네 도메인에 맞춰 수정
            std::cout << t.market << " | trade_price=" << t.ticker_trade_price
                << " | acc_trade_volume_24h=" << t.acc_trade_volume_24h << "\n";
        }

        // 호가창 테스트
        std::optional<std::string> level = std::nullopt;
        std::optional<int> count = 10;
        std::vector<std::string> te;
        te.push_back("KRW-BTC");

        auto rob = upbit.getOrderbooks(te, level, count);
        if (std::holds_alternative<api::rest::RestError>(rob))
        {
            printError(std::get<api::rest::RestError>(rob));
            return 1;
        }

        const auto& orderbooks = std::get<std::vector<core::Orderbook>>(rob);

        std::cout << "Orderbooks count: " << orderbooks.size() << "\n";
        std::cout << "Query: level=" << (level ? *level : std::string("(default)"))
            << ", count=" << (count ? std::to_string(*count) : std::string("(default)"))
            << "\n\n";

        // 출력: 각 마켓별 top 5 레벨만
        for (const auto& ob : orderbooks)
        {
            std::cout << "[Market] " << ob.market
                << " | ts=" << ob.timestamp
                << " | total_ask=" << ob.total_ask_size
                << " | total_bid=" << ob.total_bid_size;

            if (ob.price_unit.has_value())
                std::cout << " | price_unit=" << *ob.price_unit;
            else
                std::cout << " | price_unit=(default)";

            std::cout << "\n";

            std::size_t limit = std::min<std::size_t>(5, ob.top_levels.size());
            for (std::size_t i = 0; i < limit; ++i)
            {
                const auto& lv = ob.top_levels[i];
                std::cout << "  [" << i << "] "
                    << "ask=" << lv.ask_price << " (" << lv.ask_size << ")"
                    << " | "
                    << "bid=" << lv.bid_price << " (" << lv.bid_size << ")"
                    << "\n";
            }
            std::cout << "\n";
        }


        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cout << "[Fatal] exception: " << ex.what() << "\n";
        return 2;
    }
}