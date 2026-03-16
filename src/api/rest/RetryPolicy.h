#pragma once
#include <chrono>
#include <cstddef>


// RetryPolicy는 재시도 기준을 값으로 넘겨 API별 차이를 호출부에서 조정하게 한다.
// 동기 REST 호출이므로 기본값은 짧게 두고, 일시적 실패만 다시 시도한다.

namespace api::rest
{

	// RestClient는 이 값만 읽고 재시도 여부와 지연을 결정한다.
	struct RetryPolicy
	{
		std::size_t					max_attempts{ 2 };	// 총 시도 횟수
		std::chrono::milliseconds	base_delay{ 200 };	// 첫 재시도 전 대기 시간
		double				backoff_multiplier{ 2.0 };	// 연속 실패 시 재요청 간격을 늘린다.

		// 서버가 일시적으로 버거운 경우에만 status 재시도를 허용한다.
		bool				retry_on_429{ true };
		bool				retry_on_5xx{ true };

		// 네트워크 단절은 일시적일 수 있어 세분화해 켜고 끌 수 있게 둔다.
		bool retry_on_timeout{ true };
		bool retry_on_connect_fail{ true };
		bool retry_on_read_write_fail{ true };
	};
}
