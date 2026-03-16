#pragma once
#include <chrono>
#include <map>
#include <string>

// Rest 계층은 요청/응답을 이 공용 타입으로 주고받는다.
// 거래소별 client는 값을 채우고, RestClient는 전송과 재시도를 맡는다.

namespace api::rest
{
	// 전송 계층이 이해해야 하는 HTTP 메서드만 노출한다.
	enum class HttpMethod {Get, Post, Put, Delete};

	// 재시도와 전송 로직이 공유하는 최소 요청 정보.
	struct HttpRequest
	{
		HttpMethod method{ HttpMethod::Get };
		
		std::string host;
		std::string port{ "443" };
		// target은 path + query만 담아 host/port 교체와 재시도 로직을 단순하게 유지한다.
		std::string target;

		// 인증 같은 프로토콜 세부값은 상위 client가 채운다.
		std::map<std::string, std::string> headers;

		std::string body;

		// 동일한 상한을 각 네트워크 단계에 적용해 동기 호출 지연을 제한한다.
		std::chrono::milliseconds timeout{ 5000 };
	};

	// 파싱 전 원시 응답을 그대로 보관한다.
	struct HttpResponse
	{
		int status = 0;
		std::map<std::string, std::string> headers;
		std::string	body;
	};
}
