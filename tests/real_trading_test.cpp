// tests/TestRealTradingWithEngineRunner.cpp
// 목적: EngineRunner를 포함한 실거래 E2E 테스트(WS 수신 → POST 주문 → WS 이벤트 처리 → 전략 콜백)
//
// 흐름(요약):
//   WS(raw) → Bridge(queue push) → EngineRunner(pop)
//   Candle → Strategy(onCandle) → OrderRequest 생성 → RealOrderEngine.submit(POST)
//   MyOrder → Mapper → RealOrderEngine.onOrderSnapshot / onMyTrade / onOrderStatus
//   EngineRunner가 pollEvents() → Strategy.onFill / onOrderUpdate 호출
//
// 전제:
// - EngineRunner가 "A안(전략을 value로 소유)" 구조로 적용되어 있어야 한다.
//   (즉, 외부 전략을 reference로 들고 있지 않고, value로 소유하여 dangling reference 위험이 없어야 함)

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

// ===== 프로젝트 헤더 =====
#include "app/EngineRunner.h"
#include "app/MarketDataEventBridge.h"
#include "app/MyOrderEventBridge.h"
#include "app/StartupRecovery.h"

#include "api/auth/UpbitJwtSigner.h"
#include "api/rest/RestClient.h"
#include "api/upbit/UpbitExchangeRestClient.h"
#include "api/ws/UpbitWebSocketClient.h"

#include "engine/OrderStore.h"
#include "engine/PrivateOrderApi.h"
#include "engine/RealOrderEngine.h"
#include "engine/upbit/UpbitPrivateOrderApi.h"

#include "trading/strategies/RsiMeanReversionStrategy.h"

#include "util/Logger.h"

// ------------------------------
// Ctrl+C 종료 플래그
// ------------------------------
static std::atomic<bool> g_stop{ false };
static void onSignal(int) { g_stop.store(true, std::memory_order_relaxed); }

