#include "RestClient.h"

#include <thread>
#include <utility>

#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

namespace api::rest
{
	namespace beast = boost::beast;
	namespace http = beast::http;
	namespace net = boost::asio;
	namespace ssl = net::ssl;

	using tcp = net::ip::tcp;

	// Boost 의존성을 이 파일 안에 가둬 Rest 계층의 공용 enum을 유지한다.
	static http::verb toBeastVerb(HttpMethod m)
	{
		switch (m)
		{
		case HttpMethod::Get: return http::verb::get;
		case HttpMethod::Post: return http::verb::post;
		case HttpMethod::Put: return http::verb::put;
		case HttpMethod::Delete: return http::verb::delete_;
		}
		return http::verb::get;
	}

	// 시스템 에러 문자열을 함께 보관해 상위 로그에서 원인 추적을 쉽게 한다.
	static RestError makeError(RestErrorCode code, const beast::error_code& ec, int http_status = 0)
	{
		return RestError{ code, ec.message(), http_status };
	}

	// Asio와 Beast의 timeout을 같은 재시도 정책으로 묶는다.
	static bool isTimeoutEc(const beast::error_code& ec) noexcept
	{
		return ec == net::error::timed_out || ec == beast::error::timeout;
	}

	// 일부 서버는 close_notify 없이 종료하므로 응답 후 shutdown 오류를 완화한다.
	static bool isHarmlessShutdownEc(const beast::error_code& ec) noexcept
	{
		return ec == net::error::eof || ec == ssl::error::stream_truncated;
	}

	RestClient::RestClient(net::io_context& ioc, ssl::context& ssl_ctx)
		: ioc_(ioc), ssl_ctx_(ssl_ctx) {}

	// HTTP status 기반 재시도는 서버가 응답한 경우에만 적용한다.
	bool RestClient::shouldRetryStatus(int status, const RetryPolicy& p) noexcept
	{
		const bool is429 = (status == 429);
		const bool is5xx = (status >= 500 && status <= 599);
		if (is429) return p.retry_on_429;
		if (is5xx) return p.retry_on_5xx;
		return false;
	}

	// 네트워크 실패는 HTTP status가 없으므로 에러 분류로 재시도 여부를 판단한다.
	bool RestClient::shouldRetryError(const RestError& e, const RetryPolicy& p) noexcept
	{
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

	// 백오프 상한을 둬 동기 호출이 과도하게 늘어지지 않게 한다.
	std::chrono::milliseconds RestClient::nextDelay(std::chrono::milliseconds cur, double mult) noexcept
	{
		auto next = static_cast<long long>(cur.count() * mult);

		if (next < 0)
			next = cur.count();
		if (next > 10000)
			next = 10000;

		return std::chrono::milliseconds(next);
	}

	Result RestClient::perform(const HttpRequest& req, const RetryPolicy& retry) const
	{
		// 필수 주소 정보가 없으면 네트워크 호출 전에 실패를 확정한다.
		if (req.host.empty() || req.target.empty())
			return RestError{ RestErrorCode::InvalidArgument, "host/target is empty" };

		std::size_t attempt = 0;
		std::chrono::milliseconds delay = retry.base_delay;

		while (true)
		{
			++attempt;
			Result r = performOnce(req);

			if (std::holds_alternative<HttpResponse>(r))
			{
				auto& resp = std::get<HttpResponse>(r);

				if (shouldRetryStatus(resp.status, retry) && attempt < retry.max_attempts)
				{
					std::this_thread::sleep_for(delay);
					delay = nextDelay(delay, retry.backoff_multiplier);
					continue;
				}

				return r;
			}

			auto& err = std::get<RestError>(r);
			if (shouldRetryError(err, retry) && attempt < retry.max_attempts)
			{
				std::this_thread::sleep_for(delay);
				delay = nextDelay(delay, retry.backoff_multiplier);
				continue;
			}

			return r;
		}
	}

	Result RestClient::performOnce(const HttpRequest& req) const
	{
		beast::error_code ec;

		// Resolve를 먼저 수행해 DNS 실패와 connect 실패를 구분한다.
		tcp::resolver resolver(ioc_);
		auto results = resolver.resolve(req.host, req.port, ec);
		if (ec)
		{
			if (isTimeoutEc(ec))
				return makeError(RestErrorCode::Timeout, ec);

			return makeError(RestErrorCode::ResolveFailed, ec);
		}

		beast::ssl_stream<beast::tcp_stream> stream(ioc_, ssl_ctx_);

		// 거래소가 가상 호스트를 쓰므로 TLS SNI를 요청 host와 맞춘다.
		if (!SSL_set_tlsext_host_name(stream.native_handle(), req.host.c_str()))
		{
			return RestError{ RestErrorCode::HandshakeFailed, "SNI set failed", 0 };
		}

		// 인증서 호스트 이름도 요청 대상과 일치해야 한다.
		stream.set_verify_callback(ssl::host_name_verification(req.host));

		beast::get_lowest_layer(stream).expires_after(req.timeout);
		beast::get_lowest_layer(stream).connect(results, ec);
		if (ec)
		{
			if (isTimeoutEc(ec))
				return makeError(RestErrorCode::Timeout, ec);
			return makeError(RestErrorCode::ConnectFailed, ec);
		}

		beast::get_lowest_layer(stream).expires_after(req.timeout);
		stream.handshake(ssl::stream_base::client, ec);
		if (ec)
		{
			if (isTimeoutEc(ec))
				return makeError(RestErrorCode::Timeout, ec);
			return makeError(RestErrorCode::HandshakeFailed, ec);
		}

		http::request<http::string_body> http_req{ toBeastVerb(req.method), req.target, 11 };
		http_req.set(http::field::host, req.host);
		http_req.set(http::field::user_agent, "CoinBot/1.0");

		for (const auto& [k, v] : req.headers)
			http_req.set(k, v);

		http_req.body() = req.body;
		// Content-Length 같은 body 메타데이터를 Beast가 일관되게 계산한다.
		if (!req.body.empty())
			http_req.prepare_payload();

		beast::get_lowest_layer(stream).expires_after(req.timeout);
		http::write(stream, http_req, ec);
		if (ec)
		{
			if (isTimeoutEc(ec))
				return makeError(RestErrorCode::Timeout, ec);
			return makeError(RestErrorCode::WriteFailed, ec);
		}

		beast::flat_buffer buffer;
		http::response<http::string_body> http_res;

		beast::get_lowest_layer(stream).expires_after(req.timeout);
		http::read(stream, buffer, http_res, ec);
		if (ec)
		{
			if (isTimeoutEc(ec))
				return makeError(RestErrorCode::Timeout, ec);
			return makeError(RestErrorCode::ReadFailed, ec);
		}

		beast::get_lowest_layer(stream).expires_after(req.timeout);
		stream.shutdown(ec);
		if (ec && !isHarmlessShutdownEc(ec))
		{
			// 응답을 이미 받은 뒤라서 상위 실패로 승격하지 않는다.
		}

		HttpResponse resp;
		resp.status = static_cast<int>(http_res.result_int());
		resp.body = std::move(http_res.body());

		for (const auto& h : http_res)
		{
			resp.headers.emplace(std::string(h.name_string()), std::string(h.value()));
		}

		return resp;
	}
}
