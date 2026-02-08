#pragma once
#include <chrono>
#include <map>
#include <string>

/*
* HttpTypes.h
* REST 호출에 필요한 요청/응답의 ‘공통 데이터 구조’를 정의
* 어떤 API(Upbit 말고 다른 거래소 등)에도 재사용 가능하게 “범용 형태”로 유지
*/

namespace api::rest
{
	// HTTP 메서드 종류 정리
	enum class HttpMethod {Get, Post, Put, Delete};

	// "요청에 필요한 공통 정보"를 담는 구조체
	// - 순수 네트워크 계층 타입
	struct HttpRequest
	{
		HttpMethod method{ HttpMethod::Get };		// 기본 http 요청 타입을 GET으로
		
		// 공통으로 요청하는 host/port/target을 분리
		std::string host;							// "api.upbit.com"
		std::string port{ "443" };					// https는 443 사용
		std::string target;							// "/v1/ticker?markets=KRW-BTC"  (path + query)

		// 추가 헤더 (인증 등은 상위 client에서)
		std::map<std::string, std::string> headers;

		// request body (POST/PUT에서 주로 사용, GET은 주로 빈 문자열)
		std::string body;

		// 네트워크 timeout (connect/write/read에 적용)
		std::chrono::microseconds timeout{ 5000 };
	};

	// "응답 오는 공통 정보"를 담는 구조체
	struct HttpResponse
	{
		int status = 0;								// HTTP satus code (200, 429, 500 등)
		std::map<std::string, std::string> headers;
		std::string	body;							// JSON 원문(문자열), DTO 파싱을 이걸 받아서 수행
	};
}