#pragma once
#include <string>
#include <string_view>
#include <utility>

#include "api/upbit/dto/UpbitAssetOrderDtos.h"
#include "core/domain/Account.h"

namespace api::upbit::mappers
{
	namespace
	{
		double toDoubleOrZero(std::string_view s) noexcept
		{
			// 잔고 조회 한 항목의 숫자 형식이 깨져도 전체 계좌 조회까지 실패시키지 않는다.
			try {
				return std::stod(std::string(s));
			}
			catch (...) {
				return 0.0;
			}
		}
	}

	inline core::Account toDomain(const api::upbit::dto::AccountsDto& dto)
	{
		core::Account a{};
		a.positions.reserve(dto.account_list.size());

		for (const auto& row : dto.account_list)
		{
			const auto bal = toDoubleOrZero(row.balance);
			const auto lck = toDoubleOrZero(row.locked);
			const auto avg = toDoubleOrZero(row.avg_buy_price);

			if (row.currency == "KRW")
			{
				// KRW는 상위 로직이 자주 직접 읽으므로 요약 필드에도 별도로 보관한다.
				a.krw_free = bal;
				a.krw_locked = lck;
			}

			core::Position p{};
			p.currency = row.currency;
			p.free = bal;
			p.avg_buy_price = avg;
			p.unit_currency = row.unit_currency;

			a.positions.push_back(std::move(p));
		}
		return a;
	}
}

