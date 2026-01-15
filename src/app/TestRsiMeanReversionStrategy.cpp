#include <iostream>
#include <string>
#include <vector>

// ============================================================
// TestRsiMeanReversionStrategy.cpp
// 목적:
// - 전략을 "시나리오" 단위로 검증한다.
//   1) 진입 1회 발생
//   2) PendingEntry 상태에서 추가 진입 차단
//   3) 청산 1회 발생
//   4) PendingExit 상태에서 추가 주문 차단
//   5) 청산 체결 후 Flat 복귀(=상태 리셋) → 재진입 가능
//
// 이 테스트는 엔진/실주문과 분리된 6단계 전략 테스트이므로
// - 입력은 Candle을 직접 만들어 onCandle()에 주입
// - 체결은 FillEvent를 직접 만들어 onFill()로 전달한다.
// ============================================================

#include "core/domain/Candle.h"

#include "trading/strategies/StrategyTypes.h"             // AccountSnapshot, FillEvent, Decision
#include "trading/strategies/RsiMeanReversionStrategy.h"  // 전략 본체

namespace {

    // ----------------------------
    // 미니 테스트 러너
    // ----------------------------
    int g_fail = 0;

    void expect(bool ok, const char* msg) {
        if (!ok) {
            ++g_fail;
            std::cerr << "[FAIL] " << msg << "\n";
        }
    }

