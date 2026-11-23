# neojson Examples

Self-verifying C++17 examples covering core parsing, compatibility caveats, and steady-state memory control.

| Example | What it demonstrates |
| :--- | :--- |
| [basic_parse_dump.cpp](basic_parse_dump.cpp) | `json::parse`, insertion-order preservation, `std::as_const` reads, `dump()`, `json::parse_options` depth limits, non-throwing `allow_exceptions = false` / `is_discarded()` |
| [custom_types_and_patch.cpp](custom_types_and_patch.cpp) | `NEOJSON_DEFINE_TYPE_NON_INTRUSIVE`, `NEOJSON_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT`, `adl_serializer`, `json_pointer`, RFC 6902 `diff`/`patch`, RFC 7396 `merge_patch` |
| [lazy_cow_materialize.cpp](lazy_cow_materialize.cpp) | Lazy/COW parsing, safe copy-then-assign mutation, `materialize()` to unpin large backing `yyjson` documents, `SharedString` pinning via mutable `get_ptr`, `json::array_t` |
| [worker_pool_cache_control.cpp](worker_pool_cache_control.cpp) | Bounding worker-pool memory with `thread_cache_limits` and RAII `trim_thread_caches()` while retaining materialized summaries |
| [streaming_callback.cpp](streaming_callback.cpp) | Filtering and tagging JSON nodes during parse using `json::parser_callback_t` and `json::parse_event_t` |

## Build & Run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNEOJSON_BUILD_TESTS=OFF -DNEOJSON_BUILD_EXAMPLES=ON
cmake --build build --parallel
ctest --test-dir build -R "^example_" --output-on-failure
```
