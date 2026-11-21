// Differential and thread-safety tests for ordered_map's hash index across size thresholds, overwrites,
// deletions, copies, moves, swaps, and special keys.

#include <atomic>
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

TEST(OrderedMapIndex, ResizeAssignAndPopBackKeepIndexConsistent) {
    jk j = jk::object();
    for (int i = 0; i < 40; ++i) {
        j["k" + std::to_string(i)] = i;
    }
    auto& obj = j.get_ref<jk::object_t&>();
    obj.resize(20);
    EXPECT_EQ(obj.size(), 20u);
    EXPECT_TRUE(j.contains("k0"));
    EXPECT_TRUE(j.contains("k19"));
    EXPECT_FALSE(j.contains("k20"));
    EXPECT_FALSE(j.contains("k39"));

    obj.pop_back();
    EXPECT_EQ(obj.size(), 19u);
    EXPECT_FALSE(j.contains("k19"));
    EXPECT_TRUE(j.contains("k18"));

    obj.resize(1);
    EXPECT_EQ(obj.size(), 1u);
    EXPECT_TRUE(j.contains("k0"));
    EXPECT_FALSE(j.contains("k18"));
    j["added"] = 99;
    EXPECT_EQ(j.size(), 2u);
    EXPECT_EQ(j.at("added"), 99);

    std::vector<jk::object_t::value_type> replacement;
    for (int i = 0; i < 24; ++i) {
        replacement.emplace_back("r" + std::to_string(i), i * 10);
    }
    obj.assign(replacement.begin(), replacement.end());
    EXPECT_EQ(obj.size(), 24u);
    EXPECT_FALSE(j.contains("k0"));
    EXPECT_TRUE(j.contains("r23"));
    EXPECT_EQ(j.at("r23"), 230);

    obj.assign({{"solo", 7}});
    EXPECT_EQ(obj.size(), 1u);
    EXPECT_FALSE(j.contains("r23"));
    EXPECT_EQ(j.at("solo"), 7);
}

TEST(OrderedMapIndex, LazyIndexConcurrentFirstLookupIsRaceFree) {
    auto build_raw = [](const bool with_dups) {
        std::string s = "{";
        for (int i = 0; i < 200; ++i) {
            if (i > 0) {
                s.push_back(',');
            }
            s += "\"field_" + std::to_string(i) + "\":" + std::to_string(i * 3);
        }
        if (with_dups) {
            s += R"(,"field_0":9000,"field_100":9100,"field_199":9199)";
        }
        s.push_back('}');
        return s;
    };

    for (const bool with_dups : {false, true}) {
        const std::string raw = build_raw(with_dups);
        const oj oracle = oj::parse(raw);
        const jk doc = jk::parse(raw);

        // Materialize via const iteration without performing any key lookups (no HashIndex yet).
        std::size_t visited = 0;
        for (auto it = doc.cbegin(); it != doc.cend(); ++it) {
            (void)it.key();
            (void)it.value().get<int>();
            ++visited;
        }
        ASSERT_EQ(visited, 200u);

        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&, t]() {
                while (!go.load(std::memory_order_acquire)) {
                }
                for (int rep = 0; rep < 300; ++rep) {
                    const int idx = (rep + t * 29) % 200;
                    const std::string k = "field_" + std::to_string(idx);
                    int expected = idx * 3;
                    if (with_dups && (idx == 0 || idx == 100 || idx == 199)) {
                        expected = 9000 + idx;
                    }
                    ASSERT_TRUE(doc.contains(k));
                    ASSERT_FALSE(doc.contains("missing_" + std::to_string(idx)));
                    ASSERT_EQ(doc.at(k).get<int>(), expected);
                    auto fit = doc.find(k);
                    ASSERT_NE(fit, doc.cend());
                    ASSERT_EQ(fit.value().get<int>(), expected);
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& th : threads) {
            th.join();
        }
        assert_equivalent(oracle, doc);
    }
}

TEST(OrderedMapIndex, LargeObjectDuplicateKeysMaterializePreservesOrderAndLastWins) {
    std::string raw = "{";
    for (int i = 0; i < 64; ++i) {
        if (i > 0) {
            raw.push_back(',');
        }
        raw += "\"k_" + std::to_string(i) + "\":" + std::to_string(i);
    }
    raw += R"(,"k_0":1000,"k_31":{"nested":[1,2]},"k_63":"last_win","k_17":true)";
    raw.push_back('}');

    const oj oracle = oj::parse(raw);
    const jk doc = jk::parse(raw);

    // Iterate first (single-pass DupProbeTable path without HashIndex).
    std::vector<std::string> got_keys;
    for (auto it = doc.cbegin(); it != doc.cend(); ++it) {
        got_keys.push_back(it.key());
    }
    std::vector<std::string> exp_keys;
    for (auto it = oracle.cbegin(); it != oracle.cend(); ++it) {
        exp_keys.push_back(it.key());
    }
    EXPECT_EQ(got_keys, exp_keys);

    // Trigger lazy HashIndex construction on lookups after iteration.
    EXPECT_TRUE(doc.contains("k_0"));
    EXPECT_TRUE(doc.contains("k_31"));
    EXPECT_TRUE(doc.contains("k_63"));
    EXPECT_TRUE(doc.contains("k_17"));
    EXPECT_FALSE(doc.contains("k_64"));
    EXPECT_EQ(doc.at("k_0").get<int>(), 1000);
    EXPECT_EQ(doc.find("k_31").value().at("nested").at(1).get<int>(), 2);
    EXPECT_EQ(doc.at("k_63").get<std::string>(), "last_win");
    EXPECT_TRUE(doc.at("k_17").get<bool>());
    EXPECT_EQ(doc.dump(), oracle.dump());
    assert_equivalent(oracle, doc);
}

}  // namespace
