// app/EngineRunner.cpp
#include "app/EngineRunner.h"

#include <iostream>
#include <variant>
#include <vector>
#include <chrono>
#include <sstream>
#include <unordered_map>

#include <json.hpp>

// myOrder DTO/mapper는 app에서만 사용(계층 경계)
#include "api/upbit/mappers/MyOrderMapper.h"
#include "api/upbit/mappers/CandleMapper.h"

namespace app
{
    namespace
    {
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

        // (1) 어떤 타입이 ostream(<<)으로 출력 가능한지 체크
        template <typename T>
        concept OStreamable = requires(std::ostream & os, const T & v) {
            { os << v } -> std::same_as<std::ostream&>;
        };

        // (2) 값 하나를 로그용 문자열로 바꾸기
        template <typename T>
        static std::string toLogValue(const T& v)
        {
            if constexpr (OStreamable<T>) {
                std::ostringstream oss;
                oss << v;
                return oss.str();
            }
            else if constexpr (std::is_arithmetic_v<T>) {
                return std::to_string(v);
            }
            else {
                // Price/Volume이 래퍼라 << 도 안 되고 산술도 아니면 여기로 온다.
                // 이 경우엔 “프로젝트 타입에 맞춘 특수화”를 아래에 추가하면 된다.
                return "<non-streamable>";
            }
        }

        // (3) optional을 로그용 문자열로 바꾸기
        template <typename T>
        static std::string optToLog(const std::optional<T>& opt, std::string_view none = "<none>")
        {
            return opt ? toLogValue(*opt) : std::string(none);
        }

        // OrderSize를 로그용 문자열로 변환
        static std::string orderSizeToLog(const core::OrderSize& size)
        {
            return std::visit([](const auto& s) -> std::string {
                using T = std::decay_t<decltype(s)>;

                std::ostringstream oss;

                if constexpr (std::is_same_v<T, core::VolumeSize>)
                {
                    oss << "VOL=" << s.value;
                }
                else if constexpr (std::is_same_v<T, core::AmountSize>)
                {
                    oss << "AMOUNT=" << s.value;
                }
                else
                {
                    oss << "<UNKNOWN_SIZE>";
                }

                return oss.str();
                }, size);
        }
    } // namespace

    std::string EngineRunner::extractCurrency_(std::string_view market)
    {
        // "KRW-BTC" -> "BTC"
        const auto pos = market.find('-');
        if (pos == std::string_view::npos) return {};
        return std::string(market.substr(pos + 1));
    }

    void EngineRunner::rebuildAccountSnapshot_()
    {
        // 엔진이 유지/갱신하는 core::Account에서 필요한 값만 뽑아서
        //    trading::AccountSnapshot 형태로 "얕게" 캐싱한다.
        last_account_.krw_available = static_cast<double>(account_.krw_free);

        const std::string cur = extractCurrency_(market_);
        double coin = 0.0;

        // positions에서 해당 currency를 찾아 balance를 coin_available로 사용
        for (const auto& p : account_.positions)
        {
            if (p.currency == cur)
            {
                coin = p.free;
                break;
            }
        }
        last_account_.coin_available = coin;
    }

    void EngineRunner::run(std::atomic<bool>& stop_flag)
    {
        using namespace std::chrono_literals;

        while (!stop_flag.load(std::memory_order_relaxed))
        {
            // 무기한 블로킹 대신 "짧게 대기" 후 깨어나 stop_flag를 재확인
            auto maybe = private_q_.pop_for(200ms);

            if (maybe.has_value())
                handleOne_(*maybe);

            // 엔진이 쌓아둔 이벤트를 전략으로 전달
            // (항상 엔진 스레드에서만 poll)
            auto out = engine_.pollEvents();
            if (!out.empty())
                handleEngineEvents_(out);
        }
    }

