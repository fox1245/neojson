// Parse-cost tests for large objects. Each test parses an object of 100,000 to 200,000 members. With a cost that
// grows with the square of the member count they run for minutes, so CMake gives this binary a short CTest timeout
// when NEOJSON_TEST_TIME_BOUNDS is ON. Sanitizer builds turn that option OFF: they run this code several hundred
// times slower than an uninstrumented build, and there the tests check results only.

#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "neojson/json.hpp"

namespace {

constexpr std::size_t kMembers = 200000;

// {"k0":0,"k1":1,...,"k<n-1>":<n-1>}
std::string distinct_members(std::size_t n) {
    std::string text = "{";
    for (std::size_t i = 0; i < n; ++i) {
        if (i != 0) {
            text += ',';
        }
        text += "\"k" + std::to_string(i) + "\":" + std::to_string(i);
    }
    text += '}';
    return text;
}

TEST(ParseCost, ObjectWithDistinctMembers) {
    const std::string text = distinct_members(kMembers);
    const neojson::json j = neojson::json::parse(text);

    ASSERT_TRUE(j.is_object());
    ASSERT_EQ(j.size(), kMembers);
    auto it = j.begin();
    EXPECT_EQ(it.key(), "k0");
    EXPECT_EQ(it.value().get<std::size_t>(), 0u);
    for (std::size_t i = 0; i < kMembers; ++i, ++it) {
        ASSERT_EQ(it.key(), "k" + std::to_string(i)) << "member " << i;
        ASSERT_EQ(it.value().get<std::size_t>(), i) << "member " << i;
    }
    --it;
    EXPECT_EQ(it.key(), "k" + std::to_string(kMembers - 1));
}

// Every key occurs twice: the second pass over k0..k<n/2-1> follows the first. A repeated key keeps the position of
// its first occurrence and the value of its last one.
TEST(ParseCost, ObjectWithEveryKeyRepeatedAfterTheBlock) {
    const std::size_t distinct = kMembers / 2;
    std::string text = "{";
    for (std::size_t pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < distinct; ++i) {
            if (pass != 0 || i != 0) {
                text += ',';
            }
            text += "\"k" + std::to_string(i) + "\":" + std::to_string(pass * distinct + i);
        }
    }
    text += '}';

    const neojson::json j = neojson::json::parse(text);
    ASSERT_EQ(j.size(), distinct);
    auto it = j.begin();
    for (std::size_t i = 0; i < distinct; ++i, ++it) {
        ASSERT_EQ(it.key(), "k" + std::to_string(i)) << "member " << i;
        ASSERT_EQ(it.value().get<std::size_t>(), distinct + i) << "member " << i;
    }
}

// Every key occurs twice in a row: "k0":a,"k0":b,"k1":a,"k1":b,...
TEST(ParseCost, ObjectWithEveryKeyRepeatedNextToItself) {
    const std::size_t distinct = kMembers / 2;
    std::string text = "{";
    for (std::size_t i = 0; i < distinct; ++i) {
        if (i != 0) {
            text += ',';
        }
        text += "\"k" + std::to_string(i) + "\":-1,\"k" + std::to_string(i) +
                "\":" + std::to_string(i);
    }
    text += '}';

    const neojson::json j = neojson::json::parse(text);
    ASSERT_EQ(j.size(), distinct);
    auto it = j.begin();
    for (std::size_t i = 0; i < distinct; ++i, ++it) {
        ASSERT_EQ(it.key(), "k" + std::to_string(i)) << "member " << i;
        ASSERT_EQ(it.value().get<std::size_t>(), i) << "member " << i;
    }
}

// Many large objects in one array, and a large object below the top level.
TEST(ParseCost, LargeObjectsInsideOtherValues) {
    const std::string member_text = distinct_members(kMembers / 4);
    const std::string text = "[" + member_text + ",{\"inner\":" + member_text + "}]";
    const neojson::json j = neojson::json::parse(text);
    ASSERT_EQ(j.size(), 2u);
    EXPECT_EQ(j[0].size(), kMembers / 4);
    EXPECT_EQ(j[1]["inner"].size(), kMembers / 4);
}

} // namespace
