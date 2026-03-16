// api/ws/UpbitWebSocketClient.h
//
// 업비트 WebSocket 클라이언트
// - TLS 연결, 캔들/myOrder 구독, raw JSON EventRouter로 수신
// - 전략/도메인 파싱은 담당하지 않음
//
// 생명주기: setMessageHandler → connectPublic/Private → subscribeXxx → start() → stop()
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>       // std::jthread, std::stop_token (C++20)
#include <unordered_map>
#include <variant>
#include <vector>
#include <cstdint>

namespace api::ws
{
    using tcp = boost::asio::ip::tcp;
    namespace beast     = boost::beast;
    namespace websocket = beast::websocket;
    namespace http      = beast::http;

    class UpbitWebSocketClient final
    {
    public:
        // TCP 위에 TLS를 올리고, 그 위에 WebSocket을 올린 최종 통신 객체
        using WsStream          = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;
        using MessageHandler    = std::function<void(std::string_view)>; // raw JSON
        using ReconnectCallback = std::function<void()>;  // 재연결 성공 후 호출
        using FatalCallback     = std::function<void()>;  // 재연결 한도 초과 시 호출

        UpbitWebSocketClient(boost::asio::io_context& ioc,
                             boost::asio::ssl::context& ssl_ctx);
        ~UpbitWebSocketClient();

        // 복사/이동 금지 (io_context 참조 보유)
        UpbitWebSocketClient(const UpbitWebSocketClient&) = delete;
        UpbitWebSocketClient& operator=(const UpbitWebSocketClient&) = delete;

        // ---- 연결 명령 등록 API (커맨드 큐 경유, start() 전후 어느 시점에든 호출 가능) ----

        // PUBLIC 채널 연결 (캔들 등 인증 불필요)
        void connectPublic(const std::string& host,
                           const std::string& port,
                           const std::string& target);

        // PRIVATE 채널 연결 (myOrder 등 JWT 인증 필요)
        void connectPrivate(const std::string& host,
                            const std::string& port,
                            const std::string& target,
                            const std::string& bearer_jwt);

        // ---- 구독 요청 예약 ----

        // 캔들 구독: type = "candle.1s", "candle.1m" 등
        void subscribeCandles(const std::string& type,
                              const std::vector<std::string>& markets,
                              bool is_only_snapshot = false,
                              bool is_only_realtime = false,
                              const std::string& format = "DEFAULT");

        // 내 주문 체결 구독
        void subscribeMyOrder(const std::vector<std::string>& markets,
                              bool is_only_realtime = true,
                              const std::string& format = "DEFAULT");

        // ---- 애플리케이션 레벨 하트비트 ----

        enum class HeartbeatMode { None, UpbitTextPing };

        // start() 전에만 호출할 것 — runReadLoop_()와 데이터 레이스 방지
        void setHeartbeatMode(HeartbeatMode mode) { heartbeat_mode_ = mode; }
        void setHeartbeatInterval(std::chrono::seconds s) { heartbeat_interval_ = s; }

        // ---- 수신 콜백 ----

        // start() 전에 설정 권장
        void setMessageHandler(MessageHandler cb);

        // 재연결 성공 + 재구독 완료 후 호출되는 콜백 (계좌 동기화 트리거 등)
        void setReconnectCallback(ReconnectCallback cb);

        // 재연결 한도 초과 시 호출되는 콜백 (systemd 재시작 유발용)
        // 주의: start() 전에만 호출할 것
        void setFatalCallback(FatalCallback cb);

        // ---- 생명주기 ----

        // 내부 jthread 시작 (수신 루프 가동)
        void start();

        // 수신 루프 종료 + join (소멸자에서도 자동 호출)
        void stop();

    private:
        // ---- 커맨드 큐 타입 ----
        struct CmdConnect {
            std::string host, port, target;
            std::optional<std::string> bearer_jwt; // nullopt → public
        };
        struct CmdSubCandles {
            std::string type;
            std::vector<std::string> markets;
            bool is_only_snapshot{};
            bool is_only_realtime{};
            std::string format;
        };
        struct CmdSubMyOrder {
            std::vector<std::string> markets;
            bool is_only_realtime{};
            std::string format;
        };

        using Command = std::variant<CmdConnect, CmdSubCandles, CmdSubMyOrder>;

        void pushCommand(Command c);

        // ws_ 생성/정리
        void resetStream();

        // WS 스레드 전용 텍스트 프레임 송신
        bool sendTextFrame(const std::string& text);

        // 재연결 + 재구독
        bool reconnectOnce_(std::stop_token stoken);
        void resubscribeAll();

        // 다음 재연결 sleep 시간 계산 (지수 backoff + jitter)
        std::chrono::milliseconds computeReconnectDelay_();

        // 내부 공통 연결 루틴: resolve → tcp → tls → ws handshake
        void connectImpl(const std::string& host,
                         const std::string& port,
                         const std::string& target,
                         std::optional<std::string> bearer_jwt);

        // 수신 루프 (jthread에서 실행, stop_token으로 종료 감지)
        void runReadLoop_(std::stop_token stoken);

        // 구독 요청 JSON 프레임 생성
        static std::string makeTicket();
        static std::string buildCandleSubJsonFrame(
            const std::string& ticket,
            const std::string& type,
            const std::vector<std::string>& markets,
            bool is_only_snapshot,
            bool is_only_realtime,
            const std::string& format);
        static std::string buildMyOrderSubJsonFrame(
            const std::string& ticket,
            const std::vector<std::string>& markets,
            bool is_only_realtime,
            const std::string& format);

        // ---- 멤버 변수 ----

        boost::asio::io_context&  ioc_;
        boost::asio::ssl::context& ssl_ctx_;

        tcp::resolver resolver_;

        // WebSocket stream (재연결 시 새로 생성하므로 포인터 보관)
        std::unique_ptr<WsStream> ws_;

        // ws_ 수명 보호 (stop_callback의 cancel과 reconnect의 reset 동기화)
        std::mutex ws_mu_;

        // 내부 수신 스레드 (stop_token 내장)
        std::jthread thread_;

        // ping 주기 / 재연결 backoff / 텍스트 하트비트
        std::chrono::seconds      ping_interval_{ 25 };
        HeartbeatMode             heartbeat_mode_{ HeartbeatMode::None };
        std::chrono::seconds      heartbeat_interval_{ 60 };
        std::chrono::milliseconds reconnect_min_backoff_{ 500 };
        std::chrono::milliseconds reconnect_max_backoff_{ 30'000 };
        double                    reconnect_jitter_ratio_{ 0.20 };

        std::uint32_t reconnect_failures_{ 0 };

        // 연결 정보 저장 (재연결 대비)
        std::string host_;
        std::string port_;
        std::string target_;
        std::optional<std::string> bearer_jwt_;

        // 재연결 후 재구독을 위해 마지막 subscribe frame 보관
        std::unordered_map<std::string, std::string> last_sub_frames_;

        // 커맨드 큐
        std::mutex cmd_mu_;
        std::deque<Command> cmd_q_;

        // 수신 콜백
        MessageHandler on_msg_;

        // 재연결 성공 콜백
        ReconnectCallback on_reconnect_;

        // 재연결 한도 초과 콜백
        FatalCallback fatal_cb_{};
    };

} // namespace api::ws
