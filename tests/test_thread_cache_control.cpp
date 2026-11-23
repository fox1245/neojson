// Tests for thread-local cache trimming and configurable retention budgets
// (`trim_thread_caches`, `get_thread_cache_limits`, `set_thread_cache_limits`).

#include <gtest/gtest.h>

#include "neojson/json.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define NEOJSON_TEST_HAS_SANITIZER_HEAP 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define NEOJSON_TEST_HAS_SANITIZER_HEAP 1
#endif
#endif

#if defined(NEOJSON_TEST_HAS_SANITIZER_HEAP)
extern "C" std::size_t __sanitizer_get_current_allocated_bytes();
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
#include <malloc.h>
#define NEOJSON_TEST_HAS_MALLINFO2 1
#endif

namespace {

using jk = neojson::json;

struct LimitsRestorer {
    neojson::thread_cache_limits saved;
    LimitsRestorer() noexcept : saved(neojson::get_thread_cache_limits()) {}
    ~LimitsRestorer() noexcept {
        neojson::set_thread_cache_limits(saved);
        neojson::trim_thread_caches();
    }
    LimitsRestorer(const LimitsRestorer&) = delete;
    LimitsRestorer& operator=(const LimitsRestorer&) = delete;
};

std::optional<std::size_t> maybe_heap_in_use() noexcept {
#if defined(NEOJSON_TEST_HAS_SANITIZER_HEAP)
    return __sanitizer_get_current_allocated_bytes();
#elif defined(NEOJSON_TEST_HAS_MALLINFO2)
    const struct mallinfo2 info = mallinfo2();
    return info.uordblks + info.hblkhd;
#else
    return std::nullopt;
#endif
}

std::string make_large_payload(const std::size_t approx_bytes, const int tag) {
    std::string body(approx_bytes, 'x');
    jk doc = {
        {"jsonrpc", "2.0"},
        {"id", tag},
        {"method", "resources/subscribe"},
        {"params", {{"uri", "file:///" + body}, {"items", jk::array({1, 2, 3, 4})}}},
    };
    return doc.dump();
}

TEST(ThreadCacheControl, DefaultLimitsMatchDocumentedValues) {
    const auto limits = neojson::get_thread_cache_limits();
    EXPECT_EQ(limits.large_cache_max_entry_bytes,
              neojson::thread_cache_limits::default_large_cache_max_entry_bytes);
    EXPECT_EQ(limits.large_cache_max_total_bytes,
              neojson::thread_cache_limits::default_large_cache_max_total_bytes);
    EXPECT_EQ(limits.max_cached_empty_slabs,
              neojson::thread_cache_limits::default_max_cached_empty_slabs);
    EXPECT_EQ(limits.enable_string_pool,
              neojson::thread_cache_limits::default_enable_string_pool);
    EXPECT_EQ(limits.large_cache_max_entry_bytes, std::size_t{16u} * 1024u * 1024u);
    EXPECT_EQ(limits.large_cache_max_total_bytes, std::size_t{24u} * 1024u * 1024u);
    EXPECT_EQ(limits.max_cached_empty_slabs, 64u);
    EXPECT_TRUE(limits.enable_string_pool);
}

TEST(ThreadCacheControl, TrimOnUntouchedThreadIsSafeNoOp) {
    std::thread worker([]() {
        neojson::trim_thread_caches();
        neojson::set_thread_cache_limits(neojson::get_thread_cache_limits());
        neojson::trim_thread_caches();

        // Thread-local caches must remain usable after trimming before first use.
        jk v = jk::parse(R"({"ok":true,"arr":[1,2,3]})");
        EXPECT_EQ(v["ok"], true);
        EXPECT_EQ(v.dump(), R"({"ok":true,"arr":[1,2,3]})");
    });
    worker.join();
}

TEST(ThreadCacheControl, TrimReleasesLargeParseAndDumpRetention) {
    LimitsRestorer restore;
    static constexpr int kWorkers = 8;
    const std::string payload = make_large_payload(512u * 1024u, 1);

    std::mutex mu;
    std::condition_variable cv;
    int phase = 0; // 0=start, 1=warmed, 2=worked, 3=trimmed, 4=reused, 5=exit
    int arrived = 0;

    auto wait_for_phase = [&](const int next_phase) {
        std::unique_lock<std::mutex> lk(mu);
        ++arrived;
        if (arrived == kWorkers) {
            cv.notify_all();
        }
        cv.wait(lk, [&]() { return phase >= next_phase; });
    };

    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (int t = 0; t < kWorkers; ++t) {
        workers.emplace_back([&, t]() {
            // Warm small allocations on the thread first so baseline thread overhead exists.
            {
                jk small = jk::parse(R"({"warmup":1})");
                EXPECT_EQ(small["warmup"], 1);
            }
            wait_for_phase(2);

            // Parse and dump a >512 KiB JSON payload while staying alive.
            for (int r = 0; r < 2; ++r) {
                jk parsed = jk::parse(payload);
                const std::string dumped = parsed.dump();
                EXPECT_EQ(parsed["id"], 1);
                EXPECT_GT(dumped.size(), 512u * 1024u);
            }
            wait_for_phase(3);

            neojson::trim_thread_caches();
            wait_for_phase(4);

            // Re-use caches after trim across another cycle, then trim again.
            {
                jk parsed = jk::parse(payload);
                parsed["tag"] = t;
                const std::string dumped = parsed.dump();
                EXPECT_NE(dumped.find("\"tag\":" + std::to_string(t)), std::string::npos);
            }
            neojson::trim_thread_caches();
            wait_for_phase(5);
        });
    }

    auto advance = [&](const int next_phase) {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&]() { return arrived == kWorkers; });
        arrived = 0;
        phase = next_phase;
        cv.notify_all();
    };

    // Wait for warmup completion.
    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&]() { return arrived == kWorkers; });
    }
    const std::optional<std::size_t> base = maybe_heap_in_use();
    {
        std::lock_guard<std::mutex> lk(mu);
        arrived = 0;
        phase = 2;
        cv.notify_all();
    }

    // Measure while caches hold freed large parse/dump buffers, then let workers trim.
    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&]() { return arrived == kWorkers; });
    }
    const std::optional<std::size_t> cached = maybe_heap_in_use();
    {
        std::lock_guard<std::mutex> lk(mu);
        arrived = 0;
        phase = 3;
        cv.notify_all();
    }

    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&]() { return arrived == kWorkers; });
    }
    const std::optional<std::size_t> trimmed = maybe_heap_in_use();
    {
        std::lock_guard<std::mutex> lk(mu);
        arrived = 0;
        phase = 4;
        cv.notify_all();
    }

    advance(5);
    for (std::thread& w : workers) {
        w.join();
    }

    if (base && cached && trimmed) {
        const std::size_t cached_delta = (*cached > *base) ? (*cached - *base) : 0u;
        const std::size_t trimmed_delta = (*trimmed > *base) ? (*trimmed - *base) : 0u;
        // 8 workers * (~1 MiB val_hdr + ~512 KiB native-parser str_pool + DumpBuffer)
        // retains > 4 MiB untrimmed; trimming must bring retention well below 1.5 MiB.
        EXPECT_GT(cached_delta, 4u * 1024u * 1024u);
        EXPECT_LT(trimmed_delta, 1536u * 1024u);
    }
}

