<div align="center">

<img src="assets/logo.svg" width="160">

# neojson

**An nlohmann-compatible C++ JSON API on top of yyjson.**

[![CI](https://github.com/fox1245/neojson/actions/workflows/ci.yml/badge.svg)](https://github.com/fox1245/neojson/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](CMakeLists.txt)

</div>

---

An implementation of the `nlohmann::ordered_json` API on top of yyjson's parser: `json`, an owned node tree with
nlohmann's semantics, and `document`, an immutable yyjson document for parse, read and forward. Differential tests
against nlohmann/json are the specification. Work in progress.

License: MIT. yyjson (vendor/yyjson) is MIT licensed.

## Limits

`json::parse` and `json::accept` accept arrays and objects nested up to 1100 levels deep (`json::parse_options::default_max_depth`). A document nested deeper is a `parse_error` (id 120, "nesting limit exceeded"), a discarded value when exceptions are off, and `false` from `accept`. Pass a `json::parse_options` as the last argument to change the limit for one call: `json::parse(text, nullptr, true, false, json::parse_options{256})` or `json::accept(text, false, json::parse_options{256})`. Parsing and destroying a tree need no deep call stack, however deep the tree is, including a tree that a program built without parsing. Copying, dumping and comparing a tree with `==` or `<` recurse once per level, as they do in nlohmann/json, so they need stack in proportion to the depth: a thread with a small stack should use a smaller limit, or avoid those calls on a very deep tree.
