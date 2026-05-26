cmake_minimum_required(VERSION 3.16)

add_library(shortcog
    src/driver.cpp
    src/header.cpp
    src/reader.cpp
    src/predictor.cpp
)

target_include_directories(shortcog
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(GDAL REQUIRED)
find_package(zstd REQUIRED)

target_link_libraries(shortcog
    PUBLIC GDAL::GDAL
    PRIVATE zstd::libzstd_shared
)

set_target_properties(shortcog PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)
