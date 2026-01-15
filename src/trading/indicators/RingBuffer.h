#pragma once

#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>
#include <stdexcept>

namespace trading::indicators 
{

	/*
	RollingWindow (double 전용, 고정 길이 링버퍼)

	역할
	- 지표 계산에서 최근 N개 값(종가, 변화량 등)을 O(1)로 관리하기 위함
	- "최근 N개 값"을 저장하고 oldest -> latest 기준으로 접근할 수 있게 한다.
	- push(새 값 추가) 시 버퍼가 가득 차면 가장 오래된 값을 덮어씀
	- SMA, 표준편차(rolling stdev), close[N] 같은 롤링 지표 구현에 최적
	- 지표 클래스 내부에서만 쓰이는 저장소로, Candle/Strategy/Engine에 대해 아무것도 모른다.

	핵심 설계 의도
	1) push(x) 는 새 값을 넣고, 버퍼가 꽉 찬 상태에서 덮어쓴 값이 있으면 반환한다.
	   - SMA: 덮어쓴 값을 sum에서 빼기 위해 필요
	   - RollingStdev: 덮어쓴 값을 sum/sumsq에서 빼기 위해 필요
	2) at(i)는 oldest 기준 i번째를 반환한다. (0=oldest, count-1=latest)
	3) latest()는 가장 최근 값을 빠르게 반환한다.
	4) capacity=0도 안전하게 동작 (push는 nullopt, at/latest는 nullopt)

	성능
	- push/at/latest 모두 O(1)
	//- 내부 vector는 생성 시 capacity로 고정, 이후 realloc 없음
	//- 그래서 T는 기본 생성 가능(default-constructible)해야 함
	//(double 기반 지표에서는 문제 없음)
	*/

	template <typename T>
	class RingBuffer final
	{
		// vector를 capacity만큼 미리 채우기 때문에 T{}가 가능해야 함
		static_assert(std::is_default_constructible_v<T>,
			"RingBuffer<T>는 T가 기본 생성 가능해야 합니다 (미리 할당된 vector 사용)");
		// push 시 덮어쓰기 때문에 대입이 가능해야 함(이동/복사)
		static_assert(std::is_move_assignable_v<T> || std::is_copy_assignable_v<T>,
			"RingBuffer<T>는 move-assignable 또는 copy-assignable이어야 합니다");

	public:
		using value_type = T;

		// 기본 생성자: capacity=0인 비활성 버퍼
		RingBuffer() = default;

		// capacity를 지정하는 생성자: 내부 vector를 capacity만큼 미리 확보
		explicit RingBuffer(std::size_t capacity)
			: buf_(capacity), cap_(capacity) {
		}

		/*
		* reset: capacity를 새로 지정하면서 내부 상태 초기화(내용 비움)
		* - 지표 윈도우 크기가 바뀌거나 재시작할 때 유용
		*/
		void reset(std::size_t capacity) {
			buf_.assign(capacity, T{});
			cap_ = capacity;
			head_ = 0;  // 가장 오래된 요소의 물리 인덱스
			size_ = 0;  // 현재 유효 데이터 개수
		}

		/*
		* clear: 내용만 비우기 (vector 메모리는 유지)
		* - reset보다 훨씬 가볍고 빠름
		*/
		void clear() noexcept {
			head_ = 0;
			size_ = 0;
		}

		// 용량/크기 조회
		[[nodiscard]] std::size_t capacity() const noexcept { return cap_; }
		[[nodiscard]] std::size_t size() const noexcept { return size_; }
		[[nodiscard]] bool empty() const noexcept { return size_ == 0; }
		[[nodiscard]] bool full() const noexcept { return cap_ != 0 && size_ == cap_; }

