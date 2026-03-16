add_library(coinbot_util INTERFACE)

target_include_directories(coinbot_util INTERFACE
    ${CMAKE_SOURCE_DIR}/src
)

if(COINBOT_NLOHMANN_INCLUDE)
    target_include_directories(coinbot_util INTERFACE ${COINBOT_NLOHMANN_INCLUDE})
endif()

target_compile_features(coinbot_util INTERFACE cxx_std_20)
