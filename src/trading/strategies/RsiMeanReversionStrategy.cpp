#include "RsiMeanReversionStrategy.h"

#include <algorithm> // std::max, std::min
#include <cmath>     // std::abs
#include <utility>   // std::move

// 재시작/멀티프로세스 안전한 client_order_id를 위해 UUID 사용
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/random_generator.hpp>

namespace trading::strategies {

    // thread_local: 멀티스레드 환경에서도 경쟁을 줄이고 생성기 안전성을 높임
    std::string makeUuidV4()
    {
        static thread_local boost::uuids::random_generator gen;
        return boost::uuids::to_string(gen());
    }



    RsiMeanReversionStrategy::RsiMeanReversionStrategy(std::string market, Params p)
        : market_(std::move(market)), params_(p)
    {
        // 지표 초기화(윈도우/길이 세팅)
        rsi_.reset(params_.rsiLength);
        closeN_.reset(params_.trendLookWindow);
        vol_.reset(params_.volatilityWindow);

        reset();
    }

    void RsiMeanReversionStrategy::reset()
    {
        // 상태 초기화
        state_ = State::Flat;
        pending_client_id_.reset();

        // 부분/복수 체결 누적값 초기화
        // - 실거래에서는 한 주문이 여러 번 나눠 체결될 수 있으므로,
        //   FillEvent가 올 때마다 누적하고, 최종 확정은 주문상태(Filled)에서 처리한다.
        pending_filled_volume_ = 0.0;
        pending_cost_sum_ = 0.0;
        pending_last_price_ = 0.0;

        // 포지션 정보 초기화
        entry_price_.reset();
        stop_price_.reset();
        target_price_.reset();

        // 지표 내부 상태 초기화
        rsi_.clear();
        closeN_.clear();
        vol_.clear();

        last_snapshot_ = Snapshot{};
    }

    void RsiMeanReversionStrategy::syncOnStart(const trading::PositionSnapshot& pos)
    {
        // - 미체결 주문은 앱/엔진 시작 루틴에서 “전부 취소”한다.
        // - 따라서 전략은 미체결을 이어받지 않고, 포지션만 복구한다.

        // 시작 시 pending 상태는 항상 제거(미체결 이어받기 X)
        pending_client_id_.reset();
        pending_filled_volume_ = 0.0;
        pending_cost_sum_ = 0.0;
        pending_last_price_ = 0.0;

        if (pos.hasPosition())
        {
            state_ = State::InPosition;

            // avg_entry_price가 신뢰 가능하면(entry/stop/target 복구 가능)
            if (pos.avg_entry_price > 0.0)
            {
                entry_price_ = pos.avg_entry_price;
                setStopsFromEntry(*entry_price_);
            }
            else
            {
                // 평균매수가가 없거나 신뢰가 낮으면,
                // 전략이 “임의로 기준가를 만들어” stop/target을 설정하는 것은 위험
                entry_price_.reset();
                stop_price_.reset();
                target_price_.reset();
            }
        }
        else
        {
            state_ = State::Flat;
            entry_price_.reset();
            stop_price_.reset();
            target_price_.reset();
        }
    }

    Decision RsiMeanReversionStrategy::onCandle(const core::Candle& c, const AccountSnapshot& account)
    {
        // 관점 A: market 고정 전략이므로 다른 market 봉이 들어오면 아무것도 하지 않음
        if (c.market != market_)
            return Decision::noAction();

        // 1) 지표/필터 스냅샷 생성(여기서 update가 모두 끝남)
        const Snapshot s = buildSnapshot(c);

        // 2) 상태에 따라 “진입” 또는 “청산” 판단
        switch (state_) {
        case State::Flat:
            return maybeEnter(s, account);

        case State::PendingEntry:
            // 주문 넣고 체결 대기 중에는 추가 주문 금지
            return Decision::noAction();

        case State::InPosition:
            return maybeExit(s, account);

        case State::PendingExit:
            return Decision::noAction();

        default:
            return Decision::noAction();
        }
    }

    Snapshot RsiMeanReversionStrategy::buildSnapshot(const core::Candle& c)
    {
        Snapshot s{};
        s.close = static_cast<double>(c.close_price);

        // --- 지표 업데이트(핵심: “한 봉에 한 번씩” update) ---
        s.rsi = rsi_.update(c);
        s.closeN = closeN_.update(c);
        s.volatility = vol_.update(c);



        // --- 파생 필터: trendStrength = abs(close - closeN) / closeN ---
        if (s.closeN.ready && s.closeN.v != 0.0) {
            s.trendReady = true;
            s.trendStrength = std::abs(s.close - s.closeN.v) / s.closeN.v;
        }
        else {
            s.trendReady = false;
            s.trendStrength = 0.0;
        }

        // --- 시장 적합성 판단(필터 종합) ---
        // 1) 추세가 너무 강하면 평균회귀가 “역추세 잡기”가 되어 위험해짐 -> 배제
        const bool trendOk = s.trendReady ? (s.trendStrength <= params_.maxTrendStrength) : false;

        // 2) 변동성이 너무 낮으면 “움직임이 없음” -> 목표/손절 도달이 어려움 -> 배제
        // ChangeVolatilityIndicator는 “수익률(퍼센트 변화율) 표준편차”를 반환하므로
        // minVolatility=0.01 은 “대략 1% 수준” 기준으로 해석 가능
        const bool volOk = s.volatility.ready ? (s.volatility.v >= params_.minVolatility) : false;

        // 3) RSI 준비 여부도 포함(준비 안 된 상태면 진입/청산 판단 자체를 유예하는게 안전)
        const bool rsiOk = s.rsi.ready;

        s.marketOk = (trendOk && volOk && rsiOk);

        // 마지막 스냅샷 저장 (테스트에서 RSI를 여기서 읽게 됨)
        last_snapshot_ = s;
        return s;
    }

