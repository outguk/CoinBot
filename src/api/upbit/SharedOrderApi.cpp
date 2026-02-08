// src/api/upbit/SharedOrderApi.cpp

#include "SharedOrderApi.h"
#include "api/upbit/UpbitExchangeRestClient.h"

namespace api::upbit {

    namespace {
        struct InFlightGuard {
            std::atomic<int>& in_flight;
            std::atomic<int>& max_in_flight;

            explicit InFlightGuard(std::atomic<int>& f, std::atomic<int>& m)
                : in_flight(f), max_in_flight(m)
            {
                const int cur = in_flight.fetch_add(1) + 1;
                int prev = max_in_flight.load();
                while (prev < cur && !max_in_flight.compare_exchange_weak(prev, cur)) {
                    // retry
                }
            }
            ~InFlightGuard() {
                in_flight.fetch_sub(1);
            }
        };
    } // namespace

    SharedOrderApi::SharedOrderApi(std::unique_ptr<api::rest::UpbitExchangeRestClient> client)
        : client_(std::move(client))
    {
        if (!client_) {
            throw std::invalid_argument("SharedOrderApi: client cannot be null");
        }
    }

    std::variant<core::Account, api::rest::RestError>
    SharedOrderApi::getMyAccount()
    {
        std::lock_guard<std::mutex> lock(mtx_);

        // 테스트 용
        // IMPORTANT: increment happens *after* lock acquired (this is the proof point)
        InFlightGuard g(in_flight_, max_in_flight_);

        return client_->getMyAccount();
    }

    std::variant<std::vector<core::Order>, api::rest::RestError>
    SharedOrderApi::getOpenOrders(std::string_view market)
    {
        std::lock_guard<std::mutex> lock(mtx_);

        // IMPORTANT: increment happens *after* lock acquired (this is the proof point)
        InFlightGuard g(in_flight_, max_in_flight_);

        return client_->getOpenOrders(market);
    }

    std::variant<bool, api::rest::RestError>
    SharedOrderApi::cancelOrder(const std::optional<std::string>& uuid,
                                 const std::optional<std::string>& identifier)
    {
        std::lock_guard<std::mutex> lock(mtx_);

        // IMPORTANT: increment happens *after* lock acquired (this is the proof point)
        InFlightGuard g(in_flight_, max_in_flight_);

        return client_->cancelOrder(uuid, identifier);
    }

    std::variant<std::string, api::rest::RestError>
    SharedOrderApi::postOrder(const core::OrderRequest& req)
    {
        std::lock_guard<std::mutex> lock(mtx_);

        // IMPORTANT: increment happens *after* lock acquired (this is the proof point)
        InFlightGuard g(in_flight_, max_in_flight_);

        return client_->postOrder(req);
    }

} // namespace api::upbit
