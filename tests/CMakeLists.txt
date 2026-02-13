# Tests CMakeLists.txt

# Real trading test (main executable)
add_executable(real_trading_test
    "single_real_trading_test.cpp"
)

target_include_directories(real_trading_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        "C:/git-repository/boost_1_89_0"
        "C:/git-repository/nlohmann_json"
)

target_link_libraries(real_trading_test
    PRIVATE
        Boost::thread
        Boost::chrono
        OpenSSL::SSL
        OpenSSL::Crypto
)

# SharedOrderApi thread-safety test
add_executable(test_shared_order_api
    
    ${CMAKE_SOURCE_DIR}/src/api/upbit/SharedOrderApi.cpp
    ${CMAKE_SOURCE_DIR}/src/api/upbit/UpbitExchangeRestClient.cpp
    ${CMAKE_SOURCE_DIR}/src/api/rest/RestClient.cpp
    ${CMAKE_SOURCE_DIR}/src/api/rest/RestError.cpp
    ${CMAKE_SOURCE_DIR}/src/api/auth/UpbitJwtSigner.cpp
)

target_include_directories(test_shared_order_api
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        "C:/git-repository/boost_1_89_0"
        "C:/git-repository/nlohmann_json"
)

target_link_libraries(test_shared_order_api
    PRIVATE
        Boost::thread
        Boost::chrono
        OpenSSL::SSL
        OpenSSL::Crypto
)

# SharedOrderApi advanced test (serialization proof, exception safety)
add_executable(test_shared_order_api_advanced
    test_shared_order_api_advanced.cpp
    ${CMAKE_SOURCE_DIR}/src/api/upbit/SharedOrderApi.cpp
    ${CMAKE_SOURCE_DIR}/src/api/upbit/UpbitExchangeRestClient.cpp
    ${CMAKE_SOURCE_DIR}/src/api/rest/RestClient.cpp
    ${CMAKE_SOURCE_DIR}/src/api/rest/RestError.cpp
    ${CMAKE_SOURCE_DIR}/src/api/auth/UpbitJwtSigner.cpp
)

target_include_directories(test_shared_order_api_advanced
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        "C:/git-repository/boost_1_89_0"
        "C:/git-repository/nlohmann_json"
)

target_link_libraries(test_shared_order_api_advanced
    PRIVATE
        Boost::thread
        Boost::chrono
        OpenSSL::SSL
        OpenSSL::Crypto
)

# AccountManager unified test (basic + edge cases)
add_executable(test_account_manager_unified
    test_account_manager_unified.cpp
    ${CMAKE_SOURCE_DIR}/src/trading/allocation/AccountManager.cpp
)

target_include_directories(test_account_manager_unified
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        "C:/git-repository/boost_1_89_0"
        "C:/git-repository/nlohmann_json"
)

target_link_libraries(test_account_manager_unified
    PRIVATE
        Boost::thread
        Boost::chrono
)

# MarketEngine unit test (manual test style, no GoogleTest)
add_executable(test_market_engine
    test_market_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/MarketEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/OrderStore.cpp
    ${CMAKE_SOURCE_DIR}/src/trading/allocation/AccountManager.cpp
)

target_include_directories(test_market_engine
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests
        "C:/git-repository/boost_1_89_0"
        "C:/git-repository/nlohmann_json"
)

target_link_libraries(test_market_engine
    PRIVATE
        Boost::thread
        Boost::chrono
)

# EventRouter unit test (fast path, fallback, backpressure, multi-market, stats)
add_executable(test_event_router
    test_event_router.cpp
    ${CMAKE_SOURCE_DIR}/src/app/EventRouter.cpp
)

target_include_directories(test_event_router
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests
        "C:/git-repository/nlohmann_json"
)

# MarketEngineManager integration test
add_executable(test_market_engine_manager
    test_market_engine_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/app/MarketEngineManager.cpp
    ${CMAKE_SOURCE_DIR}/src/app/StartupRecovery.cpp
    ${CMAKE_SOURCE_DIR}/src/app/EventRouter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/MarketEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/OrderStore.cpp
    ${CMAKE_SOURCE_DIR}/src/trading/allocation/AccountManager.cpp
    ${CMAKE_SOURCE_DIR}/src/trading/strategies/RsiMeanReversionStrategy.cpp
    ${CMAKE_SOURCE_DIR}/src/trading/indicators/RsiWilder.cpp
    ${CMAKE_SOURCE_DIR}/src/trading/indicators/Sma.cpp
    ${CMAKE_SOURCE_DIR}/src/trading/indicators/ClosePriceWindow.cpp
    ${CMAKE_SOURCE_DIR}/src/trading/indicators/ChangeVolatilityIndicator.cpp
    ${CMAKE_SOURCE_DIR}/src/api/auth/UpbitJwtSigner.cpp
    ${CMAKE_SOURCE_DIR}/src/api/rest/RestError.cpp
    ${CMAKE_SOURCE_DIR}/src/api/rest/RestClient.cpp
    ${CMAKE_SOURCE_DIR}/src/api/upbit/UpbitExchangeRestClient.cpp
)

target_include_directories(test_market_engine_manager
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests
        "C:/git-repository/boost_1_89_0"
        "C:/git-repository/nlohmann_json"
)

target_link_libraries(test_market_engine_manager
    PRIVATE
        Boost::thread
        Boost::chrono
        OpenSSL::SSL
        OpenSSL::Crypto
)

# Other test executables (currently not building, can be added later)
# These would require proper library separation first

# add_executable(coinbot_tests
#     test_upbit_public.cpp
#     test_strategy.cpp
#     test_indicators.cpp
#     test_candle_web.cpp
# )
#
# target_include_directories(coinbot_tests
#     PRIVATE
#         ${CMAKE_SOURCE_DIR}/src
#         "C:/git-repository/boost_1_89_0"
#         "C:/git-repository/nlohmann_json"
# )
#
# target_link_libraries(coinbot_tests
#     PRIVATE
#         Boost::thread
#         Boost::chrono
#         OpenSSL::SSL
#         OpenSSL::Crypto
# )
