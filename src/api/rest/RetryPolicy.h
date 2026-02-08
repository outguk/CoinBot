#pragma once
#include <chrono>
#include <cstddef>


/*
* RetryPolicy.h
* 재시도 정책을 데이터로 분리
* 429, 5xx 등 “일시적 오류”에 대한 재시도/백오프 룰을 한 곳에서 정의
*/

namespace api::rest
{

	// 재시도 정책을 "코드"가 아닌 "데이터"로 분리
	// - RestClinet가 이 정책을 보고 동작하도록
	struct RetryPolicy
	{
		std::size_t					max_attempts{ 3 };	// 총 시도 횟수
		std::chrono::milliseconds	base_delay{ 200 };	// 첫 대기시간
		double				backoff_multiplier{ 2.0 };	// 지수 백오프 배수 (요청이 실패 시 재시도 사이 시간을 점점 늘림)

		// status 기반 재시도 옵션
		bool				retry_on_429{ true };		// 요청이 너무 많음
		bool				retry_on_5xx{ true };		// 500~599 서버 오류

		// 네트워크 오류 재시도 범위
		bool retry_on_timeout{ true };          // timed out
		bool retry_on_connect_fail{ true };     // connect reset, connection refused 등
		bool retry_on_read_write_fail{ true };  // transient read/write 실패
	};
}