TEST(ThreadCacheControl, LiveValuesSurviveTrimThreadCaches) {
    LimitsRestorer restore;

    // Cover lazy parsed DOM, materialized subtree, SmallObject4, SmallArray2, CompactObject,
    // SharedString, and direct slab allocations.
    const std::string payload = make_large_payload(256u * 1024u, 42);
    jk lazy_doc = jk::parse(payload);
    jk materialized_doc = lazy_doc;
    materialized_doc.materialize();

    jk small_obj = jk::object({{"a", 1}, {"b", "two"}});
    jk small_arr = jk::array({10, 20});
    jk compact = jk::object();
    for (int i = 0; i < 18; ++i) {
        compact["k_" + std::to_string(i)] = "val_" + std::to_string(i);
    }
    jk shared_str = std::string(128, 'z');
    jk shared_str_copy = shared_str;

    neojson::trim_thread_caches();

    // Every live value must remain intact, readable, and mutable after trimming caches.
    EXPECT_EQ(lazy_doc["id"], 42);
    EXPECT_EQ(lazy_doc["params"]["items"][2], 3);
    EXPECT_EQ(materialized_doc["id"], 42);
    EXPECT_EQ(small_obj["b"], "two");
    EXPECT_EQ(small_arr[1], 20);
    EXPECT_EQ(compact["k_17"], "val_17");
    EXPECT_EQ(shared_str, std::string(128, 'z'));
    EXPECT_EQ(shared_str_copy, std::string(128, 'z'));

    lazy_doc["added"] = "after_trim";
    small_obj["c"] = true;
    small_arr.push_back(30);
    compact.erase("k_0");
    shared_str_copy.get_ref<jk::string_t&>()[0] = 'A';

    EXPECT_EQ(lazy_doc["added"], "after_trim");
    EXPECT_EQ(small_obj["c"], true);
    EXPECT_EQ(small_arr.size(), 3u);
    EXPECT_FALSE(compact.contains("k_0"));
    EXPECT_EQ(shared_str, std::string(128, 'z'));
    EXPECT_EQ(shared_str_copy.get_ref<const jk::string_t&>()[0], 'A');

    // Destroy live values and trim again to verify deferred doc credits and slabs flush cleanly.
    lazy_doc = nullptr;
    materialized_doc = nullptr;
    small_obj = nullptr;
    small_arr = nullptr;
    compact = nullptr;
    shared_str = nullptr;
    shared_str_copy = nullptr;
    neojson::trim_thread_caches();
}

