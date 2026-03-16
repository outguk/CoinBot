# CMakeLists.txt : CMake project for CoinBot

cmake_minimum_required(VERSION 3.8)

project("CoinBot")

# MSVC 환경에서 UTF-8 소스 인코딩을 강제하고 Windows 타겟 버전을 명시
if(MSVC)
    add_compile_options(/utf-8)
    add_compile_definitions(_WIN32_WINNT=0x0A00)
endif()

# 전역 C++ 표준 설정
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# (선택) VS IntelliSense에 도움이 되는 경우가 많음
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Enable Hot Reload for MSVC compilers if supported.
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

# 의존성 경로 + find_package (Boost, OpenSSL, nlohmann)
include(cmake/deps.cmake)

# 모듈별 서브디렉토리
add_subdirectory(src/core)
add_subdirectory(src/util)
add_subdirectory(src/database)
add_subdirectory(src/api)
add_subdirectory(src/trading)
add_subdirectory(src/engine)
add_subdirectory(src/app)

# 최종 실행 파일
add_executable(CoinBot src/app/CoinBot.cpp)

target_compile_features(CoinBot PRIVATE cxx_std_20)

target_link_libraries(CoinBot
    PRIVATE
        coinbot_app
        coinbot_engine
        coinbot_trading
        coinbot_api
        coinbot_database
        coinbot_util
        coinbot_core
)

# TODO: Add tests and install targets if needed.
