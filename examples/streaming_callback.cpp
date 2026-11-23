// Filtering and transforming incoming JSON during parse via parser_callback_t and parse_event_t.

#include "neojson/json.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using json = neojson::json;

void require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(msg);
    }
}

void run() {
    const std::string input = R"({
        "service": "ingest",
        "events": [
            {"id": 1, "level": "trace", "debug": {"raw": [1, 2, 3]}},
            {"id": 2, "level": "warn",  "debug": {"stack": ["a", "b"]}},
            {"id": 3, "level": "info"}
        ]
    })";

    const json::parser_callback_t filter =
        [](int depth, json::parse_event_t event, json& parsed) -> bool {
        // Returning false on key drops the key and discards its nested value subtree from the DOM.
        if (event == json::parse_event_t::key && std::as_const(parsed) == "debug") {
            return false;
        }
        // Returning false on object_end discards the finished element from its enclosing array/object.
        if (event == json::parse_event_t::object_end && depth == 2) {
            const json& obj = std::as_const(parsed);
            if (obj.contains("level") && obj.at("level") == "trace") {
                return false;
            }
            if (obj.contains("level") && obj.at("level") == "warn") {
                parsed["priority"] = "high";
            }
        }
        return true;
    };

    const json filtered = json::parse(input, filter);
    const json expected = json::parse(R"({
        "service": "ingest",
        "events": [
            {"id": 2, "level": "warn", "priority": "high"},
            {"id": 3, "level": "info"}
        ]
    })");

    require(filtered == expected, "callback dropped debug/trace entries and tagged warn event");
    std::cout << "streaming_callback: ok\n";
}

}  // namespace

int main() {
    try {
        run();
    } catch (const std::exception& err) {
        std::cerr << "streaming_callback failed: " << err.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
