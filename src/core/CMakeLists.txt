add_library(coinbot_core INTERFACE)

target_include_directories(coinbot_core INTERFACE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(coinbot_core INTERFACE cxx_std_20)
