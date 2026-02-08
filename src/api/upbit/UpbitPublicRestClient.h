#pragma once
#include <string>
#include <vector>
#include <variant>

#include "../src/api/rest/RestClient.h"
//#include "../src/api/upbit/dto/UpbitQuotationDtos.h"
#include "../src/api/upbit/mappers/MarketMapper.h"
#include "../src/api/upbit/mappers/TickerMapper.h"
#include "../src/api/upbit/mappers/CandleMapper.h"
#include "../src/api/upbit/mappers/OrderbookMapper.h"

namespace api::upbit
{
	class UpbitPublicRestClient
	{
	public:
		explicit UpbitPublicRestClient(const api::rest::RestClient& rest)
			: rest_(rest) {}

		// GET /v1/market/all?isDetails=false
		std::variant < std::vector<core::MarketInfo>, api::rest::RestError>
			getMarkets(bool isDetails = false) const;

		// GET /v1/ticker
		std::variant<std::vector<core::Ticker>, api::rest::RestError>
			getTickers(const std::vector<std::string>& markets) const;

		// GET /v1/candles/minutes/{unit}
		std::variant<std::vector<core::Candle>, api::rest::RestError>
			getCandlesMinutes(const std::string& market,
				int unit,
				int count,
				std::optional<std::string> to = std::nullopt) const;

		// GET /v1/orderbook
		std::variant<std::vector<core::Orderbook>, api::rest::RestError>
			getOrderbooks(const std::vector<std::string>& markets,
				std::optional<std::string> level = std::nullopt,
				std::optional<int> count = std::nullopt) const;

	private:
		const api::rest::RestClient rest_;

	};
}
