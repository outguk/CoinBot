#include "RestClient.h"

// 네트워크(Asio) + HTTP/SSL(Beast) 구성요소를 사용하기 위해 필요한 Boost 헤더들.
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/error.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <thread> // sleep_for (동기 호출이라 나중에 Websocket 연결 시 변경) 재시도 백오프(대기) 구현
#include <utility> // move/forward 같은 유틸


/*
* RestClient.cpp
* HTTP 요청 보내고(Boost.Beast), 타임아웃/리트라이/에러 표준화까지 책임
* Boost.Beast + Asio + SSL 로 실제 HTTPS 요청 수행
* TLS(SNI/handshake), connect/read/write timeout, 응답 파싱 등을 담당
* “DTO 파싱/Domain 변환”은 여기 넣지 않음 (도메인 오염 방지)
*/

// 현재 Rest는 동기 구조 -> 비동기로 바꿔야 하나?? (추후 해보도록)
namespace api::rest
{
	namespace beast		= boost::beast;
	namespace http		= beast::http;
	namespace net		= boost::asio;
	namespace ssl		= net::ssl;

	using tcp =	net::ip::tcp;

	// 내부 헬퍼: 프로젝트의 HttpMethod(우리 enum)를 Boost.Beast가 요구하는 타입(http::verb)으로 바꾼다
	// 내부에서만 Boost 타입을 쓰게 해서 라이브러리 종속을 줄이고 RestClient는 enum으로 만든 공용 타입만 사용
	static http::verb toBeastVerb(HttpMethod m)
	{
		switch (m) 
		{
			case HttpMethod::Get: return http::verb::get;
			case HttpMethod::Post: return http::verb::post;
			case HttpMethod::Put: return http::verb::put;
			case HttpMethod::Delete: return http::verb::delete_;
		}
		return http::verb::get; // 예상치 못한 값이 들어오면 get으로 처리
	}

	/* -------------------- 에러 매핑과 timeout을 판단하는 helper 함수들 -------------------- */

	// Boost 에러코드(ec)를 우리 프로젝트의 RestError로 포장
	static RestError makeError(RestErrorCode code, const beast::error_code& ec, int http_status = 0)
	{
		// ec.message()는 사람이 읽을 수 있는 에러 메시지를 반환
		return RestError{ code, ec.message(), http_status };
	}

	// 이 에러코드가 “타임아웃 계열인지” 판정
	// asio 레벨에서의 타임아웃 이거나 beast(http/websocket)에서의 타임아웃이면 true return
	static bool isTimeoutEc(const beast::error_code& ec) noexcept
	{
		return ec == net::error::timed_out || ec == beast::error::timeout;
	}

	// TLS shutdown 과정에서 자주 뜨는 “무해한” 에러들을 걸러준다.
	static bool isHarmlessShutdownEc(const beast::error_code& ec) noexcept
	{
		// TLS shutdown에서 아래 류는 흔히 발생(상대가 먼저 close 등)
		return ec == net::error::eof || ec == ssl::error::stream_truncated;
	}

	/* ---------------------------------------------------------------------------------- */


	/* -------------------- RestClient 생성자 및 로직 구현 (h파일 참조해서 이해) -------------------- */

	// RestClint 생성자
	RestClient::RestClient(net::io_context& ioc, ssl::context& ssl_ctx)
		// io_context와 ssl::context를 소유 x, 참조 -> 앱에서 하나 만들어 여러 클라이언트/세션이 공유하는 방식
		// 리소스 수명도 앱이 관리하는 편이 안정적
		: ioc_(ioc), ssl_ctx_(ssl_ctx) {}	

	
	/* ----------------------------- 재시도 판단 로직 ----------------------------- */
	// 각 정책의 반환 값이 true면 재시도, false면 재시도 하지 않는 것으로 설정

	// (1) HTTP status 기반 (응답이 429, 5xx등 인 경우, 정의한 RetryPolicy에 따라 재시도 결정
	bool RestClient::shouldRetryStatus(int status, const RetryPolicy& p) noexcept
	{
		const bool is429 = (status == 429);
		const bool is5xx = (status >= 500 && status <=599);
		if (is429) return p.retry_on_429;
		if (is5xx) return p.retry_on_5xx;
		return false; // 4xx 대부분은 재시도 무의미해서 그만함
	}

