//// app/smoke_demo_order_engine.cpp
//#include <cassert>
//#include <cmath>
//#include <iostream>
//#include <optional>
//#include <string>
//
//#include "engine/DemoOrderEngine.h"
//#include "core/domain/OrderRequest.h"
//#include "core/domain/Ticker.h"
//#include "core/domain/Account.h"
//#include "core/domain/Order.h"
//#include "core/domain/OrderTypes.h"
//#include "core/domain/Types.h"
//namespace
//{
//	// [상수] 부동소수 오차 허용치 (assert 등에서 사용)
//	constexpr double EPS = 1e-6;
//
//	// [헬퍼 함수] double 근사 비교 (계산에 따른 연산 오차 허용)
//	static bool nearly(double a, double b, double eps = EPS)
//	{
//		return std::fabs(a - b) < eps;
//	}
//
//	// [헬퍼 함수] 계좌 스냅샷에서 특정 마켓 포지션 찾기(읽기 전용)
//	static const core::Position* findPos(const core::Account& acc, const std::string& market) {
//		for (const auto& p : acc.positions) {
//			if (p.currency == market) return &p;
//		}
//		return nullptr;
//	}
//
//	// [헬퍼 함수] onMarket에 넣을 최소 Ticker 생성
//	// 체결 판단은 ticker.trade_price(혹은 ticker_trade_price)에 의해 이뤄짐 (데모라 최소한의 필드만)
//	static core::Ticker makeTick(std::string market, core::Price trade_price)
//	{
//		core::Ticker t;
//		t.market = std::move(market);
//		t.ticker_trade_price = trade_price;
//		return t;
//	}
//
//	// [헬퍼 함수] Market BUY 주문 요청 생성 (BID + Market + AmountSize)
//	static core::OrderRequest makeMarketBuyByAmount(std::string market, core::Amount krw_amount)
//	{
//		core::OrderRequest r{};
//		r.market = std::move(market);
//		r.position = core::OrderPosition::BID;
//		r.type = core::OrderType::Market;
//
//		// [중요] OrderSize = variant<VolumeSize, AmountSize> 를 가정
//		r.size = core::AmountSize{ krw_amount };
//		// 지정가는 비운다
//		r.price = std::nullopt;
//
//		// 추적용 메타
//		// [추적용 메타] 도메인에 오염되지 않게 문자열로만 전달하는 정책
//		r.strategy_id = "smoke";
//		r.client_order_id = "mbuy-1";
//		r.client_tag = "market-buy";
//		return r;
//	}
//	// [헬퍼 함수] Limit BUY 주문 요청 생성 (BID + Limit + AmountSize + price)
//	// - BID는 ticker_price <= limit_price 일 때 체결
//	static core::OrderRequest makeLimitBuyByAmount(std::string market, core::Amount krw_amount, core::Price limit_price) {
//		core::OrderRequest r{};
//		r.market = std::move(market);
//		r.position = core::OrderPosition::BID;
//		r.type = core::OrderType::Limit;
//
//		// BID(매수)는 금액 기반 → AmountSize
//		r.size = core::AmountSize{ krw_amount };
//
//		// Limit은 price 필수
//		r.price = limit_price;
//
//		r.strategy_id = "smoke";
//		r.client_order_id = "lbuy-1";
//		r.client_tag = "limit-buy";
//		return r;
//	}
//
//	// [헬퍼 함수] Limit SELL 주문 요청 생성 (ASK + Limit + VolumeSize + price)
//	static core::OrderRequest makeLimitSellByVolume(std::string market, core::Volume volume, core::Price limit_price) {
//		core::OrderRequest r{};
//		r.market = std::move(market);
//		r.position = core::OrderPosition::ASK;
//		r.type = core::OrderType::Limit;
//
//		// ASK(매도)는 수량 기반 → VolumeSize
//		r.size = core::VolumeSize{ volume };
//
//		r.price = limit_price;
//
//		r.strategy_id = "smoke";
//		r.client_order_id = "lsell-1";
//		r.client_tag = "limit-sell";
//		return r;
//	}
//
//	// [헬퍼 함수] 계좌 스냅샷 출력
//	static void dumpAccount(const core::Account& a, const std::string& title) {
//		std::cout << "\n=== " << title << " ===\n";
//		std::cout << std::fixed << std::setprecision(0);
//		std::cout
//			<< " availableFunds=" << a.krw_free
//			<< " totalAssetValue=" << a.totalAssetValue
//			<< " positions=" << a.positions.size() << "\n";
//		for (const auto& p : a.positions) {
//			std::cout << "  - " << p.currency
//				<< " vol=" << p.balance
//				<< " avg=" << p.avg_buy_price << "\n";
//		}
//	}
//
//	// [헬퍼 함수] EngineResult 출력
//	static void dumpResult(const engine::EngineResult& r, const std::string& title) {
//		std::cout << "\n--- " << title << " ---\n";
//		std::cout << "success=" << (r.success ? "true" : "false")
//			<< " code=" << static_cast<int>(r.code)
//			<< " msg=" << r.message << "\n";
//		if (r.order) {
//			const auto& o = *r.order;
//			std::cout << "order: id=" << o.id
//				<< " market=" << o.market
//				<< " status=" << to_string(o.status)
//				<< " pos=" << static_cast<int>(o.position)
//				<< " type=" << static_cast<int>(o.type)
//				<< " price=" << o.price
//				<< " vol=" << o.volume << "\n";
//			if (o.fees) std::cout << "fees=" << *o.fees << "\n";
//		}
//		if (r.account) dumpAccount(*r.account, "Account snapshot in result");
//	}
//
//	// [헬퍼 함수] submit이 성공했을 때 order_id를 안전하게 꺼내기
//	static std::string requireOrderId(const engine::EngineResult& r) {
//		assert(r.success);
//		assert(r.order.has_value());
//		assert(!r.order->id.empty());
//		return r.order->id;
//	}
//
//	// [헬퍼 함수] 특정 주문이 Filled 상태인지 확인
//	// - 왜 필요? “submit은 접수만”이고 “onMarket에서 Filled”가 Stage 5 핵심 규칙이므로
//	//   그 규칙이 지켜졌는지 강하게 검증해야 한다.
//	static void expectFilled(const engine::DemoOrderEngine& eng, const std::string& id) {
//		auto o = eng.get(id);
//		assert(o.has_value());
//		assert(o->status == core::OrderStatus::Filled);
//	}
//
//	// [헬퍼 함수] 특정 주문이 Canceled 상태인지 확인
//	// - cancel이 예약 해제를 동반해야 availableFunds가 복구된다.
//	static void expectCanceled(const engine::DemoOrderEngine& eng, const std::string& id) {
//		auto o = eng.get(id);
//		assert(o.has_value());
//		assert(o->status == core::OrderStatus::Canceled);
//	}
//}	// namespace
//
//int OrderEngineDemo()
//{
//	// [테스트 실행 진입점]
//	std::cout << "=== Smoke Test / DemoOrderEngine (Stage 5) ===\n";
//
//	// [검증 대상 엔진 인스턴스]
//	// 1,000만원 시작, 수수료 0.05%
//	engine::DemoOrderEngine eng(10000000, engine::FeePolicy{ 0.0005 }); 
//
//	// [테스트 대상 마켓]
//	const std::string MKT = "KRW-BTC";
//
//	// [초기 계좌 상태 스냅샷] - 이후 검증은 스냅샷 대비 변화로 판단
//	auto a0 = eng.accountSnapshot();
//	dumpAccount(a0, "Initial");
//	assert(a0.availableFunds > 0.0);
//
//	// --------- [1] Market BUY: submit(접수) -> onMarket(다음 tick 체결) ----------
//	// 의도 검증 포인트:
//	// 1) submit 시점에는 “즉시 Filled 되면 안 됨”(DemoOrderEngine.h 설계)
//	// 2) submit 시점에 예약이 걸려 availableFunds가 감소해야 함 (BID 예약)
//	// 3) onMarket 이후 Filled가 되고, BTC 포지션이 증가해야 함
//	{
//		const double buy_amount = 1000000; // 100만원어치 매수
//
//		// submit 전 계좌 상태
//		auto before = eng.accountSnapshot();
//
//		// submit: Market BUY 주문 접수
//		auto r_submit = eng.submit(makeMarketBuyByAmount(MKT, buy_amount));
//		dumpResult(r_submit, "[1] submit Market Buy");
//		const auto order_id = requireOrderId(r_submit);
//
//		// submit 직후 계좌 상태 (예약 반영 확인)
//		auto after_submit = eng.accountSnapshot();
//		dumpAccount(after_submit, "[1] After submit");
//
//		// [검증] 예약으로 availableFunds 감소(또는 동일)해야 한다.
//		// - 감소하지 않는다면: applyReservation/recalcAvailableFunds 미반영 가능성
//		assert(after_submit.availableFunds <= before.availableFunds + EPS);
//
//		// onMarket: 다음 tick에서 체결 처리
//		auto ev = eng.onMarket(makeTick(MKT, 50'000'000.0)); // 체결가 50'000'000.0원 가정
//		std::cout << "\n[1] onMarket events=" << ev.size() << "\n";
//		for (size_t i = 0; i < ev.size(); ++i)
//			dumpResult(ev[i], "[1] event #" + std::to_string(i));
//
//		// [검증] 주문이 Filled로 바뀌어야 한다.
//		expectFilled(eng, order_id);
//
//		// [검증] Filled 후 포지션이 존재하고 volume이 0보다 커야 한다.
//		auto after_fill = eng.accountSnapshot();
//		dumpAccount(after_fill, "[1] After fill");
//
//		const auto* p = findPos(after_fill, MKT);
//		assert(p != nullptr);
//		assert(p->volume > 0.0);
//	}
//
//	// [2] Limit BUY: 미체결 -> 체결 조건 확인
//	//
//	// 의도 검증 포인트:
//	// - BID Limit은 ticker_price <= limit_price 일 때 체결 (DemoOrderEngine.h의 규칙)
//	// - 따라서 tick이 limit보다 “높으면” 미체결, “낮거나 같으면” 체결이어야 한다.
//	{
//		// limit을 10,000,000으로 설정: 위에서/아래에서 tick을 넣어 조건을 명확히 분리
//		// krw_amount는 내가 사용할 예산
//		auto r_submit = eng.submit(makeLimitBuyByAmount(MKT, 50'000.0, 10'000'000.0));
//		dumpResult(r_submit, "[2] submit Limit BUY");
//		const auto id = requireOrderId(r_submit);
//
//		// [미체결 케이스] 현재가가 limit보다 높다 → BID는 체결되면 안 됨
//		{
//			auto ev = eng.onMarket(makeTick(MKT, 10'500'000.0));
//			std::cout << "\n[2] onMarket(high) events=" << ev.size() << "\n";
//			for (size_t i = 0; i < ev.size(); ++i)
//				dumpResult(ev[i], "[2] event(high) #" + std::to_string(i));
//
//			auto o = eng.get(id);
//			assert(o.has_value());
//			assert(o->status != core::OrderStatus::Filled);
//		}
//
//		// [체결 케이스] 현재가가 limit보다 낮다 → BID는 체결되어야 함
//		{
//			auto ev = eng.onMarket(makeTick(MKT, 9'900'000.0));
//			std::cout << "\n[2] onMarket(low) events=" << ev.size() << "\n";
//			for (size_t i = 0; i < ev.size(); ++i)
//				dumpResult(ev[i], "[2] event(low) #" + std::to_string(i));
//
//			expectFilled(eng, id);
//		}
//	}
//
//	// [3] Cancel: 예약 해제 + 주문 상태 Canceled
//	//
//	// 의도 검증 포인트:
//	// 1) 체결되지 않도록 아주 낮은 limit으로 BID 주문을 “대기 상태”로 만든다.
//	// 2) submit 후 availableFunds가 감소(예약)해야 한다.
//	// 3) cancel 후 availableFunds가 “원래대로(근사)” 복구되어야 한다.
//	// 4) 주문 상태는 Canceled가 되어야 한다.
//	{
//		// 이 블록 시작 시점에는 “미체결 예약 주문이 없다”는 가정이 중요.
//		// - 위 테스트들은 모두 Filled로 끝났으니 예약이 남아있지 않아야 한다.
//		// - 만약 여기서 before와 after_cancel이 크게 다르면, 엔진 내부 예약 해제가 누락됐을 가능성.
//		auto before = eng.accountSnapshot();
//
//		// 체결되지 않도록 거의 0에 가까운 limit으로 대기 주문 생성
//		auto r_submit = eng.submit(makeLimitBuyByAmount(MKT, 80'000.0, 1.0));
//		dumpResult(r_submit, "[3] submit Limit BUY (cancel target)");
//		const auto id = requireOrderId(r_submit);
//
//		auto after_submit = eng.accountSnapshot();
//		dumpAccount(after_submit, "[3] After submit");
//
//		// [검증] 예약으로 availableFunds 감소
//		assert(after_submit.availableFunds <= before.krw_free + EPS);
//
//		// cancel 실행
//		auto r_cancel = eng.cancel(id);
//		dumpResult(r_cancel, "[3] cancel");
//
//		// [검증] 상태 Canceled
//		expectCanceled(eng, id);
//
//		auto after_cancel = eng.accountSnapshot();
//		dumpAccount(after_cancel, "[3] After cancel");
//
//		// [핵심 검증] cancel은 예약을 풀어야 하므로 availableFunds가 “원래대로(근사)” 복구되어야 함
//		// - 이 검증이 실패하면: releaseReservation / recalcAvailableFunds / reservations_ 정리 중 누락 의심
//		assert(nearly(after_cancel.availableFunds, before.availableFunds));
//	}
//
//	// [4] InsufficientFunds: 거절 코드 + 계좌 변형 없음
//	//
//	// 의도 검증 포인트:
//	// - krw_free보다 큰 금액으로 BID Market 주문을 넣으면 거절되어야 한다.
//	// - 거절이면 계좌(availableFunds)가 바뀌면 안 된다.
//	{
//		auto before = eng.accountSnapshot();
//
//		auto too_big = before.krw_free + 1'000'000.0;
//		auto r = eng.submit(makeMarketBuyByAmount(MKT, too_big));
//		dumpResult(r, "[4] submit Market BUY (insufficient)");
//
//		// [검증] 실패해야 한다.
//		assert(!r.success);
//
//		// [검증] 실패 사유 코드가 InsufficientFunds 또는 OrderRejected 계열로 나와야 의미가 맞다.
//		// - 구현에 따라 canReserve에서 InsufficientFunds로 내거나, normalize/정책에서 OrderRejected로 낼 수 있어 둘 다 허용.
//		assert(r.code == engine::EngineErrorCode::InsufficientFunds ||
//			r.code == engine::EngineErrorCode::OrderRejected);
//
//		// [검증] 거절이면 계좌가 변하면 안 된다.
//		auto after = eng.accountSnapshot();
//		dumpAccount(after, "[4] After reject");
//		assert(nearly(after.availableFunds, before.availableFunds));
//	}
//
//	// [5] Limit SELL: 보유 물량 내에서만 가능 + 조건 충족 시 체결
//	//
//	// 의도 검증 포인트:
//	// 1) 이전 Market BUY로 포지션이 생겼다는 전제 확인
//	// 2) ASK는 Volume 기반 주문이므로 VolumeSize로 size를 넣는다.
//	// 3) ASK Limit은 ticker_price >= limit_price 일 때 체결이어야 한다.
//	{
//		auto acc = eng.accountSnapshot();
//		const auto* p = findPos(acc, MKT);
//
//		// [검증] 포지션이 없으면 이 테스트는 성립하지 않음 (1번 테스트가 실패했거나 체결 로직 문제)
//		assert(p != nullptr);
//		assert(p->balance > 0.0);
//
//		// 보유량의 절반만 매도(스모크에서 안전)
//		const double sell_vol = p->balance * 0.5;
//
//		// Limit SELL 제출
//		auto r_submit = eng.submit(makeLimitSellByVolume(MKT, sell_vol, 60'000'000.0));
//		dumpResult(r_submit, "[5] submit Limit SELL");
//		const auto id = requireOrderId(r_submit);
//
//		// [미체결] 현재가가 limit보다 낮다 → ASK는 체결되면 안 됨
//		{
//			auto ev = eng.onMarket(makeTick(MKT, 55'000'000.0));
//			std::cout << "\n[5] onMarket(low) events=" << ev.size() << "\n";
//			for (size_t i = 0; i < ev.size(); ++i)
//				dumpResult(ev[i], "[5] event(low) #" + std::to_string(i));
//
//			auto o = eng.get(id);
//			assert(o.has_value());
//			assert(o->status != core::OrderStatus::Filled);
//		}
//
//		// [체결] 현재가가 limit 이상 → ASK는 체결되어야 함
//		{
//			auto ev = eng.onMarket(makeTick(MKT, 60'000'000.0));
//			std::cout << "\n[5] onMarket(high) events=" << ev.size() << "\n";
//			for (size_t i = 0; i < ev.size(); ++i)
//				dumpResult(ev[i], "[5] event(high) #" + std::to_string(i));
//
//			expectFilled(eng, id);
//		}
//	}
//
//	std::cout << "\n=== Smoke Test PASSED ===\n";
//	return 0;
//}