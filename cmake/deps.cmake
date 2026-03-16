# cmake/deps.cmake — 의존성 경로 및 find_package 선언
# COINBOT_BOOST_ROOT, COINBOT_OPENSSL_ROOT, COINBOT_NLOHMANN_DIR 캐시 변수가
# 정의되어 있으면 해당 경로를 사용하고, 없으면 시스템 경로를 탐색 (Linux 호환)

if(WIN32)
    if(DEFINED COINBOT_BOOST_ROOT)
        set(BOOST_ROOT "${COINBOT_BOOST_ROOT}")
        set(BOOST_LIBRARYDIR "${BOOST_ROOT}/stage/lib")
        set(Boost_NO_SYSTEM_PATHS ON)
    endif()
    set(Boost_USE_STATIC_LIBS ON)

    if(DEFINED COINBOT_OPENSSL_ROOT)
        set(OPENSSL_ROOT_DIR "${COINBOT_OPENSSL_ROOT}")
    endif()
    set(OPENSSL_USE_STATIC_LIBS OFF)

    if(DEFINED COINBOT_NLOHMANN_DIR)
        set(COINBOT_NLOHMANN_INCLUDE "${COINBOT_NLOHMANN_DIR}")
    endif()
else()
    # Linux: apt install libboost-all-dev libssl-dev nlohmann-json3-dev
    set(Boost_USE_STATIC_LIBS OFF)
    if(NOT DEFINED COINBOT_NLOHMANN_INCLUDE)
        set(COINBOT_NLOHMANN_INCLUDE "/usr/include/nlohmann")
    endif()
endif()

find_package(Boost 1.80 REQUIRED COMPONENTS thread chrono)
find_package(OpenSSL REQUIRED)
