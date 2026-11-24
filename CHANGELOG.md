# Changelog

All notable compatibility and behavioral changes in `neojson` are documented in this file.

## [Unreleased]

## [0.1.0] - 2026-11-24

### Added
- CMake packaging (`NEOJSON_INSTALL`, `find_package(neojson CONFIG)`, `neojsonConfig.cmake` / `neojsonConfigVersion.cmake` / `neojsonTargets.cmake`), `pkg-config` (`neojson.pc`), public `NEOJSON_VERSION_{MAJOR,MINOR,PATCH,STRING}` macros, library target honoring `BUILD_SHARED_LIBS` on ELF/Mach-O (with `VERSION`/`SOVERSION` and `POSITION_INDEPENDENT_CODE ON`) while staying `STATIC` on Windows, `cxx_std_17` interface compile feature, and subproject-safe defaults disabling tests/examples/install under `FetchContent` or `add_subdirectory`.
- Lazy `yyjson`-backed Copy-on-Write (COW) DOM for `json::parse` with `sizeof(neojson::json) == 16`, atomic-refcounted COW `SharedString` values (with unshareable pinning on mutable `string_t*` / `string_t&` access), lazy equality/comparison/dumping, direct `yyjson` serialization for unmaterialized unique-key lazy subtrees, iterative deep destruction across mixed lazy and materialized subtrees, and explicit `json::materialize()` to detach containers and shared strings from shared storage.
- Runtime nesting-depth enforcement (`json::parse_options::max_depth`, default `1100`) integrated directly into the vendored `yyjson` parser.
- Native x86-64 AVX2 (`Avx2ParserOps`) and ARM64 NEON (`NeonParserOps`) SIMD + SWAR JSON parsers with Eisel-Lemire float conversion and Keiser-Lemire UTF-8 validation (with automatic `yyjson` fallback), `DocArena` immutable const DOM views with `ArenaShapeCache` / `KeyInternTable` / short-and-long string interning, compact/inline container layouts (`SmallArray2`, `SmallObject4`, `CompactObject`, zero-allocation empty containers, lazy `ordered_map` `HashIndex`), fused single-pass direct `dump()` UTF-8 validation/escape writer (`AVX2`/`SSE2`/`NEON`/scalar), and thread-local `ThreadSlabCache` / `ThreadLargeCache` / `ThreadDocCredit` pooling.
- Runtime thread-local cache budget control and explicit cache trimming (`neojson::thread_cache_limits`, `neojson::get_thread_cache_limits()`, `neojson::set_thread_cache_limits()`, and `neojson::trim_thread_caches()`) to bound steady-state RSS across long-lived worker pools.
- Runnable C++17 examples under `examples/` gated by `NEOJSON_BUILD_EXAMPLES` (default `OFF`, built and smoke-tested in CI).
- Reproducible cross-library benchmark suite under `bench/` (`yyjson`, `simdjson`, `RapidJSON`, `cpp-yyjson`, `nlohmann::json`, `nlohmann::ordered_json`), SVG charts in `assets/`, and x86-64 AVX2 / ARM64 NEON benchmark report in `docs/benchmarks.md` + `README.md`.

### Changed / Compatibility Notes
- **Floating-point formatting**: Compact and pretty `dump()` now use vendored `yyjson` Schubfach shortest IEEE-754 round-trip conversion with `nlohmann`-compatible fixed/scientific bounds (`-4 < dot_ofs <= 15`) and signed two-digit padded exponents (`e+XX` / `e-XX`). Every dumped finite double round-trips bit-for-bit identically (`std::memcpy` equality) while occasionally emitting fewer or different shortest decimal digits than Grisu2.
- **Lazy/COW shared `const` access and `SharedString` pinning**: References, pointers, and iterators obtained through `const` access to shared COW storage become invalid if any handle sharing that storage detaches upon mutation while shared (including single-statement self-assignments such as `j["a"] = std::as_const(j)["b"];` while shared; copy `json tmp = std::as_const(j)["b"];` first or call `j.materialize()` beforehand). Mutable `get_ptr<string_t*>()` and `get_ref<string_t&>()` detach if shared and pin the `SharedString` unshareable (`refcount == 0`). Use `std::as_const` for read-only traversal on shared values, and `materialize()` to release the underlying `yyjson` document and detach strings early.
- **`noexcept` / `constexpr` signatures**: `get_ptr` on `const object_t*` / `const array_t*` is no longer `constexpr`. `noexcept` operations that inspect, traverse, or detach shared/lazy storage (`size()`, `clear()`, `get_ptr()`, `==`, `<`) may allocate during on-demand materialization or COW detach (including `get_ptr<string_t*>()` and `clear()` on a shared string) and invoke `std::terminate` on out-of-memory (`clear()` reuses sole-owner materialized storage in place without allocating).

## Earlier Changes (Mainline Pre-Lazy History)

### Changed / Source Compatibility
- **Container allocator (`array_t`, `object_t`)**: `json::array_t` changed from `std::vector<json>` (`std::allocator<json>`) to `std::vector<json, detail::pool_allocator<json>>`, and `json::object_t` uses `detail::pool_allocator` for its internal storage. Value conversions (`json(vec)`, `j.get<std::vector<json>>()`) still compile and work, but code ported from `nlohmann::ordered_json` that assumes `std::is_same_v<json::array_t, std::vector<json>>` (`get_ptr<const std::vector<json>*>()`, `get_ref<std::vector<json>&>()`, `swap(std::vector<json>&)`, or direct reference binding to `std::vector<json>`) must be updated to use `json::array_t` / `json::object_t`.
