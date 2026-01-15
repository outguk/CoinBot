#pragma once
#include "HttpTypes.h"
#include "RestError.h"
#include "RetryPolicy.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <variant>

/*
* RestClient.h
* RestClient의 공용 인터페이스
* 
*/


namespace api::rest
{
	// 요청에 성공하면 HttpResponse, 실패 시 RestError
	// - 예외를 남발하지 않고 호출자가 분기 처리 가능
	using Result = std::variant< HttpResponse, RestError>;

	class RestClient
	{
	public:
		// io_context/ssl_context는 앱에서 1개를 만들고 공유하는 구조가 보통 안정적
		// &를 통해 사용만하고 소유 x
		RestClient(boost::asio::io_context& ioc,
			boost::asio::ssl::context& ssl_ctx);

		// perform - 재시도를 포함한 "고수준" 호출
		// RetryPolicy를 파라미터로 받아 호출마다 다르게 적용 가능
		Result perform(const HttpRequest& req, const RetryPolicy& retry = {}) const;

	private:
		// performOnce - 재시도 없는 1회 시도
		// - 실제 HTTPS 요청/응답 처리의 핵심
		Result performOnce(const HttpRequest& req) const;

		// 재시도 판단 로직 분리 (가독성/테스트성↑)
		static bool shouldRetryStatus(int status, const RetryPolicy& p) noexcept;
		static bool shouldRetryError(const RestError& e, const RetryPolicy& p) noexcept;
		static std::chrono::milliseconds nextDelay(std::chrono::milliseconds cur, double mult) noexcept;

		boost::asio::io_context& ioc_;
		boost::asio::ssl::context& ssl_ctx_;
	};
}