// util/JsonOptional.h
//
// nlohmann/json은 std::optional<T>의 from_json을 기본 제공하지 않는다.
// (3.12.0 미만 버전, 혹은 JSON_USE_IMPLICIT_CONVERSIONS=1 기본 설정 시)
//
// 이 헤더는 adl_serializer를 특수화해서 get_to(optional_field) 가 동작하도록 한다.
// - from_json: json → std::optional<T> 변환 (null이면 nullopt)
// - to_json:   std::optional<T> → json 변환 (nullopt이면 null)
//
// 사용법: std::optional 필드에 get_to를 쓰는 DTO 헤더에서 이 파일을 include한다.
#pragma once

#include <json.hpp>
#include <optional>

namespace nlohmann {

    template <typename T>
    struct adl_serializer<std::optional<T>> {

        // json 값이 null이면 nullopt, 아니면 T로 역직렬화해서 optional에 저장
        static void from_json(const json& j, std::optional<T>& opt) {
            opt = j.is_null() ? std::nullopt : std::optional<T>{ j.get<T>() };
        }

        // optional 값이 없으면 null, 있으면 담긴 값을 json으로 직렬화
        static void to_json(json& j, const std::optional<T>& opt) {
            j = opt.has_value() ? json(*opt) : json(nullptr);
        }
    };

} // namespace nlohmann
