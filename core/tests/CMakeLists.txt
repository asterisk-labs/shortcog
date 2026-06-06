# test_fixtures   round-trips the whole corpus: build blob, open through
#                 SHORTCOG, SHA256 each band, compare to the sidecar.
# test_threading  opens the multi-band fixture with NUM_THREADS=4 and reads it
#                 from several threads at once.
# test_parser     feeds malformed blobs to parse_blob, all built in memory.

find_package(nlohmann_json 3.10 QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found, fetching v3.11.3")
    include(FetchContent)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install    OFF CACHE INTERNAL "")
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

function(shortcog_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE shortcog::shortcog ${ARGN})
    target_compile_features(${name} PRIVATE cxx_std_23)
    set_target_properties(${name} PROPERTIES
        CXX_VISIBILITY_PRESET     hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
endfunction()

shortcog_add_test(test_fixtures  nlohmann_json::nlohmann_json)
shortcog_add_test(test_threading nlohmann_json::nlohmann_json)
shortcog_add_test(test_parser)

# Override from the command line when CI mounts the corpus elsewhere.
set(SHORTCOG_TEST_FIXTURE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/data"
    CACHE PATH
    "Directory with manifest.json and the .tif/.json fixture pairs")

add_test(NAME test_fixtures
         COMMAND $<TARGET_FILE:test_fixtures> "${SHORTCOG_TEST_FIXTURE_DIR}")
add_test(NAME test_threading
         COMMAND $<TARGET_FILE:test_threading> "${SHORTCOG_TEST_FIXTURE_DIR}")
add_test(NAME test_parser
         COMMAND $<TARGET_FILE:test_parser>)

set_tests_properties(test_fixtures PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL|cannot open|manifest.json parse error"
    LABELS                  "shortcog;roundtrip")
set_tests_properties(test_threading PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL|cannot open|manifest.json parse error"
    LABELS                  "shortcog;threading")
set_tests_properties(test_parser PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL"
    LABELS                  "shortcog;parser")