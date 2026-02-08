#pragma once

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace util
{
    // 로그 레벨
    enum class LogLevel : int
    {
        DEBUG = 0,
        INFO = 1,
        WARN = 2,
        LV_ERROR = 3
    };

    /*
     * Logger - 구조화된 로깅 유틸리티
     *
     * 특징:
     * - 타임스탬프 자동 추가
     * - 로그 레벨 필터링
     * - 콘솔 + 파일 동시 출력
     * - 스레드 안전성
     *
     * 사용 예시:
     *   Logger::instance().info("Server started");
     *   Logger::instance().warn("High CPU usage: ", cpu_percent);
     *   Logger::instance().error("Failed to connect: ", error_msg);
     */
    class Logger final
    {
    public:
        // 싱글톤 인스턴스
        static Logger& instance()
        {
            static Logger logger;
            return logger;
        }

        // 로그 레벨 설정
        void setLevel(LogLevel level)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            min_level_ = level;
        }

        // 파일 출력 활성화
        void enableFileOutput(const std::string& filename)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            file_stream_.open(filename, std::ios::app);
            if (!file_stream_.is_open())
            {
                std::cerr << "[Logger] Failed to open log file: " << filename << "\n";
            }
        }

        // 콘솔 출력 비활성화
        void disableConsoleOutput()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            console_enabled_ = false;
        }

        // 로그 메서드
        template <typename... Args>
        void debug(Args&&... args)
        {
            log(LogLevel::DEBUG, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void info(Args&&... args)
        {
            log(LogLevel::INFO, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void warn(Args&&... args)
        {
            log(LogLevel::WARN, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void error(Args&&... args)
        {
            log(LogLevel::LV_ERROR, std::forward<Args>(args)...);
        }

    private:
        Logger() = default;

        // 로그 레벨 문자열 변환
        static constexpr std::string_view levelToString(LogLevel level)
        {
            switch (level)
            {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::LV_ERROR: return "ERROR";
            }
            return "UNKNOWN";
        }

        // 타임스탬프 생성
        static std::string getTimestamp()
        {
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;

            std::tm tm_buf{};
#ifdef _WIN32
            localtime_s(&tm_buf, &time_t_now);
#else
            localtime_r(&time_t_now, &tm_buf);
#endif

            std::ostringstream oss;
            oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
                << '.' << std::setfill('0') << std::setw(3) << ms.count();
            return oss.str();
        }

        // 실제 로그 출력
        template <typename... Args>
        void log(LogLevel level, Args&&... args)
        {
            // 레벨 필터링
            if (level < min_level_)
                return;

            std::lock_guard<std::mutex> lock(mutex_);

            // 메시지 조합
            std::ostringstream oss;
            oss << "[" << getTimestamp() << "] "
                << "[" << levelToString(level) << "] ";
            (oss << ... << args);
            oss << "\n";

            const std::string message = oss.str();

            // 콘솔 출력
            if (console_enabled_)
            {
                std::cout << message << std::flush;
            }

            // 파일 출력
            if (file_stream_.is_open())
            {
                file_stream_ << message << std::flush;
            }
        }

    private:
        std::mutex mutex_;
        LogLevel min_level_{ LogLevel::INFO };
        bool console_enabled_{ true };
        std::ofstream file_stream_;
    };

    // 전역 로거 접근 헬퍼
    inline Logger& log() { return Logger::instance(); }

} // namespace util