TEST(ThreadCacheControl, ZeroLimitsPreventLargeRetention) {
    LimitsRestorer restore;
    neojson::thread_cache_limits zero_limits;
    zero_limits.large_cache_max_entry_bytes = 0u;
    zero_limits.large_cache_max_total_bytes = 0u;
    zero_limits.max_cached_empty_slabs = 0u;
    zero_limits.enable_string_pool = false;
    neojson::set_thread_cache_limits(zero_limits);

    static constexpr int kWorkers = 8;
    const std::string payload = make_large_payload(512u * 1024u, 7);

    std::mutex mu;
    std::condition_variable cv;
    int phase = 0;
    int arrived = 0;

    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (int t = 0; t < kWorkers; ++t) {
        workers.emplace_back([&, t]() {
            // Warm up steady-state small per-thread allocator classes and scratch first so the
            // delta below isolates whether freed large parse/dump buffers are retained.
            {
                jk warm = jk::parse(R"({"id":0,"worker":-1,"s":"warmup_string_over_15"})");
                warm["worker"] = t;
                const std::string d = warm.dump();
                EXPECT_FALSE(d.empty());
            }
            {
                std::unique_lock<std::mutex> lk(mu);
                ++arrived;
                if (arrived == kWorkers) {
                    cv.notify_all();
                }
                cv.wait(lk, [&]() { return phase >= 1; });
            }

            for (int r = 0; r < 2; ++r) {
                jk parsed = jk::parse(payload);
                parsed["worker"] = t;
                const std::string dumped = parsed.dump();
                EXPECT_EQ(parsed["id"], 7);
                EXPECT_GT(dumped.size(), 512u * 1024u);
            }

            {
                std::unique_lock<std::mutex> lk(mu);
                ++arrived;
                if (arrived == kWorkers) {
                    cv.notify_all();
                }
                cv.wait(lk, [&]() { return phase >= 2; });
            }
        });
    }

    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&]() { return arrived == kWorkers; });
    }
    const std::optional<std::size_t> before = maybe_heap_in_use();
    {
        std::lock_guard<std::mutex> lk(mu);
        arrived = 0;
        phase = 1;
        cv.notify_all();
    }

    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&]() { return arrived == kWorkers; });
    }
    const std::optional<std::size_t> after = maybe_heap_in_use();
    {
        std::lock_guard<std::mutex> lk(mu);
        arrived = 0;
        phase = 2;
        cv.notify_all();
    }

    for (std::thread& w : workers) {
        w.join();
    }

    if (before && after) {
        const std::size_t retained = (*after > *before) ? (*after - *before) : 0u;
        // Without calling trim_thread_caches(), zero limits must keep large-buffer delta bounded.
        EXPECT_LT(retained, 1024u * 1024u);
    }
}

