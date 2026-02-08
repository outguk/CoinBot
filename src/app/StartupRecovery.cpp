#include "app/StartupRecovery.h"

#include <algorithm>
#include <iostream>

namespace app {

    // - 문자열이 prefix로 시작하는지 검사한다.
    // - "봇 주문만 취소"를 위해 가장 중요한 안전장치.
    bool StartupRecovery::startsWith(std::string_view s, std::string_view prefix) noexcept
    {
        return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
    }

    // - market: "KRW-BTC" -> "KRW"
    // - 형식이 다르면 빈 string_view 반환.
    std::string_view StartupRecovery::unitCurrency(std::string_view market)
    {
        const auto p = market.find('-');
        if (p == std::string_view::npos) return {};
        return market.substr(0, p);
    }
    // - market: "KRW-BTC" -> "BTC"
    std::string_view StartupRecovery::baseCurrency(std::string_view market)
    {
        const auto p = market.find('-');
        if (p == std::string_view::npos) return {};
        return market.substr(p + 1);
    }

    void StartupRecovery::cancelBotOpenOrders(api::rest::UpbitExchangeRestClient& api,
        std::string_view market,
        const Options& opt)
    {
        // 0) prefix가 비어있으면 "아무거나 취소"로 이어질 수 있다.
        //    -> 안전을 위해 빈 prefix면 취소 루틴을 건너뛰는 편이 더 안전하다.
        if (opt.bot_identifier_prefix.empty()) {
            std::cout << "[Startup][Warn] bot_identifier_prefix is empty. skip cancel.\n";
            return;
        }

        // 1) 미체결 주문 조회
         //    - UpbitExchangeRestClient::getOpenOrders()가 성공하면 vector<Order>, 실패하면 RestError
        auto r = api.getOpenOrders(market);
        if (std::holds_alternative<api::rest::RestError>(r)) {
            const auto& e = std::get<api::rest::RestError>(r);
            std::cout << "[Startup][Warn] getOpenOrders failed: " << e.message << "\n";
            return; // 시작 정책상: 조회 실패면 취소 없이 진행(원하면 여기서 종료해도 됨)
        }

        auto open = std::get<std::vector<core::Order>>(std::move(r));
        int cancel_count = 0;

        for (const auto& o : open)
        {
            // identifier(프로그램에서 부여)가 없으면 봇 주문 여부 판단 불가 → 건너뜀
            if (!o.identifier.has_value())
                continue;

            // 오취소 방지: prefix 미일치면 건너뜀
            if (!startsWith(*o.identifier, opt.bot_identifier_prefix))
                continue;

            // Upbit는 uuid 또는 identifier 둘 중 하나로 취소 가능.
            // 여기서는 uuid를 우선 사용(도메인 Order.id에 uuid가 들어간다는 전제).
            const std::optional<std::string> uuid =
                o.id.empty() ? std::nullopt : std::optional<std::string>(o.id);

            bool ok = false;

            // 3) 취소 재시도
           //    - 네트워크 일시 오류, Upbit 응답 지연 등에 대비
            for (int i = 0; i < opt.cancel_retry; ++i)
            {
                auto cr = api.cancelOrder(uuid, o.identifier);

                if (std::holds_alternative<bool>(cr) && std::get<bool>(cr)) {
                    ok = true;
                    break;
                }

                // 보완 포인트:
                // - 실패 시 RestError 내용을 로그로 남기면 디버깅에 매우 유리
                // - (다만 지금 cancelOrder()는 variant<bool, RestError>라서
                //   실패가 bool(false)인지 RestError인지 구분 가능하게 구현하는 게 좋음)
                //
                // if (std::holds_alternative<api::rest::RestError>(cr)) { ... }
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

        // 2) 취소 반영 확인(선택)
        //    - Upbit 쪽에서 취소 반영이 즉시 되지 않을 수 있어서, 몇 번 재조회로 확인한다.
        //    - verify_retry 사이에 짧은 sleep/backoff가 있으면 안정성이 좋아진다.
        for (int v = 0; v < opt.verify_retry; ++v)
        {
            auto rr = api.getOpenOrders(market);
            if (std::holds_alternative<api::rest::RestError>(rr))
                break; // 조회 실패하면 검증 중단

            const auto remain = std::get<std::vector<core::Order>>(std::move(rr));

            const bool anyBotRemain = std::any_of(remain.begin(), remain.end(),
                [&](const core::Order& o) {
                    return o.identifier.has_value()
                        && startsWith(*o.identifier, opt.bot_identifier_prefix);
                });

            if (!anyBotRemain)
                break;

            std::cout << "[Startup] bot open orders remain. re-check #" << (v + 1) << "\n";
        }

        std::cout << "[Startup] cancelBotOpenOrders done. cancel_count=" << cancel_count << "\n";
    }

    trading::PositionSnapshot StartupRecovery::buildPositionSnapshot(api::rest::UpbitExchangeRestClient& api,
        std::string_view market)
    {
        trading::PositionSnapshot pos{};

        // 1) 계정 조회
        auto ar = api.getMyAccount();
        if (std::holds_alternative<api::rest::RestError>(ar)) {
            const auto& e = std::get<api::rest::RestError>(ar);
            std::cout << "[Startup][Warn] getMyAccount failed: " << e.message << "\n";
            return pos; // 실패 시 coin=0으로 반환 → 전략은 Flat으로 복구
        }

        const auto acc = std::get<core::Account>(std::move(ar));

        // 2) market("KRW-BTC")에서 base/unit 추출
        const std::string_view base = baseCurrency(market); // BTC
        const std::string_view unit = unitCurrency(market); // KRW

        // 보완 포인트(구현됨)
        // - market 형식이 깨졌으면 base/unit이 빈값이 될 수 있음.
        //   이 경우 그냥 pos(0,0)를 반환하면 Flat으로 복구됨.
        if (base.empty() || unit.empty()) {
            std::cout << "[Startup][Warn] invalid market format: " << market << "\n";
            return pos;
        }

        // 3) 계정 포지션에서 해당 currency 찾기
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

} // namespace app
