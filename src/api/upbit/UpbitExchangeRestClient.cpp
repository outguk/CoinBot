#include "UpbitExchangeRestClient.h"

#include <json.hpp>

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

        // query builder: "a=b&c=d"
        inline std::string buildQuery(std::initializer_list<std::pair<std::string_view, std::string_view>> items) {
            std::string q;
            for (auto& kv : items) {
                if (!q.empty()) q.push_back('&');
                q.append(kv.first);
                q.push_back('=');
                q.append(kv.second);
            }
            return q;
        }
    }

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
        const std::string query = buildQuery({ {"market", market} });

        api::rest::HttpRequest req;
        req.host = "api.upbit.com";
        req.port = "443";
        req.method = api::rest::HttpMethod::Get;
        req.target = std::string("/v1/orders/open?") + query;

        req.headers.emplace("Accept", "application/json");
        req.headers.emplace("Authorization", signer_.makeBearerToken(query));

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
            e.code = RestErrorCode::InvaildArgiment;
            e.http_status = 0;
            e.message = "cancelOrder requires uuid or identifier";
            return e;
        }

        // query 생성: uuid 우선, 없으면 identifier
        std::string query;
        if (uuid.has_value()) {
            query = buildQuery({ {"uuid", *uuid} });
        }
        else {
            query = buildQuery({ {"identifier", *identifier} });
        }

        api::rest::HttpRequest req;
        req.host = "api.upbit.com";
        req.port = "443";
        req.method = api::rest::HttpMethod::Delete;
        req.target = std::string("/v1/order?") + query;

        req.headers.emplace("Accept", "application/json");
        req.headers.emplace("Authorization", signer_.makeBearerToken(query));

        auto r = rest_.perform(req);
        if (std::holds_alternative<api::rest::RestError>(r))
            return std::get<api::rest::RestError>(r);

        const auto& resp = std::get<api::rest::HttpResponse>(r);
        if (!isSuccessStatus(resp.status))
            return makeHttpStatusError(resp.status, "Upbit DELETE /v1/order", resp.body);

        // 케이스 A에서는 “취소 성공 여부”만 있으면 충분
        return true;
    }

} // namespace api::rest
