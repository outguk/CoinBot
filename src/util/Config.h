#pragma once

#include <chrono>
#include <cstddef>

namespace util
{
    // 전략 설정
    struct StrategyConfig
    {
        double min_notional_krw = 5000.0;       // 최소 주문 금액 (KRW)
        double volume_safety_eps = 1e-7;        // 수량 오버셀 방지 여유분 (AccountConfig::coin_epsilon과 일치)
    };

    // 엔진 설정
    struct EngineConfig
    {
        std::size_t max_seen_trades = 20000;    // 중복 체결 방지 세트 크기
        int max_private_batch = 256;            // myOrder 배치 크기

        // 예약 여유분 (수수료 0.05% 커버 + 안전 마진)
        // 시장가 매수 시 executed_funds + fee가 예약을 초과하지 않도록 방지
        // 1.001 = 0.01% 여유 (수수료 0.005% + 추가 0.005%)
        double reserve_margin = 1.001;

        // 기본 거래 수수료 (Upbit 기준)
        // trade_fee 필드 누락 시 fallback으로 사용 (리스크 3 대응)
        // Upbit 표준 수수료: 0.05% (Maker/Taker 동일)
        double default_trade_fee_rate = 0.0005; // 0.05%
    };

    // 이벤트 브릿지 설정
    struct EventBridgeConfig
    {
        std::size_t max_backlog = 5000;         // 시장 데이터 큐 백프레셔 한계
    };

    // WebSocket 설정
    struct WebSocketConfig
    {
        std::chrono::seconds idle_timeout{1};   // 유휴 타임아웃
        int max_reconnect_attempts = 5;         // 재연결 최대 시도
    };

    // 자산 관리 설정 (AccountManager)
    //
    // [Dust 처리 정책 - 이중 체크]
    // AccountManager와 전략이 동일한 기준으로 "의미 있는 포지션"을 판단해야 상태 불일치 방지
    //
    // 1. 수량 기준 (coin_epsilon): 부동소수점 오차 제거
    //    - formatDecimalFloor(8자리)로 인한 미세 잔량
    //    - 사용 위치: finalizeFillSell, syncWithAccount (코인 식별)
    //
    // 2. 가치 기준 (init_dust_threshold_krw): 거래 불가 잔량 제거
    //    - 거래소 최소 주문 금액 미만의 코인
    //    - 사용 위치: 생성자, syncWithAccount, finalizeFillSell
    //    - 전략 일관성: RsiMeanReversionStrategy의 hasMeaningfulPos와 동일 기준
    //
    struct AccountConfig
    {
        // [1] 수량 기준 dust (부동소수점 오차)
        // formatDecimalFloor(8자리)로 인한 미세 잔량 처리
        // StrategyConfig::volume_safety_eps와 일치
        double coin_epsilon = 1e-7;             // 0.0000001 BTC

        // [2] KRW dust (원 단위 이하 잔량)
        // 주문 완료 후 reserved_krw 정리
        double krw_dust_threshold = 10.0;       // 10원 미만

        // [3] 가치 기준 dust (거래 불가 잔량)
        // 코인 가치 < 이 값 → dust로 처리
        // StrategyConfig::min_notional_krw과 동일 (거래소 최소 주문 금액)
        // 사용: 생성자, syncWithAccount, finalizeFillSell
        double init_dust_threshold_krw = 5000.0; // 5000원 미만
    };

    // 통합 설정 (나중에 JSON 로딩 추가 가능)
    struct AppConfig
    {
        StrategyConfig strategy;
        EngineConfig engine;
        EventBridgeConfig event_bridge;
        WebSocketConfig websocket;
        AccountConfig account;

        // 싱글톤 접근
        static AppConfig& instance()
        {
            static AppConfig config;
            return config;
        }
    };

} // namespace util
