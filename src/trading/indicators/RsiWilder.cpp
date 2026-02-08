#include "RsiWilder.h"

namespace trading::indicators
{
	// length로 리셋
	void RsiWilder::reset(std::size_t length)
	{
		length_ = length;
		clear();
	}

	void RsiWilder::clear() noexcept
	{
		prev_price_.reset();

		seed_count_ = 0;
		seed_gain_sum_ = 0.0;
		seed_loss_sum_ = 0.0;

		avg_gain_ = 0.0;
		avg_loss_ = 0.0;

		last_.ready = false;
		last_.v = 0.0;
	}

	trading::Value<double> RsiWilder::update(double close_price)
	{
		trading::Value<double> out{};
		out.ready = false;
		out.v = 0.0;

		// length==0이면 지표 비활성 상태(아무것도 계산하지 않음)
		if (length_ == 0)
			return out;

		// 첫 가격은 변화량(delta)을 만들 수 없으니 prev만 세팅하고 종료
		if (!prev_price_.has_value())
		{
			prev_price_ = close_price;
			return out;
		}

		// 변화량 계산
		const double delta = close_price - *prev_price_;
		prev_price_ = close_price;

		// gain/loss 분리 (loss는 절대값로 저장)
		const double gain = (delta > 0.0) ? delta : 0.0;
		const double loss = (delta < 0.0) ? (-delta) : 0.0;

		// 1) seed 구간: length개의 변화량을 모아 초기 평균 gain/loss를 만든다.
		if (seed_count_ < length_)
		{
			seed_gain_sum_ += gain;
			seed_loss_sum_ += loss;
			++seed_count_;

			// seed가 아직 덜 찼으면 ready=false
			if (seed_count_ < length_)
				return out;

			// seed 완료: 초기 avgGain/avgLoss 확정
			const double n = static_cast<double>(length_);
			avg_gain_ = seed_gain_sum_ / n;
			avg_loss_ = seed_loss_sum_ / n;

			out.ready = true;
			out.v = computeRsi(avg_gain_, avg_loss_);
			last_ = out;
			return out;
		}

		// 2) seed 이후: Wilder smoothing
		const double n = static_cast<double>(length_);
		avg_gain_ = (avg_gain_ * (n - 1.0) + gain) / n;
		avg_loss_ = (avg_loss_ * (n - 1.0) + loss) / n;

		out.ready = true;
		out.v = computeRsi(avg_gain_, avg_loss_);
		last_ = out;
		return out;
	}

	trading::Value<double> RsiWilder::update(const core::Candle& c)
	{
		return update(static_cast<double>(c.close_price));
	}

	trading::Value<double> RsiWilder::value() const noexcept
	{
		return last_;
	}

	double RsiWilder::computeRsi(double avg_gain, double avg_loss) noexcept
	{
		// 변화가 전혀 없는 경우(완전 횡보): 중립값 50
		if (avg_gain == 0.0 && avg_loss == 0.0)
			return 50.0;

		// 손실이 0이면 RSI는 100(상승만 존재)
		if (avg_loss == 0.0)
			return 100.0;

		// 이득이 0이면 RSI는 0(하락만 존재)
		if (avg_gain == 0.0)
			return 0.0;

		const double rs = avg_gain / avg_loss;
		const double rsi = 100.0 - (100.0 / (1.0 + rs));
		return rsi;
	}
} // namespace trading::indicators
