<div align="center">

<img src="assets/logo.svg" width="160">

# neojson

**An nlohmann-compatible C++ JSON API combining native AVX2/NEON/SWAR SIMD parsers, vendored yyjson, and a lazy COW DOM.**

[![CI](https://github.com/fox1245/neojson/actions/workflows/ci.yml/badge.svg)](https://github.com/fox1245/neojson/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/fox1245/neojson)](https://github.com/fox1245/neojson/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](CMakeLists.txt)

</div>

---

An implementation of the `nlohmann::ordered_json` API combining native x86-64 AVX2 and ARM64 NEON SIMD parsers (with vendored `yyjson` fallback on unsupported targets and small/comment/BOM inputs) and a lazy Copy-on-Write DOM. The library exposes one value type, `neojson::json`: an owned node tree with nlohmann's semantics (objects keep the order of insertion, integers are signed or unsigned, and so on) that materializes lazily from the flat parsed document. Differential tests against nlohmann/json serve as the behavioral specification.

License: MIT. yyjson (vendor/yyjson) is MIT licensed.

## Benchmarks

Single-thread in-memory throughput across the standard 4-file JSON benchmark corpus (`canada.json`, `citm_catalog.json`, `gsoc-2018.json`, `twitter.json`; `7.94 MB` total, median of 7 alternating rounds × 7 batches, GCC 13.3.0 `-O3 -DNDEBUG`, no `-march=native`, C++20 benchmark harnesses with `libneojson` built as C++17 per root `CMakeLists.txt`).

On x86-64 AVX2 (`Intel Core i7-8750H`), `neojson` parses **31.1× faster** than `nlohmann::ordered_json`, **1.85× faster** than the `yyjson` C API, and **1.45× faster** than `simdjson` DOM, while also leading Parse + Full Read (**1.29×** vs `yyjson`, **1.10×** vs `simdjson`), Write (**1.54×** vs `yyjson`), and Field Lookup (**1.71×** vs `yyjson`). On ARM64 NEON (`ARM Neoverse-N1`), with its native NEON SIMD parser backend, `neojson` parses **34.5× faster** than `nlohmann::ordered_json`, **1.65× faster** than `yyjson` C, and **1.61× faster** than `simdjson` DOM (`1,885` vs `1,170 MB/s`), while leading Parse + Full Read (**1,155 MB/s**, **1.19×** faster than both `yyjson` C and `simdjson` DOM), Write (**1.44×** vs `yyjson`), and Field Lookup (**1.26×** vs `yyjson`).

![neojson throughput on x86-64 AVX2](assets/bench_overview_x86_64.svg)

| Architecture / CPU | Workload | `neojson` | `yyjson (C)` | `simdjson` (DOM) | `cpp-yyjson` | `RapidJSON` | `nlohmann::json` | `nlohmann::ordered` |
|---|---|---|---|---|---|---|---|---|
| **x86-64 AVX2**<br>Intel Core i7-8750H | Parse (4 files) | **2,575 MB/s** (`3.08 ms`) | 1,392 MB/s | 1,779 MB/s | 1,382 MB/s | 599 MB/s | 96 MB/s | 83 MB/s |
| | Parse + Full Read | **1,539 MB/s** (`5.16 ms`) | 1,191 MB/s | 1,405 MB/s | 1,196 MB/s | 564 MB/s | 94 MB/s | 81 MB/s |
| | Write (Minified) | **2,783 MB/s** (`2.85 ms`) | 1,809 MB/s | 1,604 MB/s | 1,755 MB/s | 716 MB/s | 277 MB/s | 321 MB/s |
| | Mutable Build | 2,569 MB/s (`3.09 ms`) | 3,962 MB/s | — *(read-only)* | 4,171 MB/s | **5,197 MB/s** | 359 MB/s | 233 MB/s |
| | Field Lookup (`twitter.json`) | **165.7 K/s** (`6.03 µs`) | 97.1 K/s | 76.5 K/s | 88.8 K/s | 104.7 K/s | 37.0 K/s | 124.8 K/s |
| **ARM64 NEON**<br>ARM Neoverse-N1 | Parse (4 files) | **1,885 MB/s** (`4.21 ms`) | 1,144 MB/s | 1,170 MB/s | 1,143 MB/s | 444 MB/s | 62 MB/s | 55 MB/s |
| | Parse + Full Read | **1,155 MB/s** (`6.87 ms`) | 974 MB/s | 967 MB/s | 1,004 MB/s | 420 MB/s | 60 MB/s | 53 MB/s |
| | Write (Minified) | **1,918 MB/s** (`4.14 ms`) | 1,330 MB/s | 1,075 MB/s | 1,332 MB/s | 550 MB/s | 257 MB/s | 263 MB/s |
| | Mutable Build | 1,691 MB/s (`4.70 ms`) | 3,422 MB/s | — *(read-only)* | 3,158 MB/s | **4,151 MB/s** | 235 MB/s | 167 MB/s |
| | Field Lookup (`twitter.json`) | **123.9 K/s** (`8.07 µs`) | 98.2 K/s | 63.5 K/s | 80.5 K/s | 96.4 K/s | 49.0 K/s | 106.7 K/s |

### Why is `neojson` faster than `yyjson` (C)?

1. **Native AVX2/NEON/SWAR parsers + pooled buffers (`1.85×` parse on x86-64, `1.65×` on ARM64)**: For standard inputs (`>= 32 B`, no comments/BOM), `json::parse()` bypasses `yyjson_read()` and dispatches an arch-neutral native SIMD parser core (`Avx2ParserOps` on x86-64 AVX2 and `NeonParserOps` on ARM64 NEON), featuring SIMD whitespace/string/Keiser-Lemire UTF-8 scanning, outlined zero-waste 128 B AVX2 / 64 B NEON long-string unrolling (`parse_string_val_long`), value-returned wide-indent whitespace fast paths (`skip_spaces_for_key` / `KeySpaceRes` avoiding stack-canary spills), SWAR 8-digit integer & fractional float parsing, Eisel-Lemire multipliers, and tight coordinate-pair runs into a `yyjson_doc`-compatible layout backed by bounded thread-local caches (`ThreadLargeCache`, configurable via `set_thread_cache_limits()` / `trim_thread_caches()`), avoiding per-parse `malloc`/`free`.
2. **Lazy Copy-on-Write DOM & direct `DocArena` traversal (`1.29×` read x86-64, `1.19×` ARM64)**: `json::parse()` wraps the flat parsed document in a 16-byte lazy handle instead of allocating per-node containers up front. Read-only traversal materializes compact bump-allocated `DocArena` views with shape caching (`ArenaShapeCache`), doc-stamped `ShortStr7Cache` + MRU `LongStrInternCache` interning, batched NEON flat-object shape verification, AVX2/overlapping block string copies (`copy_str_ge16`), and bounded scratch reuse — outperforming both `yyjson (C)` and `simdjson (DOM)` on aggregate `Parse + Full Read` on both architectures.
3. **Fused single-pass serializer into `std::string` (`1.54×` write x86-64, `1.44×` ARM64)**: Unmaterialized lazy documents serialize straight into a pre-reserved `std::string` via an 8 KiB staging buffer, `YYJSON_SUBTYPE_NOESC` fast paths, numeric-pair coordinate runs, and fused single-pass UTF-8 validation/escape batching (`write_valid_utf8_escaped_flat_{avx2,sse2,neon,scalar}` with NEON movemask/verbatim scanning on ARM64) — avoiding `yyjson_write()`'s `malloc` + copy + `free` cycle and caching object key uniqueness after first inspection.
4. **Contiguous keys + open-addressed indexing & compact DOM builders (`1.71×` lookup x86-64, `1.26×` ARM64)**: Unlike `yyjson`'s flat preorder object scan (`O(N)`), `ArenaObject` stores keys contiguously and builds an open-addressed `ArenaHashIndex` lazily above 16 keys (`kOrderedMapHashThreshold`), while mutable `json` containers use compact `SmallArray2`/`SmallObject4` slabs with NEON/SSE2 tag scanning and single-probe `ordered_map` indexing (`11.0×` / `10.1×` faster build than `nlohmann::ordered_json` on x86-64 / ARM64).

See [docs/benchmarks.md](docs/benchmarks.md) for the ARM64 NEON chart, full per-file breakdown chart and tables, methodology notes (`simdjson` pre-padded DOM, `RapidJSON` baseline SIMD flags), and reproduction commands under [`bench/`](bench/).

## Installation

### CMake `FetchContent` (Recommended)

When consumed as a CMake subproject via `FetchContent` or `add_subdirectory`, `NEOJSON_BUILD_TESTS`, `NEOJSON_BUILD_EXAMPLES`, and `NEOJSON_INSTALL` default to `OFF` automatically:

```cmake
include(FetchContent)
FetchContent_Declare(
    neojson
    GIT_REPOSITORY https://github.com/fox1245/neojson.git
    GIT_TAG        v0.1.0
)
FetchContent_MakeAvailable(neojson)

target_link_libraries(my_app PRIVATE neojson::neojson)
```

### System / Prefix Install (`find_package`)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNEOJSON_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --parallel
cmake --install build
```

Then in your downstream `CMakeLists.txt`:

```cmake
find_package(neojson 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE neojson::neojson)
```

### `pkg-config`

```bash
g++ -std=c++17 main.cpp $(pkg-config --cflags --libs neojson) -o my_app
```

### Quick Start

```cpp
#include <neojson/json.hpp>
#include <iostream>
#include <utility>

int main() {
    neojson::json doc = neojson::json::parse(R"({"z":1,"a":[10,20]})");
    const auto& ro = std::as_const(doc);
    std::cout << ro.at("a").at(1).get<int>() << '\n';  // 20
    std::cout << ro.dump() << '\n';                    // {"z":1,"a":[10,20]}
}
```

## Examples

Runnable C++17 examples are listed in [examples/README.md](examples/README.md) (`NEOJSON_BUILD_EXAMPLES` defaults to `OFF` so downstream `FetchContent` consumers do not build examples unless requested):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNEOJSON_BUILD_EXAMPLES=ON
cmake --build build --parallel
ctest --test-dir build -R "^example_" --output-on-failure
```

## Limits

`json::parse` and `json::accept` accept arrays and objects nested up to 1100 levels deep (`json::parse_options::default_max_depth`). A document nested deeper is a `parse_error` (id 120, "nesting limit exceeded"), a discarded value when exceptions are off, and `false` from `accept`. Pass a `json::parse_options` as the last argument to change the limit for one call: `json::parse(text, nullptr, true, false, json::parse_options{256})` or `json::accept(text, false, json::parse_options{256})`. Parsing and destroying a tree need no deep call stack, however deep the tree is, including a tree that a program built without parsing. Copying, dumping and comparing a tree with `==` or `<` recurse once per level, as they do in nlohmann/json, so they need stack in proportion to the depth: a thread with a small stack should use a smaller limit, or avoid those calls on a very deep tree.

### Thread-Local Caches

For throughput, `neojson` caches parsed large buffers (`ThreadLargeCache`, up to 16 MiB per entry and 24 MiB total per thread by default), empty 64 KiB slabs (`max_cached_empty_slabs`, default `64`), string/key buffers (`enable_string_pool`, default `true`), and per-thread materialization scratch buffers until the thread exits. Long-lived worker pools that occasionally handle large payloads can release idle thread-local caches explicitly or tighten process-wide budgets:

- `neojson::trim_thread_caches() noexcept`: flushes deferred document refcount credits, clears materialization scratch/string caches, frees all cached large buffers, and returns unused 64 KiB slabs on the calling thread while preserving live `json` values (safe no-op on untouched threads).
- `neojson::get_thread_cache_limits() noexcept` / `neojson::set_thread_cache_limits(const neojson::thread_cache_limits&) noexcept`: inspects or updates `large_cache_max_entry_bytes`, `large_cache_max_total_bytes`, `max_cached_empty_slabs`, and `enable_string_pool`, immediately trimming the calling thread to the new limits.

## Compatibility Notes

- **Container allocator types (`array_t`, `object_t`)**: For allocation performance, `neojson::json::array_t` is `std::vector<json, neojson::detail::pool_allocator<json>>` and `neojson::json::object_t` uses `neojson::detail::pool_allocator`. Value conversions (`json(std::vector<json>)`, `j.get<std::vector<json>>()`) still compile and work, but code ported from `nlohmann::ordered_json` that binds or requests exact standard-allocator container types (`get_ptr<const std::vector<json>*>()`, `get_ref<std::vector<json>&>()`, `swap(std::vector<json>&)`, `const std::vector<json>&`) must use `json::array_t` / `json::object_t`.
- **Lazy/COW parsing, `SharedString`, and `const` references**: `json::parse` returns a lazy DOM backed by a shared immutable `yyjson` document, and copies share backing containers and string payloads (`detail::SharedString`) copy-on-write:
  - Prefer `std::as_const` for read-only traversal on shared values; non-`const` `operator[]`, `at`, `find`, or `get_ptr<string_t*>`/`get_ref<string_t&>` detaches shared storage as needed.
  - Non-`const` `get_ptr<string_t*>()` and `get_ref<string_t&>()` pin the string as unshareable (`refcount == 0`) so later copies clone eagerly and cannot alias mutations through the obtained mutable handle.
  - As with other implicitly-shared containers, references, pointers, or iterators obtained through `const` access to shared storage become invalid if any holder sharing that storage mutates and detaches while shared. This includes single-statement expressions such as `j["a"] = std::as_const(j)["b"];` when `j` shares storage; copy the value first (`json tmp = std::as_const(j)["b"]; j["a"] = std::move(tmp);`) or call `j.materialize()` beforehand.
  - Any live unmaterialized lazy descendant pins the underlying `yyjson` document; call `j.materialize()` to greedily detach all containers and shared strings in a subtree into standalone owned storage.
- **Floating-point `dump()` formatting**: Finite doubles serialize via vendored `yyjson` Schubfach shortest IEEE-754 round-trip formatting using `nlohmann`-compatible fixed/scientific bounds (`-4 < dot_ofs <= 15`) and signed two-digit padded exponents (`e+XX` / `e-XX`). Output always round-trips bit-exact to the original `double` and is never longer than Grisu2, though individual boundary floats may differ cleanly in shortest decimal digits from older `nlohmann::json` versions.
- **`constexpr` / `noexcept` differences**: `get_ptr` for `const object_t*` and `const array_t*` is not `constexpr`. Queries marked `noexcept` (`size()`, `clear()`, `get_ptr()`, `==`, `<`) on shared or unmaterialized storage may allocate during on-demand materialization or COW string detach (`get_ptr<string_t*>()`, `clear()` on a shared string) and call `std::terminate` on out-of-memory.
