# jsonkit (working name)

An implementation of the `nlohmann::ordered_json` API on top of yyjson's parser: `json`, an owned node tree with
nlohmann's semantics, and `document`, an immutable yyjson document for parse, read and forward. Differential tests
against nlohmann/json are the specification. Work in progress.

License: MIT. yyjson (vendor/yyjson) is MIT licensed.
