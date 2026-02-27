#include "ClosePriceWindow.h"

#include "core/domain/Candle.h" // close_price

namespace trading::indicators {

    void ClosePriceWindow::reset(std::size_t length) {
        length_ = length;

        // close[N]를 얻기 위해서는 최소 N+1개를 유지해야 한다.
        // 예: N=20 -> 최근 21개 종가를 저장
        window_.reset(length_ + 1);
    }

    void ClosePriceWindow::clear() noexcept {
        window_.clear();
    }

    trading::Value<double> ClosePriceWindow::update(double close) {
        trading::Value<double> out{};

        // window_는 reset에서 length_+1로 잡히지만,
        // 혹시라도 length_+1이 0이 되는 케이스는 없다(size_t).
        // (length_==0이면 capacity==1 -> close[0]은 최신값)
        window_.push(close);

        // length_는 window_ 크기와 같으므로, valueFromBack(length_)가 값이 존재한다면 full 상태라고 볼 수 있다.
        const auto v = window_.valueFromBack(length_);
        if (!v.has_value()) {
            out.ready = false;
            out.v = 0.0;
            return out;
        }

        out.ready = true;
        out.v = *v; // close[N]
        return out;
    }

    trading::Value<double> ClosePriceWindow::update(const core::Candle& c) {
        return update(static_cast<double>(c.close_price));
    }

    trading::Value<double> ClosePriceWindow::closeN() const noexcept {
        trading::Value<double> out{};
        const auto v = window_.valueFromBack(length_);
        if (!v.has_value()) {
            out.ready = false;
            out.v = 0.0;
            return out;
        }
        out.ready = true;
        out.v = *v;
        return out;
    }

} // namespace trading::indicators
