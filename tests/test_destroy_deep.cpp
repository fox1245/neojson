// Destroying a tree that a program built (not parsed) and that is nested far deeper than the parse limit. The
// destructor must not need stack in proportion to the depth: these trees are destroyed on a thread with a 512 KiB
// stack, which a call per level (a few dozen bytes each) cannot manage for a million levels.

#include <cstddef>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "neojson/json.hpp"
#include "small_stack.hpp"

namespace {

using neojson::json;

constexpr std::size_t kLevels = 1000000;
constexpr std::size_t kSmallStack = 512 * 1024;

// Wraps `inner` in one more array, without copying it.
void wrap_in_array(json& inner) {
    json outer(json::value_t::array);
    outer.push_back(std::move(inner));
    inner = std::move(outer);
}

// Wraps `inner` in an object with one member, without copying it.
void wrap_in_object(json& inner) {
    json outer(json::value_t::object);
    outer["k"] = std::move(inner);
    inner = std::move(outer);
}

// Wraps `inner` in an array that has other elements before and after it, plain and not.
void wrap_with_siblings(json& inner) {
    json outer(json::value_t::array);
    outer.push_back(1);
    outer.push_back(json::array());
    outer.push_back(std::move(inner));
    outer.push_back("text");
    outer.push_back(json::object());
    inner = std::move(outer);
}

TEST(DestroyDeep, ArraysNestedAMillionLevelsDeep) {
    const std::string error = neojson::test::run_with_stack(kSmallStack, [] {
        json j = 0;
        for (std::size_t i = 0; i < kLevels; ++i) {
            wrap_in_array(j);
        }
        ASSERT_TRUE(j.is_array());
        // j is destroyed when this function returns.
    });
    EXPECT_EQ(error, "");
}

TEST(DestroyDeep, ObjectsNestedAMillionLevelsDeep) {
    const std::string error = neojson::test::run_with_stack(kSmallStack, [] {
        json j = "innermost";
        for (std::size_t i = 0; i < kLevels; ++i) {
            wrap_in_object(j);
        }
        ASSERT_TRUE(j.is_object());
    });
    EXPECT_EQ(error, "");
}

TEST(DestroyDeep, ArraysAndObjectsAlternatingWithSiblings) {
    const std::string error = neojson::test::run_with_stack(kSmallStack, [] {
        json j = nullptr;
        for (std::size_t i = 0; i < kLevels / 4; ++i) {
            wrap_in_array(j);
            wrap_in_object(j);
            wrap_with_siblings(j);
            wrap_in_object(j);
        }
        ASSERT_TRUE(j.is_object());
    });
    EXPECT_EQ(error, "");
}

TEST(DestroyDeep, AssigningOverADeepTreeDestroysItIteratively) {
    const std::string error = neojson::test::run_with_stack(kSmallStack, [] {
        json j = 0;
        for (std::size_t i = 0; i < kLevels; ++i) {
            wrap_in_array(j);
        }
        j = nullptr;
        EXPECT_TRUE(j.is_null());

        json k = 0;
        for (std::size_t i = 0; i < kLevels; ++i) {
            wrap_in_object(k);
        }
        k.clear();
        EXPECT_TRUE(k.empty());
    });
    EXPECT_EQ(error, "");
}

TEST(DestroyDeep, AWideAndDeepTreeKeepsEveryValueUntilItIsDestroyed) {
    // A tree of shallow subtrees hanging from a deep spine: destroying the original leaves its copy intact.
    json spine = json::array();
    for (std::size_t i = 0; i < 2000; ++i) {
        json level = json::array({i, json::object({{"a", json::array({1, 2, 3})}}), "s"});
        level.push_back(std::move(spine));
        spine = std::move(level);
    }
    const json copy = spine;
    const std::string dumped = copy.dump();
    {
        json victim = std::move(spine);
    }
    EXPECT_EQ(copy.dump(), dumped);
    EXPECT_EQ(copy.size(), 4u);

    // An object with many members, some of them containers, is taken apart without losing the other members' storage.
    json many = json::object();
    for (std::size_t i = 0; i < 5000; ++i) {
        many["k" + std::to_string(i)] = (i % 3 == 0) ? json::array({i, json::object({{"x", i}})}) : json(i);
    }
    EXPECT_EQ(many.size(), 5000u);
    many = json();
    EXPECT_TRUE(many.is_null());
}

} // namespace
