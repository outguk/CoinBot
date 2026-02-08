#include "ChangeVolatilityIndicator.h"

#include <cmath> // std::sqrt, std::max

#include "core/domain/Candle.h"

namespace trading::indicators {

    // 크기부터 전체를 비움
    void ChangeVolatilityIndicator::reset(std::size_t window) {
        window_ = (window < 2) ? 2 : window;    // 윈도우 설정
        returns_.reset(window_);    // 버퍼도 윈도우로 맞춤
        sum_ = 0.0;
        sumsq_ = 0.0;
        prevClose_.reset();
    }

    // 데이터만 비운다
    void ChangeVolatilityIndicator::clear() noexcept {
        returns_.clear();
        sum_ = 0.0;
        sumsq_ = 0.0;
        prevClose_.reset();
    }

    trading::Value<double> ChangeVolatilityIndicator::update(double close) {
        trading::Value<double> out{};

        // 비활성
        if (window_ == 0) {
            out.ready = false;
            out.v = 0.0;
            return out;
        }

        // 변화율을 만들기 위해서는 이전 종가가 필요
        if (!prevClose_.has_value()) {
            prevClose_ = close;
            out.ready = false;
            out.v = 0.0;
            return out;
        }

        const double prev = *prevClose_;
        prevClose_ = close;

        // 분모 0 방어: prev가 0이면 변화율 정의 불가 -> 이번 샘플은 스킵
        // (암호화폐 가격이 0이 되는 건 현실적으로 없지만, 안전하게 처리)
        if (prev == 0.0) {
            out.ready = false;
            out.v = 0.0;
            return out;
        }

        // 퍼센트 변화율
        const double r = (close - prev) / prev;

        // returns_에 push하고, 덮어쓴 값이 있으면 sum/sumsq에서 제거
        const auto overwritten = returns_.push(r);
        if (overwritten.has_value()) {
            const double old = *overwritten;
            sum_ -= old;
            sumsq_ -= old * old;
        }

        // 새 값 반영
        sum_ += r;
        sumsq_ += r * r;

        // 윈도우가 꽉 찼을 때만 변동성(stdev)을 신뢰(ready=true)
        out.ready = returns_.full();
        out.v = out.ready ? stdev_() : 0.0;
        return out;
    }

    trading::Value<double> ChangeVolatilityIndicator::update(const core::Candle& c) {
        return update(static_cast<double>(c.close_price));
    }

    trading::Value<double> ChangeVolatilityIndicator::value() const noexcept {
        trading::Value<double> out{};
        out.ready = (window_ > 0) && returns_.full();
        out.v = out.ready ? stdev_() : 0.0;
        return out;
    }

    double ChangeVolatilityIndicator::stdev_() const noexcept {
        // returns_가 full일 때만 호출된다고 가정(호출부에서 ready 체크)
        const double n = static_cast<double>(window_);
        if (n <= 2.0) return 0.0;

        const double mean = sum_ / n;
        // Var = E[x^2] - (E[x])^2
        double var = (sumsq_ / n) - (mean * mean);

        // 부동소수 오차로 음수로 살짝 내려갈 수 있어 clamp
        if (var < 0.0) var = 0.0;

        return std::sqrt(var);
    }

} // namespace trading::indicators
