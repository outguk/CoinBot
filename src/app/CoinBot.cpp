// app/CoinBot.cpp
//
// CoinBot 진입점
//
// 조립 순서:
//   1) io_context / ssl_context 구성
//   2) REST API 클라이언트 구성 (JwtSigner → RestClient → UpbitExchangeRestClient)
//   3) 공유 자원 구성 (OrderStore, AccountManager)
//   4) MarketEngineManager 구성 (계좌 동기화 + 마켓별 복구)
//   5) EventRouter 구성
//   6) WebSocket 클라이언트 구성 (public: 캔들, private: myOrder)
//   7) start() → SIGINT 대기 → stop()

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>

#include "api/auth/UpbitJwtSigner.h"
#include "api/rest/RestClient.h"
#include "api/upbit/UpbitExchangeRestClient.h"
#include "api/upbit/SharedOrderApi.h"
#include "api/ws/UpbitWebSocketClient.h"
#include "app/EventRouter.h"
#include "app/MarketEngineManager.h"
#include "engine/OrderStore.h"
#include "trading/allocation/AccountManager.h"
#include "util/Config.h"
#include "util/Logger.h"

// ---- 시그널 핸들러 ----
// SIGINT(Ctrl+C) / SIGTERM 수신 시 종료 플래그를 세운다.
namespace {
    volatile std::sig_atomic_t g_stop_requested = 0;

    void onSignal(int) { g_stop_requested = 1; }
}

// ---- API 키 로딩 ----
// 환경 변수 UPBIT_ACCESS_KEY / UPBIT_SECRET_KEY 에서 읽는다.
// 실제 배포 시 secrets 관리 방식에 맞춰 교체할 것.
static std::string requireEnv(const char* name)
{
    const char* val = std::getenv(name);
    if (!val || *val == '\0')
        throw std::runtime_error(std::string("환경 변수가 없습니다: ") + name);
    return val;
}

// ---- 마켓 목록 로딩 ----
// 환경 변수 UPBIT_MARKETS (CSV) 우선, 없으면 AppConfig 기본값 사용
static std::vector<std::string> loadMarkets()
{
    const char* env = std::getenv("UPBIT_MARKETS");
    if (!env || *env == '\0')
        return util::AppConfig::instance().bot.markets;

    std::vector<std::string> result;
    std::istringstream ss(env);
    std::string token;
    while (std::getline(ss, token, ','))
        if (!token.empty()) result.push_back(token);

    return result.empty() ? util::AppConfig::instance().bot.markets : result;
}

// ---- 봇 실행 본체 ----
// MarketEngineManager 생성자가 계좌 동기화 실패 시 throw → main에서 catch
static int run(const std::string& access_key,
               const std::string& secret_key,
               const std::vector<std::string>& markets)
{
    auto& logger = util::Logger::instance();
    // 작업 디렉터리와 무관하게 항상 프로젝트 루트 로그 폴더로 저장
    logger.enableMarketFileOutput("C:\\cpp\\CoinBot\\market_logs");

    // ---- 네트워크 컨텍스트 ----
    boost::asio::io_context   ioc;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv12_client);
    ssl_ctx.set_default_verify_paths();  // 시스템 CA 인증서 사용

    // ---- REST 클라이언트 ----
    api::auth::UpbitJwtSigner signer(access_key, secret_key);
    api::rest::RestClient     rest_client(ioc, ssl_ctx);

    // UpbitExchangeRestClient: 순수 HTTP 담당 (스레드 비안전)
    // SharedOrderApi: IOrderApi 구현 + mutex 직렬화 (멀티마켓 워커 스레드 공유용)
    auto exchange_client = std::make_unique<api::rest::UpbitExchangeRestClient>(
        rest_client, std::move(signer));
    api::upbit::SharedOrderApi shared_api(std::move(exchange_client));

    // ---- 공유 자원 ----
    engine::OrderStore                  order_store;
    trading::allocation::AccountManager account_mgr(core::Account{}, markets);

    // ---- MarketEngineManager ----
    // 생성자 내부에서 계좌 동기화 + 마켓별 미체결 복구 수행
    // 계좌 동기화 실패 시 std::runtime_error → run() 밖으로 전파
    logger.info("[CoinBot] Initializing MarketEngineManager...");
    app::MarketEngineManager engine_mgr(
        shared_api,
        order_store,
        account_mgr,
        markets);

    // ---- EventRouter ----
    app::EventRouter router;
    engine_mgr.registerWith(router);

    // ---- WebSocket: PUBLIC (캔들) ----
    api::ws::UpbitWebSocketClient ws_public(ioc, ssl_ctx);
    ws_public.setMessageHandler([&router](std::string_view json) {
        router.routeMarketData(json);
    });
    ws_public.connectPublic("api.upbit.com", "443", "/websocket/v1");
    ws_public.subscribeCandles("candle.1m", markets, false, true);

    // ---- WebSocket: PRIVATE (myOrder) ----
    // JWT는 Private WS 핸드셰이크에서 한 번만 사용되므로 no-query 토큰으로 충분
    api::auth::UpbitJwtSigner signer_for_ws(access_key, secret_key);
    const std::string ws_bearer = signer_for_ws.makeBearerToken(std::nullopt);

    api::ws::UpbitWebSocketClient ws_private(ioc, ssl_ctx);
    ws_private.setMessageHandler([&router](std::string_view json) {
        router.routeMyOrder(json);
    });
    ws_private.connectPrivate("api.upbit.com", "443", "/websocket/v1/private", ws_bearer);
    ws_private.subscribeMyOrder(markets, true);

    // ---- 시작 ----
    logger.info("[CoinBot] Starting...");
    engine_mgr.start();
    ws_public.start();
    ws_private.start();
    logger.info("[CoinBot] Running. Press Ctrl+C to stop.");

    // ---- SIGINT / SIGTERM 대기 ----
    while (!g_stop_requested)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // ---- 정지 (역순) ----
    logger.info("[CoinBot] Stopping...");
    ws_private.stop();
    ws_public.stop();
    engine_mgr.stop();

    logger.info("[CoinBot] Goodbye.");
    return 0;
}

int main()
{
    auto& logger = util::Logger::instance();

    // ---- 시그널 등록 ----
    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    // ---- API 키 ----
    std::string access_key, secret_key;
    try {
        access_key = requireEnv("UPBIT_ACCESS_KEY");
        secret_key = requireEnv("UPBIT_SECRET_KEY");
    } catch (const std::exception& e) {
        logger.error("[CoinBot] ", e.what());
        return 1;
    }

    // ---- 거래 마켓 목록 ----
    const std::vector<std::string> markets = loadMarkets();

    // ---- 실행 ----
    try {
        return run(access_key, secret_key, markets);
    } catch (const std::exception& e) {
        logger.error("[CoinBot] Fatal: ", e.what());
        return 1;
    }
}
