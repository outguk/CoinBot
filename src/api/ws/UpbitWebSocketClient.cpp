// api/ws/UpbitWebSocketClient.cpp

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

#include "UpbitWebSocketClient.h"
#include "util/Config.h"

namespace api::ws {

namespace {

    // Authorization 헤더는 WebSocket 핸드셰이크(HTTP Upgrade) 요청에 포함되어야 한다.
    // ws.handshake() 이후에는 Upgrade 요청이 끝났으므로, 직전에 decorator로 삽입한다.
    void applyAuthorizationDecorator(
        websocket::stream<beast::ssl_stream<beast::tcp_stream>>& ws,
        const std::optional<std::string>& bearer_jwt)
    {
        if (!bearer_jwt.has_value()) return;

        const std::string auth = *bearer_jwt;
        ws.set_option(websocket::stream_base::decorator(
            [auth](websocket::request_type& req) {
                req.set(http::field::authorization, auth);
            }));
    }

    // 커맨드/종료 반응성/CPU 균형값
    constexpr std::chrono::milliseconds kIdleReadTimeout{ 200 };

} // anonymous namespace

// ========== 생성자 / 소멸자 ==========

UpbitWebSocketClient::UpbitWebSocketClient(
    boost::asio::io_context& ioc,
    boost::asio::ssl::context& ssl_ctx)
    : ioc_(ioc)
    , ssl_ctx_(ssl_ctx)
    , resolver_(boost::asio::make_strand(ioc))
{
    // ws_는 connectImpl에서 resetStream으로 처음 생성
}

UpbitWebSocketClient::~UpbitWebSocketClient()
{
    stop();
}

// ========== start / stop ==========

void UpbitWebSocketClient::start()
{
    if (thread_.joinable()) return; // 이미 실행 중

    thread_ = std::jthread([this](std::stop_token stoken) {
        runReadLoop_(stoken);
    });
}

void UpbitWebSocketClient::stop()
{
    // request_stop() → runReadLoop_ 내 stoken.stop_requested() 감지
    thread_.request_stop();
    if (thread_.joinable())
        thread_.join();
}

// ========== 수신 콜백 ==========

void UpbitWebSocketClient::setMessageHandler(MessageHandler cb)
{
    on_msg_ = std::move(cb);
}

// ========== 커맨드 큐 ==========

void UpbitWebSocketClient::pushCommand(Command c)
{
    std::lock_guard lk(cmd_mu_);
    cmd_q_.push_back(std::move(c));
}

// ========== 연결 예약 ==========

void UpbitWebSocketClient::connectPublic(
    const std::string& host,
    const std::string& port,
    const std::string& target)
{
    pushCommand(CmdConnect{ host, port, target, std::nullopt });
}

void UpbitWebSocketClient::connectPrivate(
    const std::string& host,
    const std::string& port,
    const std::string& target,
    const std::string& bearer_jwt)
{
    pushCommand(CmdConnect{ host, port, target, bearer_jwt });
}

// ========== 구독 예약 ==========

void UpbitWebSocketClient::subscribeCandles(
    const std::string& type,
    const std::vector<std::string>& markets,
    bool is_only_snapshot,
    bool is_only_realtime,
    const std::string& format)
{
    pushCommand(CmdSubCandles{ type, markets, is_only_snapshot, is_only_realtime, format });
}

void UpbitWebSocketClient::subscribeMyOrder(
    const std::vector<std::string>& markets,
    bool is_only_realtime,
    const std::string& format)
{
    pushCommand(CmdSubMyOrder{ markets, is_only_realtime, format });
}

// ========== 내부 유틸 ==========

void UpbitWebSocketClient::resetStream()
{
    ws_ = std::make_unique<WsStream>(ioc_, ssl_ctx_);

    websocket::stream_base::timeout opt{};
    opt.handshake_timeout = std::chrono::seconds(30);
    opt.idle_timeout      = kIdleReadTimeout;
    opt.keep_alive_pings  = false; // ping은 직접 전송
    ws_->set_option(opt);
}

bool UpbitWebSocketClient::sendTextFrame(const std::string& text)
{
    if (!ws_ || !ws_->is_open()) return false;

    boost::system::error_code ec;
    ws_->text(true);
    ws_->write(boost::asio::buffer(text), ec);

    if (ec) {
        std::cout << "[WS] write error: " << ec.message() << "\n";
        return false;
    }
    return true;
}

std::chrono::milliseconds UpbitWebSocketClient::computeReconnectDelay_()
{
    const std::uint32_t f   = reconnect_failures_;
    const std::uint32_t exp = std::min<std::uint32_t>(f > 0 ? (f - 1) : 0, 10);
    const long long base_ms = reconnect_min_backoff_.count() * (1LL << exp);
    const long long capped  = std::min<long long>(base_ms, reconnect_max_backoff_.count());

    const double j  = std::max(0.0, reconnect_jitter_ratio_);
    const long long lo = std::max<long long>(0, static_cast<long long>(capped * (1.0 - j)));
    const long long hi = std::max<long long>(lo + 1, static_cast<long long>(capped * (1.0 + j)));

    thread_local std::mt19937 rng{
        static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count())
    };
    return std::chrono::milliseconds(std::uniform_int_distribution<long long>(lo, hi)(rng));
}

