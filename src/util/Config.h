#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace util
{
    // 전략 설정
    struct StrategyConfig
    {
        // 거래소 최소 주문 금액. 진입 판단과 dust 이탈 판단에 모두 사용.
        // AccountConfig::init_dust_threshold_krw와 동일하게 유지해야 함.
        double min_notional_krw = 5000.0;
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
    struct AccountConfig
    {
        double coin_epsilon = 1e-8;              // 업비트 최소 수량 단위(8자리), 수량 기준 dust
        double krw_dust_threshold = 1.0;         // 원 단위 이하 KRW 잔량 정리 기준

        // 코인 가치 기준 dust. StrategyConfig::min_notional_krw와 동일하게 유지해야 함.
        double init_dust_threshold_krw = 5000.0;
    };

    // 봇 운영 설정 (거래 마켓 목록 등)
    struct BotConfig
    {
        // 거래할 마켓 목록
        // 환경 변수 UPBIT_MARKETS (CSV) 로 재정의 가능
        std::vector<std::string> markets = { "KRW-ADA", "KRW-ETH", "KRW-XRP" };

        // 실시간 WS에서 구독할 분봉 단위 (분)
        // 현재 구조는 단일 live unit만 가정한다.
        // 기본값 15를 유지해 배치 수집/대시보드의 기존 기준과 맞춘다.
        int live_candle_unit_minutes = 1;

        // SQLite DB 파일 경로 (실행 파일과 동일 디렉토리 기준 상대 경로)
        // EC2: systemd WorkingDirectory=/home/ubuntu/coinbot 설정 시 해당 위치에 생성
        std::string db_path = "db/coinbot.db";
    };

    // 통합 설정 (나중에 JSON 로딩 추가 가능)
    struct AppConfig
    {
        BotConfig bot;
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
