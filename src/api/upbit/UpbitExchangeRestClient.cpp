#include "UpbitExchangeRestClient.h"

#include <json.hpp>
#include <sstream>
#include <iomanip>
#include <iostream>

#include "api/upbit/dto/UpbitAssetOrderDtos.h"
#include "api/upbit/mappers/AccountMapper.h"
#include "api/upbit/mappers/OpenOrdersMapper.h"

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


        // --------------------------------------
        // encode, decode
        // --------------------------------------
        struct QueryStrings
        {
            std::string encoded; // HTTP 요청 URL에 붙일 용도 (URL 인코딩 적용)
            std::string hash; // JWT의 query_hash를 만들 입력 문자열 용도 (인코딩 결과를 다시 percent-decode 해서 “인코딩되지 않은 형태”로 맞춤)
        };

        inline std::string urlEncode(std::string_view s)
        {
            std::ostringstream oss;
            oss << std::uppercase << std::hex;

            for (unsigned char c : std::string(s))
            {
                const bool unreserved =
                    (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~';

                if (unreserved)
                {
                    oss << static_cast<char>(c);
                }
                else
                {
                    oss << '%'
                        << std::setw(2) << std::setfill('0')
                        << static_cast<int>(c);
                }
            }
            return oss.str();
        }

        inline void appendQueryParam(std::string& q, std::string_view key, std::string_view value)
        {
            if (!q.empty()) q.push_back('&');
            q.append(key);
            q.push_back('=');
            q.append(urlEncode(value));
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
        inline int fromHex_(char c) noexcept
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            return -1;
        }

        inline std::string percentDecodeForHash(std::string_view s)
        {
            // 주의: '+' -> 공백 변환은 하지 않는다.
            //       (우리 urlEncode는 공백도 %20으로 보내며, '+'를 만들지 않는다)
            std::string out;
            out.reserve(s.size());

            for (std::size_t i = 0; i < s.size(); ++i)
            {
                const char c = s[i];
                if (c == '%' && i + 2 < s.size())
                {
                    const int hi = fromHex_(s[i + 1]);
                    const int lo = fromHex_(s[i + 2]);
                    if (hi >= 0 && lo >= 0)
                    {
                        out.push_back(static_cast<char>((hi << 4) | lo));
                        i += 2;
                        continue;
                    }
                }
                out.push_back(c);
            }

            return out;
        }

        inline QueryStrings makeQueryStrings(
            std::initializer_list<std::pair<std::string_view, std::string_view>> items)
        {
            QueryStrings qs{};
            qs.encoded.reserve(128);

            for (const auto& kv : items)
            {
                appendQueryParam(qs.encoded, kv.first, kv.second);
            }

            // query_hash는 "디코딩된 형태"를 입력으로 사용
            qs.hash = percentDecodeForHash(qs.encoded);
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

    // DELETE /v1/order?uuid=... 또는 identifier=...
    std::variant<bool, api::rest::RestError>
        UpbitExchangeRestClient::cancelOrder(const std::optional<std::string>& uuid,
            const std::optional<std::string>& identifier)
    {
        // Upbit: uuid 또는 identifier 중 하나는 필수
        if (!uuid.has_value() && !identifier.has_value()) {
            RestError e{};
            e.code = RestErrorCode::InvalidArgument;
            e.http_status = 0;
            e.message = "cancelOrder requires uuid or identifier";
            return e;
        }

        // query 생성: uuid 우선, 없으면 identifier
        QueryStrings qs;
        if (uuid.has_value()) {
            qs = makeQueryStrings({ {"uuid", *uuid} });
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
        // 케이스 C: 실제로 POST가 아예 실행되지 않았는지 확인(이 로그 나타나면 실행된거
        std::cout << "[REST][postOrder] ENTER market=" << reqIn.market
            << " type=" << static_cast<int>(reqIn.type)
            //<< " ident=" << reqIn.identifier
            << "\n";

        // 0) 입력 검증 (실거래에서 가장 취약한 부분이 입력 값 오류로 인한 BadRequest)
        if (reqIn.market.empty())
        {
            RestError e{};
            e.code = RestErrorCode::InvalidArgument;
            e.http_status = 0;
            e.message = "postOrder: market is empty";
            return e;
        }

        // 1) Upbit 파라미터 생성 (항상 같은 순서로 넣어서 query_hash가 같은 값으로 계산되게 함)
        const std::string side = toUpbitSide(reqIn.position);
        const std::string ordType = toUpbitOrdType(reqIn);

        std::string q; // key=value&key2=value2 ... (URL-encoded values)
        q.reserve(256);

        appendQueryParam(q, "market", reqIn.market);
        appendQueryParam(q, "side", side);
        appendQueryParam(q, "ord_type", ordType);

        // identifier(client_order_id)는 선택. 사용 시 다음 것들을 가능하게 해준다.
        // - WS 내 주문/체결 이벤트를 전략과 매칭
        // - 재시작 후 복구(StartupRecovery) 시 보조 키로 사용
        if (!reqIn.identifier.empty())
            appendQueryParam(q, "identifier", reqIn.identifier);

        // 2) 주문 타입 별 필수 필드
        // - limit  : price + volume
        // - price  : (market buy) price=KRW amount, volume omitted
        // - market : (market sell) volume=coin amount, price omitted
        if (ordType == "limit")
        {
            if (!reqIn.price.has_value())
            {
                RestError e{};
                e.code = RestErrorCode::InvalidArgument;
                e.http_status = 0;
                e.message = "postOrder: limit order requires price";
                return e;
            }

            if (!std::holds_alternative<core::VolumeSize>(reqIn.size))
            {
                RestError e{};
                e.code = RestErrorCode::InvalidArgument;
                e.http_status = 0;
                e.message = "postOrder: limit order requires VolumeSize";
                return e;
            }

            const auto vol = std::get<core::VolumeSize>(reqIn.size).value;
            if (vol <= 0.0)
            {
                RestError e{};
                e.code = RestErrorCode::InvalidArgument;
                e.http_status = 0;
                e.message = "postOrder: limit volume must be > 0";
                return e;
            }

            appendQueryParam(q, "price", formatDecimalFloor(*reqIn.price, 0));
            appendQueryParam(q, "volume", formatDecimalFloor(vol, 8));
        }
        else if (ordType == "price")
        {
            // market buy by KRW amount
            if (!std::holds_alternative<core::AmountSize>(reqIn.size))
            {
                RestError e{};
                e.code = RestErrorCode::InvalidArgument;
                e.http_status = 0;
                e.message = "postOrder: ord_type=price requires AmountSize";
                return e;
            }

            const auto amount = std::get<core::AmountSize>(reqIn.size).value;
            if (amount <= 0.0)
            {
                RestError e{};
                e.code = RestErrorCode::InvalidArgument;
                e.http_status = 0;
                e.message = "postOrder: amount must be > 0";
                return e;
            }

            appendQueryParam(q, "price", formatDecimalFloor(amount, 0));
        }
        else // ordType == "market"
        {
            // market sell by volume
            if (!std::holds_alternative<core::VolumeSize>(reqIn.size))
            {
                RestError e{};
                e.code = RestErrorCode::InvalidArgument;
                e.http_status = 0;
                e.message = "postOrder: ord_type=market requires VolumeSize";
                return e;
            }

            const auto vol = std::get<core::VolumeSize>(reqIn.size).value;
            if (vol <= 0.0)
            {
                RestError e{};
                e.code = RestErrorCode::InvalidArgument;
                e.http_status = 0;
                e.message = "postOrder: volume must be > 0";
                return e;
            }

            appendQueryParam(q, "volume", formatDecimalFloor(vol, 8));
        }

        // 3) HTTP request
        api::rest::HttpRequest http;
        http.host = "api.upbit.com";
        http.port = "443";
        http.method = api::rest::HttpMethod::Post;
        http.target = "/v1/orders";

        http.headers.emplace("Accept", "application/json");
        http.headers.emplace("Content-Type", "application/x-www-form-urlencoded");
        // JWT query_hash는 "percent-decode된 query_string"을 입력으로 사용해야 한다.
        // (Upbit 예제의 unquote(urlencode(params)) / decodeURIComponent(...) 흐름)
        const std::string q_hash = percentDecodeForHash(q);
        http.headers.emplace("Authorization", signer_.makeBearerToken(q_hash));

        // Upbit는 POST 파라미터를 body로 받는다.
        // (query_hash가 body 파라미터와 동일해야 하므로, q를 그대로 body로 넣는다.)
        http.body = q;

        // 요청이 어떤 ord_type/size로 만들어졌는지 확인
        std::cout << "[UpbitExchangeRestClient][postOrder] REQ target=" << http.target
            << " body=" << http.body
            << "\n";

        auto r = rest_.perform(http);

        if (std::holds_alternative<api::rest::RestError>(r))
        {
            const auto& e = std::get<api::rest::RestError>(r);
            std::cout << "[UpbitExchangeRestClient][postOrder] PERFORM FAIL restCode="
                << static_cast<int>(e.code)
                << " http=" << e.http_status
                << " msg=" << e.message
                << "\n";

            return std::get<api::rest::RestError>(r);
        }


        const auto& resp = std::get<api::rest::HttpResponse>(r);
        if (!isSuccessStatus(resp.status))
        {
            std::cout << "[UpbitExchangeRestClient][postOrder] HTTP REJECT status="
                << resp.status
                << " body=" << resp.body.substr(0, 400)
                << "\n";

            return makeHttpStatusError(resp.status, "Upbit POST /v1/orders", resp.body);
        }


        // 4) 최소한의 필드만 파싱해서 domain order로 반환
        // - engine/order store에서는 id(uuid), market, status 정도만 있어도 추적이 가능
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(resp.body);
        }
        catch (const std::exception& ex) {
            return makeParseError(resp.status, "Upbit POST /v1/orders", ex.what(), resp.body);
        }

        std::string uuid;
        try {
            uuid = j.value("uuid", "");
        }
        catch (const std::exception& ex) {
            return makeParseError(resp.status, "Upbit POST /v1/orders (map uuid)", ex.what(), resp.body);
        }

        if (uuid.empty())
        {
            return makeParseError(resp.status, "Upbit POST /v1/orders (missing uuid)", "uuid is empty", resp.body);
        }

        return uuid;
    }

} // namespace api::rest
