#include "app/StartupRecovery.h"

#include <algorithm>
#include <iostream>

namespace app {

    // 유틸리티 함수들 (템플릿 헬퍼에서도 사용)
    namespace {

        bool startsWithImpl(std::string_view s, std::string_view prefix) noexcept
        {
            return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
        }

        // "KRW-BTC" -> "KRW"
        std::string_view unitCurrencyImpl(std::string_view market)
        {
            const auto p = market.find('-');
            if (p == std::string_view::npos) return {};
            return market.substr(0, p);
        }

        // "KRW-BTC" -> "BTC"
        std::string_view baseCurrencyImpl(std::string_view market)
        {
            const auto p = market.find('-');
            if (p == std::string_view::npos) return {};
            return market.substr(p + 1);
        }

        // 템플릿 헬퍼: UpbitExchangeRestClient와 IOrderApi 모두 동일한 로직 수행
        template <class ApiT>
        void cancelBotOpenOrdersImpl(ApiT& api, std::string_view market,
            const StartupRecovery::Options& opt)
        {
            if (opt.bot_identifier_prefix.empty()) {
                std::cout << "[Startup][Warn] bot_identifier_prefix is empty. skip cancel.\n";
                return;
            }

            auto r = api.getOpenOrders(market);
            if (std::holds_alternative<api::rest::RestError>(r)) {
                const auto& e = std::get<api::rest::RestError>(r);
                std::cout << "[Startup][Warn] getOpenOrders failed: " << e.message << "\n";
                return;
            }

            auto open = std::get<std::vector<core::Order>>(std::move(r));
            int cancel_count = 0;

            for (const auto& o : open)
            {
                if (!o.identifier.has_value())
                    continue;

                if (!startsWithImpl(*o.identifier, opt.bot_identifier_prefix))
                    continue;

                const std::optional<std::string> uuid =
                    o.id.empty() ? std::nullopt : std::optional<std::string>(o.id);

                bool ok = false;
                for (int i = 0; i < opt.cancel_retry; ++i)
                {
                    auto cr = api.cancelOrder(uuid, o.identifier);
                    if (std::holds_alternative<bool>(cr) && std::get<bool>(cr)) {
                        ok = true;
                        break;
                    }
                }

                if (ok) {
                    ++cancel_count;
                    std::cout << "[Startup] cancel ok: uuid=" << o.id
                        << " identifier=" << *o.identifier << "\n";
                }
                else {
                    std::cout << "[Startup][Warn] cancel failed: uuid=" << o.id
                        << " identifier=" << *o.identifier << "\n";
                }
            }

            for (int v = 0; v < opt.verify_retry; ++v)
            {
                auto rr = api.getOpenOrders(market);
                if (std::holds_alternative<api::rest::RestError>(rr))
                    break;

                const auto remain = std::get<std::vector<core::Order>>(std::move(rr));

                const bool anyBotRemain = std::any_of(remain.begin(), remain.end(),
                    [&](const core::Order& o) {
                        return o.identifier.has_value()
                            && startsWithImpl(*o.identifier, opt.bot_identifier_prefix);
                    });

                if (!anyBotRemain)
                    break;

                std::cout << "[Startup] bot open orders remain. re-check #" << (v + 1) << "\n";
            }

            std::cout << "[Startup] cancelBotOpenOrders done. cancel_count=" << cancel_count << "\n";
        }

        template <class ApiT>
        trading::PositionSnapshot buildPositionSnapshotImpl(ApiT& api, std::string_view market)
        {
            trading::PositionSnapshot pos{};

            auto ar = api.getMyAccount();
            if (std::holds_alternative<api::rest::RestError>(ar)) {
                const auto& e = std::get<api::rest::RestError>(ar);
                std::cout << "[Startup][Warn] getMyAccount failed: " << e.message << "\n";
                return pos;
            }

            const auto acc = std::get<core::Account>(std::move(ar));

            const std::string_view base = baseCurrencyImpl(market);
            const std::string_view unit = unitCurrencyImpl(market);

            if (base.empty() || unit.empty()) {
                std::cout << "[Startup][Warn] invalid market format: " << market << "\n";
                return pos;
            }

            for (const auto& p : acc.positions)
            {
                if (p.currency == base && p.unit_currency == unit)
                {
                    pos.coin = p.free;
                    pos.avg_entry_price = p.avg_buy_price;
                    break;
                }
            }

            std::cout << "[Startup] PositionSnapshot: coin=" << pos.coin
                << " avg_entry_price=" << pos.avg_entry_price
                << " (market=" << market << ")\n";

            return pos;
        }

    } // anonymous namespace

    // 클래스 메서드는 내부 헬퍼에 위임
    bool StartupRecovery::startsWith(std::string_view s, std::string_view prefix) noexcept
    {
        return startsWithImpl(s, prefix);
    }

    std::string_view StartupRecovery::unitCurrency(std::string_view market)
    {
        return unitCurrencyImpl(market);
    }

    std::string_view StartupRecovery::baseCurrency(std::string_view market)
    {
        return baseCurrencyImpl(market);
    }

    // UpbitExchangeRestClient 오버로드 → 템플릿 헬퍼 위임
    void StartupRecovery::cancelBotOpenOrders(api::rest::UpbitExchangeRestClient& api,
        std::string_view market, const Options& opt)
    {
        cancelBotOpenOrdersImpl(api, market, opt);
    }

    // IOrderApi 오버로드 → 동일 템플릿 헬퍼 위임
    void StartupRecovery::cancelBotOpenOrders(api::upbit::IOrderApi& api,
        std::string_view market, const Options& opt)
    {
        cancelBotOpenOrdersImpl(api, market, opt);
    }

    // UpbitExchangeRestClient 오버로드
    trading::PositionSnapshot StartupRecovery::buildPositionSnapshot(
        api::rest::UpbitExchangeRestClient& api, std::string_view market)
    {
        return buildPositionSnapshotImpl(api, market);
    }

    // IOrderApi 오버로드
    trading::PositionSnapshot StartupRecovery::buildPositionSnapshot(
        api::upbit::IOrderApi& api, std::string_view market)
    {
        return buildPositionSnapshotImpl(api, market);
    }

} // namespace app