TEST(ThreadCacheControl, OverflowAndMismatchedEntryTotalLimits) {
    LimitsRestorer restore;

    // 1. max_entry > max_total must cap effective cached entry at max_total without crash/leak.
    neojson::thread_cache_limits tight_total;
    tight_total.large_cache_max_entry_bytes = 16u * 1024u * 1024u;
    tight_total.large_cache_max_total_bytes = 64u * 1024u;
    tight_total.max_cached_empty_slabs = 2u;
    tight_total.enable_string_pool = true;
    neojson::set_thread_cache_limits(tight_total);

    const std::string big_payload = make_large_payload(256u * 1024u, 9);
    {
        jk doc = jk::parse(big_payload);
        EXPECT_EQ(doc["id"], 9);
        EXPECT_GT(doc.dump().size(), 256u * 1024u);
    }

    // 2. SIZE_MAX entry/total budgets must not wrap around unsigned subtraction in eviction checks.
    neojson::thread_cache_limits huge;
    huge.large_cache_max_entry_bytes = (std::numeric_limits<std::size_t>::max)();
    huge.large_cache_max_total_bytes = (std::numeric_limits<std::size_t>::max)();
    huge.max_cached_empty_slabs = 8u;
    huge.enable_string_pool = false;
    neojson::set_thread_cache_limits(huge);

    {
        jk doc = jk::parse(big_payload);
        EXPECT_EQ(doc["id"], 9);
    }

    // Shrinking limits afterward must immediately trim on the calling thread without trim_thread_caches().
    const std::optional<std::size_t> before_shrink = maybe_heap_in_use();
    neojson::thread_cache_limits shrunk;
    shrunk.large_cache_max_entry_bytes = 1024u;
    shrunk.large_cache_max_total_bytes = 1024u;
    shrunk.max_cached_empty_slabs = 0u;
    shrunk.enable_string_pool = false;
    neojson::set_thread_cache_limits(shrunk);
    const std::optional<std::size_t> after_shrink = maybe_heap_in_use();
    if (before_shrink && after_shrink) {
        EXPECT_LT(*after_shrink + 128u * 1024u, *before_shrink);
    }
    neojson::trim_thread_caches();
}

TEST(ThreadCacheControl, ConstArenaMatStrPoolDupProbeDocCreditAndEmptySlabsTrim) {
    LimitsRestorer restore;

    // Build a >16-key JSON object with >=48-byte string values plus a large trailing string so
    // const access and dump() populate ArenaMatTLS, DupProbeTable, ThreadStrPool, and ThreadDocCredit.
    jk src = jk::object();
    for (int i = 0; i < 32; ++i) {
        src["key_" + std::to_string(i)] =
            std::string(64u, static_cast<char>('a' + (i % 26))) + "_" + std::to_string(i);
    }
    src["large_blob"] = std::string(512u * 1024u, 'm');
    const std::string payload = src.dump();
    src = nullptr;
    neojson::trim_thread_caches();

    const std::optional<std::size_t> baseline = maybe_heap_in_use();
    {
        std::vector<jk> copies;
        copies.reserve(40);
        jk parsed = jk::parse(payload);
        // Trigger DupProbeTable (>16 keys) via dump() and ArenaMatTLS + SharedStrBlock via const lookup.
        EXPECT_EQ(parsed.dump().size(), payload.size());
        const jk& cdoc = parsed;
        EXPECT_EQ(cdoc.at("key_0").get_ref<const jk::string_t&>().size(), 66u);
        EXPECT_EQ(cdoc.at("large_blob").get_ref<const jk::string_t&>().size(), 512u * 1024u);

        // Batch >32 SharedYyDoc refcount credits into t_doc_credit, then survive a live trim.
        for (int i = 0; i < 40; ++i) {
            copies.push_back(parsed);
        }
        neojson::trim_thread_caches();
        EXPECT_EQ(copies.back().at("key_31"),
                  std::string(64u, static_cast<char>('a' + (31 % 26))) + "_31");
    }
    // At this point SharedYyDoc may still be held solely by batched t_doc_credit, plus
    // ThreadStrPool / ArenaMatTLS / DupProbeTable / ThreadLargeCache.
    neojson::trim_thread_caches();
    const std::optional<std::size_t> after_trim = maybe_heap_in_use();
    if (baseline && after_trim) {
        const std::size_t kept = (*after_trim > *baseline) ? (*after_trim - *baseline) : 0u;
        EXPECT_LT(kept, 512u * 1024u);
    }

    // Subsequent const parse/access must re-initialize TLS caches cleanly.
    {
        const jk reparsed = jk::parse(payload);
        EXPECT_EQ(reparsed.at("key_15"),
                  std::string(64u, static_cast<char>('a' + (15 % 26))) + "_15");
    }
    neojson::trim_thread_caches();

    // Exercise max_cached_empty_slabs enforcement directly via set_thread_cache_limits().
    // Allocate >1 MiB of pooled SmallArray2/SharedString objects so multiple 64 KiB empty
    // slabs accumulate in ThreadPool::empty_slabs when freed.
    {
        std::vector<jk> slabs;
        slabs.reserve(25000);
        for (int i = 0; i < 25000; ++i) {
            slabs.push_back(jk::array({i, i + 1, "slab_item_payload_" + std::to_string(i)}));
        }
        slabs.clear();
    }
    const std::optional<std::size_t> before_slab_cap = maybe_heap_in_use();
    neojson::thread_cache_limits cap_slabs = neojson::get_thread_cache_limits();
    cap_slabs.max_cached_empty_slabs = 1u;
    neojson::set_thread_cache_limits(cap_slabs);
    const std::optional<std::size_t> after_slab_cap = maybe_heap_in_use();
    if (before_slab_cap && after_slab_cap) {
        EXPECT_LT(*after_slab_cap + 256u * 1024u, *before_slab_cap);
    }
    neojson::trim_thread_caches();
}

