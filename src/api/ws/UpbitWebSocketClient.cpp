#include <atomic>
#include <json.hpp>
#include <chrono>
#include <iostream>
#include <random>
#include <sstream>

#include "UpbitWebSocketClient.h"

#include "api/upbit/dto/UpbitQuotationDtos.h"
#include "api/upbit/mappers/CandleMapper.h"

namespace api::ws {

    UpbitWebSocketClient::UpbitWebSocketClient(
        boost::asio::io_context& ioc,
        boost::asio::ssl::context& ssl_ctx)
        : ioc_(ioc)
        , ssl_ctx_(ssl_ctx)
        , resolver_(boost::asio::make_strand(ioc))  // 문법 유의
        , ws_(boost::asio::make_strand(ioc), ssl_ctx) {
        // strand:
        // - 멀티스레드 환경에서 handler 동시 실행 방지
    }

    // 외부에서 메시지 처리 로직
    // - 현재 단계에서는 단순 로그 출력용
    // -다음 단계(7 - 2)에서 DTO 파싱 진입점
    void UpbitWebSocketClient::setMessageHandler(MessageHandler cb) {
        on_msg_ = std::move(cb);
    }
    void UpbitWebSocketClient::setCandleHandler(CandleHandler cb) {
        on_candle_ = std::move(cb);
    }


    // WebSocket 연결
    void UpbitWebSocketClient::connect(
        const std::string& host,
        const std::string& port,
        const std::string& target) 
    {

        host_ = host;
        target_ = target;

        boost::system::error_code ec;

        // 1) DNS 조회 (host → IP)
        // "api.upbit.com" → 실제 접속 가능한 IP 목록 
        auto results = resolver_.resolve(host, port, ec);   // resolve가 뭐하는건지?
        std::cout << "[WS] resolve: " << (ec ? ec.message() : "OK") << "\n";
        if (ec) return;

        // 2) TCP 연결
        beast::get_lowest_layer(ws_).connect(results, ec);
        std::cout << "[WS] tcp connect: " << (ec ? ec.message() : "OK") << "\n";
        if (ec) return;

        // 3) TLS SNI 설정
        // - TLS 서버가 어떤 도메인에 대한 인증서인지 판단하는 데 필요
        // 3) SNI (TLS에서 매우 자주 실패 원인)
        if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), host.c_str())) {
            std::cout << "[WS] SNI: FAIL\n";
            return;
        }
        std::cout << "[WS] SNI: OK\n";


        /*SSL_set_tlsext_host_name(
            ws_.next_layer().native_handle(),
            host.c_str()
        );*/

        // 4) TLS Handshake
        // - 인증서 검증
        // - 암호화 채널 수립
        ws_.next_layer().handshake(boost::asio::ssl::stream_base::client, ec);
        std::cout << "[WS] tls handshake: " << (ec ? ec.message() : "OK") << "\n";
        if (ec) return;

        // 5) WebSocket Handshake
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws_.handshake(host, target, ec);
        std::cout << "[WS] ws handshake: " << (ec ? ec.message() : "OK") << "\n";
        if (ec) return;

        std::cout << "[WS] Connected Upbit\n";
    }

    // 캔들 구독 요청 전송
    // - 이 함수는 “요청 생성 + 전송”만 담당
    void UpbitWebSocketClient::subscribeCandles(
        const std::string& type,
        const std::vector<std::string>& markets,
        bool is_only_snapshot,
        bool is_only_realtime,
        const std::string& format) {

        // 구독 요청 식별자
        const std::string ticket = makeTicket();

        // 업비트 규격에 맞는 JSON 배열 생성
        const std::string frame =
            buildCandleSubJsonFrame(
                ticket, type, markets,
                is_only_snapshot, is_only_realtime, format);

        // 업비트 WS는 text frame 사용
        ws_.text(true);

        // 실제 네트워크 전송
        ws_.write(boost::asio::buffer(frame));

        std::cout << "[WS] Candle Subscribe sent\n";
    }

    // 메시지 수신 루프
    // - 서버가 보내는 데이터를 계속 읽는다
    // - 이 루프가 살아 있어야 연결도 유지

    /*
        현재 단계(7 - 1)에서는
        - 단순 블로킹 read
        - 구조 이해가 목적

        다음 단계(7 - 2)에서는
        - async_read
        - 메시지 큐 / 버퍼링 구조로 발전
     */
    void UpbitWebSocketClient::runReadLoop() {
        beast::flat_buffer buffer;
        // 서버가 끊지 않는 한 계속 수신
        for (;;) 
        {
            buffer.clear();

            // 서버 메시지 수신(블로킹)
            boost::system::error_code ec;
            ws_.read(buffer, ec);

            if (ec) {
                // 정상 종료(서버 close 등)와 오류를 구분해서 로그
                std::cout << "[WS] read error: " << ec.message() << "\n";
                break; // 재연결은 상위(메인)에서 정책적으로 결정하는 게 깔끔
            }

            // buffer -> string 으로 복사: string_view 수명 문제 해결
            const std::string msg = beast::buffers_to_string(buffer.data());

            // raw 로그(앞 200자만)
            constexpr std::size_t kMaxLog = 20;
            if (msg.size() <= kMaxLog) {
                std::cout << "[WS] RX: " << msg << "\n";
            }
            else {
                std::cout << "[WS] RX: " << msg.substr(0, kMaxLog) << "...\n";
            }

            // JSON 파싱 + 필드 존재 여부만 확인 (예외 없이)
            nlohmann::json j = nlohmann::json::parse(msg, nullptr, false);
            if (j.is_discarded()) {
                std::cout << "[WS] JSON parse failed (discarded)\n";
                continue;
            }

            const bool ok = j.is_object()
                && j.contains("type")
                && j.contains("code")
                && j.contains("trade_price")
                && j.contains("timestamp")
                && j.contains("stream_type");

            if (!ok) {
                std::cout << "[WS] JSON ok, but required fields missing\n";
                continue;
            }

            const std::string type = j.value("type", "");
            const bool is_candle = (type.rfind("candle.", 0) == 0); // "candle." prefix

            if (is_candle)
            {
                // WS 캔들은 market 대신 code를 준다 -> dto.market에 매핑
                api::upbit::dto::CandleDto_Minute dto{};
                dto.market = j.value("code", "");

                // CandleMapper가 쓰는 필드들을 WS JSON에서 채움
                dto.opening_price = j.value("opening_price", 0.0);
                dto.high_price = j.value("high_price", 0.0);
                dto.low_price = j.value("low_price", 0.0);
                dto.trade_price = j.value("trade_price", 0.0);

                dto.candle_acc_trade_volume = j.value("candle_acc_trade_volume", 0.0);
                dto.candle_date_time_kst = j.value("candle_date_time_kst", "");

                // unit은 없을 수도 있어서 default 0 (현재는 변환에 필수 아님)
                dto.unit = j.value("unit", 0);

                // DTO -> Domain 변환 (도메인은 WS/JSON을 모른다)
                const core::Candle c = api::upbit::mappers::toDomain(dto);

                // "저장/출력"은 지금 안 함. 변환 결과만 외부로 전달.
                if (on_candle_) on_candle_(c);
            }

            // 외부 처리 로직으로 전달
            if (on_msg_) {
                on_msg_(std::string_view(msg));
            }
        }
    }

    // 정상 종료
    void UpbitWebSocketClient::close() {
        boost::system::error_code ec;
        ws_.close(websocket::close_code::normal, ec);
        std::cout << "[WS] close: " << (ec ? ec.message() : "OK") << "\n";
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

} // namespace api::ws
