add_library(trading_indicators
    RingWindow.h
    ClosePriceWindow.h
    RsiWilder.h
    RsiWilder.cpp
    Sma.h
    Sma.cpp
    ChangeVolatilityIndicator.h
    ChangeVolatilityIndicator.cpp
)

target_compile_features(trading_indicators PUBLIC cxx_std_20)

# include 경로를 "프로젝트 루트"로 잡으면
# 코드에서 #include "trading/indicators/RsiWilder.h" 형태가 안정적으로 동작
target_include_directories(trading_indicators
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)