		/*
		 * push: 새 값을 버퍼에 넣는다.
		 *
		 * 동작
		 * - 아직 가득 차지 않았으면: 뒤에 추가(size 증가), 덮어쓴 값 없음(nullopt)
		 * - 가득 찼으면: 가장 오래된 요소를 덮어씀
		 *   → 덮어쓴(oldest) 값을 optional로 반환
		 *
		 * 왜 덮어쓴 값을 반환하나?
		 * - SMA: sum += new - old
		 * - RollingStdDev: sum/sumsq 갱신에서 old 제거가 필요
		 *   이 때 old를 즉시 알 수 있으면 O(1) 유지 가능
		 */
		std::optional<T> push(const T& v)
			noexcept(std::is_nothrow_copy_assignable_v<T>&& std::is_nothrow_copy_constructible_v<T>)
		{
			return pushImpl(v);
		}

		std::optional<T> push(T&& v)
			noexcept(std::is_nothrow_move_assignable_v<T>&& std::is_nothrow_move_constructible_v<T>)
		{
			return pushImpl(std::move(v));
		}


		/*
		 * at(index_from_oldest)
		 * - “가장 오래된 값 기준” 인덱싱
		 * - 0 = oldest, size-1 = newest
		 * - 범위 벗어나면 예외 발생(std::out_of_range)
		 *
		 * 지표 구현에서 “N개 중 i번째” 접근이 필요할 때 사용
		 */
		[[nodiscard]] T& at(std::size_t index_from_oldest) {
			if (index_from_oldest >= size_) throw std::out_of_range("RingBuffer::at (oldest) out of range");
			return buf_[physicalIndexFromOldest(index_from_oldest)];
		}

		[[nodiscard]] const T& at(std::size_t index_from_oldest) const {
			if (index_from_oldest >= size_) throw std::out_of_range("RingBuffer::at (oldest) out of range");
			return buf_[physicalIndexFromOldest(index_from_oldest)];
		}

		/*
		 * operator[]
		* - at()과 동일하지만 범위 체크 없음(빠름)
		 * - 호출자가 인덱스 범위를 보장해야 함
		*/
		[[nodiscard]] T& operator[](std::size_t index_from_oldest) noexcept {
			return buf_[physicalIndexFromOldest(index_from_oldest)];
		}

		[[nodiscard]] const T& operator[](std::size_t index_from_oldest) const noexcept {
			return buf_[physicalIndexFromOldest(index_from_oldest)];
		}

		/*
		 * newest(): 가장 최신 값(가장 마지막에 push된 값)
		 * - 비어있으면 예외
		 */
		[[nodiscard]] T& newest() {
			if (size_ == 0) throw std::out_of_range("RingBuffer::newest on empty buffer");
			return buf_[physicalIndexFromNewest(0)];
		}

		[[nodiscard]] const T& newest() const {
			if (size_ == 0) throw std::out_of_range("RingBuffer::newest on empty buffer");
			return buf_[physicalIndexFromNewest(0)];
		}

		/*
		 * oldest(): 가장 오래된 값
		 * - 비어있으면 예외
		 */
		[[nodiscard]] T& oldest() {
			if (size_ == 0) throw std::out_of_range("RingBuffer::oldest on empty buffer");
			return buf_[physicalIndexFromOldest(0)];
		}

		[[nodiscard]] const T& oldest() const {
			if (size_ == 0) throw std::out_of_range("RingBuffer::oldest on empty buffer");
			return buf_[physicalIndexFromOldest(0)];
		}

		/*
		 * valueFromBack(back_index)
		 * - “최신 기준” 인덱싱(뒤에서부터)
		 * - back_index = 0 -> newest
		 * - back_index = 1 -> newest보다 1개 이전
		 *
		 * close[N] 같은 지표에 유용:
		 * - 예: N=20일 때, back_index=20으로 과거 값 접근(단, size가 충분해야)
		 *
		 * 반환값:
		 * - 충분한 데이터가 없으면 nullopt
		 * - 있으면 값(복사) 반환
		 */
		[[nodiscard]] std::optional<T> valueFromBack(std::size_t back_index) const
			noexcept(std::is_nothrow_copy_constructible_v<T>)
		{
			if (back_index >= size_) return std::nullopt;
			return buf_[physicalIndexFromNewest(back_index)];
		}

