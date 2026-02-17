#include <iostream>
#include <string>
#include <vector>

// ============================================================
// TestRsiMeanReversionStrategy.cpp
// ����:
// - ������ "�ó�����" ������ �����Ѵ�.
//   1) ���� 1ȸ �߻�
//   2) PendingEntry ���¿��� �߰� ���� ����
//   3) û�� 1ȸ �߻�
//   4) PendingExit ���¿��� �߰� �ֹ� ����
//   5) û�� ü�� �� Flat ����(=���� ����) �� ������ ����
//
// �� �׽�Ʈ�� ����/���ֹ��� �и��� 6�ܰ� ���� �׽�Ʈ�̹Ƿ�
// - �Է��� Candle�� ���� ����� onCandle()�� ����
// - ü���� FillEvent�� ���� ����� onFill()�� �����Ѵ�.
// ============================================================

#include "core/domain/Candle.h"

#include "trading/strategies/StrategyTypes.h"             // AccountSnapshot, FillEvent, Decision
#include "trading/strategies/RsiMeanReversionStrategy.h"  // ���� ��ü

namespace {

    // ----------------------------
    // �̴� �׽�Ʈ ����
    // ----------------------------
    int g_fail = 0;

    void expect(bool ok, const char* msg) {
        if (!ok) {
            ++g_fail;
            std::cerr << "[FAIL] " << msg << "\n";
        }
    }

    // �׽�Ʈ�� Candle ������
    // - �ǽð�/REST/WS ���̵� ������ �Ǵ��ϵ��� close�� �����Ѵ�.
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
    // (A) �׽�Ʈ �Ķ���� ����
    // - ���� � �Ķ���Ͱ� �ƴ϶� "���� ���� �׽�Ʈ"�� ���� �����Ű�� ���� �����̴�.
    // - ready ������ ���� ����� ���� rsiLength, volatilityWindow ���� �۰� �����Ѵ�.
    // - ���� ������ ������ ������ ��Ȳ�� ���Ϸ��� minVolatility�� 0���� �д�.
    // ============================================================
    RsiMeanReversionStrategy::Params p{};
    p.rsiLength = 3;          // RSI seed�� ���� ä�������� �۰�
    p.trendLookWindow = 2;    // close[2] ��굵 ���� ready
    p.volatilityWindow = 3;   // �������� ������ ready
    p.maxTrendStrength = 1.0; // �߼� ���� ������ ��ȭ(���� ������ ������ �� �ּ�ȭ)
    p.minVolatility = 0.0;    // ������ ���� OFF (�̹� �׽�Ʈ ����: ���� ����)
    p.oversold = 30.0;        // �϶� �������� RSI�� ���� oversold �����ϵ���
    p.overbought = 70.0;
    p.utilization = 0.5;      // �ֹ� ũ�� ��꿡 ���(���� ���� ������ ���� ����)
    p.stopLossPct = 1.0;      // ����/������ ������ ��ġ�ǵ��� ª��
    p.profitTargetPct = 1.0;

    const std::string market = "KRW-BTC";

    // ���� �ν��Ͻ� ���� (�� ���� ���� ���� �ӽ��� ���ο� ������)
    RsiMeanReversionStrategy strat(market, p);

    // ���� ������(������ �ֹ� ũ�⸦ ����� �� �ֵ��� �ּ� ���� ����)
    // - krw_available: �ż� ������ KRW
    // - coin_available: ���� ���� ����(û�� �Ǵܿ� �ʿ�)
    AccountSnapshot acc{};
    acc.krw_available = 100'000.0;
    acc.coin_available = 0.0;

    // ============================================================
    // (B) Warm-up + ���� 1ȸ ����
    // - �϶� �������� �־� RSI�� oversold�� �����,
    // - ��ǥ���� ready�� �Ǵ� ���� ���Ŀ� ������ "ENTRY �ֹ�"�� ������ ����.
    // ============================================================
    Decision d{};
    std::vector<double> closes = { 100.0, 99.0, 98.0, 97.0, 96.0 };

    for (double px : closes) {
        // candle�� �� ���� ������ ������ "�ֹ� �ǵ�"�� �����ϴ��� ����
        d = strat.onCandle(makeCandle(market, px), acc);

        // �ֹ��� ������(=���� 1ȸ) ���� ����
        if (d.hasOrder()) break;
    }

    // (B-1) ���� �ֹ��� ������ 1�� �߻��ߴ���
    expect(d.hasOrder(), "Strategy: should submit ENTRY order once conditions are met");

    // (B-2) ���� �ֹ��� ���°� ���� ������(����/����/�ֹ�Ÿ�� ��)
    if (d.hasOrder()) {
        // ���� ��ġ
        expect(d.order->market == market, "Strategy: entry order market mismatch");
        // �ż�(BID)�̾�� ��
        expect(d.order->position == core::OrderPosition::BID, "Strategy: entry order should be BID");
        // ����A(���尡 ����) �����̸� Market �ֹ��̾�� ��
        expect(d.order->type == core::OrderType::Market, "Strategy: entry order should be Market");
        // ü�� �̺�Ʈ ��Ī�� ���� client_order_id�� �־�� ��
        expect(!d.order->identifier.empty(), "Strategy: entry order should have client_order_id");
    }

