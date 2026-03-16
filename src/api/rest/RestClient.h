#pragma once
#include <chrono>
#include <variant>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>

#include "HttpTypes.h"
#include "RestError.h"
#include "RetryPolicy.h"

// 동기 HTTPS 호출과 재시도 정책 적용을 캡슐화한다.
// 호출자는 HttpRequest만 구성하고, 실패는 RestError로 일관되게 받는다.

namespace api::rest
{
	// 예외 대신 성공/실패를 같은 반환 경로로 묶어 호출부 분기를 단순하게 한다.
	using Result = std::variant< HttpResponse, RestError>;

	class RestClient
	{
	public:
		// io_context와 ssl_context는 외부가 수명을 관리하고 RestClient는 재사용만 한다.
		RestClient(boost::asio::io_context& ioc,
			boost::asio::ssl::context& ssl_ctx);

		// perform은 1회 호출과 재시도 정책 적용을 함께 처리한다.
		Result perform(const HttpRequest& req, const RetryPolicy& retry = RetryPolicy{}) const;

	private:
		// performOnce는 재시도 없이 HTTPS 요청 1회를 수행한다.
		Result performOnce(const HttpRequest& req) const;

		// 재시도 판단을 분리해 호출 흐름과 정책을 따로 읽을 수 있게 한다.
		static bool shouldRetryStatus(int status, const RetryPolicy& p) noexcept;
		static bool shouldRetryError(const RestError& e, const RetryPolicy& p) noexcept;
		static std::chrono::milliseconds nextDelay(std::chrono::milliseconds cur, double mult) noexcept;

		boost::asio::io_context& ioc_;
		boost::asio::ssl::context& ssl_ctx_;
	};
}