	// (2) 네트워크 에러 기반 (응답 자체를 못 받았거나 요청/응답 송수신 중 오류가 난 경우)
	bool RestClient::shouldRetryError(const RestError& e, const RetryPolicy& p) noexcept
	{
		// Policy에 따라 timeout, connect, read/write를 제어한다
		switch (e.code)
		{
		case RestErrorCode::Timeout:
			return p.retry_on_timeout;

		case RestErrorCode::ConnectFailed:
		case RestErrorCode::ResolveFailed:
		case RestErrorCode::HandshakeFailed:
			return p.retry_on_connect_fail;

		case RestErrorCode::ReadFailed:
		case RestErrorCode::WriteFailed:
			return p.retry_on_read_write_fail;

		default:
			return false;
		}
	}

	// (3) 백오프 다음 지연 계산 (현재 지연 시간(cur)에 배수(mult)를 곱해서 다음 지연을 만듬)
	std::chrono::milliseconds RestClient::nextDelay(std::chrono::milliseconds cur, double mult) noexcept
	{
		auto next = static_cast<long long>(cur.count() * mult);

		if (next < 0) 
			next = cur.count();
		// 무한정 폭주 방지 상한 설정 (10초)
		if (next > 10000) 
			next = 10000;

		return std::chrono::milliseconds(next);		// 지연시간 반환
	}

	/* ------------------------------------------------------------------------------ */


	/* ------------------------ perform: 재시도 루프 (핵심 컨트롤러) ---------------------- */

	Result RestClient::perform(const HttpRequest& req, const RetryPolicy& retry) const
	{
		// 요청에 필수인 host와 target이 없으면 즉시 실패 반환
		if (req.host.empty() || req.target.empty())
			return RestError{ RestErrorCode::InvalidArgument, "host/target is empty" };

		std::size_t attempt = 0;								// 현재 몇번째 시도인지 카운트
		std::chrono::milliseconds delay = retry.base_delay;		// 재시도 대기 시간

		// 루프 안에서 매번 1회 호출을 시도
		while (true)
		{
			++attempt;
			Result r = performOnce(req);	// performOnce가 실제 네트워크 호출을 실행

			// (A) 응답이 온 경우
			if (std::holds_alternative<HttpResponse>(r))
			{
				auto& resp = std::get<HttpResponse>(r);		// 어떤 HttpResponse인지 꺼냄

				// status 오류라면 (재시도 판단)
				if (shouldRetryStatus(resp.status, retry) && attempt < retry.max_attempts)
				{
					// sleep_for(delay) 만큼 기다렸다가 delay를 증가 시키고 다음 루프로 재시도
					std::this_thread::sleep_for(delay);
					delay = nextDelay(delay, retry.backoff_multiplier);
					continue;
				}

				return r;	// 재시도 대상이 아니거나 시도 횟수 초과라면 그대로 Response 반환
			}

			// (B) 네트워크/프로토콜 실패인 경우
			auto& err = std::get<RestError>(r);		// 실패 케이스에서 RestError를 꺼냄

			// 에러 타입에 따라 재시도할 가치가 있으면(정책이 true면) 동일한 방식으로 재시도
			if (shouldRetryError(err, retry) && attempt < retry.max_attempts)
			{
				std::this_thread::sleep_for(delay);
				delay = nextDelay(delay, retry.backoff_multiplier);
				continue;
			}

			return r;		// 재시도하지 않거나, 최대 횟수 초과면 실패 그대로 반환
		}
	}