bool UpbitWebSocketClient::reconnectOnce_(std::stop_token stoken)
{
    if (stoken.stop_requested()) return false;

    ++reconnect_failures_;

    const auto delay = computeReconnectDelay_();
    std::cout << "[WS] reconnect attempt=" << reconnect_failures_
              << " sleep=" << delay.count() << "ms\n";

    // 긴 backoff 중에도 stop 요청을 짧게 체크
    auto remaining = delay;
    while (remaining.count() > 0 && !stoken.stop_requested()) {
        const auto step = std::min<std::chrono::milliseconds>(remaining, std::chrono::milliseconds(50));
        std::this_thread::sleep_for(step);
        remaining -= step;
    }

    if (stoken.stop_requested()) return false;

    if (ws_ && ws_->is_open()) {
        boost::system::error_code ignore;
        ws_->close(websocket::close_code::normal, ignore);
    }
    ws_.reset();

    connectImpl(host_, port_, target_, bearer_jwt_);
    const bool ok = (ws_ && ws_->is_open());

    if (ok) {
        reconnect_failures_ = 0;
        std::cout << "[WS] reconnect success\n";
    } else {
        std::cout << "[WS] reconnect failed (will backoff)\n";
    }
    return ok;
}

void UpbitWebSocketClient::resubscribeAll()
{
    for (const auto& [key, frame] : last_sub_frames_)
        (void)sendTextFrame(frame);

    std::cout << "[WS] resubscribe done. count=" << last_sub_frames_.size() << "\n";
}

void UpbitWebSocketClient::connectImpl(
    const std::string& host,
    const std::string& port,
    const std::string& target,
    std::optional<std::string> bearer_jwt)
{
    host_       = host;
    port_       = port;
    target_     = target;
    bearer_jwt_ = std::move(bearer_jwt);

    resetStream();

    boost::system::error_code ec;

    auto results = resolver_.resolve(host, port, ec);
    std::cout << "[WS] resolve: " << (ec ? ec.message() : "OK") << "\n";
    if (ec) return;

    beast::get_lowest_layer(*ws_).connect(results, ec);
    std::cout << "[WS] tcp connect: " << (ec ? ec.message() : "OK") << "\n";
    if (ec) return;

    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host.c_str())) {
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

    std::cout << "[WS] Connected"
              << (bearer_jwt_.has_value() ? " (private)" : " (public)") << "\n";
}

// ========== 수신 루프 (jthread 진입점) ==========

