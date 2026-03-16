add_subdirectory(indicators)

add_library(coinbot_trading STATIC
    strategies/RsiMeanReversionStrategy.cpp
    allocation/AccountManager.cpp
)

target_include_directories(coinbot_trading PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(coinbot_trading PUBLIC cxx_std_20)

target_link_libraries(coinbot_trading
    PUBLIC
        coinbot_core
        trading_indicators
    PRIVATE
        coinbot_util
        Boost::boost
)
