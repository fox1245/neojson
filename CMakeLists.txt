cmake_minimum_required(VERSION 3.20)
project(jsonkit VERSION 0.0.1 LANGUAGES C CXX)

# The working name of the library is `jsonkit` (namespace, target, headers); it is renamed in one step at publication.
option(JSONKIT_BUILD_TESTS "Build the tests (differential tests need nlohmann/json, fetched at configure time)" ON)
option(JSONKIT_SANITIZE "Build everything with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(JSONKIT_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(jsonkit_yyjson OBJECT vendor/yyjson/yyjson.c)
set_target_properties(jsonkit_yyjson PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)

file(GLOB JSONKIT_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.cpp)
add_library(jsonkit ${JSONKIT_SOURCES} $<TARGET_OBJECTS:jsonkit_yyjson>)
add_library(jsonkit::jsonkit ALIAS jsonkit)
target_include_directories(jsonkit
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE ${PROJECT_SOURCE_DIR}/vendor/yyjson)
target_compile_options(jsonkit PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)

if(JSONKIT_BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG b514bdc898e2951020cbdca1304b75f5950d1f59)  # v1.15.2
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03)  # v3.11.3, the oracle of the differential tests
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    # MSVC: build googletest with the same runtime library as the tests (the default /MD), or the link fails.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(googletest nlohmann_json)
    file(GLOB JSONKIT_TEST_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/*.cpp)
    add_executable(jsonkit_tests ${JSONKIT_TEST_SOURCES})
    target_link_libraries(jsonkit_tests PRIVATE jsonkit::jsonkit nlohmann_json::nlohmann_json GTest::gtest_main)
    # The differential tests nest documents past a thousand levels, and the oracle copies, compares and dumps them
    # recursively; Windows' default 1 MB main-thread stack overflows on arm64. Give the test binary the 8 MB that Linux
    # and macOS give by default. (How deep the library itself goes on a 1 MB stack is measured separately.)
    if(MSVC)
        target_link_options(jsonkit_tests PRIVATE /STACK:8388608)
    endif()
    include(GoogleTest)
    gtest_discover_tests(jsonkit_tests DISCOVERY_TIMEOUT 120)
endif()
