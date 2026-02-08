#include "UpbitJwtSigner.h"

#include <json.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <array>
#include <sstream>
#include <iomanip>
#include <vector>

// Upbit 인증용 JWT(HS256) 토큰을 생성해서, HTTP 요청 헤더에 넣을 수 있게 해주는 역할

namespace {

    // 1) base64url 인코딩 함수
    // JWT는 base64가 아니라 "base64url"을 쓴다
    std::string base64UrlEncode(const unsigned char* data, std::size_t len) {
        // 간단 구현(프로덕션에선 util로 분리 추천)
        // OpenSSL EVP_EncodeBlock은 base64(+/ =) 형태라 URL-safe로 치환
        static const char* b64 =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((len + 2) / 3) * 4);

        // 3바이트씩 읽어서 4개의 base64 문자로 변환
        std::size_t i = 0;
        while (i + 3 <= len) {
            const unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
            out.push_back(b64[(v >> 18) & 63]);
            out.push_back(b64[(v >> 12) & 63]);
            out.push_back(b64[(v >> 6) & 63]);
            out.push_back(b64[v & 63]);
            i += 3;
        }

        // 남은 바이트가 1~2개면 padding '=' 포함하여 마무리
        if (i < len) {
            unsigned v = data[i] << 16;
            out.push_back(b64[(v >> 18) & 63]);
            if (i + 1 < len) {
                v |= data[i + 1] << 8;
                out.push_back(b64[(v >> 12) & 63]);
                out.push_back(b64[(v >> 6) & 63]);
                out.push_back('=');
            }
            else {
                out.push_back(b64[(v >> 12) & 63]);
                out.push_back('=');
                out.push_back('=');
            }
        }

        // URL-safe: + -> -, / -> _, padding 제거, URL-safe 치환
        for (auto& c : out) {
            if (c == '+') c = '-';
            else if (c == '/') c = '_';
        }

        // padding '=' 제거
        while (!out.empty() && out.back() == '=') out.pop_back();
        return out;
    }

    // 2) sha512 해시를 hex 문자열로 변환
    std::string sha512Hex(std::string_view s) {
        std::array<unsigned char, SHA512_DIGEST_LENGTH> hash{};
        SHA512(reinterpret_cast<const unsigned char*>(s.data()), s.size(), hash.data());

        std::ostringstream oss;
        for (auto b : hash) oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
        return oss.str();
    }

    std::string hmacSha256(const std::string& key, std::string_view msg) {
        unsigned int len = 0;
        unsigned char out[EVP_MAX_MD_SIZE]{};

        HMAC(EVP_sha256(),
            key.data(), (int)key.size(),
            reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
            out, &len);

        return std::string(reinterpret_cast<char*>(out), len);
    }

} // namespace

namespace api::auth {

    UpbitJwtSigner::UpbitJwtSigner(std::string accessKey, std::string secretKey)
        : access_(std::move(accessKey)), secret_(std::move(secretKey)) {
    }

    std::string UpbitJwtSigner::makeBearerToken(std::optional<std::string> query_string) const {
        using nlohmann::json;

        const json header = { {"alg","HS256"}, {"typ","JWT"} };

        // nonce는 UUID v4
        const auto nonce = boost::uuids::to_string(boost::uuids::random_generator()());

        json payload = {
            {"access_key", access_},
            {"nonce", nonce}
        };

        if (query_string && !query_string->empty()) {
            payload["query_hash"] = sha512Hex(*query_string);
            payload["query_hash_alg"] = "SHA512";
        }

        const auto headerDump = header.dump();
        const auto payloadDump = payload.dump();

        const auto encHeader = base64UrlEncode(reinterpret_cast<const unsigned char*>(headerDump.data()), headerDump.size());
        const auto encPayload = base64UrlEncode(reinterpret_cast<const unsigned char*>(payloadDump.data()), payloadDump.size());

        const std::string signingInput = encHeader + "." + encPayload;
        const auto sigBin = hmacSha256(secret_, signingInput);
        const auto encSig = base64UrlEncode(reinterpret_cast<const unsigned char*>(sigBin.data()), sigBin.size());

        return "Bearer " + signingInput + "." + encSig;
    }

} // namespace api::auth
