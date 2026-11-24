#include <neojson/json.hpp>

#include <string>
#include <string_view>
#include <utility>

static_assert(NEOJSON_VERSION_MAJOR == 0 && NEOJSON_VERSION_MINOR == 1 && NEOJSON_VERSION_PATCH == 0,
              "unexpected NEOJSON_VERSION_* macros");

int main() {
    if (std::string_view(NEOJSON_VERSION_STRING) != "0.1.0") {
        return 1;
    }

    const std::string input =
        R"({"service":"neojson-packaging-smoke","version":"0.1.0","items":[1,2,3,4,5],"meta":{"active":true,"note":"hello world from installed neojson target"}})";

    if (!neojson::json::accept(input)) {
        return 2;
    }

    neojson::json doc = neojson::json::parse(input);
    const auto& ro = std::as_const(doc);
    if (ro.at("service").get<std::string>() != "neojson-packaging-smoke") {
        return 3;
    }
    if (ro.at("items").size() != 5 || !ro.at("meta").at("active").get<bool>()) {
        return 4;
    }

    const std::string dumped = ro.dump();
    if (neojson::json::parse(dumped) != ro) {
        return 5;
    }

    doc.materialize();
    doc.merge_patch(neojson::json::parse(R"({"meta":{"active":false}})"));
    if (doc.at("meta").at("active").get<bool>()) {
        return 6;
    }

    const neojson::json patched = doc.patch(neojson::json::parse(R"([{"op":"replace","path":"/version","value":"0.1.0-rc"}])"));
    if (patched.at("version").get<std::string>() != "0.1.0-rc") {
        return 7;
    }

    const neojson::thread_cache_limits limits = neojson::get_thread_cache_limits();
    neojson::set_thread_cache_limits(limits);
    neojson::trim_thread_caches();

    return 0;
}
