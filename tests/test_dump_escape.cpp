// dump of strings against nlohmann/json: every pair and triple of the fragments below, as a string value and as an
// object key, through assert_equivalent (which dumps with ensure_ascii on and off and with each error handler). The
// fragments cover the bytes that are copied as they are, the escapes, every length of a UTF-8 sequence, and sequences
// that are invalid or cut off, so that runs of copied bytes start and end next to each kind.

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "differential.hpp"

namespace {

using neojson::test::assert_equivalent;

const std::vector<std::string>& fragments() {
    static const std::vector<std::string> f = {
        "",
        "a",
        "plain text",
        "\"",
        "\\",
        "/",
        std::string("\0", 1),
        "\x01",
        "\x08",
        "\t",
        "\n",
        "\x0C",
        "\r",
        "\x1F",
        " ",
        "~",
        "\x7F",
        "\xC3\xA9",          // 2 bytes
        "\xE2\x82\xAC",      // 3 bytes
        "\xEF\xBF\xBF",      // 3 bytes, U+FFFF
        "\xF0\x9F\x98\x80",  // 4 bytes
        "\xF4\x8F\xBF\xBF",  // 4 bytes, U+10FFFF
        "\x80",              // a continuation byte alone
        "\xFF",
        "\xC0\xAF",          // overlong
        "\xED\xA0\x80",      // surrogate
        "\xF4\x90\x80\x80",  // above U+10FFFF
        "\xC3",              // cut off
        "\xE2\x82",          // cut off
        "\xF0\x9F\x98",      // cut off
    };
    return f;
}

void check(const std::string& s) {
    {
        nlohmann::ordered_json o = s;
        neojson::json j = s;
        assert_equivalent(o, j);
    }
    {
        nlohmann::ordered_json o = nlohmann::ordered_json::object();
        neojson::json j = neojson::json::object();
        o[s] = 1;
        j[s] = 1;
        assert_equivalent(o, j);
    }
}

TEST(DumpEscape, PairsOfFragments) {
    for (const auto& a : fragments()) {
        for (const auto& b : fragments()) {
            check(a + b);
            if (::testing::Test::HasFailure()) return;
        }
    }
}

TEST(DumpEscape, TriplesOfFragments) {
    for (const auto& a : fragments()) {
        for (const auto& b : fragments()) {
            for (const auto& c : fragments()) {
                check(a + b + c);
                if (::testing::Test::HasFailure()) return;
            }
        }
    }
}

TEST(DumpEscape, RandomBytes) {
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> len(0, 40);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> kind(0, 3);
    for (int n = 0; n < 4000; ++n) {
        std::string s;
        const int l = len(rng);
        for (int i = 0; i < l; ++i) {
            // Mostly printable ASCII, so that long runs of copied bytes occur between the other bytes.
            s.push_back(static_cast<char>(kind(rng) == 0 ? byte(rng) : 0x20 + byte(rng) % 0x5F));
        }
        check(s);
        if (::testing::Test::HasFailure()) return;
    }
}

}  // namespace
