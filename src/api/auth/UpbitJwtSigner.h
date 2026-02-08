#pragma once
#include <optional>
#include <string>

namespace api::auth {

    // Upbit REST 인증 토큰 생성기(HS256 JWT)
    class UpbitJwtSigner {
    public:
        UpbitJwtSigner(std::string accessKey, std::string secretKey);

        // query_string: "market=KRW-BTC&state=wait" 같은 원문 (정확히 이 문자열로 sha512)
        // 없으면 query_hash 없이 토큰 생성
        std::string makeBearerToken(std::optional<std::string> query_string = std::nullopt) const;

    private:
        std::string access_;
        std::string secret_;
    };

}