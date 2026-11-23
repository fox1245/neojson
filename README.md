<div align="center">

<img src="assets/logo.svg" width="160">

# neojson

**An nlohmann-compatible C++ JSON API combining a native AVX2/SWAR parser, vendored yyjson, and a lazy COW DOM.**

[![CI](https://github.com/fox1245/neojson/actions/workflows/ci.yml/badge.svg)](https://github.com/fox1245/neojson/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](CMakeLists.txt)

</div>

---

An implementation of the `nlohmann::ordered_json` API combining a native x86-64 AVX2/SWAR parser (with vendored `yyjson` fallback on non-AVX2 hosts and small/unsupported inputs) and a lazy Copy-on-Write DOM. The library exposes one value type, `neojson::json`: an owned node tree with nlohmann's semantics (objects keep the order of insertion, integers are signed or unsigned, and so on) that materializes lazily from the flat parsed document. Differential tests against nlohmann/json are the specification. Work in progress.

License: MIT. yyjson (vendor/yyjson) is MIT licensed.

## Benchmarks

Single-thread in-memory throughput across the standard 4-file JSON benchmark corpus (`canada.json`, `citm_catalog.json`, `gsoc-2018.json`, `twitter.json`; `7.94 MB` total, median of 7 alternating rounds × 7 batches, GCC 13.3.0 `-O3 -DNDEBUG`, no `-march=native`, C++20 benchmark harnesses with `libneojson` built as C++17 per root `CMakeLists.txt`).

On x86-64 AVX2 (`Intel Core i7-8750H`), `neojson` parses **26.2× faster** than `nlohmann::ordered_json` and **1.53× faster** than the `yyjson` C API while serializing **1.46× faster**. On ARM64 NEON (`ARM Neoverse-N1`), where `neojson` currently uses its portable scalar `yyjson` + COW path rather than native ARM SIMD intrinsics, parse throughput stays within **2.5%** of `yyjson` (`0.97×`) while serialization remains **1.27× faster**.

![neojson throughput on x86-64 AVX2](assets/bench_overview_x86_64.svg)

| Architecture / CPU | Workload | `neojson` | `yyjson (C)` | `simdjson` (DOM) | `cpp-yyjson` | `RapidJSON` | `nlohmann::json` | `nlohmann::ordered` |
|---|---|---|---|---|---|---|---|---|
| **x86-64 AVX2**<br>Intel Core i7-8750H | Parse (4 files) | **2,180 MB/s** (`3.64 ms`) | 1,425 MB/s | 1,754 MB/s | 1,391 MB/s | 598 MB/s | 97 MB/s | 83 MB/s |
| | Parse + Full Read | 1,336 MB/s (`5.94 ms`) | 1,224 MB/s | **1,379 MB/s** | 1,202 MB/s | 564 MB/s | 94 MB/s | 82 MB/s |
| | Write (Minified) | **2,553 MB/s** (`3.11 ms`) | 1,745 MB/s | 1,600 MB/s | 1,807 MB/s | 716 MB/s | 311 MB/s | 280 MB/s |
| | Mutable Build | 2,530 MB/s (`3.14 ms`) | 4,129 MB/s | — *(read-only)* | 4,034 MB/s | **5,177 MB/s** | 353 MB/s | 224 MB/s |
| | Field Lookup (`twitter.json`) | **168.6 K/s** (`5.93 µs`) | 99.0 K/s | 74.5 K/s | 89.6 K/s | 105.2 K/s | 36.4 K/s | 119.2 K/s |
| **ARM64 NEON**<br>ARM Neoverse-N1 | Parse (4 files) | 1,115 MB/s (`7.12 ms`) | 1,145 MB/s | **1,174 MB/s** | 1,145 MB/s | 443 MB/s | 62 MB/s | 56 MB/s |
| | Parse + Full Read | 779 MB/s (`10.2 ms`) | 973 MB/s | 968 MB/s | **1,005 MB/s** | 419 MB/s | 60 MB/s | 55 MB/s |
| | Write (Minified) | **1,702 MB/s** (`4.66 ms`) | 1,337 MB/s | 1,083 MB/s | 1,340 MB/s | 557 MB/s | 258 MB/s | 267 MB/s |
| | Mutable Build | 1,670 MB/s (`4.75 ms`) | 3,421 MB/s | — *(read-only)* | 3,096 MB/s | **4,137 MB/s** | 240 MB/s | 168 MB/s |
| | Field Lookup (`twitter.json`) | **124.8 K/s** (`8.02 µs`) | 98.2 K/s | 63.5 K/s | 84.2 K/s | 96.4 K/s | 49.1 K/s | 106.6 K/s |

### Why is `neojson` faster than `yyjson` (C)?

1. **Native AVX2/SWAR parser + pooled document buffers (`1.53×` parse on x86-64)**: For standard inputs (`>= 32 B`, no comments/BOM) on x86-64 AVX2 hosts, `json::parse()` bypasses `yyjson_read()` and runs its runtime-dispatched AVX2/SWAR parser (32-byte whitespace/string/Keiser-Lemire UTF-8 scanning, SWAR integer parsing, Eisel-Lemire floats) directly into a `yyjson_doc`-compatible layout backed by bounded thread-local pooled buffers (`ThreadLargeCache`, configurable via `set_thread_cache_limits()` / `trim_thread_caches()`), avoiding per-parse `malloc`/`free`. On ARM64 (`NEOJSON_X86_SIMD == 0`), it falls back to vendored `yyjson` + pooled allocator, which is why ARM64 parse tracks `yyjson` within `2.5%`.
2. **Lazy COW DOM (no eager `std::map`/`std::vector`/`std::string` tree build)**: `json::parse()` wraps the parsed flat document in a 16-byte lazy handle without allocating per-node containers up front, materializing compact read-only `DocArena` views only on first traversal and owned mutable containers only on mutation — so even full-traversal `Parse + Read` stays ahead of C `yyjson` on x86-64 (`1,336` vs `1,224 MB/s`).
3. **Flat direct serializer into `std::string` (`1.46×` x86-64, `1.27×` ARM64)**: Unmaterialized lazy documents serialize directly into the caller's `std::string` using an 8 KiB stack staging buffer, pre-reserved input-size hint, `YYJSON_SUBTYPE_NOESC` fast paths, numeric-pair coordinate runs, and AVX2 escape scanning on x86-64 — skipping `yyjson_write()`'s separate `malloc` + copy + `free` cycle and caching object key-uniqueness checks after the first dump/view.
4. **Contiguous keys + lazy open-addressed hash index (`1.70×` lookup x86-64, `1.27×` ARM64)**: Immutable `yyjson` objects are flat preorder arrays without a hash index (`O(N)` linear scan via `yyjson_obj_getn`), whereas `neojson`'s `ArenaObject` stores keys contiguously and lazily builds an open-addressed hash table (`ArenaHashIndex`) above 16 keys (`kOrderedMapHashThreshold`), giving cache-local linear scans on small objects and `O(1)` lookup on larger ones.

See [docs/benchmarks.md](docs/benchmarks.md) for the ARM64 NEON chart, full per-file breakdown chart and tables, methodology notes (`simdjson` pre-padded DOM, `RapidJSON` baseline SIMD flags), and reproduction commands under [`bench/`](bench/).

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
