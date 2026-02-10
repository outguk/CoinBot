// tests/test_utils.h
//
// 테스트 공통 유틸리티
// - NDEBUG에 영향받지 않는 검증 매크로
// - 실패 시 예외 기반 처리 (try/catch 가능)
// - 명확한 실패 위치 출력

#pragma once

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cmath>
#include <string>

namespace test {

    // 테스트 실패 예외
    /*
    * std::runtime_error를 상속해서 표준 예외 체계로 던질 수 있게함 - catch로 잡힘
    */
    class TestFailure : public std::runtime_error {
    public:
        TestFailure(const std::string& condition, const char* file, int line)
            : std::runtime_error(buildMessage(condition, file, line))
            , condition_(condition)
            , file_(file)
            , line_(line)
        {}

        const std::string& condition() const { return condition_; }
        const std::string& file() const { return file_; }
        int line() const { return line_; }

    private:
        static std::string buildMessage(const std::string& condition, const char* file, int line) {
            std::ostringstream oss;
            oss << "Assertion failed: " << condition
                << " at " << file << ":" << line;
            return oss.str();
        }

        std::string condition_;
        std::string file_;
        int line_;
    };

    // 검증 함수 (내부 구현)
    inline void verifyImpl(bool condition, const char* expr, const char* file, int line) {
        if (!condition) {
            std::cerr << "  [FAIL] " << expr << "\n";
            std::cerr << "         at " << file << ":" << line << "\n";
            throw TestFailure(expr, file, line);
        }
    }

    // 부동소수점 비교 헬퍼
    inline bool almostEqual(double a, double b, double epsilon = 1e-7) {
        return std::abs(a - b) < epsilon;
    }

} // namespace test

// 테스트 검증 매크로
// - assert() 대신 사용
// - NDEBUG에 영향받지 않음
// - 실패 시 예외를 던져 try/catch로 처리 가능
// cond를 평가해서 bool로 verifyImpl에 전달 -> 실패면 cerr 출력, TestFailure throw
#define TEST_ASSERT(cond) \
    test::verifyImpl(cond, #cond, __FILE__, __LINE__)

// 동등성 검증 (더 명확한 메시지)
#define TEST_ASSERT_EQ(actual, expected) \
    do { \
        auto _actual = (actual); \
        auto _expected = (expected); \
        if (_actual != _expected) { \
            std::cerr << "  [FAIL] " << #actual << " == " << #expected << "\n"; \
            std::cerr << "         actual: " << _actual << "\n"; \
            std::cerr << "         expected: " << _expected << "\n"; \
            std::cerr << "         at " << __FILE__ << ":" << __LINE__ << "\n"; \
            throw test::TestFailure(#actual " == " #expected, __FILE__, __LINE__); \
        } \
    } while(0)

// 부동소수점 동등성 검증
#define TEST_ASSERT_DOUBLE_EQ(actual, expected) \
    do { \
        auto _actual = (actual); \
        auto _expected = (expected); \
        if (!test::almostEqual(_actual, _expected)) { \
            std::cerr << "  [FAIL] " << #actual << " ~= " << #expected << "\n"; \
            std::cerr << "         actual: " << _actual << "\n"; \
            std::cerr << "         expected: " << _expected << "\n"; \
            std::cerr << "         diff: " << std::abs(_actual - _expected) << "\n"; \
            std::cerr << "         at " << __FILE__ << ":" << __LINE__ << "\n"; \
            throw test::TestFailure(#actual " ~= " #expected, __FILE__, __LINE__); \
        } \
    } while(0)
