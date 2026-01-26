#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <algorithm>

#include "UpbitWebSocketClient.h"

namespace api::ws {
    namespace
    {
        /*
         * 왜 decorator가 필요한가?
         * - Authorization 헤더는 WebSocket "핸드셰이크(HTTP Upgrade)" 요청에 포함되어야 한다.
         * - ws.handshake() 이후에는 이미 Upgrade 요청이 끝났으므로 헤더 삽입이 불가능하다.
         * - 따라서 handshake 호출 '직전'에 decorator로 request header를 꾸민다.
         */
        void applyAuthorizationDecorator(
            websocket::stream<beast::ssl_stream<beast::tcp_stream>>& ws,
            const std::optional<std::string>& bearer_jwt)
        {
            if (!bearer_jwt.has_value()) return;

            // 업비트 private WS 인증 형식: Authorization: Bearer <JWT>
            const std::string auth = *bearer_jwt;

            ws.set_option(websocket::stream_base::decorator(
                [auth](websocket::request_type& req)
                {
                    req.set(http::field::authorization, auth);
                }));
        }

        // runReadLoop에서 “주기적으로 깨어나 커맨드/핑을 처리”하기 위한 타임아웃(너무 짧지 않게)
        constexpr std::chrono::seconds kIdleReadTimeout{ 1 };
    }

    UpbitWebSocketClient::UpbitWebSocketClient(
        boost::asio::io_context& ioc,
        boost::asio::ssl::context& ssl_ctx)
        : ioc_(ioc)
        , ssl_ctx_(ssl_ctx)
        , resolver_(boost::asio::make_strand(ioc))  // 문법 유의
    {
        // ws_는 connectImpl에서 resetStream으로 처음 생성
    }

    // 외부에서 메시지 처리 로직
    void UpbitWebSocketClient::setMessageHandler(MessageHandler cb) {
        on_msg_ = std::move(cb);
    }

    void UpbitWebSocketClient::pushCommand(Command c)
    {
        {
            std::lock_guard lk(cmd_mu_);
            cmd_q_.push_back(std::move(c));
        }
        cmd_cv_.notify_one();
    }

    // 연결/재연결에 필요한 내부 유틸
    void UpbitWebSocketClient::resetStream()
    {
        ws_ = std::make_unique<WsStream>(ioc_, ssl_ctx_);

        // “커맨드 처리/핑 처리”를 위해 read가 영원히 블로킹되지 않게 idle timeout을 짧게 둔다.
        websocket::stream_base::timeout opt{};
        opt.handshake_timeout = std::chrono::seconds(30);
        opt.idle_timeout = kIdleReadTimeout;
        opt.keep_alive_pings = false; // ping은 우리가 직접 보냄
        ws_->set_option(opt);
    }

    bool UpbitWebSocketClient::sendTextFrame(const std::string& text)
    {
        if (!ws_ || !ws_->is_open()) return false;

        boost::system::error_code ec;
        ws_->text(true);
        ws_->write(boost::asio::buffer(text), ec);

        if (ec)
        {
            std::cout << "[WS] write error: " << ec.message() << "\n";
            return false;
        }
        return true;
    }

    std::chrono::milliseconds UpbitWebSocketClient::computeReconnectDelay_()
    {
        // failures_는 "이번 재연결 시도 직전" 이미 증가된 상태라고 가정
        // failures_=1 -> min_backoff
        // failures_=2 -> min_backoff*2
        // failures_=3 -> min_backoff*4 ...
        const std::uint32_t f = reconnect_failures_;

        // 지수 성장의 폭을 제한(너무 큰 shift 방지)
        const std::uint32_t exp = std::min<std::uint32_t>(f > 0 ? (f - 1) : 0, 10); // 2^10=1024배까지만
        const long long base_ms =
            reconnect_min_backoff_.count() * (1LL << exp);

        const long long capped_ms =
            std::min<long long>(base_ms, reconnect_max_backoff_.count());

        // jitter: [base*(1-j), base*(1+j)]
        const double j = std::max(0.0, reconnect_jitter_ratio_);
        const long long lo = static_cast<long long>(capped_ms * (1.0 - j));
        const long long hi = static_cast<long long>(capped_ms * (1.0 + j));

        // 최소 0ms 방지
        const long long lo2 = std::max<long long>(0, lo);
        const long long hi2 = std::max<long long>(lo2 + 1, hi); // range 보장

        // 간단한 per-call RNG (thread_local로 비용/경합 줄임)
        thread_local std::mt19937 rng{
            static_cast<unsigned>(
                std::chrono::steady_clock::now().time_since_epoch().count())
        };
        std::uniform_int_distribution<long long> dist(lo2, hi2);

        return std::chrono::milliseconds(dist(rng));
    }

