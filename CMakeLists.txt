cmake_minimum_required(VERSION 3.16)
project(shortcog VERSION 0.1.0 LANGUAGES CXX)

option(SHORTCOG_BUILD_TESTS "Build shortcog C++ tests" OFF)

add_subdirectory(core)

if(SHORTCOG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(core/tests)
endif()