		/*
		 * refFromBack(back_index)
		 * - valueFromBack과 동일한 의미지만 “참조”를 반환
		 * - 범위 벗어나면 예외
		 *
		 * 성능:
		 * - 큰 타입이면 복사 대신 참조가 유리
		 * - double이면 큰 차이 없지만 API로 제공해두면 확장성 좋음
		 */
		[[nodiscard]] const T& refFromBack(std::size_t back_index) const {
			if (back_index >= size_) throw std::out_of_range("RingBuffer::refFromBack out of range");
			return buf_[physicalIndexFromNewest(back_index)];
		}

		/*
		* has(n): 최소 n개 이상의 데이터가 쌓였는지
		* - 지표 준비 여부(윈도우 충족) 체크에 바로 사용 가능
		*/
		[[nodiscard]] bool hasEnough(std::size_t n) const noexcept { return size_ >= n; }

	private:

		/*
		* pushImpl: push의 공통 구현
		* - const& / rvalue를 모두 처리하기 위해 템플릿으로 작성
		*/
		template <class U>
		std::optional<T> pushImpl(U&& v)
			noexcept((std::is_nothrow_assignable_v<T&, U&&>) &&
				(std::is_nothrow_move_constructible_v<T> || std::is_nothrow_copy_constructible_v<T>))
		{
			// capacity가 0이면 어떤 값도 저장하지 않음(안전한 no-op)
			if (cap_ == 0) return std::nullopt;

			std::optional<T> overwritten;

			if (size_ == cap_) {
				/*
				 * 가득 찬 상태:
				 * - head_ 위치(= oldest)를 덮어쓴다.
				 * - 덮어쓰기 전에 기존 값을 overwritten으로 반환(지표 롤링 업데이트용)
				 * - head_를 한 칸 앞으로 이동 → 이제 다음 oldest가 됨
				 */
				overwritten.emplace(std::move(buf_[head_]));
				buf_[head_] = std::forward<U>(v);
				head_ = (head_ + 1) % cap_;
			}
			else {
				/*
				 * 아직 덜 찬 상태:
				 * - head_는 그대로 두고, (head_ + size_) 위치에 새 값을 넣는다.
				 * - size_ 증가
				 */
				const std::size_t idx = (head_ + size_) % cap_;
				buf_[idx] = std::forward<U>(v);
				++size_;
			}
			return overwritten;
		}

		/*
		* physicalIndexFromOldest(i)
		* - “oldest 기준 i번째”가 내부 vector에서 어디에 있는지(물리 인덱스) 계산
		* - oldest는 head_가 가리킴
		*/
		[[nodiscard]] std::size_t physicalIndexFromOldest(std::size_t i) const noexcept {
			return (cap_ == 0) ? 0 : (head_ + i) % cap_;
		}

		/*
		* physicalIndexFromNewest(back_index)
		 * - “newest 기준 back_index번째”가 내부 vector에서 어디인지 계산
		 * - newest는 (head_ + size_ - 1) 위치
		 * - 그 위치에서 back_index만큼 뒤로 이동한 인덱스를 반환
		*/
		[[nodiscard]] std::size_t physicalIndexFromNewest(std::size_t back_index) const noexcept {
			if (cap_ == 0) return 0;

			const std::size_t newest = (head_ + size_ - 1) % cap_;
			return (newest + cap_ - (back_index % cap_)) % cap_;
		}

		// 실제 저장소(고정 크기). cap_만큼 미리 확보
		std::vector<T> buf_{};
		std::size_t cap_{ 0 };

		// head_는 “oldest”의 물리 인덱스를 가리킨다( size_ > 0일 때 의미 있음 )
		std::size_t head_{ 0 };

		// 현재 유효한 데이터 개수 (0..cap_)
		std::size_t size_{ 0 };
	};
}