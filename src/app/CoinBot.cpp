// app/DomainSmokeTest.cpp
#include <iostream>
#include "../core/domain/Account.h"
#include "../core/domain/Order.h"
#include "../core/domain/Ticker.h"

#include <boost/version.hpp>
#include <json.hpp>

int main() {
    using namespace core;

    Instrument btcKrw{ "KRW-BTC", "BTC", "KRW", false, "NONE"};

    // Fix: Use constructor or ordered member initialization for Account
    Account account;
    account.balance = 1'000'000.0;
    account.positions = {};

    Order buyOrder{
        .id = "test-order-1",
        .instrument = btcKrw,
        .position = OrderPosition::BUY,
        .type = OrderType::Limit,
        .price = 50'000'000.0,
        .volume = 0.01,
        .status = OrderStatus::New,
        .fees = std::nullopt,
    };

    Ticker ticker{
        .instrument = btcKrw,
        .tradePrice = 49'800'000.0,
        .accVolume24h = 123.45,
        .accAmount24h = 6'000'000'000.0,
        .changeRate = 0.0123,
        .timestamp = std::chrono::system_clock::now(),
    };

    std::cout << "Domain smoke test OK\n";
    std::cout << "Order id: " << buyOrder.id << ", market: " << buyOrder.instrument.market << "\n";
    std::cout << "Account cash: " << account.balance << "\n";
    std::cout << "Ticker price: " << ticker.tradePrice << "\n";
}
