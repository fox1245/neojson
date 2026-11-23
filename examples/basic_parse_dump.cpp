// Basic parsing, insertion-order preservation, serialization, depth limits, and non-throwing parsing.

#include "neojson/json.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using json = neojson::json;

void require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(msg);
    }
}

void run() {
    const std::string input = R"({"z": 1, "a": {"nested": [10, 20]}, "m": true})";
    require(json::accept(input), "expected valid JSON input to be accepted");

    json doc = json::parse(input);
    const json& ro = std::as_const(doc);

    // neojson::json mirrors nlohmann::ordered_json semantics: object keys keep insertion order.
    std::vector<std::string> keys;
    for (auto it = ro.begin(); it != ro.end(); ++it) {
        keys.push_back(it.key());
    }
    require(keys == std::vector<std::string>({"z", "a", "m"}), "insertion order preserved");
    require(ro.at("z").get<int>() == 1, "z value matches");
    require(ro.at("a").at("nested").at(1).get<int>() == 20, "nested array value matches");
    require(ro.value("missing", 99) == 99, "value() default fallback works");

    // Compact and pretty dump round-trip identically.
    const std::string compact = ro.dump();
    const std::string pretty = ro.dump(2);
    require(compact == R"({"z":1,"a":{"nested":[10,20]},"m":true})", "compact dump order");
    require(json::parse(pretty) == ro, "pretty dump round-trips");

    // Override max_depth per parse/accept call for untrusted or thread-constrained inputs.
    const std::string deep = "[[[[[1]]]]]";
    require(!json::accept(deep, false, json::parse_options{4}), "accept rejects depth > 4");
    bool caught_depth_error = false;
    try {
        (void)json::parse(deep, nullptr, true, false, json::parse_options{4});
    } catch (const json::parse_error& err) {
        caught_depth_error = (err.id == 120);
    }
    require(caught_depth_error, "expected parse_error id 120 when max_depth is exceeded");
    require(json::parse(deep, false, false, json::parse_options{4}).is_discarded(),
            "non-throwing parse returns discarded on max_depth overflow");

    // Non-throwing syntax validation via allow_exceptions = false.
    const json invalid = json::parse("{broken json", false);
    require(invalid.is_discarded(), "invalid JSON yields discarded value");

    std::cout << "basic_parse_dump: ok\n";
}

}  // namespace

int main() {
    try {
        run();
    } catch (const std::exception& err) {
        std::cerr << "basic_parse_dump failed: " << err.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