    // 테스트용 Candle 생성기
    // - 실시간/REST/WS 없이도 전략이 판단하도록 close를 주입한다.
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

int TestRsiMeanReversionStrategy() {
    using namespace trading;
    using namespace trading::strategies;

    std::cout << "== TestRsiMeanReversionStrategy ==\n";

    // ============================================================
    // (A) 테스트 파라미터 구성
    // - 실제 운영 파라미터가 아니라 "상태 전이 테스트"를 쉽게 통과시키기 위한 값들이다.
    // - ready 조건을 빨리 만들기 위해 rsiLength, volatilityWindow 등을 작게 설정한다.
    // - 필터 때문에 진입이 막히는 상황을 피하려고 minVolatility를 0으로 둔다.
    // ============================================================
    RsiMeanReversionStrategy::Params p{};
    p.rsiLength = 3;          // RSI seed가 빨리 채워지도록 작게
    p.trendLookWindow = 2;    // close[2] 계산도 빨리 ready
    p.volatilityWindow = 3;   // 변동성도 빠르게 ready
    p.maxTrendStrength = 1.0; // 추세 강도 제한은 완화(필터 때문에 막히는 걸 최소화)
    p.minVolatility = 0.0;    // 변동성 필터 OFF (이번 테스트 목적: 상태 전이)
    p.oversold = 30.0;        // 하락 시퀀스로 RSI가 쉽게 oversold 도달하도록
    p.overbought = 70.0;
    p.riskPercent = 50.0;     // 주문 크기 계산에 사용(전략 내부 로직에 맞춰 유지)
    p.stopLossPct = 1.0;      // 손절/익절은 빠르게 터치되도록 짧게
    p.profitTargetPct = 1.0;

    const std::string market = "KRW-BTC";

    // 전략 인스턴스 생성 (한 종목에 대한 상태 머신을 내부에 가진다)
    RsiMeanReversionStrategy strat(market, p);

    // 계좌 스냅샷(전략이 주문 크기를 계산할 수 있도록 최소 정보 제공)
    // - krw_available: 매수 가능한 KRW
    // - coin_available: 보유 코인 수량(청산 판단에 필요)
    AccountSnapshot acc{};
    acc.krw_available = 100'000.0;
    acc.coin_available = 0.0;

    // ============================================================
    // (B) Warm-up + 진입 1회 유도
    // - 하락 시퀀스를 넣어 RSI를 oversold로 만들고,
    // - 지표들이 ready가 되는 시점 이후에 전략이 "ENTRY 주문"을 내는지 본다.
    // ============================================================
    Decision d{};
    std::vector<double> closes = { 100.0, 99.0, 98.0, 97.0, 96.0 };

    for (double px : closes) {
        // candle을 한 개씩 넣으며 전략이 "주문 의도"를 생성하는지 관찰
        d = strat.onCandle(makeCandle(market, px), acc);

        // 주문이 나오면(=진입 1회) 루프 종료
        if (d.hasOrder()) break;
    }

    // (B-1) 진입 주문이 실제로 1번 발생했는지
    expect(d.hasOrder(), "Strategy: should submit ENTRY order once conditions are met");

    // (B-2) 진입 주문의 형태가 기대와 같은지(시장/방향/주문타입 등)
    if (d.hasOrder()) {
        // 종목 일치
        expect(d.order->market == market, "Strategy: entry order market mismatch");
        // 매수(BID)이어야 함
        expect(d.order->position == core::OrderPosition::BID, "Strategy: entry order should be BID");
        // 관점A(시장가 고정) 기준이면 Market 주문이어야 함
        expect(d.order->type == core::OrderType::Market, "Strategy: entry order should be Market");
        // 체결 이벤트 매칭을 위해 client_order_id가 있어야 함
        expect(!d.order->identifier.empty(), "Strategy: entry order should have client_order_id");
    }

    // ============================================================
    // (C) PendingEntry 차단 검증
    // - 아직 onFill(entry)를 호출하지 않았기 때문에
    //   전략 내부 상태는 PendingEntry 일 가능성이 높다.
    // - 이 상태에서 또 다른 candle이 와도 "추가 주문"이 나가면 안 된다.
    // ============================================================
    {
        auto d2 = strat.onCandle(makeCandle(market, 95.0), acc);

        // 대기 상태에서는 주문이 없어야 한다.
        expect(!d2.hasOrder(), "Strategy: should block additional orders while PendingEntry");

        // Decision이 "아무 것도 하지 않음" 플래그를 갖고 있다면,
        // 이 테스트는 그 의도를 확인해준다.
        expect(d2.is_no_action, "Strategy: PendingEntry should return noAction() to signal 'blocked'");
    }

    // ============================================================
    // (D) 진입 체결 시뮬레이션
    // - 엔진이 실제로 체결을 알려준 것처럼 FillEvent를 만들어 onFill() 호출
    // - PendingEntry -> InPosition 으로 상태가 바뀌어야 한다.
    // ============================================================
    const std::string entryCid = d.order ? d.order->identifier : std::string{};
    const double entryFillPrice = 95.0;

    // FillEvent는 "어떤 주문이 어떤 방향으로 어떤 가격에 체결됐는지"를 전달하는 최소 단위
    strat.onFill(FillEvent(entryCid, core::OrderPosition::BID, entryFillPrice));

    // 계좌도 최소한 "코인을 보유한 상태"로 바꿔준다.
    // (전략이 청산 판단 시 asset_available을 사용할 수 있음)
    acc.krw_available = 50'000.0;
    acc.coin_available = 1.0;

    // ============================================================
    // (E) 청산 1회 유도
    // - 목표가(profitTarget)를 넘는 가격을 넣어 청산 신호가 나오게 한다.
    // - 여기서 1회만 청산 주문이 생성되는지 확인.
    // ============================================================
    Decision exitDecision = strat.onCandle(makeCandle(market, entryFillPrice * 1.02), acc);

    expect(exitDecision.hasOrder(), "Strategy: should submit EXIT order when target hit");

    if (exitDecision.hasOrder()) {
        // 청산은 매도(ASK)
        expect(exitDecision.order->position == core::OrderPosition::ASK, "Strategy: exit order should be ASK");
        // 관점A(시장가 고정)이면 청산도 Market
        expect(exitDecision.order->type == core::OrderType::Market, "Strategy: exit order should be Market");
        // fill 매칭 위해 client_order_id 필요
        expect(!exitDecision.order->identifier.empty(), "Strategy: exit order should have client_order_id");
    }

    // ============================================================
    // (F) PendingExit 차단 검증
    // - 아직 exit onFill을 호출하지 않았으므로 PendingExit 상태일 수 있다.
    // - 이 상태에서 candle이 더 와도 추가 주문(재청산/재진입)이 나오면 안 된다.
    // ============================================================
    {
        auto d3 = strat.onCandle(makeCandle(market, entryFillPrice * 1.03), acc);

        // PendingExit이면 주문이 없어야 한다.
        expect(!d3.hasOrder(), "Strategy: should block additional orders while PendingExit");
        expect(d3.is_no_action, "Strategy: PendingExit should return noAction() to signal 'blocked'");
    }

    // ============================================================
    // (G) 청산 체결 시뮬레이션
    // - PendingExit -> Flat 으로 복귀해야 한다(상태 리셋).
    // - 상태 getter가 없다면, "다시 진입이 가능한가"로 간접 확인한다.
    // ============================================================
    const std::string exitCid = exitDecision.order ? exitDecision.order->identifier : std::string{};
    strat.onFill(FillEvent(exitCid, core::OrderPosition::ASK, entryFillPrice * 1.02));

    // 계좌도 다시 "현금만 있는 상태"로 돌려둔다(재진입 가능 조건)
    acc.krw_available = 100'000.0;
    acc.coin_available = 0.0;

    // (G-1) 다시 하락 시퀀스를 넣어 재진입이 가능한지 확인
    // - 청산 후에도 Pending이나 InPosition 상태로 남아있다면 재진입이 막힐 것이다.
    Decision re = Decision::none();
    for (double px : std::vector<double>{ 100.0, 99.0, 98.0, 97.0, 96.0 }) {
        re = strat.onCandle(makeCandle(market, px), acc);
        if (re.hasOrder()) break;
    }

    expect(re.hasOrder(), "Strategy: after exit fill, state should reset to allow new entry");

    // ============================================================
    // (H) 최종 결과 출력
    // ============================================================
    if (g_fail == 0) {
        std::cout << "[OK] Strategy scenario tests passed.\n";
        return 0;
    }

    std::cout << "[NG] Strategy tests failed: " << g_fail << "\n";
    return 1;
}
