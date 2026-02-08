#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <charconv>
#include <cstdlib>
#include <cerrno>

#include "core/domain/Order.h"
#include "api/upbit/dto/UpbitAssetOrderDtos.h"

namespace api::upbit::mapper
{
	// -----------------------------
	// string -> double (no-throw)
	// -----------------------------
	inline double parseDoubleOr(std::string_view sv, double fallback = 0.0) noexcept
	{
		if (sv.empty()) return fallback;

#if defined(__cpp_lib_to_chars) && (__cpp_lib_to_chars >= 201611L)
		// 일부 표준 라이브러리에서 double from_chars 지원이 미완일 수 있어 폴백을 둔다.
		double out{};
		auto res = std::from_chars(sv.data(), sv.data() + sv.size(), out);
		if (res.ec == std::errc() && res.ptr == sv.data() + sv.size())
			return out;
#endif
		std::string tmp(sv);
		char* end = nullptr;
		errno = 0;
		const double v = std::strtod(tmp.c_str(), &end);
		if (errno != 0) return fallback;
		if (end == tmp.c_str()) return fallback;
		return v;
	}

	inline std::optional<double> parseOptDouble(const std::optional<std::string>& s) noexcept
	{
		if (!s.has_value()) return std::nullopt;
		return parseDoubleOr(*s, 0.0);
	}

	// -----------------------------
	// enum mapping (DTO -> domain)
	// -----------------------------
	inline core::OrderPosition toDomainPosition(api::upbit::dto::Side s) noexcept
	{
		using S = api::upbit::dto::Side;
		switch (s)
		{
		case S::bid: return core::OrderPosition::BID;
		case S::ask: return core::OrderPosition::ASK;
		default:     return core::OrderPosition::BID;
		}
	}

	inline core::OrderType toDomainOrderType(std::string_view ord_type) noexcept
	{
		// 도메인 최소 구분: limit vs (그 외) market
		if (ord_type == "limit") return core::OrderType::Limit;
		return core::OrderType::Market;
	}

	inline core::OrderStatus toDomainStatus(api::upbit::dto::OrdState st) noexcept
	{
		using St = api::upbit::dto::OrdState;
		switch (st)
		{
		case St::wait:   return core::OrderStatus::Open;     // 미체결 대기
		case St::watch:  return core::OrderStatus::Pending;  // 예약대기
		case St::done:   return core::OrderStatus::Filled;
		case St::cancel: return core::OrderStatus::Canceled;
		default:         return core::OrderStatus::Pending;
		}
	}

	// -----------------------------
	// WaitOrderResponseDto -> core::Order
	// -----------------------------
	inline core::Order toDomain(const api::upbit::dto::WaitOrderResponseDto& dto) noexcept
	{
		core::Order o;

		o.market = dto.market;
		o.id = dto.uuid;
		o.identifier = dto.identifier;

		o.position = toDomainPosition(dto.side);
		o.type = toDomainOrderType(dto.ord_type);
		o.status = toDomainStatus(dto.state);

		o.created_at = dto.created_at;

		// 요청값
		o.price = parseOptDouble(dto.price);
		o.volume = parseOptDouble(dto.volume);

		// 부분 체결/잔여
		o.executed_volume = parseDoubleOr(dto.executed_volume, 0.0);
		o.remaining_volume = parseDoubleOr(dto.remaining_volume, 0.0);
		o.trades_count = dto.trades_count;

		// 누적 체결 금액
		o.executed_funds = parseDoubleOr(dto.executed_funds, 0.0);

		// 수수료/잠금
		o.reserved_fee = parseDoubleOr(dto.reserved_fee, 0.0);
		o.remaining_fee = parseDoubleOr(dto.remaining_fee, 0.0);
		o.paid_fee = parseDoubleOr(dto.paid_fee, 0.0);
		o.locked = parseDoubleOr(dto.locked, 0.0);

		return o;
	}

	inline std::vector<core::Order> toDomain(const api::upbit::dto::WaitOrdersResponseDto& dtoList)
	{
		std::vector<core::Order> out;
		out.reserve(dtoList.wait_order_list.size());
		for (const auto& dto : dtoList.wait_order_list)
			out.push_back(toDomain(dto));
		return out;
	}
}
