#include "UpbitExchangeRestClient.h"

#include <algorithm>
#include <json.hpp>
#include <sstream>
#include <iomanip>

#include "util/Logger.h"

#include "api/upbit/dto/UpbitAssetOrderDtos.h"
#include "api/upbit/mappers/AccountMapper.h"
#include "api/upbit/mappers/OpenOrdersMapper.h"
#include "util/UrlCodec.h"

/*
* UpbitExchangeRestClient.cpp
*
* Private(인증 필요) Upbit REST 엔드포인트를 "도메인 관점" 함수로 제공한다.
*
* 책임 분리
* - RestClient   : 네트워크/TLS/timeout/retry + RestError 표준화
* - DTO          : Upbit JSON 구조를 그대로 표현(from_json)
* - Mapper       : DTO -> core::Account (도메인 오염 방지)
* - 이 파일      : (1) 요청 구성 (2) Authorization(JWT) 헤더 추가 (3) status/parse 오류를 RestError로 변환
*/

namespace api::rest {

    namespace {
        struct OrderPayloadFields
        {
            std::optional<std::string> price_field;
            std::optional<std::string> volume_field;
        };

        inline bool isSuccessStatus(int status) noexcept { return status >= 200 && status <= 299; }

        inline std::string bodySnippet(const std::string& body, std::size_t maxLen = 256) {
            if (body.size() <= maxLen) return body;
            return body.substr(0, maxLen);
        }

        inline RestError makeHttpStatusError(int status, const std::string& where, const std::string& body) {
            RestError e{};
            e.code = RestErrorCode::BadStatus;
            e.http_status = status;
            e.message = where + " failed, http = " + std::to_string(status)
                + ", body = " + bodySnippet(body);
            return e;
        }

        inline RestError makeParseError(int status, const std::string& where, const std::string& what, const std::string& body) {
            RestError e{};
            e.code = RestErrorCode::ParseError;
            e.http_status = status;
            e.message = where + " parse failed: " + what
                + ", body = " + bodySnippet(body);
            return e;
        }

        inline RestError makeInvalidArgumentError(std::string_view message)
        {
            RestError e{};
            e.code = RestErrorCode::InvalidArgument;
            e.http_status = 0;
            e.message = std::string(message);
            return e;
        }


        // --------------------------------------
        // encode, decode
        // --------------------------------------
        struct QueryStrings
        {
            std::string encoded; // HTTP 요청 URL에 붙일 용도 (URL 인코딩 적용)
            std::string hash; // JWT의 query_hash를 만들 입력 문자열 용도 (인코딩 결과를 다시 percent-decode 해서 “인코딩되지 않은 형태”로 맞춤)
        };

        inline void appendQueryParam(std::string& q, std::string_view key, std::string_view value)
        {
            if (!q.empty()) q.push_back('&');
            q.append(key);
            q.push_back('=');
            q.append(util::url::encodeComponent(value));
        }



        // --------------------------------------
        // Upbit JWT query_hash 계산용 문자열 표준화
        //
        // Upbit의 인증은 JWT 안에 query_hash(= 파라미터 문자열 SHA512)를 넣는다.
        // 이때 Upbit 예제는 대체로 아래 흐름을 따른다.
        //   - urlencode(params) 후 unquote(...) 또는
        //   - URLSearchParams(...).toString() 후 decodeURIComponent(...)
        //
        // 즉, "전송용으로 인코딩된 문자열"을 그대로 해시하지 않고,
        // "인코딩된 문자열을 다시 percent-decode한 문자열"을 해시 입력으로 사용한다.
        //
        // 이유:
        // - identifier처럼 ':' 등 특수문자가 있을 때,
        //   전송 문자열(예: %3A)과 서버가 재구성하는 파라미터 문자열(예: :)의
        //   표현이 달라질 수 있어 invalid_query_payload(401)가 발생할 수 있다.
        // --------------------------------------
        // GET/DELETE 호출 사이트용: braced-init({ {"key", sv} }) 패턴
        // 모든 원소가 lvalue(string_view 파라미터 또는 const string& 역참조)이므로 dangling 없음
        inline QueryStrings makeQueryStrings(
            std::initializer_list<std::pair<std::string_view, std::string_view>> items)
        {
            QueryStrings qs{};
            qs.encoded.reserve(128);
            for (const auto& kv : items)
                appendQueryParam(qs.encoded, kv.first, kv.second);
            qs.hash = util::url::decodePercent(qs.encoded);
            return qs;
        }

