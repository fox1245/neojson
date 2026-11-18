// Destroying a tree that a program built (not parsed) and that is nested far deeper than the parse limit. The
// destructor must not need stack in proportion to the depth: these trees are destroyed on a thread with a 512 KiB
// stack, which a call per level (a few dozen bytes each) cannot manage for a million levels.

#include <cstddef>
#include <memory>
#include <string>
#include <thread>

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

// An array nested `levels` deep around a number: `levels` arrays, one inside the other.
json chain(std::size_t levels) {
    json j = 0;
    for (std::size_t i = 0; i < levels; ++i) {
        wrap_in_array(j);
    }
    return j;
}

constexpr std::size_t kBound = neojson::detail::kMaxRecursiveDestroyDepth;

// The number of arrays and objects that went through the stack on the heap while `f` ran on this thread.
template <typename F>
std::size_t heap_stack_uses_during(F&& f) {
    const std::size_t before = neojson::detail::destroy_heap_stack_uses();
    f();
    return neojson::detail::destroy_heap_stack_uses() - before;
}

TEST(DestroyDepth, ATreeNoDeeperThanTheBoundIsDestroyedByRecursionAlone) {
    ASSERT_EQ(neojson::detail::destroy_depth(), 0u);
    for (std::size_t levels : {std::size_t{1}, std::size_t{2}, kBound - 1, kBound}) {
        SCOPED_TRACE("levels " + std::to_string(levels));
        EXPECT_EQ(heap_stack_uses_during([&] { json j = chain(levels); }), 0u);
        EXPECT_EQ(neojson::detail::destroy_depth(), 0u);
    }
}

TEST(DestroyDepth, ATreeDeeperThanTheBoundUsesTheHeapStackBelowTheBound) {
    // The arrays below the bound are destroyed through the heap stack, one use each.
    for (std::size_t extra : {std::size_t{1}, std::size_t{2}, std::size_t{10}, std::size_t{1000}}) {
        SCOPED_TRACE("extra " + std::to_string(extra));
        EXPECT_EQ(heap_stack_uses_during([&] { json j = chain(kBound + extra); }), extra);
        EXPECT_EQ(neojson::detail::destroy_depth(), 0u);
    }
}

TEST(DestroyDepth, ParsedAndWideDocumentsNeverUseTheHeapStack) {
    std::string text = "[";
    for (std::size_t i = 0; i < 2000; ++i) {
        if (i != 0) {
            text += ',';
        }
        text += "{\"a\":" + std::to_string(i) + ",\"b\":[1,[2,[3]]],\"c\":{\"d\":{\"e\":\"x\"}}}";
    }
    text += ']';
    EXPECT_EQ(heap_stack_uses_during([&] { json j = json::parse(text); }), 0u);
    EXPECT_EQ(neojson::detail::destroy_depth(), 0u);
}

TEST(DestroyDepth, TheCountIsRestoredAfterManyTreesSoLaterTreesStillChooseTheRightWay) {
    // Many trees one after another, shallow, at the bound and past it, on a thread with a small stack. If the count
    // were left raised by any of them, a later shallow tree would be taken apart on the heap (the use count would
    // show it); if it were left lowered, a later deep tree would recurse and overflow the stack.
    const std::string error = neojson::test::run_with_stack(kSmallStack, [] {
        for (std::size_t round = 0; round < 300; ++round) {
            { json j = chain(1 + round % kBound); }
            { json j = chain(kBound + 1 + round % 5); }
            { json j = json::parse(R"({"a":[1,{"b":[2]}],"c":{"d":{"e":[3]}}})"); }
            ASSERT_EQ(neojson::detail::destroy_depth(), 0u) << "round " << round;
        }
        EXPECT_EQ(heap_stack_uses_during([] { json j = chain(kBound); }), 0u);
        EXPECT_EQ(heap_stack_uses_during([] { json j = chain(kBound / 2); }), 0u);
        EXPECT_EQ(heap_stack_uses_during([] { json j = chain(kLevels); }), kLevels - kBound);
        EXPECT_EQ(neojson::detail::destroy_depth(), 0u);
    });
    EXPECT_EQ(error, "");
}

TEST(DestroyDepth, EveryThreadCountsForItselfAndATreeMayBeDestroyedOnAnotherThread) {
    // A thread builds a deep tree and ends while it is alive; another thread with a small stack destroys it, and a
    // third destroys a tree while this one is in the middle of destroying its own (the counts do not mix).
    std::unique_ptr<json> tree;
    std::thread builder([&tree] { tree = std::make_unique<json>(chain(kLevels)); });
    builder.join();
    ASSERT_TRUE(tree != nullptr);

    std::size_t uses_in_destroyer = 0;
    std::size_t depth_in_destroyer = 1;
    const std::string error = neojson::test::run_with_stack(kSmallStack, [&] {
        uses_in_destroyer = heap_stack_uses_during([&] { tree.reset(); });
        depth_in_destroyer = neojson::detail::destroy_depth();
    });
    EXPECT_EQ(error, "");
    EXPECT_EQ(uses_in_destroyer, kLevels - kBound);
    EXPECT_EQ(depth_in_destroyer, 0u);
    // This thread never destroyed that tree.
    EXPECT_EQ(neojson::detail::destroy_depth(), 0u);
}

} // namespace
