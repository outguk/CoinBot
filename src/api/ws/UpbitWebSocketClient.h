#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/domain/Candle.h"

namespace api::ws
{
	using tcp = boost::asio::ip::tcp;
	namespace beast = boost::beast;
	namespace websocket = beast::websocket;

	// ─────────────────────────────────────────────
	// 이 클래스의 역할
	// - 업비트 WebSocket 서버에 TLS로 연결
	// - 캔들 데이터 구독 메시지를 전송
	// - 서버에서 오는 raw JSON 문자열을 수신
	// (전략/도메인 파싱은 여기서 하지 않음)
	// ─────────────────────────────────────────────
	// 추후 비동기 방식으로 여러 마켓을 관리하도록 변경
	class UpbitWebSocketClient final
	{
	public:
		/* 
		* Handler는 “UpbitWebSocketClient가 자기 책임을 끝낸 뒤, 다음 책임자에게 데이터를 넘겨주는 출구”다.
		*/
		// WS에서 받은 원본 JSON 문자열 그대로 전달
		using MessageHandler = std::function<void(std::string_view)>; // raw json text
		// 변환된 캔들 전달 콜백
		using CandleHandler = std::function<void(const core::Candle&)>;

		// io_context: 네트워크 이벤트 루프
		// ssl_ctx   : TLS 인증/암호화 설정
		UpbitWebSocketClient(boost::asio::io_context& ioc,
			boost::asio::ssl::context& ssl_ctx);

		// ─────────────────────────────────────────
		// 1) WebSocket 연결
		// - DNS resolve
		// - TCP 연결
		// - TLS Handshake
		// - WebSocket Handshake
		// ─────────────────────────────────────────
		void connect(const std::string& host,
			const std::string& port,
			const std::string& target);


		// ─────────────────────────────────────────
		// 2) 캔들 구독 요청 전송
		// - markets: ["KRW-BTC", "KRW-ETH"]
		// - unit   : "candle.1s", "candle.1m" ...
		// - format : DEFAULT / SIMPLE_LIST
		// ─────────────────────────────────────────
		void subscribeCandles(const std::string& type,
			const std::vector<std::string>& markets,
			bool is_only_snapshot = false,
			bool is_only_realtime = false,
			const std::string& format = "DEFAULT");

		// ─────────────────────────────────────────
		// 3) 메시지 수신 루프
		// - 서버가 보내는 데이터를 계속 읽는다
		// - 연결 유지 확인용
		// ─────────────────────────────────────────
		void runReadLoop();

		// 수신 콜백(선택) - 외부에서 메시지 처리 로직을 주입
		void setMessageHandler(MessageHandler cb);
		void setCandleHandler(CandleHandler cb);

		// 정상적인 종료
		void close();
		
	private:
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

	private:
		boost::asio::io_context& ioc_;			// 네트워크 이벤트 루프
		boost::asio::ssl::context& ssl_ctx_;	// TLS 설정 컨텍스트

		tcp::resolver resolver_;				// DNS → IP 변환

		// WebSocket(TLS 위에서 동작)
		websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;

		// 연결 정보 저장 (재연결 대비)
		std::string host_;
		std::string target_;

		// 수신 콜백(다시 넘겨주기)
		MessageHandler on_msg_;
		CandleHandler  on_candle_;
	};
}