        // postOrder 전용: 조건부로 구성된 vector (소유된 string, dangling 없음)
        inline QueryStrings makeQueryStrings(
            const std::vector<std::pair<std::string, std::string>>& items)
        {
            QueryStrings qs{};
            qs.encoded.reserve(128);
            for (const auto& kv : items)
                appendQueryParam(qs.encoded, kv.first, kv.second);
            qs.hash = util::url::decodePercent(qs.encoded);
            return qs;
        }

        inline std::string toUpbitSide(core::OrderPosition p)
        {
            return (p == core::OrderPosition::BID) ? "bid" : "ask";
        }

        // Upbit ord_type mapping
        // - limit  : price + volume
        // - price  : market buy by KRW amount (BID only) => price=amount
        // - market : market sell by volume (ASK only) => volume
        inline std::string toUpbitOrdType(const core::OrderRequest& req)
        {
            if (req.type == core::OrderType::Limit) return "limit";

            // Market order: depend on size variant (AmountSize vs VolumeSize)
            if (std::holds_alternative<core::AmountSize>(req.size)) return "price"; // market buy by amount
            return "market"; // market sell by volume
        }

        // 매도 시 반올림을 내림으로 방지하고 8자리 소수를 기준으로 변경
        inline std::string formatDecimalFloor(double v, int decimals)
        {
            if (decimals < 0) decimals = 0;

            const double scale = std::pow(10.0, static_cast<double>(decimals));
            // "내림"으로 잔고 초과를 원천 차단 (반올림 금지)
            const double floored = std::floor(v * scale) / scale;

            std::ostringstream oss;
            oss.setf(std::ios::fixed);
            oss << std::setprecision(decimals) << floored;

            // 불필요한 trailing zero 제거(선택)
            std::string s = oss.str();
            if (s.find('.') != std::string::npos)
            {
                while (!s.empty() && s.back() == '0') s.pop_back();
                if (!s.empty() && s.back() == '.') s.pop_back();
            }
            // "0"이나 "" 방지
            if (s.empty()) s = "0";
            return s;
        }

        std::variant<OrderPayloadFields, RestError> buildOrderPayloadFields(
            const core::OrderRequest& request, std::string_view ord_type)
        {
            // 주문 타입별 검증과 문자열 포맷을 한 곳에 모아 postOrder 본문을 단순하게 유지한다.
            OrderPayloadFields fields;

            if (ord_type == "limit")
            {
                if (!request.price.has_value())
                {
                    return makeInvalidArgumentError("postOrder: limit order requires price");
                }

                if (!std::holds_alternative<core::VolumeSize>(request.size))
                {
                    return makeInvalidArgumentError("postOrder: limit order requires VolumeSize");
                }

                const double volume = std::get<core::VolumeSize>(request.size).value;
                if (volume <= 0.0)
                {
                    return makeInvalidArgumentError("postOrder: limit volume must be > 0");
                }

                fields.price_field = formatDecimalFloor(*request.price, 0);
                fields.volume_field = formatDecimalFloor(volume, 8);
                return fields;
            }

            if (ord_type == "price")
            {
                if (!std::holds_alternative<core::AmountSize>(request.size))
                {
                    return makeInvalidArgumentError("postOrder: ord_type=price requires AmountSize");
                }

                const double amount = std::get<core::AmountSize>(request.size).value;
                if (amount <= 0.0)
                {
                    return makeInvalidArgumentError("postOrder: amount must be > 0");
                }

                fields.price_field = formatDecimalFloor(amount, 0);
                return fields;
            }

            if (!std::holds_alternative<core::VolumeSize>(request.size))
            {
                return makeInvalidArgumentError("postOrder: ord_type=market requires VolumeSize");
            }

            const double volume = std::get<core::VolumeSize>(request.size).value;
            if (volume <= 0.0)
            {
                return makeInvalidArgumentError("postOrder: volume must be > 0");
            }

            fields.volume_field = formatDecimalFloor(volume, 8);
            return fields;
        }
    } // namespace

    UpbitExchangeRestClient::UpbitExchangeRestClient(RestClient& rest, api::auth::UpbitJwtSigner signer)
        : rest_(rest), signer_(std::move(signer)) {
    }

