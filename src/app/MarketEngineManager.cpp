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
    MarketManagerConfig cfg,
    db::Database* db)
    : api_(api)
    , store_(store)
    , account_mgr_(account_mgr)
    , cfg_(std::move(cfg))
    , db_(db)
{
    auto& logger = util::Logger::instance();
    const auto logBudgets = [this, &logger](std::string_view stage)
    {
        for (const auto& [market, ctx] : contexts_)
        {
            (void)ctx;
            const auto b = account_mgr_.getBudget(market);
            if (!b.has_value())
            {
                logger.warn("[MarketEngineManager][Budget][", stage,
                    "] market=", market, " missing");
                continue;
            }

            // 분배 잘 되었는지 확인 로그
            logger.info("[MarketEngineManager][Budget][", stage, "] market=", market,
                " krw_available=", b->available_krw,
                " krw_reserved=", b->reserved_krw,
                " coin_balance=", b->coin_balance,
                " avg_entry=", b->avg_entry_price);
        }
    };

    // 1) 계좌 동기화 (1차: 실패 시 예외)
    logger.info("[MarketEngineManager] Syncing account with exchange...");
    rebuildAccountOnStartup_(/*throw_on_fail=*/true);

    // 2) 마켓별 컨텍스트 생성 + 전략 복구
    for (const auto& market : markets)
    {
        // 중복 마켓 입력 방어: 덮어쓰면 이전 큐 포인터가 댕글링될 수 있음
        if (contexts_.count(market) > 0)
        {
            logger.warn("[MarketEngineManager] Duplicate market skipped: ", market);
            continue;
        }

        auto ctx = std::make_unique<MarketContext>(market, cfg_.queue_capacity);

        // MarketEngine 생성
        ctx->engine = std::make_unique<engine::MarketEngine>(
            market, api_, store_, account_mgr_);

        // 전략 생성
        ctx->strategy = std::make_unique<trading::strategies::RsiMeanReversionStrategy>(
            market, cfg_.strategy_params);

        // DB 신호 콜백 등록: PendingEntry→InPosition, PendingExit→Flat 전이 시 signals 테이블 기록
        if (db_) {
            ctx->strategy->setSignalCallback([this](const trading::SignalRecord& sig) {
                db_->insertSignal(sig);
            });
        }

        // StartupRecovery: 미체결 취소 + 포지션 복구
        recoverMarketState_(*ctx);

        logger.info("[MarketEngineManager] Context created for market=", market);
        contexts_[market] = std::move(ctx);
    }

    // 생성 직후(1차 동기화 + 복구 반영) 분배 상태 확인 로그
    logBudgets("after_recovery");

    // 3) 미체결 취소 후 최종 계좌 동기화 (2차: 실패 시 경고만)
    logger.info("[MarketEngineManager] Final account sync after recovery...");
    rebuildAccountOnStartup_(/*throw_on_fail=*/false);

    // 2차 동기화 후 최종 분배 상태 확인 로그
    logBudgets("after_final_sync");

    logger.info("[MarketEngineManager] Initialized with ", contexts_.size(), " markets",
        (contexts_.size() < markets.size()
            ? " (" + std::to_string(markets.size() - contexts_.size()) + " duplicates skipped)"
            : ""));
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

    auto& logger = util::Logger::instance();

    // 마켓별로 스레드 생성
    for (auto& [market, ctx] : contexts_)
    {
        // jthread 생성 시 stop_token이 자동으로 전달됨
        ctx->worker = std::jthread([this, &ctx_ref = *ctx](std::stop_token stoken) {
            workerLoop_(ctx_ref, stoken);
        });

        logger.info("[MarketEngineManager] Worker started for market=", market);
    }

    started_ = true;
}

