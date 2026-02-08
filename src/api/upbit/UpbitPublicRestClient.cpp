#include <algorithm>
#include <json.hpp>

#include "../src/api/upbit/UpbitPublicRestClient.h"

/*
* UpbitPublicRestClient.cpp
* JSON 파싱 가능 여부 판단
* DTO → Domain 변환
* status 의미 해석 (2xx vs others)
*/

namespace api::upbit
{
	namespace
	{
		// 파싱 에러와 status 성공 여부는 UpbitPublicRestClient에서 관리
		inline bool isSuccessStatus(int status) noexcept { return (status >= 200 && status <= 299); }

		// 너무 긴 에러를 받을 경우 잘라서 받기 위한 snippet
		inline std::string bodySnippet(const std::string& body, std::size_t maxLen = 256)
		{
			if (body.size() <= maxLen) return body;
			return body.substr(0, maxLen);
		}

		// 여러 페어쌍을 검색할 때 , 기준으로 vector를 나누기 위한 함수
		inline std::string joinMarkets(const std::vector<std::string>& markets)
		{
			std::ostringstream oss;
			for (std::size_t i = 0; i < markets.size(); ++i)
			{
				if (i)
					oss << ",";
				oss << markets[i];
			}
			return oss.str();
		}

		// 공통: HttpResponse -> RestError (상태 코드 실패 시)
		inline api::rest::RestError makeHttpStatusError(
			int status,
			const std::string& where,
			const std::string& body)
		{
			api::rest::RestError e{};
			e.code = api::rest::RestErrorCode::BadStatus;
			e.http_status = status;
			e.message = where + "failed, http = " + std::to_string(status)
				+ ", body = " + bodySnippet(body);

			return e;
		}

		// 공통: JSON 파싱/DTO 변환 실패 -> RestError
		inline api::rest::RestError makeParseError(
			int status,
			const std::string& where,
			const std::string& what,
			const std::string& body
		)
		{
			api::rest::RestError e{};
			e.code = api::rest::RestErrorCode::ParseError;
			e.http_status = status;
			e.message = where + "parse failed : " + what
				+ ", body = " + bodySnippet(body);

			return e;
		}
	}

	// 마켓 정보 부르기
	std::variant < std::vector<core::MarketInfo>, api::rest::RestError> 
		UpbitPublicRestClient::getMarkets(bool isDetails) const
	{
		// 1) 요청 구성 (Upbit 규칙만 책임을 가짐)
		api::rest::HttpRequest req;
		req.host = "api.upbit.com";
		req.port = "443";
		req.method = api::rest::HttpMethod::Get;
		// 여기 오류 주의
		req.target = std::string("/v1/market/all?is_details=") + (isDetails ? "true" : "false");

		// RestClient가 Host/User-Agent를 세팅하더라도 Accept는 명확히 주는게 안전 (뭔솔?)
		// Accept는 요청 헤더(Request Header), “이 요청은 JSON 형식의 응답을 기대한다” 는 뜻
		req.headers.emplace("Accept", "application/json");

		// 2) 인프라 호출 (retry/timeout/SSL/에러표준화는 RestClient가 전담)
		auto r = rest_.perform(req);

		// 중복은 아닌지 유의
		if (std::holds_alternative<api::rest::RestError>(r))
		{
			return std::get<api::rest::RestError>(r);
		}

		const auto& resp = std::get <api::rest::HttpResponse>(r);

		// 3) status 해석 (의미 판단 - 재시도 판단과는 다름)
		if (!isSuccessStatus(resp.status))
		{
			return makeHttpStatusError(resp.status, "Upbit GET /v1/market/all", resp.body);
		}

		// 4) JSON -> DTO (파싱 예외를 RestError로 승격)
		nlohmann::json j;
		try		// JSON 데이터를 가져오기
		{
			j = nlohmann::json::parse(resp.body);
		}
		catch (const std::exception& ex)
		{
			return makeParseError(resp.status, "Upbit GET /v1/market/all", ex.what(), resp.body);
		}

		std::vector<api::upbit::dto::MarketDto> dtos;
		try		// JSON -> DTO 파싱
		{
			dtos = j.get < std::vector<api::upbit::dto::MarketDto>>();
		}
		catch (const std::exception& ex)
		{
			return makeParseError(resp.status, "Upbit GET /v1/market/all (DTO)", ex.what(), resp.body);
		}

		// 5) DTO -> Domain (필요한 필드만 선택 가능)
		std::vector<core::MarketInfo> out;
		out.reserve(dtos.size());
		for (const auto& d : dtos)
		{
			out.push_back(api::upbit::mappers::toDomain(d));
		}

		return out;
	}

