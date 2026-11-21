# Benchmark of neojson against cpp-yyjson, yyjson's C API and nlohmann/json. Not part of the library build.
#
#   cmake -S bench -B build-bench -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-bench
#   python3 bench/run.py --build build-bench
#
# One executable per library, so that no executable links two copies of yyjson:
#   bench_neojson               neojson (its vendored yyjson)
#   bench_yyjson                yyjson's C API, built from neojson's vendored yyjson.c
#   bench_cppyyjson_same_yyjson cpp-yyjson on the same yyjson.c
#   bench_cppyyjson_bundled     cpp-yyjson on its bundled yyjson (its default)
#   bench_nlohmann              nlohmann::ordered_json
#
# cpp-yyjson defines one library target, so the bundled variant is a second configuration of this project:
# -DBENCH_CPPYYJSON_BUNDLED=ON builds only bench_cppyyjson_bundled.
#
# Every executable gets the same flags: the build type's, no LTO, no -march (cpp-yyjson's AVX2 option is OFF, its
# default outside its own benchmarks).

cmake_minimum_required(VERSION 3.20)
project(neojson_bench LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)  # cpp-yyjson needs C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BENCH_CPPYYJSON_BUNDLED "Build only cpp-yyjson on its bundled yyjson" OFF)

set(NEOJSON_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
include(FetchContent)

# The files, checked against the hashes in corpus.json.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/corpus.json BENCH_CORPUS)
string(JSON BENCH_BASE_URL GET "${BENCH_CORPUS}" base_url)
string(JSON BENCH_FILE_COUNT LENGTH "${BENCH_CORPUS}" files)
math(EXPR BENCH_FILE_LAST "${BENCH_FILE_COUNT} - 1")
foreach(i RANGE ${BENCH_FILE_LAST})
    string(JSON name GET "${BENCH_CORPUS}" files ${i} name)
    string(JSON sha GET "${BENCH_CORPUS}" files ${i} sha256)
    file(DOWNLOAD ${BENCH_BASE_URL}${name} ${CMAKE_BINARY_DIR}/corpus/${name}
         EXPECTED_HASH SHA256=${sha} TLS_VERIFY ON)
endforeach()

add_library(bench_yyjson_c STATIC ${NEOJSON_ROOT}/vendor/yyjson/yyjson.c)
target_include_directories(bench_yyjson_c PUBLIC ${NEOJSON_ROOT}/vendor/yyjson)
set_target_properties(bench_yyjson_c PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)

function(bench_executable name lib_define)
    add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/bench.cpp)
    target_compile_definitions(${name} PRIVATE ${lib_define} BENCH_LIB_NAME="${name}")
endfunction()

if(BENCH_CPPYYJSON_BUNDLED)
    FetchContent_Declare(cpp_yyjson
        GIT_REPOSITORY https://github.com/yosh-matsuda/cpp-yyjson.git
        GIT_TAG 742cc49559b5f83d1adc43b3c9aaf582e705b7f7  # v2.1.0
        GIT_SUBMODULES "")
    set(CPPYYJSON_USE_BUNDLED_YYJSON ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cpp_yyjson)
    bench_executable(bench_cppyyjson_bundled BENCH_LIB_CPPYYJSON)
    target_link_libraries(bench_cppyyjson_bundled PRIVATE cpp_yyjson::cpp_yyjson)
    return()
endif()

set(NEOJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${NEOJSON_ROOT} neojson)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
set(JSON_BuildTests OFF CACHE INTERNAL "")

# cpp-yyjson links an existing target named `yyjson` when its bundled backend is off.
add_library(yyjson ALIAS bench_yyjson_c)
FetchContent_Declare(cpp_yyjson
    GIT_REPOSITORY https://github.com/yosh-matsuda/cpp-yyjson.git
    GIT_TAG 742cc49559b5f83d1adc43b3c9aaf582e705b7f7  # v2.1.0
    GIT_SUBMODULES "")
set(CPPYYJSON_USE_BUNDLED_YYJSON OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json cpp_yyjson)

bench_executable(bench_neojson BENCH_LIB_NEOJSON)
target_link_libraries(bench_neojson PRIVATE neojson::neojson)

bench_executable(bench_yyjson BENCH_LIB_YYJSON)
target_link_libraries(bench_yyjson PRIVATE bench_yyjson_c)

bench_executable(bench_cppyyjson_same_yyjson BENCH_LIB_CPPYYJSON)
target_link_libraries(bench_cppyyjson_same_yyjson PRIVATE cpp_yyjson::cpp_yyjson)

bench_executable(bench_nlohmann BENCH_LIB_NLOHMANN)
target_link_libraries(bench_nlohmann PRIVATE nlohmann_json::nlohmann_json)