TEST(ThreadCacheControl, MultiThreadedConcurrentTrimAndCrossThreadHandoff) {
    LimitsRestorer restore;
    static constexpr int kThreads = 8;
    static constexpr int kIters = 40;

    struct Queue {
        std::mutex mu;
        std::vector<jk> items;
    };
    std::array<Queue, kThreads> queues;
    std::atomic<int> toggle{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int tid = 0; tid < kThreads; ++tid) {
        workers.emplace_back([tid, &queues, &toggle]() {
            for (int iter = 0; iter < kIters; ++iter) {
                if ((tid == 0 || tid == 1) && (iter % 10 == 0)) {
                    const int mode = toggle.fetch_add(1, std::memory_order_relaxed) & 1;
                    neojson::thread_cache_limits l;
                    if (mode == 0) {
                        l.large_cache_max_entry_bytes = 128u * 1024u;
                        l.large_cache_max_total_bytes = 512u * 1024u;
                        l.max_cached_empty_slabs = 2u;
                        l.enable_string_pool = false;
                    }
                    neojson::set_thread_cache_limits(l);
                } else if (iter % 7 == 0) {
                    const auto cur = neojson::get_thread_cache_limits();
                    EXPECT_GE(cur.max_cached_empty_slabs, 0u);
                }

                // Exercise concurrent const inspection of parsed SharedYyDoc alongside mutable writes.
                const jk ro_doc = jk::parse(
                    R"({"k0":"long_string_payload_over_sixteen_bytes","k1":1,"k2":2,"k3":3,"k4":4,"k5":5,"k6":6,"k7":7,"k8":8,"k9":9,"k10":10,"k11":11,"k12":12,"k13":13,"k14":14,"k15":15,"k16":16,"k17":17})");
                EXPECT_EQ(ro_doc.at("k17"), 17);
                EXPECT_EQ(ro_doc.at("k0").get_ref<const jk::string_t&>().size(), 38u);

                jk doc = jk::parse(
                    R"({"id":1,"name":"worker","tags":["alpha","beta","gamma"],"nested":{"ok":true}})");
                doc["tid"] = tid;
                doc["iter"] = iter;
                for (int k = 0; k < 20; ++k) {
                    doc["extra_" + std::to_string(k)] = std::string(48, static_cast<char>('a' + (k % 26)));
                }
                const std::string dumped = doc.dump();
                EXPECT_FALSE(dumped.empty());

                const int next_tid = (tid + 1) % kThreads;
                {
                    std::lock_guard<std::mutex> lk(queues[next_tid].mu);
                    queues[next_tid].items.push_back(std::move(doc));
                }

                std::vector<jk> inbox;
                {
                    std::lock_guard<std::mutex> lk(queues[tid].mu);
                    inbox.swap(queues[tid].items);
                }
                for (const jk& msg : inbox) {
                    EXPECT_TRUE(msg.is_object());
                    EXPECT_EQ(msg["nested"]["ok"], true);
                }
                inbox.clear();

                if (iter % 4 == 0) {
                    neojson::trim_thread_caches();
                }
            }
            neojson::trim_thread_caches();
        });
    }

    for (std::thread& w : workers) {
        w.join();
    }
    for (Queue& q : queues) {
        q.items.clear();
    }
    neojson::trim_thread_caches();
}

} // namespace