	// performOnce: 실제 HTTPS 1회 호출
	Result RestClient::performOnce(const HttpRequest& req) const
	{
		beast::error_code ec;	// 네트워크 작업에서 발생한 에러를 담는 ec

		// host/port를 실제 접속 가능한 endpoint 목록으로 변환한다(DNS 조회 포함)
		tcp::resolver resolver(ioc_);	// (1) DNS Resolve
		auto results = resolver.resolve(req.host, req.port, ec);

		// resolve 실패 시 에러 반환
		if (ec)
		{
			// resolve timeout도 케이스에 따라 섞일 수 있음(환경마다)
			if (isTimeoutEc(ec))
				return makeError(RestErrorCode::Timeout, ec);

			return makeError(RestErrorCode::ResolveFailed, ec);
		}

		// (2) SSL Stream 생성 (TCP 위에 TLS(SSL) 계층을 얹은 스트림) -> 이 스트림으로 HTTPS 통신
		beast::ssl_stream<beast::tcp_stream> stream(ioc_, ssl_ctx_);

		// (3) SNI 설정 (“나는 이 host로 접속한다”를 TLS handshake에 알려주는 기능)
		if (!SSL_set_tlsext_host_name(stream.native_handle(), req.host.c_str()))
		{
			// 실패 시 handshake 자체가 꼬일 수 있어서 HandshakeFailed로 처리
			return RestError{ RestErrorCode::HandshakeFailed, "SNI set failed", 0 };
		}

		// (4) TCP connect + timeout
		beast::get_lowest_layer(stream).expires_after(req.timeout);	// 이 작업의 제한 시간 설정
		beast::get_lowest_layer(stream).connect(results, ec);		// 실제 TCP 연결 수행
		if (ec)		// 실패 시 에러 반환 (공통 에러 처리)
		{
			if (isTimeoutEc(ec)) 
				return makeError(RestErrorCode::Timeout, ec);
			return makeError(RestErrorCode::ConnectFailed, ec);
		}

		// (5) TLS handshake + timeout (TLS handshake는 암호화 연결 협상 + 인증서 검증 단계)
		beast::get_lowest_layer(stream).expires_after(req.timeout);	// 제한 시간
		stream.handshake(ssl::stream_base::client, ec);				// 클라이언트 모드로 handshake
		if (ec)		// 실패 시 에러 반환 (공통 에러 처리)
		{
			if (isTimeoutEc(ec))
				return makeError(RestErrorCode::Timeout, ec);
			return makeError(RestErrorCode::HandshakeFailed, ec);
		}

		// (6) HTTP Request 구성

		// Beast HTTP 요청 객체 생성 (11은 HTTP/1.1)
		http::request<http::string_body> http_req{ toBeastVerb(req.method), req.target, 11 };
		http_req.set(http::field::host, req.host);				// Host 헤더는 HTTP/1.1에서 중요
		http_req.set(http::field::user_agent, "CoinBot/1.0");	// User-Agent는 디버깅/서버 로그용

		// 호출자가 넣은 추가 헤더를 모두 반영(Authorization 등)
		for (const auto& [k, v] : req.headers) http_req.set(k, v);

		// body를 넣고, body가 있으면 prepare_payload()
		http_req.body() = req.body;
		if (!req.body.empty())
			http_req.prepare_payload();	// http_req의 body 부분을 검토해 있으면 그에 맞는 길이를 계산, 업으면 제거/정리한다.

		// (7) write + timeout
		beast::get_lowest_layer(stream).expires_after(req.timeout);	// 제한 시간
		http::write(stream, http_req, ec); // write HTTP 요청을 전송
		if (ec)		// 실패 시 에러 반환 (공통 에러 처리)
		{
			if (isTimeoutEc(ec))
				return makeError(RestErrorCode::Timeout, ec);
			return makeError(RestErrorCode::WriteFailed, ec);
		}

		// (8) read + timeout
		beast::flat_buffer buffer;					// 수신 데이터 임시 버퍼
		http::response<http::string_body> http_res;	// 응답을 받을 객체

		beast::get_lowest_layer(stream).expires_after(req.timeout);
		http::read(stream, buffer, http_res, ec);	// 제한 시간을 걸고 응답을 읽음
		if (ec)		// 실패 시 에러 반환 (공통 에러 처리)
		{
			if (isTimeoutEc(ec))
				return makeError(RestErrorCode::Timeout, ec);
			return makeError(RestErrorCode::ReadFailed, ec);
		}

		// (9) shutdown (무해 에러 무시)

		// TLS 연결을 정상 종료(종료 알림 교환)
		beast::get_lowest_layer(stream).expires_after(req.timeout);
		stream.shutdown(ec);
		if (ec && !isHarmlessShutdownEc(ec))
		{
			// shutdown 에러를 치명은 아님"으로 두고 로그만 남기자
		}

		// (10) 우리 공용 타입으로 변환해서 반환
		HttpResponse resp;
		resp.status = static_cast<int>(http_res.result_int());	// Beast 응답 → HttpResponse로 변환
		resp.body = std::move(http_res.body());					// body는 std::move로 복사 비용 줄이기

		// Beast 응답 헤더들을 map으로 복사해서 저장
		for (auto const& h : http_res)
		{
			resp.headers.emplace(std::string(h.name_string()), std::string(h.value()));
		}

		return resp;	// 성공 결과 반환
	}

	/* ------------------------------------------------------------------------------ */
}	