    void EngineRunner::handleOne_(const engine::input::EngineInput& in)
    {
        std::visit([&](const auto& x)
            {
                using T = std::decay_t<decltype(x)>;

                if constexpr (std::is_same_v<T, engine::input::MyOrderRaw>)
                {
                    const nlohmann::json j = nlohmann::json::parse(x.json, nullptr, false);
                    if (j.is_discarded())
                    {
                        std::cout << "[EngineRunner] myOrder JSON parse failed\n";
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
                        std::cout << "[EngineRunner] myOrder dto convert failed: " << e.what() << "\n";
                        return;
                    }

                    // 2) DTO -> (Order snapshot, MyTrade) 이벤트 분해
                    const auto events = api::upbit::mappers::toEvents(dto);

                    // 3) 엔진 상태 반영 (엔진 단일 소유권: 여기서만 호출)
                    for (const auto& ev : events)
                    {
                        if (std::holds_alternative<core::Order>(ev))
                        {
                            const auto& o = std::get<core::Order>(ev);
                            engine_.onOrderSnapshot(o);

                            // [DBG] 내가 넣은 주문의 상태 변화
                            std::cout << "[Runner][OrderEvent] snapshot status=" 
                                << static_cast<int>(o.status)
                                << " uuid=" << o.id
                                //<< " ident=" << optToLog(o.identifier)
                                << "\n";

                            // terminal 판정/발행은 엔진(onOrderSnapshot)에서 책임진다.
                        }
                        else
                        {
                            const auto& t = std::get<core::MyTrade>(ev);
                            engine_.onMyTrade(t);

                            // [DBG] 실제 체결 발생시
                            std::cout << "[Runner][TradeEvent]"
                                //<< "trade_id = " << t.trade_id
                                << " uuid=" << t.order_id
                                << " price=" << t.price
                                << " vol=" << t.volume
                                //<< " ident=" << optToLog(t.identifier)
                                << "\n";
                        }
                    }
                    rebuildAccountSnapshot_();
                    std::cout << "[Runner][Account] krw=" << last_account_.krw_available
                        << " coin=" << last_account_.coin_available << "\n";
                }
                else if constexpr (std::is_same_v<T, engine::input::MarketDataRaw>)
                {
                    // 캔들/마켓데이터는 엔진 스레드에서만 JSON 파싱 (중복 파싱 방지 방향)
                    const nlohmann::json j = nlohmann::json::parse(x.json, nullptr, false);
                    if (j.is_discarded())
                    {
                        std::cout << "[EngineRunner] MarketData JSON parse failed\n";
                        return;
                    }

                    // type 확인: candle.{unit} 형태(예: "candle.1s", "candle.3", ...)
                    const std::string type = j.value("type", "");
                    if (type.rfind("candle", 0) != 0)
                    {
                        // 지금 단계에서는 candle만 처리.
                        // ticker/orderbook 등은 추후 확장 포인트로 남김.
                        return;
                    }

                    // 1) JSON -> Candle DTO
                    api::upbit::dto::CandleDto_Minute candleDto{};
                    try
                    {
                        candleDto = j.get<api::upbit::dto::CandleDto_Minute>();
                    }
                    catch (const std::exception& e)
                    {
                        std::cout << "[EngineRunner] candle dto convert failed: " << e.what() << "\n";
                        return;
                    }

                    // 2) DTO -> core::Candle (도메인 오염 방지: json 구조를 도메인에 넣지 않음)
                    const core::Candle candle = api::upbit::mappers::toDomain(candleDto);

                    // 같은 market에서 같은 1분(ts) 캔들이 반복되면 조용히 무시
                    static std::unordered_map<std::string, std::string> last_ts_by_market;

                    auto& last_ts = last_ts_by_market[candle.market];
                    if (!last_ts.empty() && last_ts == candle.start_timestamp)
                    {
                        // 중복 업데이트는 드롭 (성능 + 로그 스팸 방지)
                        return;
                    }
                    last_ts = candle.start_timestamp;

                    // 캔들 수신 + 전략 진입 확인용 로그
                    std::cout 
                        <<"\n"
                        << "[Runner][Candle] market=" << candle.market
                        << " ts=" << candle.start_timestamp
                        << " close=" << candle.close_price
                        ;

                    // 3) 전략 실행 (RSI). account는 캐시된 최신 스냅샷을 사용한다.
                    const trading::Decision d = strategy_.onCandle(candle, last_account_);

                    // 전략에 상태 이상유무 확인 로그
                    std::cout 
                        << "\n"
                        << "[Runner][Strategy] state="
                        << toStringState(strategy_.state())
                        << "\n"
                        << "\n"
                        ;


                    // 4) 주문 의도가 있으면 엔진에 submit (시장가 전제)
                    if (d.hasOrder())
                    {
                        const auto& req = *d.order;

                        // 주문 생성 확인 로그 (식별자/side/수량/가격)
                        std::cout << "[Runner][Decision] 주문 생성"
                            << " side=" << (d.order->position == core::OrderPosition::BID ? "BUY" : "SELL")
                            //<< " ident=" << d.order->identifier
                            << " vol=" << orderSizeToLog(d.order->size)
                            << "\n";

                        const auto r = engine_.submit(req);

                        // 케이스 A: submit이 실패했는데 Runner가 무시해서 전략만 PendingEntry가 됨
                        std::cout << "[Runner][Submit] success=" << r.success
                            << " code=" << static_cast<int>(r.code)
                            << " hasOrder=" << r.order.has_value()
                            << " msg=" << r.message
                            << "\n";

                        if (!r.success)
                        {
                            std::cout << "[Runner][Submit] FAILED -> rollback strategy pending\n";
                            strategy_.onSubmitFailed();
                        }
                    }
                        
                }
                else
                {
                    // 다른 EngineInput variant가 생기면 여기서 확장
                }

            }, in);
    }

    void EngineRunner::handleEngineEvents_(const std::vector<engine::EngineEvent>& evs)
    {
        for (const auto& ev : evs)
        {
            if (std::holds_alternative<engine::EngineFillEvent>(ev))
            {
                const auto& e = std::get<engine::EngineFillEvent>(ev);

                // 엔진 이벤트 -> 전략 이벤트 변환
                const trading::FillEvent fill{
                    e.identifier,
                    e.position,
                    static_cast<double>(e.fill_price),
                    static_cast<double>(e.filled_volume)
                };

                strategy_.onFill(fill);
            }
            else if (std::holds_alternative<engine::EngineOrderStatusEvent>(ev))
            {
                const auto& e = std::get<engine::EngineOrderStatusEvent>(ev);

                // 엔진 이벤트 -> 전략 이벤트 변환
                trading::OrderStatusEvent out{
                    e.identifier,
                    e.status,
                    e.position,
                    e.executed_volume,
                    e.remaining_volume
                };

                strategy_.onOrderUpdate(out);
            }
        }
    }

}
