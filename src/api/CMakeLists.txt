add_library(coinbot_api STATIC
    rest/RestClient.cpp
    rest/RestError.cpp
    auth/UpbitJwtSigner.cpp
    upbit/UpbitPublicRestClient.cpp
    upbit/UpbitExchangeRestClient.cpp
    upbit/SharedOrderApi.cpp
    upbit/WsMessageParser.cpp
    ws/UpbitWebSocketClient.cpp
)

target_include_directories(coinbot_api PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

if(COINBOT_NLOHMANN_INCLUDE)
    target_include_directories(coinbot_api PUBLIC ${COINBOT_NLOHMANN_INCLUDE})
endif()

target_compile_features(coinbot_api PUBLIC cxx_std_20)

target_link_libraries(coinbot_api
    PUBLIC
        coinbot_core
        Boost::thread
        Boost::chrono
        OpenSSL::SSL
        OpenSSL::Crypto
)
