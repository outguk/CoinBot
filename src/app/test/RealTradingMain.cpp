// tests/TestRealTradingWithEngineRunner.cpp
// 목적: EngineRunner를 사용해 실거래 E2E(WS→전략→POST→WS→엔진/전략 갱신) 검증
//
// 흐름(고정):
//   WS(raw) → Bridge(queue push) → EngineRunner(pop)
//   Candle → Strategy(onCandle) → OrderRequest 생성 → RealOrderEngine.submit(POST)
//   MyOrder → Mapper → RealOrderEngine.onOrderSnapshot/onMyTrade/onOrderStatus
//   EngineRunner가 pollEvents() → Strategy.onFill/onOrderUpdate
//
// 전제:
// - EngineRunner는 "A 구조(전략 소유)"로 수정 완료되어 있어야 함
//   (즉, 멤버가 reference가 아니라 value로 소유하거나, 최소한 dangling reference 문제가 없어야 함)

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

// ------------------------------
// Ctrl+C 종료 플래그
// ------------------------------
static std::atomic<bool> g_stop{ false };
static void onSignal(int) { g_stop.store(true, std::memory_order_relaxed); }

int main()
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // ---- 런타임 파라미터 ----
    // 사용 예)
    //   TestRealTradingWithEngineRunner KRW-BTC candle.1m
    const std::string market = "KRW-BTC";
    const std::string candle_type = "candle.1m";

    // ---- 업비트 키 ----
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

    // 운영 권장: verify_peer + CA
    // (너의 환경에서 verify 문제로 막히면, 이전에 했던 방식대로 verify_none으로 바꾸는 선택지도 있음)
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(boost::asio::ssl::verify_none);

    api::auth::UpbitJwtSigner signer(access, secret);

    api::rest::RestClient rest(ioc, ssl_ctx);
    api::rest::UpbitExchangeRestClient exchange(rest, signer);

    // ---- 시작 시 account 로딩(로컬 캐시) ----
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

    // ---- 전략 생성(단일 전략/단일 마켓) ----
    trading::strategies::RsiMeanReversionStrategy::Params sp{};
    // 리스크 낮추고 싶으면 여기서 sp 조절 (예: riskPercent 낮추기 등)
    trading::strategies::RsiMeanReversionStrategy strategy(market, sp);

    // ---- StartupRecovery(미체결 정리 + 포지션 복구) ----
    {
        app::StartupRecovery::Options opt{};
        opt.bot_identifier_prefix = std::string(strategy.id()) + ":" + market + ":"; // 네 정책 유지
        opt.cancel_retry = 3;
        opt.verify_retry = 3;

        app::StartupRecovery::run(exchange, market, opt, strategy);
        std::cout << "[StartupRecovery] done\n";
    }

    // ---- 엔진 구성(POST 어댑터 + store + RealOrderEngine) ----
    engine::OrderStore store;                 // 네 프로젝트 구현체
    engine::upbit::UpbitPrivateOrderApi orderApi(exchange);  // POST /v1/orders
    engine::RealOrderEngine engine(orderApi, store, account);

    // 엔진 owner-thread 고정: EngineRunner.run()을 호출하기 직전에 같은 스레드에서 바인딩
    engine.bindToCurrentThread();

    // ---- EngineRunner 입력 큐 + Bridge ----
    app::EngineRunner::PrivateQueue q;
    // [PATCH] MyOrder overflow -> RESYNC mode flag
    std::atomic<bool> needs_resync{ false };

    app::MarketDataEventBridge mdBridge(q);
    app::MyOrderEventBridge myBridge(q, needs_resync);

    // ---- WS 클라이언트 2개(공용: candle / private: myOrder) ----
    api::ws::UpbitWebSocketClient ws_pub(ioc, ssl_ctx);
    api::ws::UpbitWebSocketClient ws_priv(ioc, ssl_ctx);

    // WS는 raw를 Bridge로 전달만(파싱/전략/엔진은 EngineRunner에서 단일 스레드 처리)
    ws_pub.setMessageHandler([&](std::string_view msg) {
        mdBridge.onWsMessage(msg);
        });
    ws_priv.setMessageHandler([&](std::string_view msg) {
        myBridge.onWsMessage(msg);
        });

    // ---- WS read loop 스레드 시작 ----
    std::thread th_pub([&] { ws_pub.runReadLoop(); });
    std::thread th_priv([&] { ws_priv.runReadLoop(); });

    // ---- connect + subscribe 커맨드 푸시 ----
    // 네 프로젝트에서 이미 성공시킨 endpoint/target 값을 그대로 쓰는 게 가장 안전
    const std::string host = "api.upbit.com";
    const std::string port = "443";

    // public / private target은 네 구현(UpbitWebSocketClient.cpp)에서 기대하는 값으로 맞춰야 함
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

    // ---- EngineRunner 실행(메인 스레드에서 엔진 루프) ----
    // A 구조(전략 소유)이므로 전략을 move로 넘기는 형태가 자연스럽다.
    // (너의 EngineRunner 시그니처에 맞춰 std::move(strategy) 또는 strategy 그대로 사용)

    app::StartupRecovery::Options resync_opt{};
    resync_opt.bot_identifier_prefix = std::string(strategy.id()) + ":" + market + ":";
    resync_opt.cancel_retry = 3;
    resync_opt.verify_retry = 3;

    app::EngineRunner runner(
        engine,
        std::move(strategy), // A 구조: Runner가 전략을 소유
        q,
        account,
        market,
        exchange,   // RESYNC용 REST client
        needs_resync, // overflow flag
        resync_opt  // recovery policy
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
