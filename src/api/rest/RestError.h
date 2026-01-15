#pragma once
#include <string>

/*
* RestError.h
* 실패를 “한 가지 타입”으로 통일해서 상위 레이어가 처리하기 쉽게 함
* 네트워크/프로토콜 오류와 HTTP status 오류를 같은 규격으로 포장
*/

namespace api::rest
{

	// RestClinet 레벨(공통 레벨)에서 발생 가능한 오류를 분류
	// DTO 파싱 오류는 아님

	enum class RestErrorCode
	{
		ResolveFailed,			// DNS/호스트 해석 실패
		ConnectFailed,			// TCP connect 실패 or connect timeout
		HandshakeFailed,		// TLS handshake 실패 (인증서/SNI 포함)
		WriteFailed,			// HTTP request 전송 실패
		ReadFailed,				// HTTP response 일기 실패
		Timeout,				// (추후 구체화 가능) 타임아웃 명시적 구분 시
		BadStatus,				// status 정책상 실패 (4xx/5xx)
		InvaildArgiment,		// host/target 누락 등 잘못된 입력
		ParseError,				// 파싱 에러

		Unknown					// 모르는 오류
	};

	// 오류를 표준화한 데이터 구조
	// - 상위 계층이 "왜 실패했는지" 일관되게 처리하기 위함
	struct RestError
	{
		RestErrorCode code = RestErrorCode::Unknown;	// Unknown으로 초기화
		std::string	message;							// error 또는 커스텀 메시지
		int http_status{ 0 };							// BadStatus 에서 사용
	};


	std::ostream& operator<<(std::ostream& os, RestErrorCode code)
	{
		switch (code)
		{
		case RestErrorCode::ResolveFailed:     return os << "ResolveFailed";
		case RestErrorCode::ConnectFailed:     return os << "ConnectFailed";
		case RestErrorCode::HandshakeFailed:   return os << "HandshakeFailed";
		case RestErrorCode::WriteFailed:       return os << "WriteFailed";
		case RestErrorCode::ReadFailed:        return os << "ReadFailed";
		case RestErrorCode::Timeout:           return os << "Timeout";
		case RestErrorCode::BadStatus:         return os << "BadStatus";
		case RestErrorCode::InvaildArgiment:   return os << "InvalidArgument";
		case RestErrorCode::ParseError:        return os << "ParseError";
		case RestErrorCode::Unknown:            return os << "Unknown";
		default:                               return os << "Unknown";
		}
	}
}