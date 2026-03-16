#pragma once
#include <iosfwd>
#include <string>

// Rest 계층은 실패를 이 공용 분류로 묶어 호출부 분기를 단순하게 한다.
// 원본 에러 문자열과 HTTP status를 함께 보관해 로그 맥락을 잃지 않게 한다.

namespace api::rest
{

	// 네트워크 실패와 프로토콜 실패를 한 enum으로 정리한다.

	enum class RestErrorCode
	{
		ResolveFailed,			// DNS/호스트 해석 실패
		ConnectFailed,			// TCP connect 실패 or connect timeout
		HandshakeFailed,		// TLS handshake 실패 (인증서/SNI 포함)
		WriteFailed,			// HTTP request 전송 실패
		ReadFailed,				// HTTP response 일기 실패
		Timeout,				// (추후 구체화 가능) 타임아웃 명시적 구분 시
		BadStatus,				// status 정책상 실패 (4xx/5xx)
		InvalidArgument,		// host/target 누락 등 잘못된 입력
		ParseError,				// 파싱 에러

		Unknown					// 모르는 오류
	};

	// 호출부가 예외 없이 실패 맥락을 함께 다루도록 하는 공용 에러 타입이다.
	struct RestError
	{
		RestErrorCode code = RestErrorCode::Unknown;
		std::string	message;
		int http_status{ 0 };
	};


	std::ostream& operator<<(std::ostream& os, RestErrorCode code);
}