    bool UpbitWebSocketClient::reconnectOnce()
    {
        // stop 요청이면 재연결 시도 자체를 하지 않음
        if (stop_.load(std::memory_order_relaxed))
            return false;

        // 이번 시도는 "연속 실패 +1"로 계산
        ++reconnect_failures_;

        const auto delay = computeReconnectDelay_();
        std::cout << "[WS] reconnect attempt=" << reconnect_failures_
            << " sleep=" << delay.count() << "ms\n";

        // sleep 중에도 stop이 들어올 수 있으니, 짧게 쪼개서 체크(과한 변경 없이 안전성↑)
        // (원하면 단순 sleep_for(delay)로도 충분)
        auto remaining = delay;
        while (remaining.count() > 0 && !stop_.load(std::memory_order_relaxed))
        {
            const auto step = std::min<std::chrono::milliseconds>(remaining, std::chrono::milliseconds(200));
            std::this_thread::sleep_for(step);
            remaining -= step;
        }

        if (stop_.load(std::memory_order_relaxed))
            return false;

        // 기존 ws_ 정리
        if (ws_ && ws_->is_open())
        {
            boost::system::error_code ignore;
            ws_->close(websocket::close_code::normal, ignore);
        }
        ws_.reset();

        // 마지막 연결 정보로 재접속
        connectImpl(host_, port_, target_, bearer_jwt_);
        const bool ok = (ws_ && ws_->is_open());

        if (ok)
        {
            // 성공 시 연속 실패 카운터 reset
            reconnect_failures_ = 0;
            std::cout << "[WS] reconnect success\n";
        }
        else
        {
            std::cout << "[WS] reconnect failed (will backoff)\n";
        }

        return ok;
    }


    void UpbitWebSocketClient::resubscribeAll()
    {
        // 마지막으로 보냈던 subscribe frame들을 재전송
        // (재연결 직후, 서버에는 구독 상태가 없으므로 반드시 다시 보내야 한다.)
        for (const auto& [key, frame] : last_sub_frames_)
        {
            (void)sendTextFrame(frame);
        }

        std::cout << "[WS] resubscribe done. count=" << last_sub_frames_.size() << "\n";
    }

    // WebSocket 연결
    void UpbitWebSocketClient::connectPublic(
        const std::string& host,
        const std::string& port,
        const std::string& target) 
    {
        // public 채널은 인증이 필요 없다.
        pushCommand(CmdConnect{ host, port, target, std::nullopt });
    }

    void UpbitWebSocketClient::connectPrivate(
        const std::string& host,
        const std::string& port,
        const std::string& target,
        const std::string& bearer_jwt)
    {
        // private 채널은 handshake 요청에 Authorization 헤더가 필수다.
        // bearer_jwt가 비어있으면 실수이므로 연결은 되더라도 구독이 제대로 안 될 수 있다.
        // (여기서는 방어적으로 그냥 연결 시도 대신, caller가 실수하지 않게 구조를 분리하는 것이 핵심)
        pushCommand(CmdConnect{ host, port, target, bearer_jwt });
    }

