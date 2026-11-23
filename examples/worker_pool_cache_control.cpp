// Bounding steady-state worker-pool RSS with thread_cache_limits and RAII trim_thread_caches().

#include "neojson/json.hpp"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using json = neojson::json;

void require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(msg);
    }
}

struct ScopedCacheLimits {
    explicit ScopedCacheLimits(const neojson::thread_cache_limits& next)
        : prev_(neojson::get_thread_cache_limits()) {
        neojson::set_thread_cache_limits(next);
    }

    ~ScopedCacheLimits() {
        neojson::set_thread_cache_limits(prev_);
    }

    ScopedCacheLimits(const ScopedCacheLimits&) = delete;
    ScopedCacheLimits& operator=(const ScopedCacheLimits&) = delete;

private:
    neojson::thread_cache_limits prev_;
};

struct LargePayloadTrimGuard {
    explicit LargePayloadTrimGuard(bool should_trim) noexcept : should_trim_(should_trim) {}

    ~LargePayloadTrimGuard() {
        if (should_trim_) {
            neojson::trim_thread_caches();
        }
    }

    LargePayloadTrimGuard(const LargePayloadTrimGuard&) = delete;
    LargePayloadTrimGuard& operator=(const LargePayloadTrimGuard&) = delete;

private:
    bool should_trim_;
};

void run() {
    constexpr std::size_t kWorkers = 4;
    constexpr std::size_t kTasksPerWorker = 20;
    constexpr std::size_t kTrimThreshold = 64u * 1024u;
    constexpr std::size_t kLargeBlobBytes = 256u * 1024u;

    // Tighten process-wide thread-local cache budgets while preserving small-message caching.
    neojson::thread_cache_limits policy = neojson::get_thread_cache_limits();
    policy.large_cache_max_entry_bytes = 256u * 1024u;
    policy.large_cache_max_total_bytes = 512u * 1024u;
    policy.max_cached_empty_slabs = 4u;
    policy.enable_string_pool = true;
    const ScopedCacheLimits scoped_limits(policy);

    std::vector<std::vector<json>> worker_summaries(kWorkers);
    struct ThreadJoiner {
        std::vector<std::thread> threads;
        ~ThreadJoiner() {
            for (std::thread& t : threads) {
                if (t.joinable()) {
                    t.join();
                }
            }
        }
    } pool;
    pool.threads.reserve(kWorkers);

    for (std::size_t w = 0; w < kWorkers; ++w) {
        pool.threads.emplace_back([&, w]() {
            worker_summaries[w].reserve(kTasksPerWorker);
            for (std::size_t step = 0; step < kTasksPerWorker; ++step) {
                const bool is_large = (step % 5u == 0u);
                const std::string body = is_large
                    ? std::string(kLargeBlobBytes, static_cast<char>('a' + w))
                    : std::string("ok");
                const std::string payload =
                    std::string(R"({"summary":{"worker":)") + std::to_string(w) +
                    R"(,"step":)" + std::to_string(step) + R"(,"large":)" +
                    (is_large ? "true" : "false") + R"(},"blob":")" + body + R"("})";

                // Trim idle thread caches on scope exit whenever a message crosses kTrimThreshold.
                const LargePayloadTrimGuard trim_guard(payload.size() >= kTrimThreshold);
                const json parsed = json::parse(payload);

                // Detach the small summary before parsed drops and trim_thread_caches() runs.
                json summary = parsed.at("summary");
                summary.materialize();
                worker_summaries[w].push_back(std::move(summary));
            }
            neojson::trim_thread_caches();
        });
    }

    for (std::thread& t : pool.threads) {
        t.join();
    }

    // Verify trim_thread_caches() kept every live materialized summary intact across all workers.
    for (std::size_t w = 0; w < kWorkers; ++w) {
        require(worker_summaries[w].size() == kTasksPerWorker, "worker summary count matches");
        for (std::size_t step = 0; step < kTasksPerWorker; ++step) {
            const json& s = std::as_const(worker_summaries[w][step]);
            require(s.at("worker").get<std::size_t>() == w, "worker id matches");
            require(s.at("step").get<std::size_t>() == step, "step index matches");
            require(s.at("large").get<bool>() == (step % 5u == 0u), "large flag matches");
        }
    }

    std::cout << "worker_pool_cache_control: ok\n";
}

}  // namespace

int main() {
    try {
        run();
    } catch (const std::exception& err) {
        std::cerr << "worker_pool_cache_control failed: " << err.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
