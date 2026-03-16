add_library(coinbot_app STATIC
    EventRouter.cpp
    MarketEngineManager.cpp
    StartupRecovery.cpp
)

target_include_directories(coinbot_app PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(coinbot_app PUBLIC cxx_std_20)

target_link_libraries(coinbot_app
    PRIVATE
        coinbot_engine
        coinbot_trading
        coinbot_api
        coinbot_database
        coinbot_util
        coinbot_core
)
