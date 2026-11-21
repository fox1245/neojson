# Changelog

All notable compatibility and behavioral changes in `neojson` are documented in this file.

## [Unreleased]

### Added
- Lazy `yyjson`-backed Copy-on-Write (COW) DOM for `json::parse` with `sizeof(neojson::json) == 16`, lazy equality/comparison/dumping, iterative deep destruction across mixed lazy and materialized subtrees, and explicit `json::materialize()` to detach a subtree from the shared `yyjson` document.
- Runtime nesting-depth enforcement (`json::parse_options::max_depth`, default `1100`) integrated directly into the vendored `yyjson` parser.

### Changed / Compatibility Notes
- **Lazy/COW shared `const` access**: References, pointers, and iterators obtained through `const` access to shared COW storage become invalid if any handle sharing that storage detaches upon mutation while shared (including single-statement self-assignments such as `j["a"] = std::as_const(j)["b"];` while shared; copy `json tmp = std::as_const(j)["b"];` first or call `j.materialize()` beforehand). Use `std::as_const` for read-only traversal on shared values, and `materialize()` to release the underlying `yyjson` document early.
- **`noexcept` / `constexpr` signatures**: `get_ptr` on `const object_t*` / `const array_t*` is no longer `constexpr`. `noexcept` operations that inspect or compare unmaterialized lazy containers (`size()`, `clear()`, `get_ptr()`, `==`, `<`) may allocate during on-demand materialization and invoke `std::terminate` on out-of-memory (`clear()` reuses sole-owner materialized storage in place without allocating).

## Earlier Changes (Mainline Pre-Lazy History)

### Changed / Source Compatibility
- **Container allocator (`array_t`, `object_t`)**: `json::array_t` changed from `std::vector<json>` (`std::allocator<json>`) to `std::vector<json, detail::pool_allocator<json>>`, and `json::object_t` uses `detail::pool_allocator` for its internal storage. Value conversions (`json(vec)`, `j.get<std::vector<json>>()`) still compile and work, but code ported from `nlohmann::ordered_json` that assumes `std::is_same_v<json::array_t, std::vector<json>>` (`get_ptr<const std::vector<json>*>()`, `get_ref<std::vector<json>&>()`, `swap(std::vector<json>&)`, or direct reference binding to `std::vector<json>`) must be updated to use `json::array_t` / `json::object_t`.
