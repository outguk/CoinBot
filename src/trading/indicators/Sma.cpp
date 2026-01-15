#include "Sma.h"

#include "core/domain/Candle.h"   // close_price(종가 사용)

namespace trading::indicators {

    void Sma::reset(std::size_t length) {
        length_ = length;
        window_.reset(length_);
        sum_ = 0.0;
    }

    void Sma::clear() noexcept {
        window_.clear();
        sum_ = 0.0;
    }

    trading::Value<double> Sma::update(double x) {
        trading::Value<double> out{};

        if (length_ == 0) {
            // 비활성 상태: 어떤 값도 만들지 않음
            out.ready = false;
            out.v = 0.0;
            return out;
        }

        // 덮어쓴 값이 있으면 합에서 빼고
        const auto overwritten = window_.push(x);
        if (overwritten.has_value()) {
            sum_ -= *overwritten;
        }
        // 새로 들어온 값은 합에 추가
        sum_ += x;

        // 윈도우가 가득 찼을 때만 SMA가 "정의"된다고 보고 ready=true
        out.ready = window_.full();

        // ready일 때만 평균을 계산해서 반환
        // (미준비 상태에서는 out.v를 0으로 두고, ready 플래그로 판단하도록 설계)
        out.v = out.ready ? (sum_ / static_cast<double>(length_)) : 0.0;
        return out;
    }

    trading::Value<double> Sma::update(const core::Candle& c) {
        // Candle 입력은 종가(close_price)를 SMA 입력으로 사용
        return update(static_cast<double>(c.close_price));
    }

    trading::Value<double> Sma::value() const noexcept {
        trading::Value<double> out{};
        out.ready = (length_ > 0) && window_.full();
        out.v = out.ready ? (sum_ / static_cast<double>(length_)) : 0.0;
        return out;
    }

} // namespace trading::indicators