    // ============================================================
    // (C) PendingEntry ���� ����
    // - ���� onFill(entry)�� ȣ������ �ʾұ� ������
    //   ���� ���� ���´� PendingEntry �� ���ɼ��� ����.
    // - �� ���¿��� �� �ٸ� candle�� �͵� "�߰� �ֹ�"�� ������ �� �ȴ�.
    // ============================================================
    {
        auto d2 = strat.onCandle(makeCandle(market, 95.0), acc);

        // ��� ���¿����� �ֹ��� ����� �Ѵ�.
        expect(!d2.hasOrder(), "Strategy: should block additional orders while PendingEntry");

        // Decision�� "�ƹ� �͵� ���� ����" �÷��׸� ���� �ִٸ�,
        // �� �׽�Ʈ�� �� �ǵ��� Ȯ�����ش�.
        expect(d2.is_no_action, "Strategy: PendingEntry should return noAction() to signal 'blocked'");
    }

    // ============================================================
    // (D) ���� ü�� �ùķ��̼�
    // - ������ ������ ü���� �˷��� ��ó�� FillEvent�� ����� onFill() ȣ��
    // - PendingEntry -> InPosition ���� ���°� �ٲ��� �Ѵ�.
    // ============================================================
    const std::string entryCid = d.order ? d.order->identifier : std::string{};
    const double entryFillPrice = 95.0;

    // FillEvent�� "� �ֹ��� � �������� � ���ݿ� ü��ƴ���"�� �����ϴ� �ּ� ����
    strat.onFill(FillEvent(entryCid, core::OrderPosition::BID, entryFillPrice));

    // ���µ� �ּ��� "������ ������ ����"�� �ٲ��ش�.
    // (������ û�� �Ǵ� �� asset_available�� ����� �� ����)
    acc.krw_available = 50'000.0;
    acc.coin_available = 1.0;

    // ============================================================
    // (E) û�� 1ȸ ����
    // - ��ǥ��(profitTarget)�� �Ѵ� ������ �־� û�� ��ȣ�� ������ �Ѵ�.
    // - ���⼭ 1ȸ�� û�� �ֹ��� �����Ǵ��� Ȯ��.
    // ============================================================
    Decision exitDecision = strat.onCandle(makeCandle(market, entryFillPrice * 1.02), acc);

    expect(exitDecision.hasOrder(), "Strategy: should submit EXIT order when target hit");

    if (exitDecision.hasOrder()) {
        // û���� �ŵ�(ASK)
        expect(exitDecision.order->position == core::OrderPosition::ASK, "Strategy: exit order should be ASK");
        // ����A(���尡 ����)�̸� û�굵 Market
        expect(exitDecision.order->type == core::OrderType::Market, "Strategy: exit order should be Market");
        // fill ��Ī ���� client_order_id �ʿ�
        expect(!exitDecision.order->identifier.empty(), "Strategy: exit order should have client_order_id");
    }

    // ============================================================
    // (F) PendingExit ���� ����
    // - ���� exit onFill�� ȣ������ �ʾ����Ƿ� PendingExit ������ �� �ִ�.
    // - �� ���¿��� candle�� �� �͵� �߰� �ֹ�(��û��/������)�� ������ �� �ȴ�.
    // ============================================================
    {
        auto d3 = strat.onCandle(makeCandle(market, entryFillPrice * 1.03), acc);

        // PendingExit�̸� �ֹ��� ����� �Ѵ�.
        expect(!d3.hasOrder(), "Strategy: should block additional orders while PendingExit");
        expect(d3.is_no_action, "Strategy: PendingExit should return noAction() to signal 'blocked'");
    }

    // ============================================================
    // (G) û�� ü�� �ùķ��̼�
    // - PendingExit -> Flat ���� �����ؾ� �Ѵ�(���� ����).
    // - ���� getter�� ���ٸ�, "�ٽ� ������ �����Ѱ�"�� ���� Ȯ���Ѵ�.
    // ============================================================
    const std::string exitCid = exitDecision.order ? exitDecision.order->identifier : std::string{};
    strat.onFill(FillEvent(exitCid, core::OrderPosition::ASK, entryFillPrice * 1.02));

    // ���µ� �ٽ� "���ݸ� �ִ� ����"�� �����д�(������ ���� ����)
    acc.krw_available = 100'000.0;
    acc.coin_available = 0.0;

    // (G-1) �ٽ� �϶� �������� �־� �������� �������� Ȯ��
    // - û�� �Ŀ��� Pending�̳� InPosition ���·� �����ִٸ� �������� ���� ���̴�.
    Decision re = Decision::none();
    for (double px : std::vector<double>{ 100.0, 99.0, 98.0, 97.0, 96.0 }) {
        re = strat.onCandle(makeCandle(market, px), acc);
        if (re.hasOrder()) break;
    }

    expect(re.hasOrder(), "Strategy: after exit fill, state should reset to allow new entry");

    // ============================================================
    // (H) ���� ��� ���
    // ============================================================
    if (g_fail == 0) {
        std::cout << "[OK] Strategy scenario tests passed.\n";
        return 0;
    }

    std::cout << "[NG] Strategy tests failed: " << g_fail << "\n";
    return 1;
}
