#pragma once

#include <string>
#include <string_view>
#include <sstream>
#include <iomanip>

namespace util::url {

    namespace detail {
        inline int fromHex(char c) noexcept
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            return -1;
        }
    }

    // RFC 3986 percent-encoding (unreserved chars: ALPHA / DIGIT / - _ . ~)
    inline std::string encodeComponent(std::string_view s)
    {
        std::ostringstream oss;
        oss << std::uppercase << std::hex;
        for (unsigned char c : std::string(s))
        {
            const bool unreserved =
                (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~';
            if (unreserved)
                oss << static_cast<char>(c);
            else
                oss << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
        return oss.str();
    }

    // percent-decode
    // 주의: '+' -> 공백 변환은 하지 않는다.
    // (encodeComponent는 공백을 %20으로 인코딩하며 '+'를 생성하지 않는다)
    inline std::string decodePercent(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            const char c = s[i];
            if (c == '%' && i + 2 < s.size())
            {
                const int hi = detail::fromHex(s[i + 1]);
                const int lo = detail::fromHex(s[i + 2]);
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

} // namespace util::url