int main()
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // ---- Logger 초기화 ----
    auto& logger = util::Logger::instance();
    logger.setLevel(util::LogLevel::INFO);
    logger.enableFileOutput("logs/coinbot.log");
    logger.info("CoinBot starting...");

    // ---- 테스트 파라미터 ----
    // 실행 예)
    //   TestRealTradingWithEngineRunner KRW-BTC candle.1m
    const std::string market = "KRW-BTC";
    const std::string candle_type = "candle.1m";

    // ---- 환경 변수 키 ----
    const char* access = std::getenv("UPBIT_ACCESS_KEY");
    const char* secret = std::getenv("UPBIT_SECRET_KEY");
    if (!access || !secret)
    {
        std::cout << "[Fatal] env missing: UPBIT_ACCESS_KEY / UPBIT_SECRET_KEY\n";
        return 2;
    }

    // ---- 네트워크 공용 리소스 ----
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tls_client);

    // 운영 권장: verify_peer + CA 검증
    // (테스트 환경에서는 verify 설정 때문에 막히면 verify_none으로 낮춰서 원인 분리 가능)
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(boost::asio::ssl::verify_none);

    api::auth::UpbitJwtSigner signer(access, secret);

    api::rest::RestClient rest(ioc, ssl_ctx);
    api::rest::UpbitExchangeRestClient exchange(rest, signer);

    // ---- 시작 시 account 로드(로컬 캐시) ----
    core::Account account{};
    {
        auto r = exchange.getMyAccount();
        if (!std::holds_alternative<core::Account>(r))
        {
            const auto& err = std::get<api::rest::RestError>(r);
            std::cout << "[Fatal] getMyAccount failed code=" << static_cast<int>(err.code)
                << " msg=" << err.message << "\n";
            return 3;
        }
        account = std::get<core::Account>(r);
        std::cout << "[REST] getMyAccount OK krw_free=" << account.krw_free
            << " positions=" << account.positions.size() << "\n";
    }

    // ---- 전략 생성(예: RSI Mean Reversion) ----
    trading::strategies::RsiMeanReversionStrategy::Params sp{};
    // 필요하다면 여기서 sp 값 조정(예: riskPercent, rsiPeriod 등)
    trading::strategies::RsiMeanReversionStrategy strategy(market, sp);

    // ---- StartupRecovery(미체결 정리 + 전략 상태 동기화) ----
    {
        app::StartupRecovery::Options opt{};
        opt.bot_identifier_prefix = std::string(strategy.id()) + ":" + market + ":"; // �� ��å ����
        opt.cancel_retry = 3;
        opt.verify_retry = 3;

        app::StartupRecovery::run(exchange, market, opt, strategy);
        std::cout << "[StartupRecovery] done\n";
    }

    // ---- 주문 엔진(POST 주문 + store + RealOrderEngine) ----
    engine::OrderStore store;                 // �� ������Ʈ ����ü
    engine::upbit::UpbitPrivateOrderApi orderApi(exchange);  // POST /v1/orders
    engine::RealOrderEngine engine(orderApi, store, account);

    // 엔진 owner-thread 지정: EngineRunner.run() 호출 전에 현재 스레드를 소유 스레드로 등록
    engine.bindToCurrentThread();

    // ---- EngineRunner 입력 큐 + Bridge ----
    // 큐 크기 설정: 기본 0(무제한) / WS 폭주 방지를 원하면 적절히 제한
    app::EngineRunner::PrivateQueue q(10000);

    app::MarketDataEventBridge mdBridge(q);
    app::MyOrderEventBridge myBridge(q);

    // ---- WS 클라이언트 2개(공개: candle / 개인: myOrder) ----
    api::ws::UpbitWebSocketClient ws_pub(ioc, ssl_ctx);
    api::ws::UpbitWebSocketClient ws_priv(ioc, ssl_ctx);

    // WS 내부 메시지핸들어에 람다 함수를 저장한다.
    // WS raw 메시지를 Bridge로 전달 (람다 함수를 파라미터로 넘긴 것)
    // (파싱/검증/엔진 전달은 EngineRunner에서 처리)
    ws_pub.setMessageHandler([&](std::string_view msg) {
        mdBridge.onWsMessage(msg);
        });
    ws_priv.setMessageHandler([&](std::string_view msg) {
        myBridge.onWsMessage(msg);
        });

    // ---- WS read loop 스레드 시작 ----
    std::thread th_pub([&] { ws_pub.runReadLoop(); });
    std::thread th_priv([&] { ws_priv.runReadLoop(); });

    // ---- connect + subscribe 커맨드 등록 ----
    const std::string host = "api.upbit.com";
    const std::string port = "443";

    // public / private target은 UpbitWebSocketClient 구현 정책에 맞춰 설정
    const std::string pub_target = "/websocket/v1";
    const std::string priv_target = "/websocket/v1/private";

    ws_pub.connectPublic(host, port, pub_target);
    ws_pub.subscribeCandles(candle_type, { market }, false, true, "DEFAULT");

    // private WS는 JWT bearer 필요
    const std::string bearer = signer.makeBearerToken(std::nullopt);
    ws_priv.connectPrivate(host, port, priv_target, bearer);
    ws_priv.subscribeMyOrder({ market }, true, "DEFAULT");

    std::cout << "[WS] connect+subscribe queued\n";
    std::cout << "[RUN] market=" << market << " candle=" << candle_type << "\n";
    std::cout << "      Ctrl+C to stop\n";

    // ---- EngineRunner 실행(현재 스레드에서 엔진 루프 실행) ----
    // A안(전략을 value로 Runner가 소유)이므로, strategy를 move로 넘기는 것이 자연스럽다
    app::EngineRunner runner(
        engine,
        std::move(strategy), // A안: Runner가 전략을 소유
        q,
        account,
        market
    );

    runner.run(g_stop);

    // ---- 종료 처리 ----
    std::cout << "[STOP] closing ws...\n";
    ws_pub.close();
    ws_priv.close();

    if (th_pub.joinable()) th_pub.join();
    if (th_priv.joinable()) th_priv.join();

    std::cout << "[DONE]\n";
    return 0;
}