    std::variant<core::Account, api::rest::RestError> UpbitExchangeRestClient::getMyAccount() {
        // Upbit: GET /v1/accounts (query 없음)
        api::rest::HttpRequest req;
        req.host = "api.upbit.com";
        req.port = "443";
        req.method = api::rest::HttpMethod::Get;
        req.target = "/v1/accounts";

        // Accept는 명시하는 편이 안전
        req.headers.emplace("Accept", "application/json");

        // 인증 헤더: Authorization: Bearer <jwt>
        // query_string이 없으므로 nullopt
        req.headers.emplace("Authorization", signer_.makeBearerToken(std::nullopt));

        // 1) 인프라 호출
        auto r = rest_.perform(req);
        if (std::holds_alternative<api::rest::RestError>(r))
            return std::get<api::rest::RestError>(r);

        // 2) status 확인
        const auto& resp = std::get<api::rest::HttpResponse>(r);
        if (!isSuccessStatus(resp.status))
            return makeHttpStatusError(resp.status, "Upbit GET /v1/accounts", resp.body);

        // 3) JSON -> DTO
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(resp.body);
        }
        catch (const std::exception& ex) {
            return makeParseError(resp.status, "Upbit GET /v1/accounts", ex.what(), resp.body);
        }

        api::upbit::dto::AccountsDto dto;
        try {
            // DTO는 Upbit JSON 구조를 그대로 담는다.
            // (대부분 /v1/accounts 는 "배열"을 반환하므로 AccountsDto 내부에서 그 배열을 감싼 형태로 설계했을 것)
            dto = j.get<api::upbit::dto::AccountsDto>();
        }
        catch (const std::exception& ex) {
            return makeParseError(resp.status, "Upbit GET /v1/accounts (DTO)", ex.what(), resp.body);
        }

