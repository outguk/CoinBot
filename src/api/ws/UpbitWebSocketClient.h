#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>
#include <json.hpp>
#include <cstdint>

namespace api::ws
{
	using tcp = boost::asio::ip::tcp;
	namespace beast = boost::beast;
	namespace websocket = beast::websocket;
	namespace http = beast::http;

	// 이 클래스의 역할
	// - 업비트 WebSocket 서버에 TLS로 연결
	// - 캔들 데이터 구독 메시지를 전송
	// - 서버에서 오는 raw JSON 문자열을 수신
	// (전략/도메인 파싱은 여기서 하지 않음
	// 추후 비동기 방식으로 여러 마켓을 관리하도록 변경
	class UpbitWebSocketClient final
	{
	public:
		// WebSocket stream type alias
		using WsStream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;
		/* 
		* Handler는 “UpbitWebSocketClient가 자기 책임을 끝낸 뒤, 다음 책임자에게 데이터를 넘겨주는 출구”다.
		*/
		// WS에서 받은 원본 JSON 문자열 그대로 전달
		using MessageHandler = std::function<void(std::string_view)>; // raw json text

		// io_context: 네트워크 이벤트 루프
		// ssl_ctx   : TLS 인증/암호화 설정
		UpbitWebSocketClient(boost::asio::io_context& ioc,
			boost::asio::ssl::context& ssl_ctx);

		
		// 1) WebSocket 연결
		// - DNS resolve
		// - TCP 연결
		// - TLS Handshake
		// - WebSocket Handshake
		void connectPublic(const std::string& host,
			const std::string& port,
			const std::string& target);

		// 1-2) WebSocket 연결 (PRIVATE)
		// - myOrder / myAsset 처럼 인증이 필요한 채널은 /private 엔드포인트 사용
		// - JWT 토큰을 Authorization: Bearer <token> 으로 WS 핸드셰이크 요청에 포함
		void connectPrivate(const std::string& host,
			const std::string& port,
			const std::string& target,
			const std::string& bearer_jwt);


		// 2) 캔들 구독 요청 전송
		// - markets: ["KRW-BTC", "KRW-ETH"]
		// - unit   : "candle.1s", "candle.1m" ...
		// - format : DEFAULT / SIMPLE_LIST
		void subscribeCandles(const std::string& type,
			const std::vector<std::string>& markets,
			bool is_only_snapshot = false,
			bool is_only_realtime = false,
			const std::string& format = "DEFAULT");

		// 2-2) 내 주문 및 체결(myOrder) 구독 요청
		// - markets(codes): ["KRW-BTC", "KRW-ETH"]
		// - format: DEFAULT / SIMPLE
		// - is_only_realtime: true 권장(실시간 스트림만)
		void subscribeMyOrder(const std::vector<std::string>& markets,
			bool is_only_realtime = true,
			const std::string& format = "DEFAULT");

		// 3) 메시지 수신 루프
		// - 서버가 보내는 데이터를 계속 읽는다
		// - 연결 유지 확인용
		void runReadLoop();

		// 정상적인 종료(커맨드 큐)
		void close();

		// 수신 콜백(선택) - 외부에서 메시지 처리 로직을 주입
		void setMessageHandler(MessageHandler cb);

	private:
		// ---- Command queue ----
		struct CmdConnect {
			std::string host, port, target;
			std::optional<std::string> bearer_jwt; // nullopt이면 public
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
		struct CmdClose {};

		using Command = std::variant<CmdConnect, CmdSubCandles, CmdSubMyOrder, CmdClose>;

		void pushCommand(Command c);

		// ws_ 생성/정리
		void resetStream();

		// thread-safe send
		bool sendTextFrame(const std::string& text);
		// reconnect + resubscribe
		bool reconnectOnce();
		void resubscribeAll();

		// 다음 재연결 sleep 시간을 계산(지수 backoff + jitter)
		std::chrono::milliseconds computeReconnectDelay_();

		// 내부 공통 연결 루틴: resolve->tcp->tls->ws handshake
		// - private 모드면 handshake 요청에 Authorization 헤더를 삽입한다.
		void connectImpl(const std::string& host,
			const std::string& port,
			const std::string& target,
			std::optional<std::string> bearer_jwt);

		// 업비트 구독 요청에 들어갈 ticket(구독 요청 묶음의 식별자(ID)) 값 생성
		// (요청 식별용, 완전한 UUID일 필요는 없음)
		static std::string makeTicket();

		// 업비트 WebSocket "캔들 구독" JSON 프레임 생성
		static std::string buildCandleSubJsonFrame(
			const std::string& ticket,
			const std::string& type,
			const std::vector<std::string>& markets,
			bool is_only_snapshot,
			bool is_only_realtime,
			const std::string& format
		);

		static std::string buildMyOrderSubJsonFrame(
			const std::string& ticket,
			const std::vector<std::string>& markets,
			bool is_only_realtime,
			const std::string& format
		);

	private:

		boost::asio::io_context& ioc_;			// 네트워크 이벤트 루프
		boost::asio::ssl::context& ssl_ctx_;	// TLS 설정 컨텍스트

		tcp::resolver resolver_;				// DNS → IP 변환

		// WebSocket(TLS 위에서 동작)
		// 재연결을 위해 "stream을 새로 생성"할 수 있어야 해서 포인터로 보관
		std::unique_ptr<WsStream> ws_;

		// stop 플래그 (close 커맨드로만 제어)
		std::atomic<bool> stop_{ false };

		// ping 주기 / 재연결 backoff
		std::chrono::seconds ping_interval_{ 20 };
		std::chrono::milliseconds reconnect_min_backoff_{ 800 };
		std::chrono::milliseconds reconnect_max_backoff_{ 30'000 };
		double reconnect_jitter_ratio_{ 0.20 };                       // ±20% 흔들림

		std::uint32_t reconnect_failures_{ 0 };                       // 연속 실패 횟수 (성공 시 0으로 reset)

		// 연결 정보 저장 (재연결 대비)
		std::string host_;
		std::string port_;
		std::string target_;

		// PRIVATE 채널 접속 시 사용(없으면 PUBLIC 접속)
		std::optional<std::string> bearer_jwt_;

		// 재연결 후 재구독을 위해 마지막 subscribe frame 저장
		// key 예: "candle.1m", "myOrder"
		std::unordered_map<std::string, std::string> last_sub_frames_;

		// 커맨드 큐
		std::mutex cmd_mu_;
		std::condition_variable cmd_cv_;
		std::deque<Command> cmd_q_;

		// 수신 콜백(다시 넘겨주기)
		MessageHandler on_msg_;
	};
}