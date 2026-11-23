find_package(Threads REQUIRED)

function(neojson_add_example name)
    add_executable(neojson_example_${name} ${name}.cpp)
    target_link_libraries(neojson_example_${name} PRIVATE neojson::neojson)
    target_compile_options(neojson_example_${name} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>)
    add_test(NAME example_${name} COMMAND neojson_example_${name})
    set_tests_properties(example_${name} PROPERTIES TIMEOUT 30)
endfunction()

neojson_add_example(basic_parse_dump)
neojson_add_example(custom_types_and_patch)
neojson_add_example(lazy_cow_materialize)
neojson_add_example(worker_pool_cache_control)
target_link_libraries(neojson_example_worker_pool_cache_control PRIVATE Threads::Threads)
neojson_add_example(streaming_callback)
