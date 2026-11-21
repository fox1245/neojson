// Differential and thread-safety tests for ordered_map's hash index across size thresholds, overwrites,
// deletions, copies, moves, swaps, and special keys.

#include <cstddef>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "differential.hpp"

namespace {

using jk = neojson::json;
using oj = nlohmann::ordered_json;
using neojson::test::assert_equivalent;
using neojson::test::expect_same_exception;

TEST(OrderedMapIndex, CrossThresholdInsertOverwriteEraseAndCopy) {
    const std::size_t t = neojson::detail::kOrderedMapHashThreshold;
    oj o = oj::object();
    jk j = jk::object();

    // Grow across threshold (0 -> t + 40), checking lookups at every step.
    for (std::size_t i = 0; i < t + 40; ++i) {
        const std::string k = "key_" + std::to_string(i);
        o[k] = static_cast<int>(i);
        j[k] = static_cast<int>(i);
        ASSERT_TRUE(j.contains(k));
        ASSERT_FALSE(j.contains("missing_" + std::to_string(i)));
        EXPECT_EQ(j.at(k).get<int>(), static_cast<int>(i));
    }
    assert_equivalent(o, j);

    // Overwrite existing keys while preserving insertion order.
    for (std::size_t i = 0; i < t + 40; i += 3) {
        const std::string k = "key_" + std::to_string(i);
        o[k] = static_cast<int>(i * 100);
        j[k] = static_cast<int>(i * 100);
    }
    assert_equivalent(o, j);

    // Copy must be independent and keep the hash index intact.
    oj o_copy = o;
    jk j_copy = j;
    o_copy["key_0"] = -999;
    j_copy["key_0"] = -999;
    o_copy["extra_copy_key"] = 777;
    j_copy["extra_copy_key"] = 777;
    assert_equivalent(o, j);
    assert_equivalent(o_copy, j_copy);

    // Move and swap must keep index valid.
    jk j_moved = std::move(j_copy);
    assert_equivalent(o_copy, j_moved);
    jk j_swapped = jk::object({{"small", 1}});
    j_swapped.swap(j_moved);
    assert_equivalent(o_copy, j_swapped);
    EXPECT_EQ(j_moved.size(), 1u);
    EXPECT_EQ(j_moved.at("small").get<int>(), 1);

    // Erase one by one across the threshold down to 0, verifying remaining keys each time.
    for (std::size_t i = 0; i < t + 40; ++i) {
        const std::string k = "key_" + std::to_string(i);
        EXPECT_EQ(o.erase(k), j.erase(k));
        EXPECT_FALSE(j.contains(k));
        EXPECT_EQ(j.count(k), 0u);
        if (i + 1 < t + 40) {
            const std::string next_k = "key_" + std::to_string(i + 1);
            EXPECT_TRUE(j.contains(next_k));
            EXPECT_EQ(j.at(next_k).get<int>(), o.at(next_k).get<int>());
        }
    }
    assert_equivalent(o, j);
    EXPECT_TRUE(j.empty());

    // Regrow after clear() and range-erase.
    for (std::size_t i = 0; i < t + 30; ++i) {
        const std::string k = "re_" + std::to_string(i);
        o[k] = static_cast<int>(i);
        j[k] = static_cast<int>(i);
    }
    o.erase(std::next(o.begin(), 2), std::next(o.begin(), static_cast<std::ptrdiff_t>(t + 5)));
    j.erase(std::next(j.begin(), 2), std::next(j.begin(), static_cast<std::ptrdiff_t>(t + 5)));
    assert_equivalent(o, j);

    o.clear();
    j.clear();
    assert_equivalent(o, j);
}

TEST(OrderedMapIndex, EmbeddedNulPrefixesAndRandomMutations) {
    const std::vector<std::string> special_keys = {
        "",
        std::string("\0", 1),
        std::string("\0\0", 2),
        std::string("a\0b", 3),
        std::string("a\0c", 3),
        "a",
        "aa",
        "aaa",
        "aaaa",
        "aaaaa",
        "aaaaaa",
        "aaaaaaa",
        "aaaaaaaa",
        "aaaaaaaaa",
        "long_key_prefix_0001",
        "long_key_prefix_0002",
        "long_key_prefix_0003",
        "long_key_prefix_0004",
        "long_key_prefix_0005",
        "long_key_prefix_0006",
    };

    oj o = oj::object();
    jk j = jk::object();
    for (std::size_t i = 0; i < special_keys.size(); ++i) {
        o[special_keys[i]] = static_cast<int>(i);
        j[special_keys[i]] = static_cast<int>(i);
    }
    assert_equivalent(o, j);
    for (std::size_t i = 0; i < special_keys.size(); ++i) {
        EXPECT_EQ(j.at(special_keys[i]).get<int>(), static_cast<int>(i));
    }

    // Randomized sequence of inserts, emplaces, overwrites, erases, and copies around threshold.
    std::mt19937 rng(42);
    for (int step = 0; step < 600; ++step) {
        const std::size_t idx = rng() % 64;
        const std::string k = "k_" + std::to_string(idx) + (idx % 5 == 0 ? std::string("\0x", 2) : "");
        switch (rng() % 5) {
            case 0:
            case 1:
                o[k] = step;
                j[k] = step;
                break;
            case 2: {
                auto [oit, oins] = o.emplace(k, step);
                auto [jit, jins] = j.emplace(k, step);
                EXPECT_EQ(oins, jins);
                EXPECT_EQ(oit.key(), jit.key());
                break;
            }
            case 3:
                EXPECT_EQ(o.erase(k), j.erase(k));
                break;
            case 4:
                expect_same_exception([&] { return o.at(k).get<int>(); },
                                      [&] { return j.at(k).get<int>(); });
                break;
        }
    }
    assert_equivalent(o, j);
}

TEST(OrderedMapIndex, ConcurrentConstReadsAreRaceFree) {
    jk j = jk::object();
    for (int i = 0; i < 200; ++i) {
        j["field_" + std::to_string(i)] = i * 3;
    }
    const jk& cj = j;

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&cj, t]() {
            for (int iter = 0; iter < 500; ++iter) {
                const int idx = (iter + t * 37) % 200;
                const std::string k = "field_" + std::to_string(idx);
                ASSERT_TRUE(cj.contains(k));
                ASSERT_FALSE(cj.contains("absent_" + std::to_string(idx)));
                ASSERT_EQ(cj.at(k).get<int>(), idx * 3);
                ASSERT_EQ(cj.find(k).value().get<int>(), idx * 3);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
}

}  // namespace
