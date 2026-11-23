// Custom type serialization (macros & adl_serializer), json_pointer, RFC 6902 diff/patch, and RFC 7396 merge_patch.

#include "neojson/json.hpp"

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace app {

struct ServiceEndpoint {
    std::string host;
    int port = 0;
    bool tls = false;
};

NEOJSON_DEFINE_TYPE_NON_INTRUSIVE(ServiceEndpoint, host, port, tls)

struct RetryPolicy {
    int max_attempts = 3;
    int backoff_ms = 250;
};

NEOJSON_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RetryPolicy, max_attempts, backoff_ms)

struct Version {
    int ver_major = 0;
    int ver_minor = 0;
    int ver_patch = 0;
};

inline bool operator==(const Version& lhs, const Version& rhs) noexcept {
    return lhs.ver_major == rhs.ver_major && lhs.ver_minor == rhs.ver_minor && lhs.ver_patch == rhs.ver_patch;
}

}  // namespace app

namespace neojson {

template <>
struct adl_serializer<app::Version> {
    static void to_json(json& j, const app::Version& v) {
        j = std::to_string(v.ver_major) + "." + std::to_string(v.ver_minor) + "." + std::to_string(v.ver_patch);
    }

    static void from_json(const json& j, app::Version& v) {
        const std::string raw = j.get<std::string>();
        const std::size_t dot1 = raw.find('.');
        const std::size_t dot2 = (dot1 == std::string::npos) ? std::string::npos : raw.find('.', dot1 + 1);
        if (dot1 == std::string::npos || dot2 == std::string::npos || raw.find('.', dot2 + 1) != std::string::npos) {
            throw std::invalid_argument("version string must have M.m.p format");
        }
        auto parse_part = [](std::string_view sv) -> int {
            if (sv.empty() || sv.front() < '0' || sv.front() > '9') {
                throw std::invalid_argument("invalid numeric version component");
            }
            int value = 0;
            const auto res = std::from_chars(sv.data(), sv.data() + sv.size(), value);
            if (res.ec != std::errc{} || res.ptr != sv.data() + sv.size()) {
                throw std::invalid_argument("version component parse failed");
            }
            return value;
        };
        const std::string_view sv(raw);
        v.ver_major = parse_part(sv.substr(0, dot1));
        v.ver_minor = parse_part(sv.substr(dot1 + 1, dot2 - dot1 - 1));
        v.ver_patch = parse_part(sv.substr(dot2 + 1));
    }
};

}  // namespace neojson

namespace {

using json = neojson::json;

void require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(msg);
    }
}

void run() {
    // 1. Non-intrusive struct serialization + default fallback + adl_serializer.
    const app::ServiceEndpoint endpoint{"api.example.invalid", 8443, true};
    const app::Version version{1, 4, 2};
    const json partial_retry = json::parse(R"({"max_attempts": 5})");
    const auto retry = partial_retry.get<app::RetryPolicy>();
    require(retry.max_attempts == 5 && retry.backoff_ms == 250, "WITH_DEFAULT fills omitted field");

    const json cfg = {
        {"version", version},
        {"service", endpoint},
        {"retry", retry},
        {"deprecated", true},
    };
    require(cfg.at("version").get<app::Version>() == version, "Version round-trips through adl_serializer");
    require(cfg.at("service").get<app::ServiceEndpoint>().port == 8443, "ServiceEndpoint round-trips");

    // 2. RFC 6901 json_pointer access.
    const json::json_pointer port_ptr("/service/port");
    const json::json_pointer missing_ptr("/service/region");
    require(cfg.contains(port_ptr), "json_pointer detects existing path");
    require(cfg.at(port_ptr).get<int>() == 8443, "json_pointer reads nested port");
    require(!cfg.contains(missing_ptr), "json_pointer detects absent path");
    require(cfg.value(missing_ptr, std::string("global")) == "global", "json_pointer value() default");

    // 3. RFC 6902 diff + patch.
    json updated = cfg;
    updated["service"]["port"] = 9443;
    updated["service"]["region"] = "eu-central";
    updated.erase("deprecated");
    const json rfc6902_patch = json::diff(cfg, updated);
    require(cfg.patch(rfc6902_patch) == updated, "RFC 6902 diff + patch reproduces target");

    // 4. RFC 7396 merge_patch (update, add, null removes key).
    json merged = cfg;
    merged.merge_patch(json::parse(R"({
        "service": {"port": 9443, "region": "eu-central"},
        "deprecated": null
    })"));
    require(merged == updated, "RFC 7396 merge_patch updates, adds, and deletes keys");

    std::cout << "custom_types_and_patch: ok\n";
}

}  // namespace

int main() {
    try {
        run();
    } catch (const std::exception& err) {
        std::cerr << "custom_types_and_patch failed: " << err.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
