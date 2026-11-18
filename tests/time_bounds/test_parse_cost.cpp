// Parse-cost tests for large objects and arrays. Each test parses an object of 100,000 to 200,000 members or an array
// of as many elements, without and with a callback. With a cost that grows with the square of the member count they
// run for minutes, so CMake gives this binary a short CTest timeout
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

// Parsing with a callback: the callback sees every event; the cost of building the tree stays proportional to the
// input whichever members and elements the callback keeps or drops.
using Event = neojson::json::parse_event_t;

neojson::json::parser_callback_t keep_everything() {
    return [](int, Event, neojson::json&) { return true; };
}

// {"a":0,"b":[1,2]} repeated n times in an array.
std::string small_objects(std::size_t n) {
    std::string text = "[";
    for (std::size_t i = 0; i < n; ++i) {
        if (i != 0) {
            text += ',';
        }
        text += "{\"a\":" + std::to_string(i) + ",\"b\":[1,2]}";
    }
    text += ']';
    return text;
}

TEST(ParseCostCallback, ObjectKeepingEverything) {
    const std::string text = distinct_members(kMembers);
    const neojson::json j = neojson::json::parse(text, keep_everything());
    ASSERT_EQ(j.size(), kMembers);
    EXPECT_EQ(j, neojson::json::parse(text));
}

TEST(ParseCostCallback, ObjectWithEveryKeyRepeatedKeepingEverything) {
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
    const neojson::json j = neojson::json::parse(text, keep_everything());
    ASSERT_EQ(j.size(), distinct);
    EXPECT_EQ(j, neojson::json::parse(text));
}

TEST(ParseCostCallback, ObjectDroppingEveryOtherKey) {
    const std::string text = distinct_members(kMembers);
    std::size_t keys = 0;
    const neojson::json j = neojson::json::parse(text, neojson::json::parser_callback_t([&keys](int, Event event, neojson::json&) {
        return event != Event::key || (keys++ % 2) == 0;
    }));
    ASSERT_EQ(j.size(), kMembers / 2);
    auto it = j.begin();
    for (std::size_t i = 0; i < kMembers; i += 2, ++it) {
        ASSERT_EQ(it.key(), "k" + std::to_string(i));
        ASSERT_EQ(it.value().get<std::size_t>(), i);
    }
}

// A value that is dropped leaves a discarded member in its object.
TEST(ParseCostCallback, ObjectDroppingEveryOtherValue) {
    const std::string text = distinct_members(kMembers);
    std::size_t values = 0;
    const neojson::json j = neojson::json::parse(text, neojson::json::parser_callback_t([&values](int, Event event, neojson::json&) {
        return event != Event::value || (values++ % 2) == 0;
    }));
    ASSERT_EQ(j.size(), kMembers);
    auto it = j.begin();
    for (std::size_t i = 0; i < kMembers; ++i, ++it) {
        ASSERT_EQ(it.key(), "k" + std::to_string(i));
        if (i % 2 == 0) {
            ASSERT_EQ(it.value().get<std::size_t>(), i);
        } else {
            ASSERT_TRUE(it.value().is_discarded()) << "member " << i;
        }
    }
}

// Members alternate between a number that the callback drops and a small object. Each object that ends removes the
// first discarded member of the outer object, which is the number before it.
TEST(ParseCostCallback, ObjectOfNumbersAndObjectsDroppingTheNumbers) {
    std::string text = "{";
    for (std::size_t i = 0; i < kMembers; ++i) {
        if (i != 0) {
            text += ',';
        }
        text += "\"k" + std::to_string(i) + "\":" + (i % 2 == 0 ? std::to_string(i) : "{\"x\":1}");
    }
    text += '}';
    const neojson::json j = neojson::json::parse(text, neojson::json::parser_callback_t([](int depth, Event event, neojson::json& parsed) {
        return !(event == Event::value && depth == 1 && parsed.is_number());
    }));
    ASSERT_EQ(j.size(), kMembers / 2);
    auto it = j.begin();
    for (std::size_t i = 1; i < kMembers; i += 2, ++it) {
        ASSERT_EQ(it.key(), "k" + std::to_string(i));
        ASSERT_TRUE(it.value().is_object());
    }
}

TEST(ParseCostCallback, ArrayOfObjectsKeepingEverything) {
    const std::string text = small_objects(kMembers);
    const neojson::json j = neojson::json::parse(text, keep_everything());
    ASSERT_EQ(j.size(), kMembers);
    EXPECT_EQ(j, neojson::json::parse(text));
}

TEST(ParseCostCallback, ArrayDroppingEveryOtherObject) {
    const std::string text = small_objects(kMembers);
    std::size_t ended = 0;
    const neojson::json j = neojson::json::parse(text, neojson::json::parser_callback_t([&ended](int depth, Event event, neojson::json&) {
        return !(event == Event::object_end && depth == 1 && (ended++ % 2) == 1);
    }));
    ASSERT_EQ(j.size(), kMembers / 2);
    for (std::size_t i = 0; i < kMembers / 2; ++i) {
        ASSERT_EQ(j[i]["a"].get<std::size_t>(), 2 * i) << "element " << i;
    }
}

TEST(ParseCostCallback, ArrayDroppingEveryOtherKey) {
    const std::string text = small_objects(kMembers);
    std::size_t keys = 0;
    const neojson::json j = neojson::json::parse(text, neojson::json::parser_callback_t([&keys](int, Event event, neojson::json&) {
        return event != Event::key || (keys++ % 2) == 0;
    }));
    ASSERT_EQ(j.size(), kMembers);
    for (std::size_t i = 0; i < kMembers; ++i) {
        ASSERT_EQ(j[i].size(), 1u) << "element " << i;
        ASSERT_EQ(j[i]["a"].get<std::size_t>(), i);
    }
}

} // namespace