void UpbitWebSocketClient::runReadLoop_(std::stop_token stoken)
{
    using namespace std::chrono_literals;

    beast::flat_buffer buffer;
    auto next_ping = std::chrono::steady_clock::now() + ping_interval_;

    // max_reconnect_attempts 초과 시 루프를 정상 종료하기 위한 플래그
    const int max_reconnects =
        util::AppConfig::instance().websocket.max_reconnect_attempts;
    bool give_up = false;

    // reconnect 시도 + max 초과 여부 갱신 (3곳 공통 처리)
    auto doReconnect = [&]() {
        const bool ok = reconnectOnce_(stoken);
        if (ok) {
            resubscribeAll();
            return;
        }
        if (max_reconnects > 0 &&
            reconnect_failures_ >= static_cast<std::uint32_t>(max_reconnects))
        {
            std::cout << "[WS] max reconnect attempts (" << max_reconnects
                      << ") reached, stopping\n";
            give_up = true;
        }
    };

    while (!stoken.stop_requested() && !give_up)
    {
        // 1) 커맨드 처리 (큐를 통째로 swap하여 락 범위 최소화)
        {
            std::deque<Command> local;
            {
                std::lock_guard lk(cmd_mu_);
                local.swap(cmd_q_);
            }

            for (auto& c : local) {
                if (auto* cc = std::get_if<CmdConnect>(&c)) {
                    connectImpl(cc->host, cc->port, cc->target, cc->bearer_jwt);
                    if (ws_ && ws_->is_open())
                        resubscribeAll();
                    continue;
                }
                if (auto* sc = std::get_if<CmdSubCandles>(&c)) {
                    const std::string ticket = makeTicket();
                    const std::string frame  = buildCandleSubJsonFrame(
                        ticket, sc->type, sc->markets,
                        sc->is_only_snapshot, sc->is_only_realtime, sc->format);
                    last_sub_frames_[sc->type] = frame;
                    (void)sendTextFrame(frame);
                    std::cout << "[WS] Candle subscribe sent: " << sc->type << "\n";
                    continue;
                }
                if (auto* sm = std::get_if<CmdSubMyOrder>(&c)) {
                    const std::string ticket = makeTicket();
                    const std::string frame  = buildMyOrderSubJsonFrame(
                        ticket, sm->markets, sm->is_only_realtime, sm->format);
                    last_sub_frames_["myOrder"] = frame;
                    (void)sendTextFrame(frame);
                    std::cout << "[WS] MyOrder subscribe sent\n";
                    continue;
                }
            }
        }

        if (stoken.stop_requested()) break;

        // 2) ping
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_ping) {
            next_ping = now + ping_interval_;
            if (ws_ && ws_->is_open()) {
                boost::system::error_code ec;
                ws_->ping({}, ec);
                if (ec) {
                    std::cout << "[WS] ping error: " << ec.message() << "\n";
                    if (!stoken.stop_requested())
                        doReconnect();
                }
            }
        }

        // 3) read (idle timeout 1초 → 루프 상단에서 stop/커맨드 체크)
        if (!ws_ || !ws_->is_open()) {
            // host_가 있으면 이전에 연결됐다가 끊긴 것 → 재연결
            // host_가 없으면 아직 connect 커맨드 전 → 대기
            if (!host_.empty() && !stoken.stop_requested())
                doReconnect();
            else
                std::this_thread::sleep_for(50ms);
            continue;
        }

        buffer.clear();
        boost::system::error_code ec;
        ws_->read(buffer, ec);

        if (ec) {
            // idle timeout은 정상 (메시지 없음)
            if (ec == beast::error::timeout || ec == boost::asio::error::timed_out)
                continue;

            std::cout << "[WS] read error: " << ec.message() << "\n";
            if (!stoken.stop_requested())
                doReconnect();
            continue;
        }

        const std::string msg = beast::buffers_to_string(buffer.data());

        // 캔들 메시지는 동일 ts로 반복 업데이트가 자주 와서 로그 생략
        const bool is_candle =
            (msg.find("\"type\"") != std::string::npos) &&
            (msg.find("\"candle.") != std::string::npos);

        if (!is_candle) {
            constexpr std::size_t kMaxLog = 200;
            if (msg.size() <= kMaxLog) std::cout << "[WS] RX: " << msg << "\n";
            else                       std::cout << "[WS] RX: " << msg.substr(0, kMaxLog) << "...\n";
        }

        if (on_msg_)
            on_msg_(std::string_view(msg));
    }

    // 루프 종료 시 정상 close
    if (ws_ && ws_->is_open()) {
        boost::system::error_code ignore;
        ws_->close(websocket::close_code::normal, ignore);
        std::cout << "[WS] closed: " << (ignore ? ignore.message() : "OK") << "\n";
    }
}

// ========== 프레임 생성 ==========

std::string UpbitWebSocketClient::makeTicket()
{
    std::mt19937_64 rng(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::ostringstream oss;
    oss << "ticket-" << std::hex << rng();
    return oss.str();
}

std::string UpbitWebSocketClient::buildCandleSubJsonFrame(
    const std::string& ticket,
    const std::string& type,
    const std::vector<std::string>& markets,
    bool is_only_snapshot,
    bool is_only_realtime,
    const std::string& format)
{
    nlohmann::json root = nlohmann::json::array();
    root.push_back({ {"ticket", ticket} });

    nlohmann::json body;
    body["type"]              = type;
    body["codes"]             = markets;
    body["is_only_snapshot"]  = is_only_snapshot;
    body["is_only_realtime"]  = is_only_realtime;
    root.push_back(body);

    root.push_back({ {"format", format} });
    return root.dump();
}

std::string UpbitWebSocketClient::buildMyOrderSubJsonFrame(
    const std::string& ticket,
    const std::vector<std::string>& markets,
    bool is_only_realtime,
    const std::string& format)
{
    nlohmann::json root = nlohmann::json::array();
    root.push_back({ {"ticket", ticket} });

    nlohmann::json body;
    body["type"]             = "myOrder";
    body["codes"]            = markets;
    body["is_only_realtime"] = is_only_realtime;
    root.push_back(body);

    root.push_back({ {"format", format} });
    return root.dump();
}

} // namespace api::ws
