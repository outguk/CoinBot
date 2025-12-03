# CMakeList.txt : CMake project for CoinBot, include source and define
# project specific logic here.
#

cmake_minimum_required (VERSION 3.8)


project ("CoinBot")

# 전역 C++ 표준 설정
set(CMAKE_CXX_STANDARD 20) # C++ 표준을 C++20으로 설정.
set(CMAKE_CXX_STANDARD_REQUIRED ON) # 컴파일러가 C++20을 지원하지 않는 환경이면 CMake 단계에서 실패하도록
set(CMAKE_CXX_EXTENSIONS OFF) # 컴파일러의 비표준 확장(-std=gnu++20 같은 것)을 끄고 순수한 표준 C++ 모드(-std=c++20)로 빌드하게 함

# Boost 경로 직접 지정
set(BOOST_ROOT "C:/git-repository/boost_1_89_0")
set(BOOST_LIBRARYDIR "${BOOST_ROOT}/stage/lib")   # 빌드된 .lib 위치
set(Boost_NO_SYSTEM_PATHS ON)          # 시스템 경로 무시하고 위 경로 우선
set(Boost_USE_STATIC_LIBS ON)         # 위에서 static으로 빌드 했다면 ON

# OpenSSL 경로 직접 지정
set(OPENSSL_ROOT_DIR "C:/git-repository/OpenSSL-Win64")
set(OPENSSL_USE_STATIC_LIBS OFF)

find_package(Boost 1.80 REQUIRED COMPONENTS thread chrono)
find_package(OpenSSL REQUIRED)


# 빌드 옵션 플래그 정의
option(COINBOT_BUILD_TESTS "Build unit tests" ON) # 유닛 테스트를 빌드할지 말지 선택할 수 있는 스위치 하나 만들기

# 컴파일할 때 #include 경로에 전역적으로 디렉토리 하나 추가
include_directories(${CMAKE_SOURCE_DIR}/include) # 프로젝트 루트 / include 디렉토리

# coinbot/src/ 폴더 안에 있는 CMakeLists.txt를 읽어서 처리해라
add_subdirectory(src)

# 위에서 만든 옵션 COINBOT_BUILD_TESTS가 ON 일 때만 tests 폴더를 CMake 빌드 트리 안으로 포함시켜라
if (COINBOT_BUILD_TESTS)
    add_subdirectory(tests) # coinbot/tests/ 아래에 있는 CMakeLists.txt를 불러와서 유닛 테스트용 타깃을 정의
endif()




# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

# Add source to this project's executable.
add_executable (CoinBot 
    ${PROJECT_SOURCE_DIR}/src/app/CoinBot.cpp
    ${PROJECT_SOURCE_DIR}/CoinBot.h
  "src/core/domain/OrderEnums.h" "src/core/domain/Types.h" "src/core/domain/MarketInfo.h" "src/core/domain/Order.h" "src/core/domain/Trade.h" "src/core/domain/Position.h" "src/core/domain/Account.h" "src/core/domain/Ticker.h" "src/core/domain/Candle.h" "src/core/domain/OrderBook.h" "src/api/upbit/UpbitDtos.h" "src/api/upbit/UpbitConverters.h" "src/api/upbit/UpbitPublicRestClient.h" "src/api/upbit/UpbitPublicRestClient.cpp")


if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET CoinBot PROPERTY CXX_STANDARD 20)
endif()


# ===== CoinBot에 Boost / include 경로 연결 =====
target_include_directories(CoinBot
    PRIVATE
        ${Boost_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/include
        "C:/git-repository/nlohmann_json"
)

target_link_libraries(CoinBot
    PRIVATE
        Boost::thread
        Boost::chrono
        OpenSSL::SSL
        OpenSSL::Crypto
)


# TODO: Add tests and install targets if needed.