        // 4) DTO -> Domain
        try {
            return api::upbit::mappers::toDomain(dto);
        }
        catch (const std::exception& ex) {
            // mapper에서 예외를 던질 일은 적게(= stod 실패 등을 내부에서 처리) 설계하는 게 좋지만,
            // 안전망으로 한 번 감싼다.
            return makeParseError(resp.status, "Upbit GET /v1/accounts (Mapper)", ex.what(), resp.body);
        }
    }

    // 추가: GET /v1/orders/open?market=KRW-BTC
    std::variant<std::vector<core::Order>, api::rest::RestError>
        UpbitExchangeRestClient::getOpenOrders(std::string_view market)
    {
        // query string (JWT에 포함되어야 하므로 문자열로 만든다)
        // HTTP 전송용(encoded)과 JWT 해시 입력(hash)을 분리해서 만든다
        const auto qs = makeQueryStrings({ {"market", market} });

        api::rest::HttpRequest req;
        req.host = "api.upbit.com";
        req.port = "443";
        req.method = api::rest::HttpMethod::Get;
        req.target = std::string("/v1/orders/open?") + qs.encoded;

        req.headers.emplace("Accept", "application/json");
        req.headers.emplace("Authorization", signer_.makeBearerToken(qs.hash));

        auto r = rest_.perform(req);
        if (std::holds_alternative<api::rest::RestError>(r))
            return std::get<api::rest::RestError>(r);

        const auto& resp = std::get<api::rest::HttpResponse>(r);
        if (!isSuccessStatus(resp.status))
            return makeHttpStatusError(resp.status, "Upbit GET /v1/orders/open", resp.body);

        nlohmann::json j;
        try {
            j = nlohmann::json::parse(resp.body);
        }
        catch (const std::exception& ex) {
            return makeParseError(resp.status, "Upbit GET /v1/orders/open", ex.what(), resp.body);
        }

        api::upbit::dto::WaitOrdersResponseDto dtoList;
        try {
            // 응답은 array → WaitOrdersResponseDto.from_json이 array를 vector로 읽는다.
            dtoList = j.get<api::upbit::dto::WaitOrdersResponseDto>();
        }
        catch (const std::exception& ex) {
            return makeParseError(resp.status, "Upbit GET /v1/orders/open (DTO)", ex.what(), resp.body);
        }

        // DTO -> core::Order (도메인 오염 방지)
        return api::upbit::mapper::toDomain(dtoList);
    }

    // GET /v1/order?uuid=...
    // 단건 주문 조회 — reconnect 복구 시 pending 주문 상태 확인용
    // /v1/orders/open DTO 재사용 대신 /v1/order 전용 DTO로 파싱한다.
    std::variant<core::Order, api::rest::RestError>
        UpbitExchangeRestClient::getOrder(std::string_view order_uuid)
    {
        const auto qs = makeQueryStrings({ {"uuid", order_uuid} });

        api::rest::HttpRequest req;
        req.host = "api.upbit.com";
        req.port = "443";
        req.method = api::rest::HttpMethod::Get;
        req.target = std::string("/v1/order?") + qs.encoded;

        req.headers.emplace("Accept", "application/json");
        req.headers.emplace("Authorization", signer_.makeBearerToken(qs.hash));

        auto r = rest_.perform(req);
        if (std::holds_alternative<api::rest::RestError>(r))
            return std::get<api::rest::RestError>(r);

        const auto& resp = std::get<api::rest::HttpResponse>(r);
        if (!isSuccessStatus(resp.status))
            return makeHttpStatusError(resp.status, "Upbit GET /v1/order", resp.body);

        nlohmann::json j;
        try {
            j = nlohmann::json::parse(resp.body);
        }
        catch (const std::exception& ex) {
            return makeParseError(resp.status, "Upbit GET /v1/order", ex.what(), resp.body);
        }

        api::upbit::dto::OrderResponseDto dto;
        try {
            dto = j.get<api::upbit::dto::OrderResponseDto>();
        }
        catch (const std::exception& ex) {
            return makeParseError(resp.status, "Upbit GET /v1/order (DTO)", ex.what(), resp.body);
        }

        return api::upbit::mapper::toDomain(dto);
    }

    // DELETE /v1/order?uuid=... 또는 identifier=...
    std::variant<bool, api::rest::RestError>
        UpbitExchangeRestClient::cancelOrder(const std::optional<std::string>& order_uuid,
            const std::optional<std::string>& identifier)
    {
        // Upbit: uuid 또는 identifier 중 하나는 필수
        if (!order_uuid.has_value() && !identifier.has_value()) {
            return makeInvalidArgumentError("cancelOrder requires order_uuid or identifier");
        }

        // query 생성: uuid 우선, 없으면 identifier
        QueryStrings qs;
        if (order_uuid.has_value()) {
            qs = makeQueryStrings({ {"uuid", *order_uuid} });
        }
        else {
            qs = makeQueryStrings({ {"identifier", *identifier} });
        }
        api::rest::HttpRequest req;
        req.host = "api.upbit.com";
        req.port = "443";
        req.method = api::rest::HttpMethod::Delete;
        // 전송은 encoded 문자열로 URL을 만든다
        req.target = std::string("/v1/order?") + qs.encoded;

        req.headers.emplace("Accept", "application/json");
        req.headers.emplace("Authorization", signer_.makeBearerToken(qs.hash));

        auto r = rest_.perform(req);
        if (std::holds_alternative<api::rest::RestError>(r))
            return std::get<api::rest::RestError>(r);

        const auto& resp = std::get<api::rest::HttpResponse>(r);
        if (!isSuccessStatus(resp.status))
            return makeHttpStatusError(resp.status, "Upbit DELETE /v1/order", resp.body);

        // 케이스 A에서는 “취소 성공 여부”만 있으면 충분
        return true;
    }

    // POST /v1/orders
    // - OrderRequest(domain) -> Upbit 주문 생성 요청으로 변환
    // - JWT는 query_hash가 필요하다.
    //   전송은 encoded 문자열로, query_hash는 percent-decode된 문자열로 계산한다.
    std::variant<std::string, api::rest::RestError>
        UpbitExchangeRestClient::postOrder(const core::OrderRequest& reqIn)
    {
        // 주문 파라미터를 먼저 남겨두면 실거래 실패 시 어떤 요청이 나갔는지 추적하기 쉽다.
        util::Logger::instance().debug("[REST][postOrder] ENTER market=", reqIn.market,
                                       " type=", static_cast<int>(reqIn.type));

        // 0) 입력 검증 (실거래에서 가장 취약한 부분이 입력 값 오류로 인한 BadRequest)
        if (reqIn.market.empty())
        {
            return makeInvalidArgumentError("postOrder: market is empty");
        }

        // 1) 주문 필드 계산
        const std::string side = toUpbitSide(reqIn.position);
        const std::string ordType = toUpbitOrdType(reqIn);
        // 거래소 호출 전에 필수 필드를 검증해 Bad Request를 조기에 차단한다.
        const auto payload_fields_result = buildOrderPayloadFields(reqIn, ordType);
        if (std::holds_alternative<RestError>(payload_fields_result))
        {
            return std::get<RestError>(payload_fields_result);
        }
        const OrderPayloadFields payload_fields =
            std::get<OrderPayloadFields>(payload_fields_result);

        // 3) JSON body 구성 (현재 Upbit 공식 문서 기준: 주문 생성은 JSON body 사용)
        nlohmann::json jsonBody;
        jsonBody["market"] = reqIn.market;
        jsonBody["side"] = side;
        jsonBody["ord_type"] = ordType;

        if (!reqIn.identifier.empty())
            jsonBody["identifier"] = reqIn.identifier;

        if (payload_fields.price_field.has_value())
            jsonBody["price"] = *payload_fields.price_field;
        if (payload_fields.volume_field.has_value())
            jsonBody["volume"] = *payload_fields.volume_field;

        // query_hash 입력 문자열은 body 파라미터와 완전히 동일해야 한다.
        std::vector<std::pair<std::string, std::string>> params;
        params.reserve(6);
        params.emplace_back("market", reqIn.market);
        params.emplace_back("side", side);
        params.emplace_back("ord_type", ordType);
        if (!reqIn.identifier.empty())
            params.emplace_back("identifier", reqIn.identifier);
        if (payload_fields.price_field.has_value())
            params.emplace_back("price", *payload_fields.price_field);
        if (payload_fields.volume_field.has_value())
            params.emplace_back("volume", *payload_fields.volume_field);

		// 파라미터 순서 고정: query_hash 계산을 위해 키 기준으로 정렬
        auto hash_params = params;
        std::sort(hash_params.begin(), hash_params.end(),
                  [](const auto& lhs, const auto& rhs) {
                      return lhs.first < rhs.first;
                  });

        const auto qs = makeQueryStrings(hash_params);

        // 4) HTTP request
        api::rest::HttpRequest http;
        http.host = "api.upbit.com";
        http.port = "443";
        http.method = api::rest::HttpMethod::Post;
        http.target = "/v1/orders";

        http.headers.emplace("Accept", "application/json");
        http.headers.emplace("Content-Type", "application/json; charset=utf-8");
        // Authorization은 body와 동일한 파라미터 집합으로 계산한 query_hash를 사용한다.
        http.headers.emplace("Authorization", signer_.makeBearerToken(qs.hash));

        // body는 JSON으로 전송
        http.body = jsonBody.dump();

        // 요청이 어떤 ord_type/size로 만들어졌는지 확인
        util::Logger::instance().debug("[REST][postOrder] REQ target=", http.target,
                                       " body=", http.body);

        auto r = rest_.perform(http);

        if (std::holds_alternative<api::rest::RestError>(r))
        {
            const auto& e = std::get<api::rest::RestError>(r);
            util::Logger::instance().error("[REST][postOrder] PERFORM FAIL restCode=",
                                           static_cast<int>(e.code),
                                           " http=", e.http_status,
                                           " msg=", e.message);
            return std::get<api::rest::RestError>(r);
        }


        const auto& resp = std::get<api::rest::HttpResponse>(r);
        if (!isSuccessStatus(resp.status))
        {
            util::Logger::instance().error("[REST][postOrder] HTTP REJECT status=", resp.status,
                                           " body=", resp.body.substr(0, 400));
            return makeHttpStatusError(resp.status, "Upbit POST /v1/orders", resp.body);
        }


        // 4) 최소한의 필드만 파싱해서 domain order로 반환
        // - engine/order store에서는 id(order_uuid), market, status 정도만 있어도 추적이 가능
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(resp.body);
        }
        catch (const std::exception& ex) {
            return makeParseError(resp.status, "Upbit POST /v1/orders", ex.what(), resp.body);
        }

        std::string order_uuid;
        try {
            order_uuid = j.value("uuid", "");
        }
        catch (const std::exception& ex) {
            return makeParseError(resp.status, "Upbit POST /v1/orders (map order_uuid)", ex.what(), resp.body);
        }

        if (order_uuid.empty())
        {
            return makeParseError(resp.status, "Upbit POST /v1/orders (missing order_uuid)", "order_uuid is empty", resp.body);
        }

        return order_uuid;
    }

} // namespace api::rest