    Decision RsiMeanReversionStrategy::maybeEnter(const Snapshot& s, const AccountSnapshot& account)
    {
        // 계좌에 매수할 KRW가 없으면 진입 불가
        if (!account.canBuy())
            return Decision::noAction();

        // 스냅샷이 “시장 적합성 OK”가 아니면 대기
        if (!s.marketOk)
            return Decision::noAction();

        // RSI 평균회귀 진입 조건(예: oversold 이하)
        if (!(s.rsi.v <= params_.oversold))
            return Decision::noAction();

        // 실제 매수 금액 = krw_available * riskPercent
        const double pct = std::clamp(params_.riskPercent, 0.0, 100.0);
        const double krw_to_use = account.krw_available * (pct / 100.0);

        // 너무 작은 주문 방지(데모/실거래 공통으로 “의미 없는 주문” 방지)
        if (krw_to_use <= 0.0)
            return Decision::noAction();

        // 주문 생성
        const std::string cid = makeIdentifier("entry");
        core::OrderRequest req = makeMarketBuyByAmount(krw_to_use, "entry");
        req.identifier = cid;

        // 이 주문에 대한 부분 체결 누적을 새로 시작
        pending_filled_volume_ = 0.0;
        pending_cost_sum_ = 0.0;
        pending_last_price_ = 0.0;

        // 상태 전이: Flat -> PendingEntry
        state_ = State::PendingEntry;
        pending_client_id_ = cid;

        // entry 가격은 onFill에서 누적하고, 최종 확정은 onOrderUpdate(Filled)에서
        return Decision::submit(std::move(req));
    }

    Decision RsiMeanReversionStrategy::maybeExit(const Snapshot& s, const AccountSnapshot& account)
    {
        // 보유 수량이 없으면(혹은 스냅샷 불일치) 안전하게 Flat으로 복귀시키는 것도 방법이지만,
        // 여기서는 “청산 주문 불가”로만 처리
        if (!account.canSell())
            return Decision::noAction();

        // InPosition 상태라면 entry/stop/target은 존재하는 것이 정상
        if (!entry_price_.has_value() || !stop_price_.has_value() || !target_price_.has_value())
            return Decision::noAction();

        const double close = s.close;
        const double stop = *stop_price_;
        const double target = *target_price_;

        // 청산 조건
        // 1) 손절
        const bool hitStop = (close <= stop);

        // 2) 익절
        const bool hitTarget = (close >= target);

        // 3) RSI가 과매수(overbought)면 되돌림으로 판단해서 청산(선택)
        const bool rsiExit = (s.rsi.ready && (s.rsi.v >= params_.overbought));

        if (!(hitStop || hitTarget || rsiExit))
            return Decision::noAction();

        const std::string cid = makeIdentifier("exit");
        core::OrderRequest req = makeMarketSellByVolume(account.coin_available, "exit");
        req.identifier = cid;

        // 이 주문에 대한 부분 체결 누적을 새로 시작
        pending_filled_volume_ = 0.0;
        pending_cost_sum_ = 0.0;
        pending_last_price_ = 0.0;

        // 상태 전이: InPosition -> PendingExit
        state_ = State::PendingExit;
        pending_client_id_ = cid;

        return Decision::submit(std::move(req));
    }

    // “부분체결 대응 필요”
    void RsiMeanReversionStrategy::onFill(const FillEvent& fill)
    {
        // 1) “내가 낸 pending 주문”인지 확인
        if (!pending_client_id_.has_value())
            return;

        if (fill.identifier != *pending_client_id_)
            return;

        // 2) 부분/복수 체결 누적
        // - FillEvent는 여러 번 올 수 있고, 이것만으로 완전 체결을 보장하지 않음
        // - 따라서 여기서는 "누적만" 하고, pending 해제/상태 확정은 onOrderUpdate(Filled)에서 수행한다.
        pending_last_price_ = fill.fill_price;

        // 일부 WS 구현/환경에서는 filled_volume이 0으로 올 수 있어 방어적으로 처리
        if (fill.filled_volume > 0.0) {
            pending_filled_volume_ += fill.filled_volume;
            pending_cost_sum_ += (fill.fill_price * fill.filled_volume);
        }
    }

