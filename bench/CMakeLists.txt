# Reproducible benchmark comparing neojson against yyjson (C), simdjson (DOM), RapidJSON,
# cpp-yyjson, nlohmann::json, and nlohmann::ordered_json. Not built as part of the main library.
#
#   cmake -S bench -B build-bench -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-bench --parallel
#   python3 bench/run.py --build build-bench --rounds 7 --json results.json
#
# One executable per library (so no executable links two copies of yyjson):
#   bench_neojson          neojson::ordered_json (vendored yyjson v0.13.0)
#   bench_yyjson           yyjson C API (vendored yyjson v0.13.0)
#   bench_simdjson         simdjson::dom (v5.3.0)
#   bench_rapidjson        Tencent/rapidjson (SSE2 on x86-64, NEON on ARM64)
#   bench_cppyyjson        cpp-yyjson (v2.1.0, on vendored yyjson v0.13.0)
#   bench_nlohmann_json    nlohmann::json (v3.11.3)
#   bench_nlohmann_ordered nlohmann::ordered_json (v3.11.3)
#
# Benchmark harnesses compile as C++20; neojson compiles per root CMakeLists.txt (C++17), all with identical Release flags (-O3 -DNDEBUG, no LTO, no -march=native).

cmake_minimum_required(VERSION 3.20)
project(neojson_bench LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(NEOJSON_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
include(FetchContent)

# Corpus files verified by SHA-256 against bench/corpus.json.
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

set(NEOJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NEOJSON_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${NEOJSON_ROOT} neojson)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")

# cpp-yyjson links an existing target named `yyjson` when its bundled backend is off.
add_library(yyjson ALIAS bench_yyjson_c)
FetchContent_Declare(cpp_yyjson
    GIT_REPOSITORY https://github.com/yosh-matsuda/cpp-yyjson.git
    GIT_TAG 742cc49559b5f83d1adc43b3c9aaf582e705b7f7  # v2.1.0
    GIT_SUBMODULES "")
set(CPPYYJSON_USE_BUNDLED_YYJSON OFF CACHE BOOL "" FORCE)

FetchContent_Declare(simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG b2ef1a5eb78296ddbbb9eb6a759ca38efa2d4000  # v5.3.0
    GIT_SHALLOW TRUE)
set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
set(SIMDJSON_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(nlohmann_json cpp_yyjson simdjson)

# Header-only RapidJSON fetch without invoking RapidJSON's own CMakeLists.txt (which sets -march=native).
set(RAPIDJSON_REV "297637bc0a0f00dc504dbc1a47ebdaa577f8ae19")
set(RAPIDJSON_ARCHIVE "${CMAKE_BINARY_DIR}/_deps/rapidjson.tar.gz")
set(RAPIDJSON_EXTRACT_DIR "${CMAKE_BINARY_DIR}/_deps/rapidjson-src")
set(RAPIDJSON_INC_DIR "${RAPIDJSON_EXTRACT_DIR}/rapidjson-${RAPIDJSON_REV}/include")
file(DOWNLOAD
    "https://github.com/Tencent/rapidjson/archive/${RAPIDJSON_REV}.tar.gz"
    "${RAPIDJSON_ARCHIVE}"
    EXPECTED_HASH SHA256=104f57d1512da80740a3eca7d7b2acee7f749d79dbf15328ad1869c5c962011b
    TLS_VERIFY ON)
if(NOT EXISTS "${RAPIDJSON_INC_DIR}/rapidjson/document.h")
    file(ARCHIVE_EXTRACT INPUT "${RAPIDJSON_ARCHIVE}" DESTINATION "${RAPIDJSON_EXTRACT_DIR}")
endif()
add_library(bench_rapidjson_hdr INTERFACE)
target_include_directories(bench_rapidjson_hdr INTERFACE "${RAPIDJSON_INC_DIR}")

function(bench_executable name lib_define)
    add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/bench.cpp)
    target_compile_definitions(${name} PRIVATE ${lib_define} BENCH_LIB_NAME="${name}")
endfunction()

bench_executable(bench_neojson BENCH_LIB_NEOJSON)
target_link_libraries(bench_neojson PRIVATE neojson::neojson)

bench_executable(bench_yyjson BENCH_LIB_YYJSON)
target_link_libraries(bench_yyjson PRIVATE bench_yyjson_c)

bench_executable(bench_simdjson BENCH_LIB_SIMDJSON)
target_link_libraries(bench_simdjson PRIVATE simdjson::simdjson)

bench_executable(bench_rapidjson BENCH_LIB_RAPIDJSON)
target_link_libraries(bench_rapidjson PRIVATE bench_rapidjson_hdr)

bench_executable(bench_cppyyjson BENCH_LIB_CPPYYJSON)
target_link_libraries(bench_cppyyjson PRIVATE cpp_yyjson::cpp_yyjson)

bench_executable(bench_nlohmann_json BENCH_LIB_NLOHMANN_JSON)
target_link_libraries(bench_nlohmann_json PRIVATE nlohmann_json::nlohmann_json)

bench_executable(bench_nlohmann_ordered BENCH_LIB_NLOHMANN_ORDERED)
target_link_libraries(bench_nlohmann_ordered PRIVATE nlohmann_json::nlohmann_json)
