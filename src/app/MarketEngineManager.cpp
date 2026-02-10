// app/MarketEngineManager.cpp

#include "app/MarketEngineManager.h"
#include "app/EventRouter.h"
#include "app/StartupRecovery.h"

#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <variant>

#include <json.hpp>

#include "api/upbit/mappers/MyOrderMapper.h"
#include "api/upbit/mappers/CandleMapper.h"
#include "util/Logger.h"

namespace app {

namespace {

    // 전략 상태를 로그용 문자열로 변환
    const char* toStringState(trading::strategies::RsiMeanReversionStrategy::State s)
    {
        using S = trading::strategies::RsiMeanReversionStrategy::State;
        switch (s)
        {
        case S::Flat:         return "Flat";
        case S::PendingEntry: return "PendingEntry";
        case S::InPosition:   return "InPosition";
        case S::PendingExit:  return "PendingExit";
        default:              return "Unknown";
        }
    }

    // OrderSize를 로그용 문자열로 변환
    std::string orderSizeToLog(const core::OrderSize& size)
    {
        return std::visit([](const auto& s) -> std::string {
            using T = std::decay_t<decltype(s)>;
            std::ostringstream oss;
            if constexpr (std::is_same_v<T, core::VolumeSize>)
                oss << "VOL=" << s.value;
            else if constexpr (std::is_same_v<T, core::AmountSize>)
                oss << "AMOUNT=" << s.value;
            else
                oss << "<UNKNOWN_SIZE>";
            return oss.str();
        }, size);
    }

} // anonymous namespace

// ========== 생성자 ==========
MarketEngineManager::MarketEngineManager(
    api::upbit::IOrderApi& api,
    engine::OrderStore& store,
    trading::allocation::AccountManager& account_mgr,
    const std::vector<std::string>& markets,
    Config cfg)
    : api_(api)
    , store_(store)
    , account_mgr_(account_mgr)
    , cfg_(std::move(cfg))
{
    auto& logger = util::Logger::instance();

    // 1) 계좌 동기화 (1차: 실패 시 예외)
    logger.info("[MarketEngineManager] Syncing account with exchange...");
    syncAccountWithExchange_(/*throw_on_fail=*/true);

    // 2) 마켓별 컨텍스트 생성 + 전략 복구
    for (const auto& market : markets)
    {
        auto ctx = std::make_unique<MarketContext>(market, cfg_.queue_capacity);

        // MarketEngine 생성
        ctx->engine = std::make_unique<engine::MarketEngine>(
            market, api_, store_, account_mgr_);

        // 전략 생성
        ctx->strategy = std::make_unique<trading::strategies::RsiMeanReversionStrategy>(
            market, cfg_.strategy_params);

        // StartupRecovery: 미체결 취소 + 포지션 복구
        recoverMarketState_(*ctx);

        logger.info("[MarketEngineManager] Context created for market=", market);
        contexts_[market] = std::move(ctx);
    }

    // 3) 미체결 취소 후 최종 계좌 동기화 (2차: 실패 시 경고만)
    logger.info("[MarketEngineManager] Final account sync after recovery...");
    syncAccountWithExchange_(/*throw_on_fail=*/false);

    logger.info("[MarketEngineManager] Initialized with ", markets.size(), " markets");
}

// ========== 소멸자 ==========
MarketEngineManager::~MarketEngineManager()
{
    stop();
}

// ========== registerWith ==========
void MarketEngineManager::registerWith(EventRouter& router)
{
    for (auto& [market, ctx] : contexts_)
    {
        router.registerMarket(market, ctx->event_queue);
    }
}

// ========== start ==========
void MarketEngineManager::start()
{
    if (started_) return;
    started_ = true;

    auto& logger = util::Logger::instance();

    for (auto& [market, ctx] : contexts_)
    {
        ctx->worker = std::thread([this, &ctx_ref = *ctx]() {
            workerLoop_(ctx_ref);
        });

        logger.info("[MarketEngineManager] Worker started for market=", market);
    }
}

// ========== stop ==========
void MarketEngineManager::stop()
{
    if (!started_) return;

    auto& logger = util::Logger::instance();
    logger.info("[MarketEngineManager] Stopping all workers...");

    // 모든 stop_flag 설정
    for (auto& [market, ctx] : contexts_)
        ctx->stop_flag.store(true, std::memory_order_relaxed);

    // 모든 워커 스레드 join
    for (auto& [market, ctx] : contexts_)
    {
        if (ctx->worker.joinable())
        {
            ctx->worker.join();
            logger.info("[MarketEngineManager] Worker joined for market=", market);
        }
    }

    started_ = false;
    logger.info("[MarketEngineManager] All workers stopped");
}

// ========== syncAccountWithExchange_ ==========
void MarketEngineManager::syncAccountWithExchange_(bool throw_on_fail)
{
    auto& logger = util::Logger::instance();

    for (int attempt = 1; attempt <= cfg_.sync_retry; ++attempt)
    {
        auto result = api_.getMyAccount();

        if (std::holds_alternative<core::Account>(result))
        {
            account_mgr_.syncWithAccount(std::get<core::Account>(result));
            logger.info("[MarketEngineManager] Account synced (attempt ", attempt, ")");
            return;
        }

        const auto& err = std::get<api::rest::RestError>(result);
        logger.warn("[MarketEngineManager] getMyAccount failed (attempt ",
            attempt, "/", cfg_.sync_retry, "): ", err.message);

        // 마지막 시도가 아니면 잠시 대기
        if (attempt < cfg_.sync_retry)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (throw_on_fail)
    {
        throw std::runtime_error(
            "[MarketEngineManager] Failed to sync account after "
            + std::to_string(cfg_.sync_retry) + " attempts");
    }

    logger.warn("[MarketEngineManager] Account sync failed, continuing with stale data");
}

// ========== recoverMarketState_ ==========
void MarketEngineManager::recoverMarketState_(MarketContext& ctx)
{
    auto& logger = util::Logger::instance();

    // StartupRecovery 옵션: 봇 주문 prefix = "strategy_id:market:"
    StartupRecovery::Options opt;
    opt.bot_identifier_prefix = std::string(ctx.strategy->id())
        + ":" + ctx.market + ":";

    try
    {
        StartupRecovery::run(api_, ctx.market, opt, *ctx.strategy);
        logger.info("[MarketEngineManager] Recovery done for market=", ctx.market,
            " state=", toStringState(ctx.strategy->state()));
    }
    catch (const std::exception& e)
    {
        // 복구 실패 시 경고만 (기존 정책)
        logger.warn("[MarketEngineManager] Recovery failed for market=", ctx.market,
            ": ", e.what());
    }
}

// ========== workerLoop_ ==========
void MarketEngineManager::workerLoop_(MarketContext& ctx)
{
    using namespace std::chrono_literals;
    auto& logger = util::Logger::instance();

    // 엔진을 현재 워커 스레드에 바인딩
    ctx.engine->bindToCurrentThread();

    logger.info("[MarketEngineManager][", ctx.market, "] Worker loop started");

    try
    {
        while (!ctx.stop_flag.load(std::memory_order_relaxed))
        {
            // 큐에서 이벤트 대기 (200ms 타임아웃)
            auto maybe = ctx.event_queue.pop_for(200ms);

            if (maybe.has_value())
                handleOne_(ctx, *maybe);

            // 엔진이 쌓아둔 이벤트를 전략으로 전달
            auto out = ctx.engine->pollEvents();
            if (!out.empty())
                handleEngineEvents_(ctx, out);
        }
    }
    catch (const std::exception& e)
    {
        logger.error("[MarketEngineManager][", ctx.market,
            "] Worker terminated with exception: ", e.what());
    }

    logger.info("[MarketEngineManager][", ctx.market, "] Worker loop ended");
}

// ========== handleOne_ ==========
void MarketEngineManager::handleOne_(MarketContext& ctx,
    const engine::input::EngineInput& in)
{
    std::visit([&](const auto& x)
    {
        using T = std::decay_t<decltype(x)>;

        if constexpr (std::is_same_v<T, engine::input::MyOrderRaw>)
            handleMyOrder_(ctx, x);
        else if constexpr (std::is_same_v<T, engine::input::MarketDataRaw>)
            handleMarketData_(ctx, x);

    }, in);
}

// ========== handleMyOrder_ ==========
void MarketEngineManager::handleMyOrder_(MarketContext& ctx,
    const engine::input::MyOrderRaw& raw)
{
    auto& logger = util::Logger::instance();

    const nlohmann::json j = nlohmann::json::parse(raw.json, nullptr, false);
    if (j.is_discarded())
    {
        logger.error("[MarketEngineManager][", ctx.market, "] myOrder JSON parse failed");
        return;
    }

    // 1) JSON -> DTO
    api::upbit::dto::UpbitMyOrderDto dto{};
    try
    {
        dto = j.get<api::upbit::dto::UpbitMyOrderDto>();
    }
    catch (const std::exception& e)
    {
        logger.error("[MarketEngineManager][", ctx.market,
            "] myOrder dto convert failed: ", e.what());
        return;
    }

    // 2) DTO -> (Order snapshot, MyTrade) 이벤트 분해
    const auto events = api::upbit::mappers::toEvents(dto);

    // 3) 엔진 상태 반영
    for (const auto& ev : events)
    {
        if (std::holds_alternative<core::Order>(ev))
        {
            const auto& o = std::get<core::Order>(ev);
            ctx.engine->onOrderSnapshot(o);

            logger.info("[Manager][", ctx.market, "][OrderEvent] status=",
                static_cast<int>(o.status), " uuid=", o.id);
        }
        else
        {
            const auto& t = std::get<core::MyTrade>(ev);
            ctx.engine->onMyTrade(t);

            logger.info("[Manager][", ctx.market, "][TradeEvent] uuid=",
                t.order_id, " price=", t.price, " vol=", t.volume);
        }
    }
}

// ========== handleMarketData_ ==========
void MarketEngineManager::handleMarketData_(MarketContext& ctx,
    const engine::input::MarketDataRaw& raw)
{
    auto& logger = util::Logger::instance();

    const nlohmann::json j = nlohmann::json::parse(raw.json, nullptr, false);
    if (j.is_discarded())
    {
        logger.error("[MarketEngineManager][", ctx.market, "] MarketData JSON parse failed");
        return;
    }

    // candle 타입만 처리
    const std::string type = j.value("type", "");
    if (type.rfind("candle", 0) != 0)
        return;

    // 1) JSON -> Candle DTO
    api::upbit::dto::CandleDto_Minute candleDto{};
    try
    {
        candleDto = j.get<api::upbit::dto::CandleDto_Minute>();
    }
    catch (const std::exception& e)
    {
        logger.error("[MarketEngineManager][", ctx.market,
            "] candle dto convert failed: ", e.what());
        return;
    }

    // 2) DTO -> core::Candle
    const core::Candle candle = api::upbit::mappers::toDomain(candleDto);

    // 캔들 중복 제거 (같은 타임스탬프의 반복 업데이트 무시)
    if (!ctx.last_candle_ts.empty() && ctx.last_candle_ts == candle.start_timestamp)
        return;
    ctx.last_candle_ts = candle.start_timestamp;

    logger.info("[Manager][", ctx.market, "][Candle] ts=",
        candle.start_timestamp, " close=", candle.close_price);

    // 3) AccountManager에서 예산 조회 → 전략용 스냅샷 빌드
    const trading::AccountSnapshot account = buildAccountSnapshot_(ctx.market);

    // 4) 전략 실행
    const trading::Decision d = ctx.strategy->onCandle(candle, account);

    logger.info("[Manager][", ctx.market, "][Strategy] state=",
        toStringState(ctx.strategy->state()));

    // 5) 주문 의도가 있으면 엔진에 submit
    if (d.hasOrder())
    {
        const auto& req = *d.order;

        logger.info("[Manager][", ctx.market, "][Decision] side=",
            (req.position == core::OrderPosition::BID ? "BUY" : "SELL"),
            " ", orderSizeToLog(req.size));

        const auto r = ctx.engine->submit(req);

        logger.info("[Manager][", ctx.market, "][Submit] success=", r.success,
            " code=", static_cast<int>(r.code),
            " msg=", r.message);

        if (!r.success)
        {
            logger.warn("[Manager][", ctx.market,
                "][Submit] FAILED -> rollback strategy pending");
            ctx.strategy->onSubmitFailed();
        }
    }
}

// ========== handleEngineEvents_ ==========
void MarketEngineManager::handleEngineEvents_(MarketContext& ctx,
    const std::vector<engine::EngineEvent>& evs)
{
    for (const auto& ev : evs)
    {
        if (std::holds_alternative<engine::EngineFillEvent>(ev))
        {
            const auto& e = std::get<engine::EngineFillEvent>(ev);

            const trading::FillEvent fill{
                e.identifier,
                e.position,
                static_cast<double>(e.fill_price),
                static_cast<double>(e.filled_volume)
            };

            ctx.strategy->onFill(fill);
        }
        else if (std::holds_alternative<engine::EngineOrderStatusEvent>(ev))
        {
            const auto& e = std::get<engine::EngineOrderStatusEvent>(ev);

            trading::OrderStatusEvent out{
                e.identifier,
                e.status,
                e.position,
                e.executed_volume,
                e.remaining_volume
            };

            ctx.strategy->onOrderUpdate(out);
        }
    }
}

// ========== buildAccountSnapshot_ ==========
trading::AccountSnapshot MarketEngineManager::buildAccountSnapshot_(
    std::string_view market) const
{
    trading::AccountSnapshot snap{};

    auto budget = account_mgr_.getBudget(market);
    if (budget.has_value())
    {
        snap.krw_available = budget->available_krw;
        snap.coin_available = budget->coin_balance;
    }

    return snap;
}

} // namespace app