    void RsiMeanReversionStrategy::onOrderUpdate(const OrderStatusEvent& ev)
    {
        // 주문상태 이벤트는 Pending 해제/확정/롤백의 기준점
        // - Rejected/Canceled: pending을 풀고 이전 상태로 롤백
        // - Filled: 누적 체결값(VWAP)으로 entry/exit를 최종 확정

        if (!pending_client_id_.has_value())
            return;

        if (ev.identifier != *pending_client_id_)
            return;

        // 1) 실패/취소: pending 해제 + 롤백
        if (ev.status == core::OrderStatus::Rejected || ev.status == core::OrderStatus::Canceled) 
        {
            if (pending_filled_volume_ <= 0.0)
            {
                if (state_ == State::PendingEntry)
                {
                    state_ = State::Flat;
                }
                else if (state_ == State::PendingExit)
                {
                    state_ = State::InPosition;
                }

                pending_client_id_.reset();
                pending_filled_volume_ = 0.0;
                pending_cost_sum_ = 0.0;
                pending_last_price_ = 0.0;
                return;
            }
            else
            {
                const double vwap = pending_cost_sum_ / pending_filled_volume_;
                if (state_ == State::PendingEntry) {
                    entry_price_ = vwap;
                    setStopsFromEntry(*entry_price_);
                    state_ = State::InPosition;
                }
                else if (state_ == State::PendingExit) {
                    // 부분 청산: 수량 추적은 계좌 스냅샷에 맡기고 InPosition 유지
                    state_ = State::InPosition;
                }
            }
        }

        // 2) 완전 체결: pending 해제 + 상태 확정
        if (ev.status == core::OrderStatus::Filled) 
        {
            // 평균 체결가(VWAP). 누적 수량이 없으면 마지막 가격을 폴백으로 사용
            const double final_price = (pending_filled_volume_ > 0.0)
                ? (pending_cost_sum_ / pending_filled_volume_)
                : pending_last_price_;

            if (state_ == State::PendingEntry) 
            {
                // 진입 확정
                if (final_price > 0.0) 
                {
                    entry_price_ = final_price;
                    setStopsFromEntry(*entry_price_);
                }
                state_ = State::InPosition;
            }
            else if (state_ == State::PendingExit) 
            {
                // 청산 확정
                state_ = State::Flat;
                entry_price_.reset();
                stop_price_.reset();
                target_price_.reset();
            }

            pending_client_id_.reset();
            pending_filled_volume_ = 0.0;
            pending_cost_sum_ = 0.0;
            pending_last_price_ = 0.0;
            return;
        }

        // Open/Pending/New 등의 상태는 그대로 유지한다.
        // - 상태 변화는 Filled/Canceled/Rejected에서만 확정
    };

    void RsiMeanReversionStrategy::setStopsFromEntry(double entry)
    {
        // 손절/익절 %는 “진입가 기준 퍼센트”
        const double sl = std::max(0.0, params_.stopLossPct);
        const double tp = std::max(0.0, params_.profitTargetPct);

        stop_price_ = entry * (1.0 - sl / 100.0);
        target_price_ = entry * (1.0 + tp / 100.0);
    }

    std::string RsiMeanReversionStrategy::makeIdentifier(std::string_view tag)
    {
        // 전략 내부 유니크 ID (demo/real 공통: client_order_id로 매칭)
        // 예: "rsi_mean_reversion:KRW-BTC:entry:1"
        ++seq_;

        std::string cid;
        cid.reserve(128);

        cid.append(id());
        cid.push_back(':');
        cid.append(market_);
        cid.push_back(':');
        cid.append(tag);
        cid.push_back(':');
        cid.append(makeUuidV4());

        return cid;
    }

    core::OrderRequest RsiMeanReversionStrategy::makeMarketBuyByAmount(double krw_amount, std::string_view tag)
    {
        core::OrderRequest req{};
        req.market = market_;
        req.position = core::OrderPosition::BID;
        req.type = core::OrderType::Market;

        // BID(매수)는 “금액(Amount)” 기준이 자연스러움
        req.size = core::AmountSize{ krw_amount };

        req.price.reset(); // 시장가
        req.strategy_id = std::string(id());
        req.client_tag = std::string(tag);

        // client_order_id는 바깥에서 생성해 주입(상태 전이와 맞물리기 때문)
        req.identifier.clear();
        return req;
    }

    core::OrderRequest RsiMeanReversionStrategy::makeMarketSellByVolume(double volume, std::string_view tag)
    {
        core::OrderRequest req{};
        req.market = market_;
        req.position = core::OrderPosition::ASK;
        req.type = core::OrderType::Market;

        // ASK(매도)는 “수량(Volume)” 기준이 자연스러움
        req.size = core::VolumeSize{ volume };

        req.price.reset(); // 시장가
        req.strategy_id = std::string(id());
        req.client_tag = std::string(tag);

        req.identifier.clear();
        return req;
    }

} // namespace trading::strategies
