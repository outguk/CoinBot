#include <cmath>
#include <iostream>
#include <string>

// ============================================================
// TestIndicators.cpp
// 목적:
// - 각 지표(자료구조/보조지표)가 "독립적으로" 올바르게 동작하는지 검증한다.
// - 오프바이원(윈도우 채움 타이밍), ready 조건, overwrite 동작 등을 여기서 잡는다.
// - gtest 같은 프레임워크 없이도 바로 돌릴 수 있게 main() 기반으로 구성한다.
// ============================================================

#include "core/domain/Candle.h"

#include "trading/indicators/RingBuffer.h"
#include "trading/indicators/Sma.h"
#include "trading/indicators/ClosePriceWindow.h"
#include "trading/indicators/ChangeVolatilityIndicator.h"
#include "trading/indicators/RsiWilder.h"

namespace {

    // ----------------------------
    // 미니 테스트 러너
    // ----------------------------
    // 실패 카운터: 테스트 중 FAIL이 발생하면 누적하고, 종료 시 실패 개수를 출력한다.
    int g_fail = 0;

    // expect: 조건이 false면 FAIL 출력 + 실패 카운터 증가
    void expect(bool ok, const char* msg) {
        if (!ok) {
            ++g_fail;
            std::cerr << "[FAIL] " << msg << "\n";
        }
    }

    // 부동소수 비교용(지표는 double 계산이 많아 정확히 일치 비교가 위험)
    bool near(double a, double b, double eps = 1e-9) {
        return std::fabs(a - b) <= eps;
    }

    // 테스트용 Candle 생성기
    // - 전략/지표는 Candle을 받을 수도 있고, close double을 받을 수도 있으므로
    //   Candle 입력도 테스트한다.
    core::Candle makeCandle(std::string market, double close) {
        core::Candle c{};
        c.market = std::move(market);
        c.open_price = close;
        c.high_price = close;
        c.low_price = close;
        c.close_price = close;
        c.volume = 1.0;
        c.start_timestamp = "";
        return c;
    }

} // namespace

int TestIndicators() {
    using namespace trading;
    using namespace trading::indicators;

    std::cout << "== TestIndicators ==\n";
    constexpr bool TRACE = true; // false면 기존처럼 조용히 동작

    // ============================================================
    // 1) RingBuffer 테스트
    // 핵심 포인트:
    // - capacity를 초과했을 때 overwrite가 발생하는가?
    // - oldest/newest가 올바르게 갱신되는가?
    // - valueFromBack(0)=newest 규칙이 정확한가?
    // ============================================================
    {
        // capacity=3인 링 버퍼
        RingBuffer<double> rb(3);

        // 1~3 push는 overwrite가 없어야 하므로 nullopt 기대
        expect(!rb.push(1).has_value(), "RingBuffer: first push should not overwrite");
        expect(!rb.push(2).has_value(), "RingBuffer: second push should not overwrite");
        expect(!rb.push(3).has_value(), "RingBuffer: third push should not overwrite");

        // size는 capacity만큼 채워져야 함
        expect(rb.size() == 3, "RingBuffer: size should be 3 after 3 pushes");

        // oldest/newest가 각각 1,3인지 확인
        expect(rb.oldest() == 1, "RingBuffer: oldest should be 1");
        expect(rb.newest() == 3, "RingBuffer: newest should be 3");

        // 4번째 push는 capacity를 초과 -> 가장 오래된 값(1)이 overwrite 되어 반환되어야 함
        auto ov = rb.push(4);
        expect(ov.has_value() && *ov == 1, "RingBuffer: overwrite should return 1");

        // overwrite 이후 버퍼의 논리적 내용은 (2,3,4)
        expect(rb.oldest() == 2, "RingBuffer: oldest should be 2 after overwrite");
        expect(rb.newest() == 4, "RingBuffer: newest should be 4 after overwrite");

        // newest 기준 뒤에서 접근:
        // back=0 -> newest(4)
        // back=1 -> 3
    }
}