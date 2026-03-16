add_library(trading_indicators STATIC
    RsiWilder.cpp
    Sma.cpp
    ClosePriceWindow.cpp
    ChangeVolatilityIndicator.cpp
)

target_include_directories(trading_indicators PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(trading_indicators PUBLIC cxx_std_20)