	// 현재가 정보 부르기
	std::variant<std::vector<core::Ticker>, api::rest::RestError>
		UpbitPublicRestClient::getTickers(const std::vector<std::string>& markets) const
	{
		api::rest::HttpRequest req;
		req.host = "api.upbit.com";
		req.port = "443";
		req.method = api::rest::HttpMethod::Get;

		// GET /v1/ticker?markets=KRW-BTC,KRW-ETH
		req.target = std::string("/v1/ticker?markets=") + joinMarkets(markets);
		req.headers.emplace("Accept", "application/json");

		auto r = rest_.perform(req);
		if (std::holds_alternative<api::rest::RestError>(r))
			return std::get<api::rest::RestError>(r);

		const auto& resp = std::get<api::rest::HttpResponse>(r);
		if (!isSuccessStatus(resp.status))
			return makeHttpStatusError(resp.status, "Upbit GET /v1/ticker", resp.body);

		nlohmann::json j;
		try { j = nlohmann::json::parse(resp.body); }
		catch (const std::exception& ex)
		{
			return makeParseError(resp.status, "Upbit GET /v1/ticker", ex.what(), resp.body);
		}

		std::vector<api::upbit::dto::TickerDto> dtos;
		try { dtos = j.get<std::vector<api::upbit::dto::TickerDto>>(); }
		catch (const std::exception& ex)
		{
			return makeParseError(resp.status, "Upbit GET /v1/ticker (DTO)", ex.what(), resp.body);
		}

		std::vector<core::Ticker> out;
		out.reserve(dtos.size());
		for (const auto& d : dtos)
			out.push_back(api::upbit::mappers::toDomain(d));

		return out;
	}

	// 캔들 정보 부르기
	std::variant<std::vector<core::Candle>, api::rest::RestError>
		UpbitPublicRestClient::getCandlesMinutes(const std::string& market,
			int unit,
			int count,
			std::optional<std::string> to) const
	{
		api::rest::HttpRequest req;
		req.host = "api.upbit.com";
		req.port = "443";
		req.method = api::rest::HttpMethod::Get;

		// GET /v1/candles/minutes/{unit}?market=KRW-BTC&count=10&to=...
		std::ostringstream target;
		target << "/v1/candles/minutes/" << unit
			<< "?market=" << market
			<< "&count=" << count;
		if (to && !to->empty())
			target << "&to=" << *to;

		req.target = target.str();
		req.headers.emplace("Accept", "application/json");

		auto r = rest_.perform(req);
		if (std::holds_alternative<api::rest::RestError>(r))
			return std::get<api::rest::RestError>(r);

		const auto& resp = std::get<api::rest::HttpResponse>(r);
		if (!isSuccessStatus(resp.status))
			return makeHttpStatusError(resp.status, "Upbit GET /v1/candles/minutes/{unit}", resp.body);

		nlohmann::json j;
		try { j = nlohmann::json::parse(resp.body); }
		catch (const std::exception& ex)
		{
			return makeParseError(resp.status, "Upbit GET /v1/candles/minutes/{unit}", ex.what(), resp.body);
		}

		std::vector<api::upbit::dto::CandleDto_Minute> dtos;
		try { dtos = j.get<std::vector<api::upbit::dto::CandleDto_Minute>>(); }
		catch (const std::exception& ex)
		{
			return makeParseError(resp.status, "Upbit GET /v1/candles/minutes/{unit} (DTO)", ex.what(), resp.body);
		}

		std::vector<core::Candle> out;
		out.reserve(dtos.size());
		for (const auto& d : dtos)
			out.push_back(api::upbit::mappers::toDomain(d));

		return out;
	}

	// 호가창 정보 부르기
	std::variant<std::vector<core::Orderbook>, api::rest::RestError>
		UpbitPublicRestClient::getOrderbooks(const std::vector<std::string>& markets,
			std::optional<std::string> level,
			std::optional<int> count) const
	{
		api::rest::HttpRequest req;
		req.host = "api.upbit.com";
		req.port = "443";
		req.method = api::rest::HttpMethod::Get;

		std::ostringstream target;
		target << "/v1/orderbook?markets=" + joinMarkets(markets);

		// level: string (문서 기준)
		if (level && !level->empty())
			target << "&level=" << *level;

		// count: int
		if (count && *count > 0)
			target << "&count=" << *count;

		req.target = target.str();
		req.headers.emplace("Accept", "application/json");

		auto r = rest_.perform(req);
		if (std::holds_alternative<api::rest::RestError>(r))
			return std::get<api::rest::RestError>(r);

		const auto& resp = std::get<api::rest::HttpResponse>(r);
		if (resp.status < 200 || resp.status > 299)
			return makeHttpStatusError(resp.status, "Upbit GET /v1/orderbook", resp.body);

		nlohmann::json j;
		try { j = nlohmann::json::parse(resp.body); }
		catch (const std::exception& ex)
		{
			return makeParseError(resp.status, "Upbit GET /v1/orderbook", ex.what(), resp.body);
		}

		std::vector<api::upbit::dto::OrderbookDto> dtos;
		try { dtos = j.get<std::vector<api::upbit::dto::OrderbookDto>>(); }
		catch (const std::exception& ex)
		{
			return makeParseError(resp.status, "Upbit GET /v1/orderbook (DTO)", ex.what(), resp.body);
		}

		std::vector<core::Orderbook> out;
		out.reserve(dtos.size());
		for (auto& d : dtos)
			out.push_back(api::upbit::mappers::toDomain(d));

		return out;
	}
}