// ========== stop ==========
void MarketEngineManager::stop()
{
    if (!started_) return;

    auto& logger = util::Logger::instance();
    logger.info("[MarketEngineManager] Stopping all workers...");

    // 모든 워커에 stop 요청 (request_stop → stop_token을 통해 전달)
    for (auto& [market, ctx] : contexts_)
        ctx->worker.request_stop();

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

// ========== rebuildAccountOnStartup_ ==========
bool MarketEngineManager::rebuildAccountOnStartup_(bool throw_on_fail)
{
    auto& logger = util::Logger::instance();

    for (int attempt = 1; attempt <= cfg_.sync_retry; ++attempt)
    {
        auto result = api_.getMyAccount();

        if (std::holds_alternative<core::Account>(result))
        {
            account_mgr_.rebuildFromAccount(std::get<core::Account>(result));

            // 마켓별 자본 현황 및 드리프트 관측 (avg_entry_price 기준 추정값)
            {
                auto budgets = account_mgr_.snapshot();
                double total = 0.0;
                for (const auto& [_, b] : budgets)
                    total += b.available_krw + b.coin_balance * b.avg_entry_price;

                const double target = budgets.empty() ? 0.0
                                    : total / static_cast<double>(budgets.size());

                for (const auto& [mkt, b] : budgets) {
                    const double coin_val = b.coin_balance * b.avg_entry_price;
                    const double equity   = b.available_krw + coin_val;
                    const double drift    = equity - target;
                    logger.info("[MarketEngineManager] budget: market=", mkt,
                        " krw=", b.available_krw, " coin_val=", coin_val,
                        " equity=", equity, " drift=", drift);
                    if (target > 0 && std::abs(drift) > target * 0.2)
                        logger.warn("[MarketEngineManager] Capital drift detected: market=", mkt,
                            " drift=", drift, " (", drift / target * 100.0, "%)");
                }
            }

            logger.info("[MarketEngineManager] Account synced (attempt ", attempt, ")");
            return true;
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
    return false;
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
void MarketEngineManager::workerLoop_(MarketContext& ctx, std::stop_token stoken)
{
    using namespace std::chrono_literals;
    auto& logger = util::Logger::instance();

    // 워커 스레드에서 발생한 로그를 마켓 파일로 분리하기 위한 태그 설정
    util::Logger::setThreadTag(ctx.market);
    struct ThreadTagGuard final
    {
        ~ThreadTagGuard() { util::Logger::clearThreadTag(); }
    } tag_guard;

    // 엔진을 현재 워커 스레드에 바인딩
    try { ctx.engine->bindToCurrentThread(); }
    catch (const std::exception& e)
    {
        logger.error("[MarketEngineManager][", ctx.market,
            "] bindToCurrentThread failed: ", e.what());
        return;  // 이 마켓만 포기, 전체 프로그램은 유지
    }

    logger.info("[MarketEngineManager][", ctx.market, "] Worker loop started");

    while (!stoken.stop_requested())
    {
        try
        {
            // 복구 요청은 일반 이벤트보다 먼저 처리
            if (ctx.recovery_requested.exchange(false, std::memory_order_acq_rel))
                runRecovery_(ctx);

            // 큐에서 이벤트 대기 (종료 반응성/CPU 균형값)
            auto maybe = ctx.event_queue.pop_for(50ms);

            if (maybe.has_value())
                handleOne_(ctx, *maybe);

            // 엔진이 쌓아둔 이벤트를 전략으로 전달
            auto out = ctx.engine->pollEvents();
            if (!out.empty())
                handleEngineEvents_(ctx, out);

            // Pending 상태 타임아웃 감시
            checkPendingTimeout_(ctx);
        }
        catch (const std::exception& e)
        {
            // 이벤트 하나 건너뛰고 계속 — 워커는 유지됨
            logger.error("[MarketEngineManager][", ctx.market,
                "] Event handling error (skipping): ", e.what());
        }
        catch (...)
        {
            // 비표준 예외도 잡아 스레드 밖으로 전파되지 않도록 방어
            logger.error("[MarketEngineManager][", ctx.market,
                "] Unknown exception (skipping)");
        }
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
        else if constexpr (std::is_same_v<T, engine::input::AccountSyncRequest>)
        {
            // 기본 경로는 atomic flag로 전환됨.
            // 큐에 남아있는 기존 이벤트 호환용으로 유지
            runRecovery_(ctx);
        }

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

    // 3) MyTrade 존재 여부 사전 확인 (done-only(바로체결) 감지용)
    bool has_trade = false;
    for (const auto& ev : events)
    {
        if (std::holds_alternative<core::MyTrade>(ev))
        {
            has_trade = true;
            break;
        }
    }

    // 4) 엔진 상태 반영
    for (const auto& ev : events)
    {
        if (std::holds_alternative<core::MyTrade>(ev))
        {
            const auto& t = std::get<core::MyTrade>(ev);
            ctx.engine->onMyTrade(t);

            logger.info("[Manager][", ctx.market, "][TradeEvent] order_uuid=",
                t.order_uuid, " price=", t.price, " vol=", t.volume);
        }
        else
        {
            const auto& o = std::get<core::Order>(ev);

            // done-only 케이스: MyTrade 없이 터미널 상태 도달 (4-1)
            // state="done"이 trade 없이 단독 수신되면 onOrderSnapshot만으로는
            // 자산 정산(finalizeFillBuy/Sell)이 누락됨
            // reconcileFromSnapshot은 delta 정산 후 내부에서 onOrderSnapshot을 호출함
            const bool isTerminal =
                o.status == core::OrderStatus::Filled ||
                o.status == core::OrderStatus::Canceled ||
                o.status == core::OrderStatus::Rejected;

            bool reconcile_ok = true;

            if (!has_trade && isTerminal)
            {
                logger.info("[Manager][", ctx.market,
                    "][OrderEvent] done-only detected, using reconcile path: "
                    "status=", static_cast<int>(o.status), " order_uuid=", o.id);

                reconcile_ok = ctx.engine->reconcileFromSnapshot(o);
                if (!reconcile_ok)
                {
                    logger.warn("[Manager][", ctx.market,
                        "][OrderEvent] reconcile failed, keep pending for recovery retry, order_uuid=", o.id);

                    // 정산 불확실 상태에서는 터미널 스냅샷 확정을 보류한다.
                    ctx.recovery_requested.store(true, std::memory_order_release);
                }
            }
            else
            {
                ctx.engine->onOrderSnapshot(o);
            }

            // DB 주문 이력 기록 (터미널 + 정산 성공 시에만)
            // - 터미널 시점은 state != "trade"이므로 origin 필드가 항상 올바르게 채워짐
            // - reconcile 실패 시 executed_funds 등이 불완전하므로 기록 보류
            if (db_ && isTerminal && reconcile_ok) db_->insertOrder(o);

            logger.info("[Manager][", ctx.market, "][OrderEvent] status=",
                static_cast<int>(o.status), " order_uuid=", o.id);
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
    const core::Candle incoming = api::upbit::mappers::toDomain(candleDto);

    // 동일 분봉 업데이트는 최신값으로 덮어쓰고,
    // 다음 분봉이 도착하면 이전 분봉을 "확정 close"로 처리한다.
    if (!ctx.pending_candle.has_value())
    {
        ctx.pending_candle = incoming;
        return;
    }

    if (ctx.pending_candle->start_timestamp == incoming.start_timestamp)
    {
        ctx.pending_candle = incoming;
        return;
    }

    const core::Candle candle = *ctx.pending_candle;
    ctx.pending_candle = incoming;

    // DB 확정 캔들 기록 (다음 분봉 도착 시점 = 이전 분봉 확정)
    if (db_) db_->insertCandle(ctx.market, candle);

    logger.info("[Manager][", ctx.market, "][Candle] ts=",
        candle.start_timestamp, " close=", candle.close_price);

    // 확정 캔들 close를 mark_price로 주입 (finalizeSellOrder dust 판정용)
    ctx.engine->setMarkPrice(candle.close_price);

    // 3) AccountManager에서 예산 조회 → 전략용 스냅샷 빌드
    const trading::AccountSnapshot account = buildAccountSnapshot_(ctx.market);
    logger.info("[Manager][", ctx.market, "][Account] krw_available=",
        account.krw_available, " coin_available=", account.coin_available);

    // 4) 전략 실행
    const trading::Decision d = ctx.strategy->onCandle(candle, account);
    const trading::Snapshot snap = ctx.strategy->signalSnapshot();

    // 전략 반영 여부 검증용 로그
    logger.info("[Manager][", ctx.market, "][Strategy] state=",
        toStringState(ctx.strategy->state()));
    //logger.info("[Manager][", ctx.market, "][Signal] marketOk=", snap.marketOk,
    //    //" rsi=", snap.rsi.v,
    //    " rsi_ready=", snap.rsi.ready,
    //    //" trend_strength=", snap.trendStrength,
    //    " trend_ready=", snap.trendReady,
    //    //" vol=", snap.volatility.v,
    //    " vol_ready=", snap.volatility.ready);

    // 5) 주문 의도가 있으면 엔진에 submit
    if (d.hasOrder())
    {
        const auto& req = *d.order;

        logger.info("[Manager][", ctx.market, "][Decision] side=",
            (req.position == core::OrderPosition::BID ? "BUY" : "SELL"),
            " reason=", req.client_tag,
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
                e.remaining_volume,
                e.executed_funds
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

// 복구 요청: atomic flag로 우선 처리
// 일반 큐 push 대신 flag → workerLoop_ 매 반복 시작에서 체크
// 중복 요청은 자동 병합 (flag이므로 여러 번 set해도 1회만 실행)
// pending 없는 마켓은 사전 필터링 — 불필요한 runRecovery_ 진입 차단
void MarketEngineManager::requestReconnectRecovery()
{
    int triggered = 0;

    for (auto& [market, ctx] : contexts_)
    {
        if (!ctx->has_active_pending.load(std::memory_order_acquire))
            continue;

        ctx->recovery_requested.store(true, std::memory_order_release);
        ++triggered;
    }

    if (triggered > 0)
        util::Logger::instance().info(
            "[MarketEngineManager] Reconnect recovery: ",
            triggered, " market(s) with pending orders");
    // triggered == 0: 정상 구간 — 로그 생략
}

// 주문 단건 조회 기반 복구
// 런타임에서 rebuildFromAccount 호출 금지 — 타 마켓 KRW 재분배 없음
// 복구 흐름:
//   1. pending 주문 ID 확보 → 없으면 즉시 종료
//   2. getOrder(order_uuid) 1차 → getOpenOrders 2차 fallback
//   3. reconcileFromSnapshot으로 delta 정산
//   4. 터미널 + 정산 성공이면 로그 기록 (상태 정리는 onOrderSnapshot 내부에서 완료됨)
//      정산 실패면 pending 유지(다음 recovery에서 재시도)
void MarketEngineManager::runRecovery_(MarketContext& ctx)
{
    // 1) pending 주문 ID 확보 (pre-filter 이후 안전망 — 레이스 컨디션 방어)
    const auto [buy_order_uuid, sell_order_uuid] = ctx.engine->activePendingIds();
    if (buy_order_uuid.empty() && sell_order_uuid.empty())
    {
        ctx.tracking_pending = false;
        ctx.pending_timeout_fired = false;
        return;
    }

    auto& logger = util::Logger::instance();
    logger.info("[MarketEngineManager][", ctx.market, "] Running recovery...");

    // 2) 각 pending 주문에 대해 복구 시도
    for (const auto& order_uuid : { buy_order_uuid, sell_order_uuid })
    {
        if (order_uuid.empty()) continue;

        // 1차: getOrder(order_uuid) 직접 조회 (최대 3회 재시도)
        std::optional<core::Order> order = queryOrderWithRetry_(order_uuid, 3);

        // 2차 fallback: getOpenOrders에서 동일 order_uuid 탐색
        if (!order.has_value())
            order = findOrderInOpenOrders_(ctx.market, order_uuid);

        // 3차: 모든 조회 실패 → 상태 유지, 다음 recovery 주기에서 재시도
        // 오정산보다 미정산이 안전 — KRW/예약 불변식 보호
        if (!order.has_value())
        {
            // 진단용 계좌 로그 (상태 변경 없음)
            auto diag = api_.getMyAccount();
            if (std::holds_alternative<core::Account>(diag))
            {
                const auto& acct = std::get<core::Account>(diag);
                logger.warn("[MarketEngineManager][", ctx.market,
                    "] Recovery fallback: order query failed, "
                    "actual_krw=", acct.krw_free,
                    " positions=", acct.positions.size(),
                    " keeping pending state for order=", order_uuid);
            }
            else
            {
                logger.warn("[MarketEngineManager][", ctx.market,
                    "] All recovery methods failed for order=", order_uuid);
            }
            continue;
        }

        // delta 정산 (MarketEngine 단일 경로)
        const bool reconciled = ctx.engine->reconcileFromSnapshot(*order);

        const bool isTerminal =
            order->status == core::OrderStatus::Filled ||
            order->status == core::OrderStatus::Canceled ||
            order->status == core::OrderStatus::Rejected;

        if (isTerminal)
        {
            if (!reconciled)
            {
                // 금액 미확정 주문은 상태를 닫지 않고 다음 recovery에서 재시도한다.
                logger.warn("[MarketEngineManager][", ctx.market,
                    "] Terminal order unresolved, keeping pending state, order=", order_uuid);
                continue;
            }

            // 터미널 + 정산 성공:
            // - clearPendingState: reconcileFromSnapshot → onOrderSnapshot 내부에서 이미 처리됨 (no-op)
            // - syncOnStart: EngineOrderStatusEvent.executed_funds → onOrderUpdate 폴백으로 대체됨

            // WS 유실로 handleMyOrder_를 거치지 않은 주문의 최종 상태를 DB에 반영
            if (db_) db_->insertOrder(*order);

            logger.info("[MarketEngineManager][", ctx.market,
                "] Recovery done: state=", toStringState(ctx.strategy->state()));
        }
        else
        {
            if (reconciled)
            {
                // open: 부분 체결분은 reconcileFromSnapshot에서 이미 delta 정산됨
                logger.info("[MarketEngineManager][", ctx.market,
                    "] Order still open (partial fill reconciled), "
                    "waiting for WS events, order=", order_uuid);
            }
            else
            {
                logger.warn("[MarketEngineManager][", ctx.market,
                    "] Open order unresolved, keeping pending state, order=", order_uuid);
            }
        }
    }

    ctx.tracking_pending = false;
    ctx.pending_timeout_fired = false;
}

// getOrder 재시도 (1초 간격)
std::optional<core::Order> MarketEngineManager::queryOrderWithRetry_(
    std::string_view order_uuid, int max_retries)
{
    auto& logger = util::Logger::instance();

    for (int i = 1; i <= max_retries; ++i)
    {
        auto result = api_.getOrder(order_uuid);
        if (std::holds_alternative<core::Order>(result))
        {
            const auto order = std::get<core::Order>(result);

            // 체결 수량은 있는데 체결 금액이 0이면 불완전 스냅샷일 수 있어 짧게 재조회한다.
            const bool needs_funds_retry =
                (order.executed_volume > 0.0 && order.executed_funds <= 0.0);

            if (!needs_funds_retry || i == max_retries)
                return order;

            logger.warn("[MarketEngineManager] getOrder incomplete funds (attempt ",
                i, "/", max_retries, "), retrying: order_uuid=", order_uuid,
                " executed_volume=", order.executed_volume,
                " executed_funds=", order.executed_funds);

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        const auto& err = std::get<api::rest::RestError>(result);
        logger.warn("[MarketEngineManager] getOrder failed (attempt ",
            i, "/", max_retries, "): ", err.message);

        if (i < max_retries)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return std::nullopt;
}

// getOpenOrders에서 특정 order_uuid 탐색
std::optional<core::Order> MarketEngineManager::findOrderInOpenOrders_(
    std::string_view market, std::string_view order_uuid)
{
    auto result = api_.getOpenOrders(market);
    if (!std::holds_alternative<std::vector<core::Order>>(result))
        return std::nullopt;

    const auto& orders = std::get<std::vector<core::Order>>(result);
    for (const auto& o : orders)
    {
        if (o.id == order_uuid)
            return o;
    }
    return std::nullopt;
}

// ========== checkPendingTimeout_ ==========
// 엔진의 active pending 주문이 cfg_.pending_timeout 이상 지속되면 runRecovery_ 실행
// 전략 state 대신 engine->activePendingIds()를 기준으로 추적한다.
// - self-heal이 전략 state를 먼저 바꾸더라도 engine token이 남아있으면 추적 유지
// - 정상 체결 시: onOrderSnapshot(Filled) → finalizeBuyToken_ → active ID 해제 → 자동 리셋
void MarketEngineManager::checkPendingTimeout_(MarketContext& ctx)
{
    const auto [buy_id, sell_id] = ctx.engine->activePendingIds();
    const bool is_pending = (!buy_id.empty() || !sell_id.empty());

    // WS IO thread가 읽을 수 있도록 atomic 동기화
    ctx.has_active_pending.store(is_pending, std::memory_order_release);

    // Pending 진입 시점 기록
    if (is_pending && !ctx.tracking_pending)
    {
        ctx.tracking_pending = true;
        ctx.pending_entered_at = std::chrono::steady_clock::now();
        ctx.pending_timeout_fired = false;
    }

    // Pending 해제 시 추적 리셋
    if (!is_pending)
    {
        ctx.tracking_pending = false;
        ctx.pending_timeout_fired = false;
        return;
    }

    // 이미 타임아웃 처리됨 → 다음 상태 변화까지 대기
    if (ctx.pending_timeout_fired)
        return;

    // 타임아웃 체크
    const auto elapsed = std::chrono::steady_clock::now() - ctx.pending_entered_at;
    if (elapsed >= cfg_.pending_timeout)
    {
        util::Logger::instance().warn(
            "[MarketEngineManager][", ctx.market,
            "] Pending timeout (", cfg_.pending_timeout.count(),
            "s) exceeded, buy=", buy_id, " sell=", sell_id);

        ctx.pending_timeout_fired = true;
        runRecovery_(ctx);
    }
}

} // namespace app