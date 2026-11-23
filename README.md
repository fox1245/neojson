<div align="center">

<img src="assets/logo.svg" width="160">

# neojson

**An nlohmann-compatible C++ JSON API on top of yyjson.**

[![CI](https://github.com/fox1245/neojson/actions/workflows/ci.yml/badge.svg)](https://github.com/fox1245/neojson/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](CMakeLists.txt)

</div>

---

An implementation of the `nlohmann::ordered_json` API on top of yyjson's parser. The library has one value type,
`neojson::json`: an owned node tree with nlohmann's semantics (objects keep the order of insertion, integers are
signed or unsigned, and so on) that `json::parse` fills from yyjson's parse of the text. Differential tests against
nlohmann/json are the specification. Work in progress.

License: MIT. yyjson (vendor/yyjson) is MIT licensed.

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
