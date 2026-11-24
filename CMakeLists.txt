cmake_minimum_required(VERSION 3.20)
project(neojson
    VERSION 0.1.0
    DESCRIPTION "An nlohmann-compatible C++17 JSON library with native AVX2/NEON SIMD parsers and a lazy COW DOM"
    HOMEPAGE_URL "https://github.com/fox1245/neojson"
    LANGUAGES C CXX)

string(COMPARE EQUAL "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}" NEOJSON_IS_TOP_LEVEL)

option(NEOJSON_BUILD_TESTS "Build the tests (differential tests need nlohmann/json, fetched at configure time)" ${NEOJSON_IS_TOP_LEVEL})
option(NEOJSON_BUILD_EXAMPLES "Build the example programs" OFF)
option(NEOJSON_INSTALL "Generate installation targets for neojson" ${NEOJSON_IS_TOP_LEVEL})
option(NEOJSON_SANITIZE "Build everything with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(NEOJSON_TEST_TIME_BOUNDS
    "Give the large-input parse tests a short CTest timeout, so that a cost that grows with the square of the input fails them (turn OFF for sanitizer builds, which run them several hundred times slower and should check results only)"
    ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NEOJSON_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

include(GNUInstallDirs)

add_library(neojson_yyjson OBJECT vendor/yyjson/yyjson.c)
set_target_properties(neojson_yyjson PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden)
target_compile_definitions(neojson_yyjson PRIVATE yyjson_api=)

if(WIN32)
    add_library(neojson STATIC
        src/json.cpp
        $<TARGET_OBJECTS:neojson_yyjson>)
else()
    add_library(neojson
        src/json.cpp
        $<TARGET_OBJECTS:neojson_yyjson>)
endif()
add_library(neojson::neojson ALIAS neojson)
set_target_properties(neojson PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_EXTENSIONS OFF
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
target_compile_features(neojson PUBLIC cxx_std_17)
target_include_directories(neojson
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE
        ${PROJECT_SOURCE_DIR}/vendor/yyjson)
target_compile_options(neojson PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)

if(NEOJSON_BUILD_TESTS)
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
    file(GLOB NEOJSON_TEST_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/*.cpp)
    add_executable(neojson_tests ${NEOJSON_TEST_SOURCES})
    target_link_libraries(neojson_tests PRIVATE neojson::neojson nlohmann_json::nlohmann_json GTest::gtest_main)
    # The differential tests nest documents past a thousand levels, and the oracle copies, compares and dumps them
    # recursively; Windows' default 1 MB main-thread stack overflows on arm64. Give the test binary the 8 MB that Linux
    # and macOS give by default. (How deep the library itself goes on a 1 MB stack is measured separately.)
    if(MSVC)
        target_link_options(neojson_tests PRIVATE /STACK:8388608)
    endif()
    # Some tests run threads with a small stack.
    find_package(Threads REQUIRED)
    target_link_libraries(neojson_tests PRIVATE Threads::Threads)
    include(GoogleTest)
    gtest_discover_tests(neojson_tests DISCOVERY_TIMEOUT 120)

    # Parse tests of very large inputs (tests/time_bounds): a parse whose cost is not proportional to the input fails
    # them by running into the timeout.
    file(GLOB NEOJSON_TIME_BOUND_TEST_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/time_bounds/*.cpp)
    add_executable(neojson_bounds_tests ${NEOJSON_TIME_BOUND_TEST_SOURCES})
    target_link_libraries(neojson_bounds_tests PRIVATE neojson::neojson GTest::gtest_main)
    if(NEOJSON_TEST_TIME_BOUNDS)
        gtest_discover_tests(neojson_bounds_tests DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 30)
    else()
        gtest_discover_tests(neojson_bounds_tests DISCOVERY_TIMEOUT 120)
    endif()
endif()

if(NEOJSON_BUILD_EXAMPLES)
    enable_testing()
    add_subdirectory(examples)
endif()

if(NEOJSON_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS neojson
        EXPORT neojsonTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT neojsonTargets
        FILE neojsonTargets.cmake
        NAMESPACE neojson::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/neojson)

    configure_package_config_file(
        ${PROJECT_SOURCE_DIR}/cmake/neojsonConfig.cmake.in
        ${PROJECT_BINARY_DIR}/neojsonConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/neojson)

    write_basic_package_version_file(
        ${PROJECT_BINARY_DIR}/neojsonConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion)

    install(FILES
        ${PROJECT_BINARY_DIR}/neojsonConfig.cmake
        ${PROJECT_BINARY_DIR}/neojsonConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/neojson)

    if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
        set(NEOJSON_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
        set(NEOJSON_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
    else()
        file(RELATIVE_PATH NEOJSON_PC_REL_TO_PREFIX "/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/")
        string(REGEX REPLACE "/$" "" NEOJSON_PC_REL_TO_PREFIX "${NEOJSON_PC_REL_TO_PREFIX}")
        set(NEOJSON_PC_PREFIX "\${pcfiledir}/${NEOJSON_PC_REL_TO_PREFIX}")
        set(NEOJSON_PC_LIBDIR "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
    endif()
    if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
        set(NEOJSON_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
    else()
        set(NEOJSON_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
    endif()
    configure_file(
        ${PROJECT_SOURCE_DIR}/cmake/neojson.pc.in
        ${PROJECT_BINARY_DIR}/neojson.pc
        @ONLY)
    install(FILES ${PROJECT_BINARY_DIR}/neojson.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()
