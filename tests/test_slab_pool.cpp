// Tests for the thread-local 64 KiB slab pool: size classes, alignment, large-allocation
// fallback, orphan slab reclamation after producer thread exit, multi-threaded cross-thread
// ownership handoff (targeted by TSan), and static-duration cleanup at process exit.

#include <gtest/gtest.h>

#include "neojson/json.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using jk = neojson::json;

TEST(SlabPool, AllSizeClassesAndLargeFallbackAreAligned) {
    const std::array<std::size_t, 24> sizes = {{
        0,   1,   8,   16,  17,  32,  33,  48,
        49,  64,  65,  96,  97,  128, 129, 192,
        193, 256, 257, 384, 385, 512, 513, 2048,
    }};

    struct AllocRecord {
        void* ptr;
        std::size_t size;
        std::uint8_t tag;
    };

    std::vector<AllocRecord> records;
    for (int round = 0; round < 32; ++round) {
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            const std::size_t sz = sizes[i];
            void* p = neojson::detail::pool_alloc(sz);
            ASSERT_NE(p, nullptr);
            EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignof(std::max_align_t), 0u)
                << "size=" << sz;
            const auto tag = static_cast<std::uint8_t>((round * 17 + i + 1) & 0xFFu);
            if (sz > 0) {
                std::memset(p, tag, sz);
            }
            records.push_back({p, sz, tag});
        }
    }

    for (const AllocRecord& r : records) {
        if (r.size > 0) {
            const auto* bytes = static_cast<const std::uint8_t*>(r.ptr);
            for (std::size_t b = 0; b < r.size; ++b) {
                ASSERT_EQ(bytes[b], r.tag) << "size=" << r.size << " offset=" << b;
            }
        }
        neojson::detail::pool_free(r.ptr, r.size);
    }
}

TEST(SlabPool, ProducerThreadExitReclaimsOrphanSlabsCleanly) {
    std::vector<jk> produced;
    {
        std::thread producer([&produced]() {
            produced.reserve(64);
            for (int i = 0; i < 64; ++i) {
                jk obj = jk::object();
                for (int k = 0; k < 24; ++k) {
                    obj["k_" + std::to_string(k)] = jk::array({
                        jk("value_" + std::to_string(i) + "_" + std::to_string(k)),
                        jk(i * 100 + k),
                        jk(k % 2 == 0),
                    });
                }
                // Exercise cross-threshold erasure inside producer before handoff.
                obj.erase("k_0");
                produced.push_back(std::move(obj));
            }
            // Extra temporary allocations freed before thread exits so both empty and live slabs exist.
            for (int t = 0; t < 300; ++t) {
                jk tmp = jk::parse(R"({"a":"hello","b":[1,2,3],"c":{"x":true}})");
                EXPECT_EQ(tmp["a"], "hello");
            }
        });
        producer.join();
    }

    // Producer TLS is now destroyed; its live slabs are orphaned and must be freed cleanly here.
    ASSERT_EQ(produced.size(), 64u);
    for (int i = 0; i < 64; ++i) {
        EXPECT_FALSE(produced[i].contains("k_0"));
        EXPECT_EQ(produced[i]["k_23"][0],
                  "value_" + std::to_string(i) + "_23");
        EXPECT_EQ(produced[i]["k_23"][1], i * 100 + 23);
    }
    produced.erase(produced.begin(), produced.begin() + 32);
    produced.clear();
}

TEST(SlabPool, EightThreadConcurrentCrossThreadFreeAndReuse) {
    constexpr int kThreads = 8;
    constexpr int kSlots = 64;

    struct SharedQueue {
        std::mutex mu;
        std::vector<jk> items;
    };
    std::array<SharedQueue, kThreads> queues;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int tid = 0; tid < kThreads; ++tid) {
        workers.emplace_back([tid, &queues]() {
            for (int iter = 0; iter < kSlots; ++iter) {
                jk doc = jk::parse(
                    R"({"id":1,"name":"worker","tags":["a","b","c","d"],"nested":{"ok":true}})");
                doc["tid"] = tid;
                doc["iter"] = iter;
                for (int k = 0; k < 20; ++k) {
                    doc["extra_" + std::to_string(k)] = k;
                }
                const std::string dumped = doc.dump();
                EXPECT_FALSE(dumped.empty());

                // Pass to neighbor thread so allocations are freed on a non-owning thread.
                const int target = (tid + 1) % kThreads;
                {
                    std::lock_guard<std::mutex> lk(queues[target].mu);
                    queues[target].items.push_back(std::move(doc));
                }

                std::vector<jk> local_drain;
                {
                    std::lock_guard<std::mutex> lk(queues[tid].mu);
                    local_drain.swap(queues[tid].items);
                }
                for (const jk& incoming : local_drain) {
                    EXPECT_TRUE(incoming.is_object());
                    EXPECT_EQ(incoming["nested"]["ok"], true);
                }
            }
        });
    }
    for (std::thread& w : workers) {
        w.join();
    }
    for (SharedQueue& q : queues) {
        for (const jk& remaining : q.items) {
            EXPECT_TRUE(remaining.is_object());
        }
        q.items.clear();
    }
}

TEST(SlabPool, StaticDurationJsonSurvivesProcessTeardown) {
    static const jk persistent = []() {
        jk obj = jk::parse(R"({
            "k01": "alpha", "k02": "beta", "k03": "gamma", "k04": "delta",
            "k05": "epsilon", "k06": "zeta", "k07": "eta", "k08": "theta",
            "k09": "iota", "k10": "kappa", "k11": "lambda", "k12": "mu",
            "k13": "nu", "k14": "xi", "k15": "omicron", "k16": "pi",
            "k17": "rho", "k18": [1, 2, 3, {"nested": "yes"}]
        })");
        return obj;
    }();

    EXPECT_EQ(persistent["k01"], "alpha");
    EXPECT_EQ(persistent["k17"], "rho");
    EXPECT_EQ(persistent["k18"][3]["nested"], "yes");
}

} // namespace
