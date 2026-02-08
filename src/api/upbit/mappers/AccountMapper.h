#pragma once
#include <core/domain/Account.h>
#include <api/upbit/dto/UpbitAssetOrderDtos.h>

namespace {

	double toDoubleOrZero(std::string_view s) noexcept 
	{
		double out = 0.0;
		try {
			return std::stod(std::string(s));
		}
		catch (...) {
			return 0.0;
		}
	}
} // namespace

namespace api::upbit::mappers
{

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
				a.krw_free = bal;
				a.krw_locked = lck;
			}

			core::Position p{};
			p.currency = row.currency;
			// 이 부분도 오류시 0으로 하는게 맞는지 검토 필요
			p.free = toDoubleOrZero(row.balance);
			p.avg_buy_price = toDoubleOrZero(row.avg_buy_price);
			p.unit_currency = row.unit_currency;

			a.positions.push_back(std::move(p));
		}
		return a;
	}
}

