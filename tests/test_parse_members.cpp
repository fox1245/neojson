// Objects with repeated keys, around and above the member count at which parsing switches from scanning the members
// to sorting the keys. nlohmann::ordered_json is the specification: a repeated key keeps the position of its first
// occurrence and the value of its last occurrence.

#include "differential.hpp"

#include <cstddef>
#include <random>
#include <string>
#include <vector>

using neojson::test::assert_equivalent;

namespace {

using jk = neojson::json;
using oj = nlohmann::ordered_json;

std::string key_text(std::size_t k) {
    return "\"k" + std::to_string(k) + "\"";
}

// Members with the given key numbers, in order; the value of member i is i.
std::string object_text(const std::vector<std::size_t>& keys) {
    std::string text = "{";
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i != 0) {
            text += ',';
        }
        text += key_text(keys[i]) + ":" + std::to_string(i);
    }
    text += '}';
    return text;
}

void expect_like_oracle(const std::string& text) {
    ASSERT_TRUE(oj::accept(text));
    assert_equivalent(oj::parse(text), jk::parse(text));
    // Parsing an object nested in an array and an object (the same code on a non-root object).
    const std::string wrapped = "[" + text + ",{\"w\":" + text + "}]";
    assert_equivalent(oj::parse(wrapped), jk::parse(wrapped));
}

TEST(ParseMembers, RepeatedKeyKeepsFirstPositionAndLastValue) {
    const jk j = jk::parse(R"({"a":1,"b":2,"a":3})");
    ASSERT_EQ(j.size(), 2u);
    auto it = j.begin();
    EXPECT_EQ(it.key(), "a");
    EXPECT_EQ(it.value().get<int>(), 3);
    ++it;
    EXPECT_EQ(it.key(), "b");
    EXPECT_EQ(oj::parse(R"({"a":1,"b":2,"a":3})").dump(), j.dump());
}

TEST(ParseMembers, SizesAroundTheThresholdWithRepeatedKeys) {
    const std::size_t t = neojson::detail::kObjectIndexThreshold;
    std::mt19937_64 rng(12345);
    for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{2}, t - 1, t, t + 1, t + 2,
                          2 * t, 4 * t + 3}) {
        SCOPED_TRACE("members " + std::to_string(n));

        // All distinct.
        std::vector<std::size_t> keys(n);
        for (std::size_t i = 0; i < n; ++i) {
            keys[i] = i;
        }
        expect_like_oracle(object_text(keys));

        // Every key twice in a row, every key twice with the block repeated, reversed second block.
        std::vector<std::size_t> pairs;
        std::vector<std::size_t> twice;
        std::vector<std::size_t> reversed;
        for (std::size_t i = 0; i < n; ++i) {
            pairs.push_back(i);
            pairs.push_back(i);
            twice.push_back(i);
            reversed.push_back(i);
        }
        for (std::size_t i = 0; i < n; ++i) {
            twice.push_back(i);
            reversed.push_back(n - 1 - i);
        }
        expect_like_oracle(object_text(pairs));
        expect_like_oracle(object_text(twice));
        expect_like_oracle(object_text(reversed));

        // A key three times, once as the first and once as the last member.
        if (n >= 3) {
            std::vector<std::size_t> three = keys;
            three.push_back(0);
            three.insert(three.begin() + 1, 0);
            three.push_back(n - 1);
            expect_like_oracle(object_text(three));
        }

        // Random draws from a small pool of keys, so that most keys repeat.
        for (std::size_t pool : {std::size_t{1}, std::size_t{3}, n / 2 + 1, n + 5}) {
            std::vector<std::size_t> drawn(n);
            for (auto& k : drawn) {
                k = rng() % pool;
            }
            expect_like_oracle(object_text(drawn));
        }
    }
}

TEST(ParseMembers, TwoThousandMembersOfEachRepeatedShape) {
    const std::size_t distinct = 1000;
    std::vector<std::size_t> pairs;
    std::vector<std::size_t> twice;
    for (std::size_t i = 0; i < distinct; ++i) {
        pairs.push_back(i);
        pairs.push_back(i);
        twice.push_back(i);
    }
    for (std::size_t i = 0; i < distinct; ++i) {
        twice.push_back(i);
    }
    expect_like_oracle(object_text(pairs));
    expect_like_oracle(object_text(twice));

    std::vector<std::size_t> distinct_keys(2 * distinct);
    for (std::size_t i = 0; i < distinct_keys.size(); ++i) {
        distinct_keys[i] = i;
    }
    expect_like_oracle(object_text(distinct_keys));
}

TEST(ParseMembers, KeysThatDifferInLengthOrContainSpecialCharacters) {
    // Keys that are prefixes of each other, empty, with an escaped NUL, with non-ASCII bytes.
    std::string text = "{";
    const std::vector<std::string> pool = {"\"\"",       "\"a\"",        "\"aa\"",
                                           "\"aaa\"",    "\"a\\u0000\"", "\"a\\u0000b\"",
                                           "\"\\u00e9\"", "\"\xc3\xa9\"", "\"e\\u0301\"",
                                           "\"A\"",      "\"a \"",       "\" a\""};
    std::mt19937_64 rng(7);
    for (std::size_t i = 0; i < 500; ++i) {
        if (i != 0) {
            text += ',';
        }
        text += pool[rng() % pool.size()] + ":" + std::to_string(i);
    }
    text += '}';
    expect_like_oracle(text);
}

TEST(ParseMembers, ValuesOfMembersAreNestedStructures) {
    // Repeated keys whose earlier and later values are containers: only the last survives.
    std::string text = "{";
    for (std::size_t i = 0; i < 300; ++i) {
        if (i != 0) {
            text += ',';
        }
        const std::string key = key_text(i % 100);
        text += key + ":" + (i % 2 == 0 ? "[" + std::to_string(i) + ",{\"x\":[]}]" : "{\"v\":" + std::to_string(i) + "}");
    }
    text += '}';
    expect_like_oracle(text);
}

TEST(ParseMembers, ObjectsParsedWithACallbackKeepTheRule) {
    // The callback path inserts one member at a time; it must give what the oracle gives.
    std::vector<std::size_t> keys;
    for (std::size_t i = 0; i < 200; ++i) {
        keys.push_back(i % 70);
    }
    const std::string text = object_text(keys);
    const jk::parser_callback_t keep = [](int, jk::parse_event_t, jk&) { return true; };
    const oj::parser_callback_t keep_oracle = [](int, oj::parse_event_t, oj&) { return true; };
    assert_equivalent(oj::parse(text, keep_oracle), jk::parse(text, keep));
}

} // namespace
