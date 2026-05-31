cmake_minimum_required(VERSION 3.21)

project(shortcog
    VERSION     1.0.0
    DESCRIPTION "A COG profile for AI training data"
    LANGUAGES   CXX
)

# ---- Options -----------------------------------------------------------------

option(SHORTCOG_BUILD_PLUGIN     "Build the GDAL auto-loadable plugin"          ON)
option(SHORTCOG_BUILD_SHARED_LIB "Build standalone libshortcog for direct link" OFF)
option(SHORTCOG_BUILD_TESTS      "Build tests"                                  OFF)
option(SHORTCOG_STATIC_ZSTD      "Link zstd statically"                         OFF)

# Tests link against shortcog::shortcog to exercise the public API surface,
# so enabling tests implies the standalone library.
if(SHORTCOG_BUILD_TESTS AND NOT SHORTCOG_BUILD_SHARED_LIB)
    message(STATUS "SHORTCOG_BUILD_TESTS=ON -> auto-enabling SHORTCOG_BUILD_SHARED_LIB")
    set(SHORTCOG_BUILD_SHARED_LIB ON)
endif()

# ---- Defaults ----------------------------------------------------------------

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD             23)
set(CMAKE_CXX_STANDARD_REQUIRED    ON)
set(CMAKE_CXX_EXTENSIONS           OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Hide symbols by default. The C++ surface is re-exported with SHORTCOG_API and
# the extern "C" entry points with GDAL's CPL_DLL.
set(CMAKE_CXX_VISIBILITY_PRESET     hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(GNUInstallDirs)

# ---- Dependencies ------------------------------------------------------------

find_package(GDAL 3.12 CONFIG REQUIRED)

# Modern zstd ships a CMake config; fall back to pkg-config on older systems.
find_package(zstd CONFIG QUIET)
if(zstd_FOUND)
    if(SHORTCOG_STATIC_ZSTD AND TARGET zstd::libzstd_static)
        set(SHORTCOG_ZSTD_TARGET zstd::libzstd_static)
    elseif(TARGET zstd::libzstd_shared)
        set(SHORTCOG_ZSTD_TARGET zstd::libzstd_shared)
    else()
        set(SHORTCOG_ZSTD_TARGET zstd::libzstd)
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(zstd REQUIRED IMPORTED_TARGET libzstd>=1.4)
    set(SHORTCOG_ZSTD_TARGET PkgConfig::zstd)
endif()

# ---- Sources -----------------------------------------------------------------

set(SHORTCOG_SOURCES
    src/parser.cpp
    src/builder.cpp
    src/predictor.cpp
    src/plan.cpp
    src/pattern.cpp
    src/image.cpp
    src/imagecube.cpp
)

# Compile/link config shared by every binary we produce.
add_library(shortcog_config INTERFACE)
target_include_directories(shortcog_config INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(shortcog_config INTERFACE
    GDAL::GDAL
    ${SHORTCOG_ZSTD_TARGET}
)
target_compile_options(shortcog_config INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
)

# ---- Plugin (auto-discovered by GDAL) ----------------------------------------
#
# GDAL's plugin loader scans ${prefix}/lib/gdalplugins (or $GDAL_DRIVER_PATH)
# for files named gdal_<NAME>.{so,dll,dylib} and calls GDALRegister_<NAME>().
# The empty PREFIX below strips the "lib" that CMake would otherwise prepend.

if(SHORTCOG_BUILD_PLUGIN)
    add_library(gdal_SHORTCOG MODULE ${SHORTCOG_SOURCES})
    target_link_libraries(gdal_SHORTCOG PRIVATE shortcog_config)
    # Defines the library symbols, so SHORTCOG_API exports rather than imports.
    target_compile_definitions(gdal_SHORTCOG PRIVATE SHORTCOG_BUILD)
    set_target_properties(gdal_SHORTCOG PROPERTIES
        PREFIX      ""
        OUTPUT_NAME "gdal_SHORTCOG"
    )

    install(TARGETS gdal_SHORTCOG
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gdalplugins
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/gdalplugins
    )
endif()

# ---- Standalone shared library -----------------------------------------------
#
# For apps that prefer to link directly and call shortcog::register_driver()
# themselves rather than relying on plugin auto-discovery.

if(SHORTCOG_BUILD_SHARED_LIB)
    add_library(shortcog SHARED ${SHORTCOG_SOURCES})
    add_library(shortcog::shortcog ALIAS shortcog)
    target_link_libraries(shortcog PUBLIC shortcog_config)
    # PRIVATE so consumers (tests, bindings) see SHORTCOG_API as an import.
    target_compile_definitions(shortcog PRIVATE SHORTCOG_BUILD)
    set_target_properties(shortcog PROPERTIES
        VERSION   ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )

    install(TARGETS shortcog
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(DIRECTORY include/shortcog
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp"
    )
endif()

# ---- Tests -------------------------------------------------------------------

if(SHORTCOG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- Configuration summary ---------------------------------------------------

message(STATUS "")
message(STATUS "shortcog ${PROJECT_VERSION}")
message(STATUS "  GDAL:           ${GDAL_VERSION}")
message(STATUS "  zstd:           ${SHORTCOG_ZSTD_TARGET}")
message(STATUS "  Plugin:         ${SHORTCOG_BUILD_PLUGIN}")
message(STATUS "  Standalone lib: ${SHORTCOG_BUILD_SHARED_LIB}")
message(STATUS "  Tests:          ${SHORTCOG_BUILD_TESTS}")
message(STATUS "")