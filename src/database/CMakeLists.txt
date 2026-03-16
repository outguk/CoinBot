set_source_files_properties(
    sqlite3.c
    PROPERTIES LANGUAGE C
)

add_library(coinbot_database STATIC
    sqlite3.c
    Database.cpp
)

target_include_directories(coinbot_database PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(coinbot_database PUBLIC cxx_std_20)