    void UpbitWebSocketClient::connectImpl(
        const std::string& host,
        const std::string& port,
        const std::string& target,
        std::optional<std::string> bearer_jwt)
    {
        host_ = host;
        port_ = port;
        target_ = target;
        bearer_jwt_ = std::move(bearer_jwt);

        resetStream();

        boost::system::error_code ec;

        auto results = resolver_.resolve(host, port, ec);
        std::cout << "[WS] resolve: " << (ec ? ec.message() : "OK") << "\n";
        if (ec) return;

        beast::get_lowest_layer(*ws_).connect(results, ec);
        std::cout << "[WS] tcp connect: " << (ec ? ec.message() : "OK") << "\n";
        if (ec) return;

        if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host.c_str()))
        {
            std::cout << "[WS] SNI: FAIL\n";
            return;
        }
        std::cout << "[WS] SNI: OK\n";

        ws_->next_layer().handshake(boost::asio::ssl::stream_base::client, ec);
        std::cout << "[WS] tls handshake: " << (ec ? ec.message() : "OK") << "\n";
        if (ec) return;

        applyAuthorizationDecorator(*ws_, bearer_jwt_);

        ws_->handshake(host, target, ec);
        std::cout << "[WS] ws handshake: " << (ec ? ec.message() : "OK") << "\n";
        if (ec) return;

        std::cout << "[WS] Connected Upbit"
            << (bearer_jwt_.has_value() ? " (private)" : " (public)")
            << "\n";
    }

    // 캔들 구독 요청 전송
    // - 이 함수는 “요청 생성 + 전송”만 담당
    void UpbitWebSocketClient::subscribeCandles(
        const std::string& type,
        const std::vector<std::string>& markets,
        bool is_only_snapshot,
        bool is_only_realtime,
        const std::string& format) {

        pushCommand(CmdSubCandles{ type, markets, is_only_snapshot, is_only_realtime, format });
    }

    void UpbitWebSocketClient::subscribeMyOrder(
        const std::vector<std::string>& markets,
        bool is_only_realtime,
        const std::string& format)
    {
        pushCommand(CmdSubMyOrder{ markets, is_only_realtime, format });
    }
    // 메시지 수신 루프
    // - 서버가 보내는 데이터를 계속 읽는다
    // - 이 루프가 살아 있어야 연결도 유지
    void UpbitWebSocketClient::runReadLoop() 
    {
        stop_.store(false, std::memory_order_relaxed);

        beast::flat_buffer buffer;
        auto next_ping = std::chrono::steady_clock::now() + ping_interval_;

        while (!stop_.load(std::memory_order_relaxed))
        {
            // 1) 커맨드 처리 (한 번에 여러 개 처리)
            {
                // 커맨드는 여기서 "즉시 대기"하지 않고 한 번에 비우는 방식으로 처리한다.
                // 실질적인 반영 지연은 read idle timeout(최대 1초) 주기에 의해 결정된다.    
                std::deque<Command> local;
                {
                    std::unique_lock lk(cmd_mu_);
                    local.swap(cmd_q_);
                }

                for (auto& c : local)
                {
                    if (std::holds_alternative<CmdClose>(c))
                    {
                        stop_.store(true, std::memory_order_relaxed);
                        break;
                    }

                    if (auto* cc = std::get_if<CmdConnect>(&c))
                    {
                        connectImpl(cc->host, cc->port, cc->target, cc->bearer_jwt);
                        if (ws_ && ws_->is_open())
                            resubscribeAll();
                        continue;
                    }

                    if (auto* sc = std::get_if<CmdSubCandles>(&c))
                    {
                        const std::string ticket = makeTicket();
                        const std::string frame = buildCandleSubJsonFrame(
                            ticket, sc->type, sc->markets,
                            sc->is_only_snapshot, sc->is_only_realtime, sc->format);

                        last_sub_frames_[sc->type] = frame;
                        (void)sendTextFrame(frame);
                        std::cout << "[WS] Candle Subscribe sent: " << sc->type << "\n";
                        continue;
                    }

                    if (auto* sm = std::get_if<CmdSubMyOrder>(&c))
                    {
                        const std::string ticket = makeTicket();
                        const std::string frame = buildMyOrderSubJsonFrame(
                            ticket, sm->markets, sm->is_only_realtime, sm->format);

                        last_sub_frames_["myOrder"] = frame;
                        (void)sendTextFrame(frame);
                        std::cout << "[WS] MyOrder Subscribe sent\n";
                        continue;
                    }
                }
            }

            if (stop_.load(std::memory_order_relaxed))
                break;

            // 2) ping 처리(단일 루프 내장)
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_ping)
            {
                next_ping = now + ping_interval_;

                if (ws_ && ws_->is_open())
                {
                    boost::system::error_code ec;
                    ws_->ping({}, ec);
                    if (ec)
                    {
                        std::cout << "[WS] ping error: " << ec.message() << "\n";

                        // ping/read 에러 처리: stop이면 break (재연결로 안 감)
                        if (stop_.load(std::memory_order_relaxed))
                            break;

                        const bool ok = reconnectOnce();
                        if (ok) resubscribeAll();
                    }
                }
            }

            // 3) read (idle timeout으로 1초마다 깨어나서 stop/command를 체크할 수 있음)
            if (!ws_ || !ws_->is_open())
            {
                // 연결 전이면 너무 바쁘게 돌지 않도록 잠깐 쉼
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            buffer.clear();
            boost::system::error_code ec;
            ws_->read(buffer, ec);

            if (ec)
            {
                // idle timeout: 정상(메시지 없을 뿐)
                if (ec == beast::error::timeout || ec == boost::asio::error::timed_out)
                    continue;

                std::cout << "[WS] read error: " << ec.message() << "\n";

                // stop이면 break (재연결로 안 감)
                if (stop_.load(std::memory_order_relaxed))
                    break;

                const bool ok = reconnectOnce();
                if (ok) resubscribeAll();
                continue;
            }

            const std::string msg = beast::buffers_to_string(buffer.data());

            /// (옵션) 짧은 로그
            // 캔들(candle.*) 메시지는 동일 ts로 반복 업데이트가 자주 와서 로그/IO 병목이 됨.
            // Runner에서 "새 캔들만" 로그를 찍도록 하고, 여기서는 캔들 원문 로그를 생략한다.
            const bool is_candle =
                (msg.find("\"type\"") != std::string::npos) &&
                (msg.find("\"candle.") != std::string::npos);

            if (!is_candle)
            {
                constexpr std::size_t kMaxLog = 200;
                if (msg.size() <= kMaxLog) std::cout << "[WS] RX: " << msg << "\n";
                else std::cout << "[WS] RX: " << msg.substr(0, kMaxLog) << "...\n";
            }

            if (on_msg_)
                on_msg_(std::string_view(msg));
        }

        // 4) 종료 시 close
        if (ws_ && ws_->is_open())
        {
            boost::system::error_code ignore;
            ws_->close(websocket::close_code::normal, ignore);
            std::cout << "[WS] close: " << (ignore ? ignore.message() : "OK") << "\n";
        }
    }

    // 정상 종료
    void UpbitWebSocketClient::close() {

        stop_.store(true, std::memory_order_relaxed);

        pushCommand(CmdClose{});

        cmd_cv_.notify_one();
    }

    // ticket 생성
    std::string UpbitWebSocketClient::makeTicket() {
        // 요청을 구분하기 위한 임의 문자열 생성
        std::mt19937_64 rng(
            std::chrono::steady_clock::now()
            .time_since_epoch().count());

        std::ostringstream oss;
        oss << "ticket-" << std::hex << rng();
        return oss.str();
    }

    // 캔들 구독 JSON 프레임 생성
    /*
    * 요청 구조:
    [
        { "ticket": "..." },
        {
            "type": "candle.1s",
            "codes": ["KRW-BTC"]
        },
         { "format": "DEFAULT" }
    ]
    */
    std::string UpbitWebSocketClient::buildCandleSubJsonFrame(
        const std::string& ticket,
        const std::string& type,
        const std::vector<std::string>& markets,
        bool is_only_snapshot,
        bool is_only_realtime,
        const std::string& format) {


        nlohmann::json root = nlohmann::json::array();

        // 요청 식별자
        root.push_back({ {"ticket", ticket} });

        // 실제 구독 대상
        nlohmann::json body;
        body["type"] = type;
        body["codes"] = markets;
        body["is_only_snapshot"] = is_only_snapshot;
        body["is_only_realtime"] = is_only_realtime;

        root.push_back(body);

        // 응답 포맷 지정
        root.push_back({ {"format", format} });

        return root.dump();
    }

    std::string UpbitWebSocketClient::buildMyOrderSubJsonFrame(
        const std::string& ticket,
        const std::vector<std::string>& markets,
        bool is_only_realtime,
        const std::string& format)
    {
        // myOrder도 동일한 배열 구조로 구독 프레임을 전송한다.
        // [
        //   {"ticket":"..."},
        //   {"type":"myOrder","codes":["KRW-BTC"],"is_only_realtime":true},
        //   {"format":"DEFAULT"}
        // ]
        nlohmann::json root = nlohmann::json::array();
        root.push_back({ {"ticket", ticket} });

        nlohmann::json body;
        body["type"] = "myOrder";
        body["codes"] = markets;
        body["is_only_realtime"] = is_only_realtime;
        root.push_back(body);

        root.push_back({ {"format", format} });
        return root.dump();
    }

} // namespace api::ws
