#include "neojson/json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <random>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define NEOJSON_X86_SIMD 1
#elif defined(__SSE2__)
#include <emmintrin.h>
#define NEOJSON_X86_SIMD 0
#else
#define NEOJSON_X86_SIMD 0
#endif

#if defined(_WIN32)
#include <malloc.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define NEOJSON_HAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NEOJSON_HAS_ASAN 1
#endif
#endif

#if defined(NEOJSON_HAS_ASAN)
#include <sanitizer/asan_interface.h>
#endif

#include "yyjson.h"

namespace neojson {
namespace detail {

namespace {

std::uint64_t hash_seed_init() noexcept {
    std::uint64_t s = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    try {
        std::random_device rd;
        s ^= (static_cast<std::uint64_t>(rd()) << 32u) | static_cast<std::uint64_t>(rd());
    } catch (...) {
    }
    s ^= reinterpret_cast<std::uintptr_t>(&hash_seed_init);
    return fmix64(s) | 1ULL;
}

} // namespace

std::atomic<std::uint64_t> g_hash_seed{0};

std::uint64_t hash_seed_slow() noexcept {
    const std::uint64_t existing = g_hash_seed.load(std::memory_order_acquire);
    if (existing != 0u) {
        return existing;
    }
    const std::uint64_t candidate = hash_seed_init();
    std::uint64_t expected = 0u;
    if (g_hash_seed.compare_exchange_strong(
            expected, candidate, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return candidate;
    }
    return expected;
}

std::uint64_t hash_seed() noexcept {
    const std::uint64_t s = g_hash_seed.load(std::memory_order_acquire);
    return s != 0u ? s : hash_seed_slow();
}

[[maybe_unused]] const bool g_hash_seed_eager_init = (hash_seed_slow(), true);

#if NEOJSON_X86_SIMD
inline bool has_avx2() noexcept {
    static const bool ok = __builtin_cpu_supports("avx2");
    return ok;
}
#endif

std::uint64_t hash_bytes_long(const char* data, const std::size_t len,
                              std::uint64_t h) noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(data);
    std::size_t n = len;
    while (n >= 8) {
        std::uint64_t w = 0;
        std::memcpy(&w, p, 8);
        h ^= w * 0xbf58476d1ce4e5b9ULL;
        h = rotl64(h, 31u) * 0x9e3779b97f4a7c15ULL;
        p += 8;
        n -= 8;
    }
    if (n > 0) {
        std::uint64_t tail = 0;
        std::memcpy(&tail, reinterpret_cast<const unsigned char*>(data) + len - 8, 8);
        h ^= tail * 0x94d049bb133111ebULL;
    }
    return fmix64(h);
}

[[noreturn]] void throw_invalid_iterator(const int id, const char* msg) {
    throw invalid_iterator::create(id, msg);
}

[[noreturn]] void throw_type_error(const int id, const char* prefix, const char* type_name) {
    throw type_error::create(id, std::string(prefix) + type_name);
}

namespace {

inline void flush_doc_credit_for_teardown() noexcept;

inline constexpr std::uint32_t kNumSizeClasses = static_cast<std::uint32_t>(kTCacheClasses);
inline constexpr std::size_t kMaxPooledSize = kTCacheMaxBytes;
inline constexpr std::uint32_t kMaxCachedEmptySlabs = 64u;
inline constexpr std::uint32_t kReclaimScanLimit = 8u;

inline std::atomic<std::uint64_t> g_next_pool_id{0u};

struct ThreadPool;

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
    _mm_pause();
#else
    __builtin_ia32_pause();
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER)
    __yield();
#else
    __asm__ volatile("yield" ::: "memory");
#endif
#else
    std::this_thread::yield();
#endif
}

struct SlabHeader {
    SlabHeaderBase hdr;
    SlabHeader* prev;
    SlabHeader* next;
    std::uint32_t slot_count;
    std::uint32_t bump_off;

    alignas(64) std::atomic<std::uint32_t> remote_lock;
    SlabFreeNode* remote_free;
    std::atomic<std::uint32_t> remote_count;
    std::uint32_t orphan_live;

    SlabHeader(const std::uint32_t cls, const std::uint32_t ssz, const std::uint32_t scnt,
               const std::uint64_t oid, const std::uint32_t bump, const std::uint32_t used,
               const std::uint32_t orphan) noexcept
        : hdr{nullptr, used, cls, ssz, 0u, 0u, {0u, 0u}, oid},
          prev(nullptr),
          next(nullptr),
          slot_count(scnt),
          bump_off(bump),
          remote_lock(0u),
          remote_free(nullptr),
          remote_count(0u),
          orphan_live(orphan) {}

    void lock_remote() noexcept {
        while (remote_lock.exchange(1u, std::memory_order_acquire) != 0u) {
            while (remote_lock.load(std::memory_order_relaxed) != 0u) {
                cpu_relax();
            }
        }
    }

    void unlock_remote() noexcept {
        remote_lock.store(0u, std::memory_order_release);
    }
};

static_assert(std::is_standard_layout<SlabHeader>::value,
              "SlabHeader must remain standard-layout for base pointer interconvertibility");
static_assert(offsetof(SlabHeader, hdr) == 0u,
              "SlabHeaderBase must be at offset 0 of SlabHeader");

inline constexpr std::size_t kSlabHeaderBytes = (sizeof(SlabHeader) + 63u) & ~std::size_t{63};
static_assert(kSlabHeaderBytes % 64u == 0u, "slab header must preserve slot alignment");
static_assert((kSlabSize - kSlabHeaderBytes) / kMaxPooledSize >= 15u,
              "largest pooled class must fit at least 15 slots per 64 KiB slab");

inline std::uint32_t class_for_size(const std::size_t n) noexcept {
    assert(n <= kMaxPooledSize);
    return tcache_class_for_size(n);
}

inline void* alloc_aligned_64k() {
#if defined(_WIN32)
    void* p = _aligned_malloc(kSlabSize, kSlabSize);
#else
    void* p = nullptr;
    if (::posix_memalign(&p, kSlabSize, kSlabSize) != 0) {
        p = nullptr;
    }
#endif
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

inline void free_aligned_64k(void* slab) noexcept {
    if (slab == nullptr) {
        return;
    }
#if defined(NEOJSON_HAS_ASAN)
    ASAN_UNPOISON_MEMORY_REGION(slab, kSlabSize);
#endif
#if defined(_WIN32)
    _aligned_free(slab);
#else
    std::free(slab);
#endif
}

inline void poison_region(void* p, const std::size_t bytes) noexcept {
#if defined(NEOJSON_HAS_ASAN)
    if (bytes > 0u) {
        ASAN_POISON_MEMORY_REGION(p, bytes);
    }
#else
    (void)p;
    (void)bytes;
#endif
}

inline void poison_slot_tail(void* p, const std::size_t slot_size) noexcept {
#if defined(NEOJSON_HAS_ASAN)
    if (slot_size > sizeof(SlabFreeNode)) {
        ASAN_POISON_MEMORY_REGION(static_cast<char*>(p) + sizeof(SlabFreeNode),
                                  slot_size - sizeof(SlabFreeNode));
    }
#else
    (void)p;
    (void)slot_size;
#endif
}

inline void unpoison_slot(void* p, const std::size_t slot_size) noexcept {
#if defined(NEOJSON_HAS_ASAN)
    ASAN_UNPOISON_MEMORY_REGION(p, slot_size);
#else
    (void)p;
    (void)slot_size;
#endif
}

inline SlabHeader* slab_from_ptr(void* p) noexcept {
    return reinterpret_cast<SlabHeader*>(reinterpret_cast<std::uintptr_t>(p) & kSlabMask);
}

inline void list_push(SlabHeader*& list_head, SlabHeader* s) noexcept {
    s->prev = nullptr;
    s->next = list_head;
    if (list_head != nullptr) {
        list_head->prev = s;
    }
    list_head = s;
}

inline void list_erase(SlabHeader*& list_head, SlabHeader* s) noexcept {
    if (s->prev != nullptr) {
        s->prev->next = s->next;
    } else {
        list_head = s->next;
    }
    if (s->next != nullptr) {
        s->next->prev = s->prev;
    }
    s->prev = nullptr;
    s->next = nullptr;
}

inline void queue_push_back(SlabHeader*& qhead, SlabHeader*& qtail, SlabHeader* s) noexcept {
    s->next = nullptr;
    s->prev = qtail;
    if (qtail != nullptr) {
        qtail->next = s;
    } else {
        qhead = s;
    }
    qtail = s;
}

inline void queue_erase(SlabHeader*& qhead, SlabHeader*& qtail, SlabHeader* s) noexcept {
    if (s->prev != nullptr) {
        s->prev->next = s->next;
    } else {
        qhead = s->next;
    }
    if (s->next != nullptr) {
        s->next->prev = s->prev;
    } else {
        qtail = s->prev;
    }
    s->prev = nullptr;
    s->next = nullptr;
}

inline void drain_remote(SlabHeader* s) noexcept {
    if (s->remote_count.load(std::memory_order_acquire) == 0u) {
        return;
    }
    s->lock_remote();
    SlabFreeNode* drained = s->remote_free;
    s->remote_free = nullptr;
    const std::uint32_t count = s->remote_count.load(std::memory_order_relaxed);
    s->remote_count.store(0u, std::memory_order_relaxed);
    s->unlock_remote();

    if (drained != nullptr) {
        SlabFreeNode* tail = drained;
        while (tail->next != nullptr) {
            tail = tail->next;
        }
        tail->next = s->hdr.local_free;
        s->hdr.local_free = drained;
        assert(s->hdr.local_used >= count);
        s->hdr.local_used -= count;
    }
}

struct SlabBin {
    SlabHeader* head = nullptr;
    SlabHeader* full_head = nullptr;
    SlabHeader* full_tail = nullptr;
};

struct ThreadPool {
    std::uint64_t owner_id = 0u;
    std::array<SlabBin, kNumSizeClasses> bins{};
    SlabHeader* empty_slabs = nullptr;
    std::uint32_t empty_count = 0u;

    ThreadPool() noexcept
        : owner_id(g_next_pool_id.fetch_add(1u, std::memory_order_relaxed) + 1u) {}

    void recycle_empty_slab(SlabHeader* s) noexcept {
        if (empty_count < kMaxCachedEmptySlabs) {
            s->next = empty_slabs;
            empty_slabs = s;
            ++empty_count;
        } else {
            s->~SlabHeader();
            free_aligned_64k(s);
        }
    }

    void teardown() noexcept {
        while (empty_slabs != nullptr) {
            SlabHeader* s = empty_slabs;
            empty_slabs = s->next;
            s->~SlabHeader();
            free_aligned_64k(s);
        }
        empty_count = 0u;

        auto release_list = [](SlabHeader* cur) noexcept {
            while (cur != nullptr) {
                SlabHeader* next = cur->next;
                cur->lock_remote();
                const std::uint32_t rc = cur->remote_count.load(std::memory_order_relaxed);
                assert(cur->hdr.local_used >= rc);
                const std::uint32_t live = cur->hdr.local_used - rc;
                cur->remote_free = nullptr;
                cur->remote_count.store(0u, std::memory_order_relaxed);
                cur->orphan_live = live;
                cur->hdr.owner_id.store(0u, std::memory_order_release);
                cur->unlock_remote();
                if (live == 0u) {
                    cur->~SlabHeader();
                    free_aligned_64k(cur);
                }
                cur = next;
            }
        };

        for (SlabBin& bin : bins) {
            release_list(bin.head);
            release_list(bin.full_head);
            bin.head = nullptr;
            bin.full_head = nullptr;
            bin.full_tail = nullptr;
        }
    }

    SlabHeader* alloc_new_slab(const std::uint32_t cls) {
        const std::uint32_t ssz = kTCacheSizes[cls];
        const std::uint32_t scnt =
            static_cast<std::uint32_t>((kSlabSize - kSlabHeaderBytes) / ssz);
        void* slab_mem = nullptr;
        if (empty_slabs != nullptr) {
            SlabHeader* reused = empty_slabs;
            empty_slabs = reused->next;
            --empty_count;
            reused->~SlabHeader();
            slab_mem = reused;
#if defined(NEOJSON_HAS_ASAN)
            ASAN_UNPOISON_MEMORY_REGION(slab_mem, kSlabSize);
#endif
        } else {
            slab_mem = alloc_aligned_64k();
        }
        return ::new (slab_mem) SlabHeader(
            cls, ssz, scnt, owner_id, static_cast<std::uint32_t>(kSlabHeaderBytes), 0u, 0u);
    }

    void* alloc(const std::uint32_t cls) {
        SlabBin& bin = bins[cls];
        while (bin.head != nullptr) {
            SlabHeader* s = bin.head;
            if (SlabFreeNode* node = s->hdr.local_free) {
                s->hdr.local_free = node->next;
                ++s->hdr.local_used;
                unpoison_slot(node, s->hdr.slot_size);
                return node;
            }
            if (s->bump_off + s->hdr.slot_size <= static_cast<std::uint32_t>(kSlabSize)) {
                void* p = reinterpret_cast<char*>(s) + s->bump_off;
                s->bump_off += s->hdr.slot_size;
                ++s->hdr.local_used;
                unpoison_slot(p, s->hdr.slot_size);
                return p;
            }
            drain_remote(s);
            if (SlabFreeNode* node = s->hdr.local_free) {
                s->hdr.local_free = node->next;
                ++s->hdr.local_used;
                unpoison_slot(node, s->hdr.slot_size);
                return node;
            }
            list_erase(bin.head, s);
            s->hdr.in_full = 1u;
            queue_push_back(bin.full_head, bin.full_tail, s);
        }

        for (std::uint32_t scan = 0u; scan < kReclaimScanLimit && bin.full_head != nullptr;
             ++scan) {
            SlabHeader* s = bin.full_head;
            if (s->remote_count.load(std::memory_order_acquire) != 0u) {
                drain_remote(s);
                if (SlabFreeNode* node = s->hdr.local_free) {
                    s->hdr.local_free = node->next;
                    ++s->hdr.local_used;
                    queue_erase(bin.full_head, bin.full_tail, s);
                    s->hdr.in_full = 0u;
                    list_push(bin.head, s);
                    unpoison_slot(node, s->hdr.slot_size);
                    return node;
                }
            }
            if (bin.full_head != bin.full_tail) {
                queue_erase(bin.full_head, bin.full_tail, s);
                queue_push_back(bin.full_head, bin.full_tail, s);
            } else {
                break;
            }
        }

        SlabHeader* s = alloc_new_slab(cls);
        const std::uint32_t ssz = s->hdr.slot_size;
        s->bump_off = static_cast<std::uint32_t>(kSlabHeaderBytes) + ssz;
        s->hdr.local_used = 1u;
        poison_region(reinterpret_cast<char*>(s) + kSlabHeaderBytes + ssz,
                      kSlabSize - kSlabHeaderBytes - ssz);
        list_push(bin.head, s);
        return reinterpret_cast<char*>(s) + kSlabHeaderBytes;
    }
};

struct ThreadPoolGuard {
    bool armed = false;
    ~ThreadPoolGuard();
};

inline thread_local ThreadPool* t_pool = nullptr;
inline thread_local bool t_pool_dead = false;
inline thread_local ThreadPoolGuard t_pool_guard{};

ThreadPoolGuard::~ThreadPoolGuard() {
    armed = false;
    flush_doc_credit_for_teardown();
    t_slab_cache.owner_id = 0u;
    for (std::size_t cls = 0u; cls < kTCacheClasses; ++cls) {
        SlabFastBin& fbin = t_slab_cache.bins[cls];
        if (fbin.active != nullptr) {
            auto* s = reinterpret_cast<SlabHeader*>(fbin.active);
            const std::uint32_t ssz = s->hdr.slot_size;
            if (fbin.bump < fbin.bump_end) {
                const auto unused = static_cast<std::uint32_t>(
                    static_cast<std::size_t>(fbin.bump_end - fbin.bump) / ssz);
                s->bump_off -= unused * ssz;
                assert(s->hdr.local_used >= unused);
                s->hdr.local_used -= unused;
            }
            while (SlabFreeNode* node = fbin.free_head) {
                fbin.free_head = node->next;
                node->next = s->hdr.local_free;
                s->hdr.local_free = node;
                assert(s->hdr.local_used > 0u);
                --s->hdr.local_used;
            }
            s->hdr.is_active_bump = 0u;
        }
        fbin.bump = nullptr;
        fbin.bump_end = nullptr;
        fbin.free_head = nullptr;
        fbin.active = nullptr;
    }
    t_pool_dead = true;
    ThreadPool* tp = t_pool;
    t_pool = nullptr;
    if (tp != nullptr) {
        tp->teardown();
        tp->~ThreadPool();
        std::free(tp);
    }
}

inline void* alloc_standalone_slab(const std::uint32_t cls) {
    const std::uint32_t ssz = kTCacheSizes[cls];
    const std::uint32_t scnt = static_cast<std::uint32_t>((kSlabSize - kSlabHeaderBytes) / ssz);
    void* mem = alloc_aligned_64k();
    ::new (mem) SlabHeader(cls, ssz, scnt, 0u, static_cast<std::uint32_t>(kSlabSize), 0u, 1u);
    poison_region(static_cast<char*>(mem) + kSlabHeaderBytes + ssz,
                  kSlabSize - kSlabHeaderBytes - ssz);
    return static_cast<char*>(mem) + kSlabHeaderBytes;
}

inline void remote_or_orphan_free(SlabHeader* s, void* p) noexcept {
    s->lock_remote();
    if (s->hdr.owner_id.load(std::memory_order_acquire) != 0u) {
        auto* node = ::new (p) SlabFreeNode{s->remote_free};
        poison_slot_tail(p, s->hdr.slot_size);
        s->remote_free = node;
        const std::uint32_t rc = s->remote_count.load(std::memory_order_relaxed);
        s->remote_count.store(rc + 1u, std::memory_order_release);
        s->unlock_remote();
        return;
    }
    assert(s->orphan_live > 0u);
    const std::uint32_t rem = --s->orphan_live;
    if (rem > 0u) {
        poison_region(p, s->hdr.slot_size);
    }
    s->unlock_remote();
    if (rem == 0u) {
        s->~SlabHeader();
        free_aligned_64k(s);
    }
}

} // namespace

void* pool_alloc(const std::size_t n) {
    if (n > kMaxPooledSize) {
        return ::operator new(n);
    }
    const std::uint32_t cls = class_for_size(n);
#if !defined(NEOJSON_HAS_ASAN)
    if (!t_pool_dead) {
        return tcache_alloc_class(cls);
    }
#endif
    ThreadPool* tp = t_pool;
    if (tp == nullptr) {
        if (t_pool_dead) {
            return alloc_standalone_slab(cls);
        }
        t_pool_guard.armed = true;
        void* raw = std::malloc(sizeof(ThreadPool));
        if (raw == nullptr) {
            throw std::bad_alloc();
        }
        tp = ::new (raw) ThreadPool();
        t_pool = tp;
    }
    return tp->alloc(cls);
}

void pool_free(void* p, const std::size_t n) noexcept {
    if (p == nullptr) {
        return;
    }
    if (n > kMaxPooledSize) {
        ::operator delete(p);
        return;
    }
    const std::uint32_t cls = class_for_size(n);
#if !defined(NEOJSON_HAS_ASAN)
    tcache_free_class(cls, p);
#else
    pool_free_slow(p, cls);
#endif
}

void* pool_alloc_refill(const std::uint32_t cls, ThreadSlabCache& tc) {
    if (t_pool_dead) {
        return alloc_standalone_slab(cls);
    }
    ThreadPool* tp = t_pool;
    if (tp == nullptr) {
        t_pool_guard.armed = true;
        void* raw = std::malloc(sizeof(ThreadPool));
        if (raw == nullptr) {
            throw std::bad_alloc();
        }
        tp = ::new (raw) ThreadPool();
        t_pool = tp;
    }
    if (tc.owner_id == 0u) {
        tc.owner_id = tp->owner_id;
    }
    SlabFastBin& bin = tc.bins[cls];
    assert(bin.bump == bin.bump_end && bin.free_head == nullptr);
    SlabBin& sbin = tp->bins[cls];

    auto activate_slab = [&](SlabHeader* s) noexcept -> void* {
        s->hdr.is_active_bump = 1u;
        bin.active = &s->hdr;
        const std::uint32_t ssz = s->hdr.slot_size;
        if (s->hdr.local_used == 0u) {
            s->hdr.local_free = nullptr;
            s->bump_off = static_cast<std::uint32_t>(kSlabHeaderBytes) + s->slot_count * ssz;
            s->hdr.local_used = s->slot_count;
            char* base = reinterpret_cast<char*>(s) + kSlabHeaderBytes;
            bin.bump = base + ssz;
            bin.bump_end = base + s->slot_count * ssz;
            return base;
        }
        if (s->bump_off + ssz <= static_cast<std::uint32_t>(kSlabSize)) {
            const std::uint32_t avail =
                (static_cast<std::uint32_t>(kSlabSize) - s->bump_off) / ssz;
            char* start = reinterpret_cast<char*>(s) + s->bump_off;
            s->bump_off += avail * ssz;
            s->hdr.local_used += avail;
            bin.bump = start + ssz;
            bin.bump_end = start + avail * ssz;
            return start;
        }
        const std::uint32_t ever_bumped =
            (s->bump_off - static_cast<std::uint32_t>(kSlabHeaderBytes)) / ssz;
        SlabFreeNode* first = s->hdr.local_free;
        assert(first != nullptr);
        s->hdr.local_free = nullptr;
        s->hdr.local_used = ever_bumped;
        bin.free_head = first->next;
        return first;
    };

    if (bin.active != nullptr) {
        auto* s = reinterpret_cast<SlabHeader*>(bin.active);
        if (s->hdr.local_used == 0u || s->hdr.local_free != nullptr ||
            s->bump_off + s->hdr.slot_size <= static_cast<std::uint32_t>(kSlabSize)) {
            return activate_slab(s);
        }
        drain_remote(s);
        if (s->hdr.local_free != nullptr) {
            return activate_slab(s);
        }
        s->hdr.is_active_bump = 0u;
        bin.active = nullptr;
        list_erase(sbin.head, s);
        s->hdr.in_full = 1u;
        queue_push_back(sbin.full_head, sbin.full_tail, s);
    }

    for (SlabHeader* s = sbin.head; s != nullptr;) {
        SlabHeader* next_s = s->next;
        if (s->hdr.local_used == 0u || s->hdr.local_free != nullptr ||
            s->bump_off + s->hdr.slot_size <= static_cast<std::uint32_t>(kSlabSize)) {
            return activate_slab(s);
        }
        drain_remote(s);
        if (s->hdr.local_free != nullptr) {
            return activate_slab(s);
        }
        list_erase(sbin.head, s);
        s->hdr.in_full = 1u;
        queue_push_back(sbin.full_head, sbin.full_tail, s);
        s = next_s;
    }

    for (std::uint32_t scan = 0u; scan < kReclaimScanLimit && sbin.full_head != nullptr; ++scan) {
        SlabHeader* s = sbin.full_head;
        if (s->remote_count.load(std::memory_order_acquire) != 0u) {
            drain_remote(s);
            if (s->hdr.local_free != nullptr) {
                queue_erase(sbin.full_head, sbin.full_tail, s);
                s->hdr.in_full = 0u;
                list_push(sbin.head, s);
                return activate_slab(s);
            }
        }
        if (sbin.full_head != sbin.full_tail) {
            queue_erase(sbin.full_head, sbin.full_tail, s);
            queue_push_back(sbin.full_head, sbin.full_tail, s);
        } else {
            break;
        }
    }

    SlabHeader* s = tp->alloc_new_slab(cls);
    list_push(sbin.head, s);
    return activate_slab(s);
}

void pool_free_slow(void* p, const std::uint32_t /*cls*/) noexcept {
    if (p == nullptr) {
        return;
    }
    SlabHeader* s = slab_from_ptr(p);
    ThreadPool* const cur = t_pool;
    if (cur != nullptr && s->hdr.owner_id.load(std::memory_order_relaxed) == cur->owner_id) {
        auto* node = ::new (p) SlabFreeNode{s->hdr.local_free};
        poison_slot_tail(p, s->hdr.slot_size);
        s->hdr.local_free = node;
        --s->hdr.local_used;
        SlabBin& bin = cur->bins[s->hdr.class_idx];
        if (s->hdr.in_full != 0u) {
            queue_erase(bin.full_head, bin.full_tail, s);
            s->hdr.in_full = 0u;
            if (s->hdr.local_used == 0u && s->hdr.is_active_bump == 0u && bin.head != nullptr) {
                cur->recycle_empty_slab(s);
            } else {
                list_push(bin.head, s);
            }
        } else if (s->hdr.local_used == 0u && s->hdr.is_active_bump == 0u &&
                   (bin.head != s || s->next != nullptr)) {
            list_erase(bin.head, s);
            cur->recycle_empty_slab(s);
        }
        return;
    }
    remote_or_orphan_free(s, p);
}

namespace {

inline void flush_doc_credit_for_teardown() noexcept {
    ThreadDocCredit& tc = t_doc_credit;
    tc.dead = true;
    SharedDocHeader* doc = tc.doc;
    const std::uint32_t credit = tc.credit;
    tc.doc = nullptr;
    tc.credit = 0u;
    if (doc != nullptr && credit > 0u) {
        if (doc->refcount.load(std::memory_order_acquire) == credit ||
            doc->refcount.fetch_sub(credit, std::memory_order_acq_rel) == credit) {
            destroy_shared_doc_slow(doc);
        }
    }
}

struct DocCreditGuard {
    bool armed = false;
    ~DocCreditGuard() noexcept {
        armed = false;
        flush_doc_credit_for_teardown();
    }
};

inline thread_local DocCreditGuard t_doc_credit_guard{};

} // namespace

void shared_doc_addref_slow(SharedDocHeader* hdr, ThreadDocCredit& tc) noexcept {
    if (hdr == nullptr) {
        return;
    }
    if (tc.dead) {
        hdr->refcount.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    t_doc_credit_guard.armed = true;
    if (tc.doc != hdr) {
        SharedDocHeader* prev = tc.doc;
        const std::uint32_t prev_credit = tc.credit;
        tc.doc = hdr;
        tc.credit = 0u;
        if (prev != nullptr && prev_credit > 0u) {
            if (prev->refcount.load(std::memory_order_acquire) == prev_credit ||
                prev->refcount.fetch_sub(prev_credit, std::memory_order_acq_rel) == prev_credit) {
                destroy_shared_doc_slow(prev);
            }
        }
    }
    constexpr std::uint32_t kBatch = 256u;
    hdr->refcount.fetch_add(kBatch, std::memory_order_relaxed);
    tc.credit += (kBatch - 1u);
}

void shared_doc_release_slow(SharedDocHeader* hdr, const std::uint32_t n,
                             ThreadDocCredit& tc) noexcept {
    if (hdr == nullptr || n == 0u) {
        return;
    }
    if (tc.doc == hdr) {
        if (!tc.dead && n <= 256u && tc.credit + n > 512u &&
            hdr->refcount.load(std::memory_order_acquire) > tc.credit + n) {
            const std::uint32_t drain = (tc.credit + n) - 256u;
            tc.credit = 256u;
            const std::uint32_t prev =
                hdr->refcount.fetch_sub(drain, std::memory_order_acq_rel);
            assert(prev > drain);
            (void)prev;
            return;
        }
        const std::uint32_t total = tc.credit + n;
        tc.doc = nullptr;
        tc.credit = 0u;
        if (hdr->refcount.load(std::memory_order_acquire) == total ||
            hdr->refcount.fetch_sub(total, std::memory_order_acq_rel) == total) {
            destroy_shared_doc_slow(hdr);
        }
        return;
    }
    if (hdr->refcount.load(std::memory_order_acquire) == n ||
        hdr->refcount.fetch_sub(n, std::memory_order_acq_rel) == n) {
        destroy_shared_doc_slow(hdr);
    }
}

namespace {

struct alignas(16) LargeBufHeader {
    std::size_t cap;
    std::size_t pad;
};
static_assert(sizeof(LargeBufHeader) == 16u && alignof(LargeBufHeader) == 16u,
              "LargeBufHeader must preserve 16-byte payload alignment");

struct LargeCacheSlot {
    LargeBufHeader* hdr;
    std::size_t cap;
};

inline constexpr std::size_t kLargeCacheMaxSlots = 6u;
inline constexpr std::size_t kLargeCacheMinBytes = 256u;
inline constexpr std::size_t kLargeCacheMaxEntryBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t kLargeCacheMaxTotalBytes = 24u * 1024u * 1024u;

struct ThreadLargeCache {
    LargeCacheSlot slots[kLargeCacheMaxSlots];
    std::size_t count;
    std::size_t total_bytes;
};

struct ThreadLargeCacheGuard {
    bool armed = false;
    ~ThreadLargeCacheGuard() noexcept;
};

inline thread_local ThreadLargeCache t_large_cache{};
inline thread_local bool t_large_dead = false;
inline thread_local ThreadLargeCacheGuard t_large_guard{};

ThreadLargeCacheGuard::~ThreadLargeCacheGuard() noexcept {
    armed = false;
    t_large_dead = true;
    ThreadLargeCache& cache = t_large_cache;
    for (std::size_t i = 0u; i < cache.count; ++i) {
        LargeBufHeader* hdr = cache.slots[i].hdr;
        const std::size_t cap = cache.slots[i].cap;
#if defined(NEOJSON_HAS_ASAN)
        ASAN_UNPOISON_MEMORY_REGION(hdr, sizeof(LargeBufHeader) + cap);
#else
        (void)cap;
#endif
        std::free(hdr);
    }
    cache.count = 0u;
    cache.total_bytes = 0u;
}

inline std::size_t large_buf_cap(const void* ptr) noexcept {
    const auto* hdr = reinterpret_cast<const LargeBufHeader*>(
        static_cast<const char*>(ptr) - sizeof(LargeBufHeader));
    return hdr->cap;
}

void* large_malloc_impl(const std::size_t size, const bool ephemeral) noexcept {
    if (size > (std::numeric_limits<std::size_t>::max)() - sizeof(LargeBufHeader)) {
        return nullptr;
    }
    const std::size_t req = (size == 0u) ? 1u : size;
    if (!t_large_dead && t_large_cache.count > 0u &&
        req >= kLargeCacheMinBytes && req <= kLargeCacheMaxEntryBytes) {
        ThreadLargeCache& cache = t_large_cache;
        std::size_t best_idx = kLargeCacheMaxSlots;
        std::size_t best_cap = 0u;
        for (std::size_t i = 0u; i < cache.count; ++i) {
            const std::size_t c = cache.slots[i].cap;
            if (c >= req && (ephemeral || (c - req) <= (req + 65536u))) {
                if (best_idx == kLargeCacheMaxSlots ||
                    (ephemeral ? (c > best_cap) : (c < best_cap))) {
                    best_idx = i;
                    best_cap = c;
                }
            }
        }
        if (best_idx < cache.count) {
            LargeBufHeader* hdr = cache.slots[best_idx].hdr;
            for (std::size_t i = best_idx + 1u; i < cache.count; ++i) {
                cache.slots[i - 1u] = cache.slots[i];
            }
            --cache.count;
            cache.total_bytes -= best_cap;
            void* payload = static_cast<char*>(static_cast<void*>(hdr)) + sizeof(LargeBufHeader);
#if defined(NEOJSON_HAS_ASAN)
            ASAN_UNPOISON_MEMORY_REGION(payload, req);
#endif
            return payload;
        }
    }
    void* raw = std::malloc(sizeof(LargeBufHeader) + req);
    if (raw == nullptr) {
        return nullptr;
    }
    auto* hdr = static_cast<LargeBufHeader*>(raw);
    hdr->cap = req;
    hdr->pad = 0u;
    return static_cast<char*>(raw) + sizeof(LargeBufHeader);
}

void large_free(void* /*ctx*/, void* ptr) noexcept {
    if (ptr == nullptr) {
        return;
    }
    auto* hdr = reinterpret_cast<LargeBufHeader*>(
        static_cast<char*>(ptr) - sizeof(LargeBufHeader));
    const std::size_t cap = hdr->cap;
    if (!t_large_dead && cap >= kLargeCacheMinBytes && cap <= kLargeCacheMaxEntryBytes) {
        t_large_guard.armed = true;
        ThreadLargeCache& cache = t_large_cache;
        while (cache.count > 0u &&
               (cache.count == kLargeCacheMaxSlots ||
                cache.total_bytes + cap > kLargeCacheMaxTotalBytes)) {
            LargeBufHeader* ev_hdr = cache.slots[0].hdr;
            const std::size_t ev_cap = cache.slots[0].cap;
            for (std::size_t i = 1u; i < cache.count; ++i) {
                cache.slots[i - 1u] = cache.slots[i];
            }
            --cache.count;
            cache.total_bytes -= ev_cap;
#if defined(NEOJSON_HAS_ASAN)
            ASAN_UNPOISON_MEMORY_REGION(ev_hdr, sizeof(LargeBufHeader) + ev_cap);
#endif
            std::free(ev_hdr);
        }
        if (cache.count < kLargeCacheMaxSlots &&
            cache.total_bytes + cap <= kLargeCacheMaxTotalBytes) {
#if defined(NEOJSON_HAS_ASAN)
            ASAN_POISON_MEMORY_REGION(ptr, cap);
#endif
            cache.slots[cache.count++] = LargeCacheSlot{hdr, cap};
            cache.total_bytes += cap;
            return;
        }
    }
#if defined(NEOJSON_HAS_ASAN)
    ASAN_UNPOISON_MEMORY_REGION(hdr, sizeof(LargeBufHeader) + cap);
#endif
    std::free(hdr);
}

void* large_realloc_impl(void* ptr, const std::size_t old_size, const std::size_t new_size,
                         const bool ephemeral) noexcept {
    if (ptr == nullptr) {
        return large_malloc_impl(new_size, ephemeral);
    }
    if (new_size == 0u) {
        large_free(nullptr, ptr);
        return nullptr;
    }
    const std::size_t cur_cap = large_buf_cap(ptr);
    if (new_size <= cur_cap) {
#if defined(NEOJSON_HAS_ASAN)
        ASAN_UNPOISON_MEMORY_REGION(ptr, new_size);
        if (new_size < cur_cap) {
            ASAN_POISON_MEMORY_REGION(static_cast<char*>(ptr) + new_size, cur_cap - new_size);
        }
#endif
        return ptr;
    }
    void* np = large_malloc_impl(new_size, ephemeral);
    if (np == nullptr) {
        return nullptr;
    }
    const std::size_t copy_bytes = std::min({old_size, new_size, cur_cap});
    if (copy_bytes > 0u) {
        std::memcpy(np, ptr, copy_bytes);
    }
    large_free(nullptr, ptr);
    return np;
}

void* large_malloc_bounded(void* /*ctx*/, const std::size_t size) noexcept {
    return large_malloc_impl(size, false);
}

void* large_realloc_bounded(void* /*ctx*/, void* ptr, const std::size_t old_size,
                            const std::size_t new_size) noexcept {
    return large_realloc_impl(ptr, old_size, new_size, false);
}

void* large_alloc_full_cap(const std::size_t min_cap, std::size_t& actual_cap) noexcept {
    void* p = large_malloc_impl(min_cap, true);
    if (p == nullptr) {
        return nullptr;
    }
    actual_cap = large_buf_cap(p);
#if defined(NEOJSON_HAS_ASAN)
    if (actual_cap > min_cap) {
        ASAN_UNPOISON_MEMORY_REGION(p, actual_cap);
    }
#endif
    return p;
}

void* large_realloc_full_cap(void* old_ptr, const std::size_t old_size,
                             const std::size_t min_cap, std::size_t& actual_cap) noexcept {
    void* p = large_realloc_impl(old_ptr, old_size, min_cap, true);
    if (p == nullptr) {
        return nullptr;
    }
    actual_cap = large_buf_cap(p);
#if defined(NEOJSON_HAS_ASAN)
    ASAN_UNPOISON_MEMORY_REGION(p, actual_cap);
#endif
    return p;
}

const yyjson_alc& yyjson_parse_alc() noexcept {
    static const yyjson_alc alc{&large_malloc_bounded, &large_realloc_bounded, &large_free, nullptr};
    return alc;
}

} // namespace

class DumpBuffer {
public:
    DumpBuffer() noexcept : data_(inline_buf_), size_(0), cap_(sizeof(inline_buf_)) {}

    explicit DumpBuffer(const std::size_t reserve_bytes)
        : data_(inline_buf_), size_(0), cap_(sizeof(inline_buf_)) {
        if (reserve_bytes > sizeof(inline_buf_)) {
            out_str_.reserve(reserve_bytes);
        }
    }

    ~DumpBuffer() noexcept {
        if (data_ != inline_buf_) {
            large_free(nullptr, data_);
        }
    }

    DumpBuffer(const DumpBuffer&) = delete;
    DumpBuffer& operator=(const DumpBuffer&) = delete;

    void reserve_extra(const std::size_t n) {
        if (n > cap_ - size_) {
            grow(n);
        }
    }

    char* write_ptr() noexcept {
        return data_ + size_;
    }

    char* end_ptr() noexcept {
        return data_ + cap_;
    }

    void advance_to(const char* end) noexcept {
        size_ = static_cast<std::size_t>(end - data_);
    }

    void push_unchecked(const char c) noexcept {
        data_[size_++] = c;
    }

    void push(const char c) {
        if (size_ == cap_) {
            grow(1);
        }
        data_[size_++] = c;
    }

    void append_unchecked(const char* p, const std::size_t n) noexcept {
        if (n != 0) {
            std::memcpy(data_ + size_, p, n);
            size_ += n;
        }
    }

    void append(const char* p, const std::size_t n) {
        if (n > sizeof(inline_buf_) && data_ == inline_buf_) {
            if (size_ != 0) {
                out_str_.append(inline_buf_, size_);
                size_ = 0;
            }
            spilled_ = true;
            out_str_.append(p, n);
            return;
        }
        reserve_extra(n);
        append_unchecked(p, n);
    }

    std::string to_string() const {
        if (!spilled_) {
            return std::string(data_, size_);
        }
        std::string res = out_str_;
        if (size_ != 0) {
            res.append(data_, size_);
        }
        return res;
    }

    std::string take_string() {
        if (!spilled_) {
            return std::string(data_, size_);
        }
        if (size_ != 0) {
            out_str_.append(data_, size_);
            size_ = 0;
        }
        return std::move(out_str_);
    }

private:
    void grow(std::size_t need);

    char inline_buf_[4096];
    char* data_;
    std::size_t size_;
    std::size_t cap_;
    std::string out_str_;
    bool spilled_ = false;
};

void DumpBuffer::grow(const std::size_t need) {
    constexpr std::size_t kMaxSize = (std::numeric_limits<std::size_t>::max)();
    if (need <= sizeof(inline_buf_)) {
        if (size_ != 0) {
            out_str_.append(data_, size_);
            size_ = 0;
        }
        if (data_ != inline_buf_) {
            large_free(nullptr, data_);
            data_ = inline_buf_;
            cap_ = sizeof(inline_buf_);
        }
        spilled_ = true;
        return;
    }
    if (size_ != 0) {
        out_str_.append(data_, size_);
        size_ = 0;
    }
    spilled_ = true;
    if (data_ != inline_buf_ && need <= cap_) {
        return;
    }
    std::size_t new_cap = (cap_ <= kMaxSize / 2) ? cap_ * 2 : kMaxSize;
    if (new_cap < 8192) {
        new_cap = 8192;
    }
    if (new_cap < need) {
        new_cap = need;
    }
    std::size_t actual_cap = new_cap;
    if (data_ == inline_buf_) {
        auto* p = static_cast<char*>(large_alloc_full_cap(new_cap, actual_cap));
        if (p == nullptr) {
            throw std::bad_alloc();
        }
        data_ = p;
    } else {
        auto* p = static_cast<char*>(large_realloc_full_cap(data_, 0u, new_cap, actual_cap));
        if (p == nullptr) {
            throw std::bad_alloc();
        }
        data_ = p;
    }
    cap_ = actual_cap;
}

namespace {

inline constexpr char kDigits100[201] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

NEOJSON_ALWAYS_INLINE void copy_2_digits(char* dst, const std::size_t v) noexcept {
    std::memcpy(dst, kDigits100 + v * 2u, 2u);
}

NEOJSON_ALWAYS_INLINE void write_4_digits(char* cur, const std::uint32_t v) noexcept {
    const std::uint32_t a = v / 100u;
    const std::uint32_t b = v - a * 100u;
    copy_2_digits(cur, a);
    copy_2_digits(cur + 2u, b);
}

NEOJSON_ALWAYS_INLINE void write_8_digits(char* cur, const std::uint32_t v) noexcept {
    const std::uint32_t a = v / 10000u;
    const std::uint32_t b = v - a * 10000u;
    write_4_digits(cur, a);
    write_4_digits(cur + 4u, b);
}

NEOJSON_ALWAYS_INLINE char* write_u32_le4(char* cur, const std::uint32_t v) noexcept {
    if (v < 10u) {
        *cur++ = static_cast<char>('0' + v);
        return cur;
    }
    if (v < 100u) {
        copy_2_digits(cur, v);
        return cur + 2u;
    }
    const std::uint32_t q = v / 100u;
    const std::uint32_t r = v - q * 100u;
    if (q < 10u) {
        *cur++ = static_cast<char>('0' + q);
    } else {
        copy_2_digits(cur, q);
        cur += 2u;
    }
    copy_2_digits(cur, r);
    return cur + 2u;
}

NEOJSON_ALWAYS_INLINE char* write_u32_le8(char* cur, const std::uint32_t v) noexcept {
    if (v < 10000u) {
        return write_u32_le4(cur, v);
    }
    const std::uint32_t hi = v / 10000u;
    const std::uint32_t lo = v - hi * 10000u;
    cur = write_u32_le4(cur, hi);
    write_4_digits(cur, lo);
    return cur + 4u;
}

NEOJSON_NOINLINE char* write_u64_ge1e16_to(char* cur, const std::uint64_t v) noexcept {
    const std::uint64_t hi = v / 10000000000000000ULL;
    const std::uint64_t rem = v - hi * 10000000000000000ULL;
    const auto mid = static_cast<std::uint32_t>(rem / 100000000ULL);
    const auto lo =
        static_cast<std::uint32_t>(rem - static_cast<std::uint64_t>(mid) * 100000000ULL);
    cur = write_u32_le4(cur, static_cast<std::uint32_t>(hi));
    write_8_digits(cur, mid);
    write_8_digits(cur + 8u, lo);
    return cur + 16u;
}

NEOJSON_ALWAYS_INLINE char* write_u64_fast(char* cur, const std::uint64_t v) noexcept {
    if (v < 10000u) {
        return write_u32_le4(cur, static_cast<std::uint32_t>(v));
    }
    if (v < 100000000ULL) {
        return write_u32_le8(cur, static_cast<std::uint32_t>(v));
    }
    if (NEOJSON_LIKELY(v < 10000000000000000ULL)) {
        const std::uint64_t hi = v / 100000000ULL;
        const auto lo = static_cast<std::uint32_t>(v - hi * 100000000ULL);
        cur = write_u32_le8(cur, static_cast<std::uint32_t>(hi));
        write_8_digits(cur, lo);
        return cur + 8u;
    }
    return write_u64_ge1e16_to(cur, v);
}

NEOJSON_ALWAYS_INLINE char* write_i64_fast(char* cur, const std::int64_t v) noexcept {
    if (v < 0) {
        *cur++ = '-';
        const auto u = static_cast<std::uint64_t>(-(v + 1)) + 1u;
        return write_u64_fast(cur, u);
    }
    return write_u64_fast(cur, static_cast<std::uint64_t>(v));
}

inline void append_uint(DumpBuffer& out, const std::uint64_t v) {
    out.reserve_extra(21u);
    out.advance_to(write_u64_fast(out.write_ptr(), v));
}

inline void append_int(DumpBuffer& out, const std::int64_t v) {
    out.reserve_extra(22u);
    out.advance_to(write_i64_fast(out.write_ptr(), v));
}

NEOJSON_NOINLINE char* write_f64_neojson_slow(char* cur, const double val) noexcept {
    return cur + yyjson_write_f64_neojson(cur, val);
}

#if defined(__SIZEOF_INT128__)
inline constexpr std::uint64_t kPow10Hi28[28] = {
    0x8000000000000000ULL, // 10^0
    0xA000000000000000ULL, // 10^1
    0xC800000000000000ULL, // 10^2
    0xFA00000000000000ULL, // 10^3
    0x9C40000000000000ULL, // 10^4
    0xC350000000000000ULL, // 10^5
    0xF424000000000000ULL, // 10^6
    0x9896800000000000ULL, // 10^7
    0xBEBC200000000000ULL, // 10^8
    0xEE6B280000000000ULL, // 10^9
    0x9502F90000000000ULL, // 10^10
    0xBA43B74000000000ULL, // 10^11
    0xE8D4A51000000000ULL, // 10^12
    0x9184E72A00000000ULL, // 10^13
    0xB5E620F480000000ULL, // 10^14
    0xE35FA931A0000000ULL, // 10^15
    0x8E1BC9BF04000000ULL, // 10^16
    0xB1A2BC2EC5000000ULL, // 10^17
    0xDE0B6B3A76400000ULL, // 10^18
    0x8AC7230489E80000ULL, // 10^19
    0xAD78EBC5AC620000ULL, // 10^20
    0xD8D726B7177A8000ULL, // 10^21
    0x878678326EAC9000ULL, // 10^22
    0xA968163F0A57B400ULL, // 10^23
    0xD3C21BCECCEDA100ULL, // 10^24
    0x84595161401484A0ULL, // 10^25
    0xA56FA5B99019A5C8ULL, // 10^26
    0xCECB8F27F4200F3AULL  // 10^27
};
#endif

NEOJSON_ALWAYS_INLINE char* write_f64_neojson_fast(char* cur, const double val) noexcept {
#if defined(__SIZEOF_INT128__)
    std::uint64_t raw = 0u;
    std::memcpy(&raw, &val, sizeof(raw));
    const std::uint32_t exp_raw = static_cast<std::uint32_t>((raw >> 52u) & 0x7FFu);
    const std::uint64_t sig_raw = raw & ((1ULL << 52u) - 1u);
    if (NEOJSON_LIKELY(exp_raw >= 1009u && exp_raw <= 1072u && sig_raw != 0u)) {
        const std::int32_t exp_bin = static_cast<std::int32_t>(exp_raw) - 1075;
        const std::int32_t k = (exp_bin * 315653) >> 20;
        const std::int32_t h = exp_bin + ((-k * 217707) >> 16);
        const std::uint64_t cb = (sig_raw | (1ULL << 52u)) << static_cast<unsigned>(h + 1);
        const std::uint64_t p10_hi = kPow10Hi28[-k];
        __extension__ using u128_t = unsigned __int128;
        const u128_t prod = static_cast<u128_t>(cb) * p10_hi;
        const auto s_hi = static_cast<std::uint64_t>(prod >> 64u);
        const auto s_lo = static_cast<std::uint64_t>(prod);
        const std::uint64_t mod = s_hi % 10u;
        const std::uint64_t dec = s_hi - mod;
        const std::uint64_t c = (mod << 60u) | (s_lo >> 4u);
        const std::uint64_t half_ulp = p10_hi >> static_cast<unsigned>(4 - h);
        constexpr std::uint64_t kT0 = (10ULL << 60u);
        const std::uint64_t t1 = c + half_ulp;
        if (NEOJSON_LIKELY(s_lo != (1ULL << 63u) && half_ulp != c && t1 <= kT0 &&
                           (kT0 - t1) > 1ULL)) {
            const bool u0_inside = (half_ulp > c);
            const bool w0_inside = (t1 >= kT0);
            const bool w1_inside = (s_lo > (1ULL << 63u));
            const bool trim = (u0_inside | w0_inside);
            const std::uint64_t sig_dec =
                dec + (trim ? (w0_inside ? 10u : 0u) : (mod + (w1_inside ? 1u : 0u)));
            const std::int32_t sig_len = 16 + ((sig_dec >= 10000000000000000ULL) ? 1 : 0);
            const std::int32_t dot_ofs = sig_len + k;
            if (NEOJSON_LIKELY(dot_ofs >= 1 && dot_ofs <= 15)) {
                cur[0] = '-';
                cur += (raw >> 63u);
                char* p = cur + 1;
                const auto hi9 = static_cast<std::uint32_t>(sig_dec / 100000000ULL);
                const auto lo8 = static_cast<std::uint32_t>(
                    sig_dec - static_cast<std::uint64_t>(hi9) * 100000000ULL);
                if (sig_len == 17) {
                    const std::uint32_t d0 = hi9 / 100000000u;
                    *p++ = static_cast<char>('0' + d0);
                    write_8_digits(p, hi9 - d0 * 100000000u);
                    p += 8u;
                } else {
                    write_8_digits(p, hi9);
                    p += 8u;
                }
                if (lo8 != 0u) {
                    write_8_digits(p, lo8);
                    p += 8u;
                }
                while (p[-1] == '0') {
                    --p;
                }
                const int len_written = static_cast<int>(p - (cur + 1));
                if (NEOJSON_LIKELY(len_written > dot_ofs)) {
                    if (dot_ofs == 1) {
                        cur[0] = cur[1];
                    } else if (dot_ofs == 2) {
                        cur[0] = cur[1];
                        cur[1] = cur[2];
                    } else if (dot_ofs == 3) {
                        cur[0] = cur[1];
                        cur[1] = cur[2];
                        cur[2] = cur[3];
                    } else {
                        std::memmove(cur, cur + 1, static_cast<std::size_t>(dot_ofs));
                    }
                    cur[dot_ofs] = '.';
                    return p;
                }
                std::memmove(cur, cur + 1, static_cast<std::size_t>(len_written));
                for (int z = len_written; z < dot_ofs; ++z) {
                    cur[z] = '0';
                }
                cur[dot_ofs] = '.';
                cur[dot_ofs + 1] = '0';
                return cur + dot_ofs + 2;
            }
        }
    }
#endif
    return write_f64_neojson_slow(cur, val);
}

NEOJSON_NOINLINE char* write_f64_neojson_outlined(char* cur, const double val) noexcept {
    return write_f64_neojson_fast(cur, val);
}

} // namespace

namespace dtoa_impl {

inline void append_float(DumpBuffer& out, const double val) {
    out.reserve_extra(48u);
    out.advance_to(write_f64_neojson_fast(out.write_ptr(), val));
}

} // namespace dtoa_impl

namespace utf8_impl {

static constexpr std::uint8_t UTF8_ACCEPT = 0;
static constexpr std::uint8_t UTF8_REJECT = 1;

inline std::uint8_t decode(std::uint8_t& state, std::uint32_t& codep,
                           const std::uint8_t byte) noexcept {
    static const std::array<std::uint8_t, 400> utf8d = {{
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 00..1f
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 20..3f
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 40..5f
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 60..7f
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, // 80..9f
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, // a0..bf
        8, 8, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // c0..df
        0xA, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x4, 0x3, 0x3,                 // e0..ef
        0xB, 0x6, 0x6, 0x6, 0x5, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8,                 // f0..ff
        0x0, 0x1, 0x2, 0x3, 0x5, 0x8, 0x7, 0x1, 0x1, 0x1, 0x4, 0x6, 0x1, 0x1, 0x1, 0x1,                 // s0..s0
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, // s1..s2
        1, 2, 1, 1, 1, 1, 1, 2, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, // s3..s4
        1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 1, 3, 1, 1, 1, 1, 1, 1, // s5..s6
        1, 3, 1, 1, 1, 1, 1, 3, 1, 3, 1, 1, 1, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1  // s7..s8
    }};

    const std::uint8_t type = utf8d[byte];
    codep = (state != UTF8_ACCEPT)
                ? (byte & 0x3Fu) | (codep << 6u)
                : (0xFFu >> type) & (byte);

    std::size_t index = 256u + static_cast<std::size_t>(state) * 16u + static_cast<std::size_t>(type);
    state = utf8d[index];
    return state;
}

inline void append_u4(DumpBuffer& out, std::uint16_t cp) {
    static constexpr char kHex[] = "0123456789abcdef";
    char buf[6] = {'\\', 'u',
                   kHex[(cp >> 12u) & 0x0Fu],
                   kHex[(cp >> 8u) & 0x0Fu],
                   kHex[(cp >> 4u) & 0x0Fu],
                   kHex[cp & 0x0Fu]};
    out.append(buf, 6);
}

inline void throw_invalid_utf8(std::size_t idx, std::uint8_t byte) {
    static constexpr char kHexUpper[] = "0123456789ABCDEF";
    char hex[5] = {'0', 'x', kHexUpper[byte >> 4u], kHexUpper[byte & 0x0Fu], '\0'};
    throw type_error::create(
        316, "invalid UTF-8 byte at index " + std::to_string(idx) + ": " + hex);
}

inline void throw_incomplete_utf8(std::uint8_t byte) {
    static constexpr char kHexUpper[] = "0123456789ABCDEF";
    char hex[5] = {'0', 'x', kHexUpper[byte >> 4u], kHexUpper[byte & 0x0Fu], '\0'};
    throw type_error::create(316, std::string("incomplete UTF-8 string; last byte: ") + hex);
}

constexpr std::array<std::uint8_t, 256> make_escape_lut(const bool ensure_ascii) noexcept {
    std::array<std::uint8_t, 256> lut{};
    for (std::size_t i = 0; i < 256; ++i) {
        const bool verbatim =
            i >= 0x20u && i < (ensure_ascii ? 0x7Fu : 0x80u) && i != 0x22u && i != 0x5Cu;
        lut[i] = verbatim ? 0u : 1u;
    }
    return lut;
}

inline constexpr std::array<std::uint8_t, 256> kNeedsEscapeUtf8 = make_escape_lut(false);
inline constexpr std::array<std::uint8_t, 256> kNeedsEscapeAscii = make_escape_lut(true);

inline bool has_non_verbatim_utf8_8(const std::uint64_t w) noexcept {
    constexpr std::uint64_t kHi = 0x8080808080808080ULL;
    constexpr std::uint64_t kLo = 0x0101010101010101ULL;
    constexpr std::uint64_t kSp = 0x2020202020202020ULL;
    const std::uint64_t non_ascii_or_ctrl = (w & kHi) | ((w - kSp) & ~w & kHi);
    const std::uint64_t q = w ^ 0x2222222222222222ULL;
    const std::uint64_t b = w ^ 0x5C5C5C5C5C5C5C5CULL;
    const std::uint64_t has_quote = (q - kLo) & ~q & kHi;
    const std::uint64_t has_bs = (b - kLo) & ~b & kHi;
    return (non_ascii_or_ctrl | has_quote | has_bs) != 0;
}

#if NEOJSON_X86_SIMD || defined(__SSE2__)
inline unsigned ctz32_nz(const unsigned mask) noexcept {
#if defined(_MSC_VER)
    unsigned long idx = 0;
    _BitScanForward(&idx, static_cast<unsigned long>(mask));
    return static_cast<unsigned>(idx);
#else
    return static_cast<unsigned>(__builtin_ctz(mask));
#endif
}
#endif

inline bool has_non_verbatim_valid_utf8_8(const std::uint64_t w) noexcept {
    constexpr std::uint64_t kHi = 0x8080808080808080ULL;
    constexpr std::uint64_t kLo = 0x0101010101010101ULL;
    constexpr std::uint64_t kSp = 0x2020202020202020ULL;
    const std::uint64_t lo7 = w & ~kHi;
    const std::uint64_t ctrl = (lo7 - kSp) & ~w & kHi;
    const std::uint64_t q = w ^ 0x2222222222222222ULL;
    const std::uint64_t b = w ^ 0x5C5C5C5C5C5C5C5CULL;
    const std::uint64_t has_quote = (q - kLo) & ~q & kHi;
    const std::uint64_t has_bs = (b - kLo) & ~b & kHi;
    return (ctrl | has_quote | has_bs) != 0;
}

inline std::size_t find_non_verbatim_utf8_scalar(const char* p, std::size_t pos,
                                                 const std::size_t len) noexcept {
    while (pos + 8u <= len) {
        std::uint64_t w = 0;
        std::memcpy(&w, p + pos, 8u);
        if (has_non_verbatim_utf8_8(w)) {
            break;
        }
        pos += 8u;
    }
    while (pos < len && kNeedsEscapeUtf8[static_cast<std::uint8_t>(p[pos])] == 0u) {
        ++pos;
    }
    return pos;
}

inline std::size_t find_non_verbatim_valid_utf8_scalar(const char* p, std::size_t pos,
                                                       const std::size_t len) noexcept {
    while (pos + 8u <= len) {
        std::uint64_t w = 0;
        std::memcpy(&w, p + pos, 8u);
        if (has_non_verbatim_valid_utf8_8(w)) {
            break;
        }
        pos += 8u;
    }
    while (pos < len) {
        const auto b = static_cast<std::uint8_t>(p[pos]);
        if (b < 0x20u || b == '"' || b == '\\') {
            break;
        }
        ++pos;
    }
    return pos;
}

inline std::size_t find_non_verbatim_ascii_scalar(const char* p, std::size_t pos,
                                                  const std::size_t len) noexcept {
    while (pos < len && kNeedsEscapeAscii[static_cast<std::uint8_t>(p[pos])] == 0u) {
        ++pos;
    }
    return pos;
}

#if NEOJSON_X86_SIMD
__attribute__((target("avx2")))
NEOJSON_NOINLINE std::size_t find_non_verbatim_utf8_avx2(
    const char* p, std::size_t pos, const std::size_t len) noexcept {
    const __m256i v20 = _mm256_set1_epi8(0x20);
    const __m256i vq = _mm256_set1_epi8('"');
    const __m256i vb = _mm256_set1_epi8('\\');
    while (pos + 32u <= len) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + pos));
        const __m256i ctrl_or_hi = _mm256_cmpgt_epi8(v20, v);
        const __m256i eq_q = _mm256_cmpeq_epi8(v, vq);
        const __m256i eq_b = _mm256_cmpeq_epi8(v, vb);
        const __m256i bad = _mm256_or_si256(ctrl_or_hi, _mm256_or_si256(eq_q, eq_b));
        const unsigned mask = static_cast<unsigned>(_mm256_movemask_epi8(bad));
        if (mask != 0u) {
            return pos + ctz32_nz(mask);
        }
        pos += 32u;
    }
    return find_non_verbatim_utf8_scalar(p, pos, len);
}

__attribute__((target("avx2")))
NEOJSON_NOINLINE std::size_t find_non_verbatim_valid_utf8_avx2(
    const char* p, std::size_t pos, const std::size_t len) noexcept {
    const __m256i v1f = _mm256_set1_epi8(0x1F);
    const __m256i vq = _mm256_set1_epi8('"');
    const __m256i vb = _mm256_set1_epi8('\\');
    while (pos + 32u <= len) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + pos));
        const __m256i le_1f = _mm256_cmpeq_epi8(_mm256_max_epu8(v, v1f), v1f);
        const __m256i eq_q = _mm256_cmpeq_epi8(v, vq);
        const __m256i eq_b = _mm256_cmpeq_epi8(v, vb);
        const __m256i bad = _mm256_or_si256(le_1f, _mm256_or_si256(eq_q, eq_b));
        const unsigned mask = static_cast<unsigned>(_mm256_movemask_epi8(bad));
        if (mask != 0u) {
            return pos + ctz32_nz(mask);
        }
        pos += 32u;
    }
    return find_non_verbatim_valid_utf8_scalar(p, pos, len);
}

__attribute__((target("avx2")))
NEOJSON_NOINLINE std::size_t find_non_verbatim_ascii_avx2(
    const char* p, std::size_t pos, const std::size_t len) noexcept {
    const __m256i v20 = _mm256_set1_epi8(0x20);
    const __m256i v7f = _mm256_set1_epi8(0x7F);
    const __m256i vq = _mm256_set1_epi8('"');
    const __m256i vb = _mm256_set1_epi8('\\');
    while (pos + 32u <= len) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + pos));
        const __m256i ctrl_or_hi = _mm256_cmpgt_epi8(v20, v);
        const __m256i eq_7f = _mm256_cmpeq_epi8(v, v7f);
        const __m256i eq_q = _mm256_cmpeq_epi8(v, vq);
        const __m256i eq_b = _mm256_cmpeq_epi8(v, vb);
        const __m256i bad = _mm256_or_si256(_mm256_or_si256(ctrl_or_hi, eq_7f),
                                            _mm256_or_si256(eq_q, eq_b));
        const unsigned mask = static_cast<unsigned>(_mm256_movemask_epi8(bad));
        if (mask != 0u) {
            return pos + ctz32_nz(mask);
        }
        pos += 32u;
    }
    return find_non_verbatim_ascii_scalar(p, pos, len);
}
#endif

#if defined(__SSE2__) || NEOJSON_X86_SIMD
inline std::size_t find_non_verbatim_utf8_sse2(const char* p, std::size_t pos,
                                               const std::size_t len) noexcept {
    const __m128i v20 = _mm_set1_epi8(0x20);
    const __m128i vq = _mm_set1_epi8('"');
    const __m128i vb = _mm_set1_epi8('\\');
    while (pos + 16u <= len) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + pos));
        const __m128i ctrl_or_hi = _mm_cmpgt_epi8(v20, v);
        const __m128i eq_q = _mm_cmpeq_epi8(v, vq);
        const __m128i eq_b = _mm_cmpeq_epi8(v, vb);
        const __m128i bad = _mm_or_si128(ctrl_or_hi, _mm_or_si128(eq_q, eq_b));
        const unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(bad));
        if (mask != 0u) {
            return pos + ctz32_nz(mask);
        }
        pos += 16u;
    }
    return find_non_verbatim_utf8_scalar(p, pos, len);
}

inline std::size_t find_non_verbatim_valid_utf8_sse2(const char* p, std::size_t pos,
                                                     const std::size_t len) noexcept {
    const __m128i v1f = _mm_set1_epi8(0x1F);
    const __m128i vq = _mm_set1_epi8('"');
    const __m128i vb = _mm_set1_epi8('\\');
    while (pos + 16u <= len) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + pos));
        const __m128i le_1f = _mm_cmpeq_epi8(_mm_max_epu8(v, v1f), v1f);
        const __m128i eq_q = _mm_cmpeq_epi8(v, vq);
        const __m128i eq_b = _mm_cmpeq_epi8(v, vb);
        const __m128i bad = _mm_or_si128(le_1f, _mm_or_si128(eq_q, eq_b));
        const unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(bad));
        if (mask != 0u) {
            return pos + ctz32_nz(mask);
        }
        pos += 16u;
    }
    return find_non_verbatim_valid_utf8_scalar(p, pos, len);
}

inline std::size_t find_non_verbatim_ascii_sse2(const char* p, std::size_t pos,
                                                const std::size_t len) noexcept {
    const __m128i v20 = _mm_set1_epi8(0x20);
    const __m128i v7f = _mm_set1_epi8(0x7F);
    const __m128i vq = _mm_set1_epi8('"');
    const __m128i vb = _mm_set1_epi8('\\');
    while (pos + 16u <= len) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + pos));
        const __m128i ctrl_or_hi = _mm_cmpgt_epi8(v20, v);
        const __m128i eq_7f = _mm_cmpeq_epi8(v, v7f);
        const __m128i eq_q = _mm_cmpeq_epi8(v, vq);
        const __m128i eq_b = _mm_cmpeq_epi8(v, vb);
        const __m128i bad = _mm_or_si128(_mm_or_si128(ctrl_or_hi, eq_7f),
                                         _mm_or_si128(eq_q, eq_b));
        const unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(bad));
        if (mask != 0u) {
            return pos + ctz32_nz(mask);
        }
        pos += 16u;
    }
    return find_non_verbatim_ascii_scalar(p, pos, len);
}
#endif

inline std::size_t find_non_verbatim_utf8(const char* p, std::size_t pos,
                                          const std::size_t len) noexcept {
#if defined(__SSE2__) || NEOJSON_X86_SIMD
    const std::size_t rem = len - pos;
#if NEOJSON_X86_SIMD
    if (rem >= 32u && has_avx2()) {
        return find_non_verbatim_utf8_avx2(p, pos, len);
    }
#endif
    if (rem >= 16u) {
        return find_non_verbatim_utf8_sse2(p, pos, len);
    }
#endif
    return find_non_verbatim_utf8_scalar(p, pos, len);
}

inline std::size_t find_non_verbatim_valid_utf8(const char* p, std::size_t pos,
                                                const std::size_t len) noexcept {
#if defined(__SSE2__) || NEOJSON_X86_SIMD
    const std::size_t rem = len - pos;
#if NEOJSON_X86_SIMD
    if (rem >= 32u && has_avx2()) {
        return find_non_verbatim_valid_utf8_avx2(p, pos, len);
    }
#endif
    if (rem >= 16u) {
        return find_non_verbatim_valid_utf8_sse2(p, pos, len);
    }
#endif
    return find_non_verbatim_valid_utf8_scalar(p, pos, len);
}

inline std::size_t find_non_verbatim_ascii(const char* p, std::size_t pos,
                                           const std::size_t len) noexcept {
#if defined(__SSE2__) || NEOJSON_X86_SIMD
    const std::size_t rem = len - pos;
#if NEOJSON_X86_SIMD
    if (rem >= 32u && has_avx2()) {
        return find_non_verbatim_ascii_avx2(p, pos, len);
    }
#endif
    if (rem >= 16u) {
        return find_non_verbatim_ascii_sse2(p, pos, len);
    }
#endif
    return find_non_verbatim_ascii_scalar(p, pos, len);
}

// The algorithm of nlohmann/json's serializer::dump_escaped, with the same output and the same errors. The bytes that
// are copied as they are form spans of `s`, and each span is appended with one call from `start_pos` onward.
inline void dump_escaped_from(DumpBuffer& out, const std::string_view s, const std::size_t start_pos,
                              const bool ensure_ascii, const error_handler_t error_handler) {
    std::uint32_t codepoint = 0;
    std::uint8_t state = UTF8_ACCEPT;
    std::size_t undumped_chars = 0;

    // s[span_begin, span_end) is copied to out but not yet appended.
    std::size_t span_begin = start_pos;
    std::size_t span_end = start_pos;
    const auto flush = [&] {
        if (span_end > span_begin) {
            out.append(s.data() + span_begin, span_end - span_begin);
        }
        span_begin = span_end;
    };
    const auto copy = [&](const std::size_t begin, const std::size_t end) {
        if (begin != span_end) {
            flush();
            span_begin = begin;
        }
        span_end = end;
    };

    for (std::size_t i = start_pos; i < s.size(); ++i) {
        // With no sequence open (state is UTF8_ACCEPT, so undumped_chars is 0), a run of verbatim bytes needs no
        // decoding: each would decode to itself and leave the state as it is.
        if (state == UTF8_ACCEPT) {
            const std::size_t end =
                ensure_ascii ? find_non_verbatim_ascii(s.data(), i, s.size())
                             : find_non_verbatim_utf8(s.data(), i, s.size());
            if (end > i) {
                copy(i, end);
                i = end - 1;
                continue;
            }
        }

        const auto byte = static_cast<std::uint8_t>(s[i]);

        switch (decode(state, codepoint, byte)) {
            case UTF8_ACCEPT: {
                switch (codepoint) {
                    case 0x08:
                        flush();
                        out.append("\\b", 2);
                        break;
                    case 0x09:
                        flush();
                        out.append("\\t", 2);
                        break;
                    case 0x0A:
                        flush();
                        out.append("\\n", 2);
                        break;
                    case 0x0C:
                        flush();
                        out.append("\\f", 2);
                        break;
                    case 0x0D:
                        flush();
                        out.append("\\r", 2);
                        break;
                    case 0x22:
                        flush();
                        out.append("\\\"", 2);
                        break;
                    case 0x5C:
                        flush();
                        out.append("\\\\", 2);
                        break;
                    default: {
                        if (codepoint <= 0x1F || (ensure_ascii && codepoint >= 0x7F)) {
                            flush();
                            if (codepoint <= 0xFFFF) {
                                append_u4(out, static_cast<std::uint16_t>(codepoint));
                            } else {
                                append_u4(out, static_cast<std::uint16_t>(0xD7C0u + (codepoint >> 10u)));
                                append_u4(out, static_cast<std::uint16_t>(0xDC00u + (codepoint & 0x3FFu)));
                            }
                        } else {
                            copy(i - undumped_chars, i + 1);
                        }
                        break;
                    }
                }
                undumped_chars = 0;
                break;
            }

            case UTF8_REJECT: {
                switch (error_handler) {
                    case error_handler_t::strict:
                        throw_invalid_utf8(i, byte);
                        break;
                    case error_handler_t::ignore:
                    case error_handler_t::replace: {
                        if (undumped_chars > 0) {
                            --i;
                        }
                        undumped_chars = 0;
                        state = UTF8_ACCEPT;

                        if (error_handler == error_handler_t::replace) {
                            flush();
                            if (ensure_ascii) {
                                out.append("\\ufffd", 6);
                            } else {
                                out.append("\xEF\xBF\xBD", 3);
                            }
                        }
                        break;
                    }
                }
                break;
            }

            default: {
                ++undumped_chars;
                break;
            }
        }
    }

    flush();

    if (state != UTF8_ACCEPT) {
        switch (error_handler) {
            case error_handler_t::strict:
                if (!s.empty()) {
                    throw_incomplete_utf8(static_cast<std::uint8_t>(s.back()));
                } else {
                    throw_incomplete_utf8(0);
                }
                break;
            case error_handler_t::ignore:
                break;
            case error_handler_t::replace:
                if (ensure_ascii) {
                    out.append("\\ufffd", 6);
                } else {
                    out.append("\xEF\xBF\xBD", 3);
                }
                break;
        }
    }
}

inline void dump_escaped(DumpBuffer& out, const std::string_view s, const bool ensure_ascii,
                         const error_handler_t error_handler) {
    dump_escaped_from(out, s, 0, ensure_ascii, error_handler);
}

inline void ensure_space(DumpBuffer& out, char*& dst, char*& end, const std::size_t need) {
    if (static_cast<std::size_t>(end - dst) < need) {
        out.advance_to(dst);
        out.reserve_extra(need);
        dst = out.write_ptr();
        end = out.end_ptr();
    }
}

inline void dump_quoted_strict_impl(DumpBuffer& out, const std::string_view s,
                                    const bool with_colon) {
    const char* p = s.data();
    const std::size_t len = s.size();
    const std::size_t extra = with_colon ? 3u : 2u;
    if (len > (std::numeric_limits<std::size_t>::max)() - 32u) {
        throw std::bad_alloc();
    }
    const std::size_t first = find_non_verbatim_utf8(p, 0, len);
    if (first == len) {
        out.reserve_extra(len + extra);
        char* dst = out.write_ptr();
        *dst++ = '"';
        if (len != 0) {
            std::memcpy(dst, p, len);
            dst += len;
        }
        *dst++ = '"';
        if (with_colon) {
            *dst++ = ':';
        }
        out.advance_to(dst);
        return;
    }

    out.reserve_extra(len + extra + 16u);
    char* dst = out.write_ptr();
    char* end = out.end_ptr();
    *dst++ = '"';
    if (first != 0) {
        std::memcpy(dst, p, first);
        dst += first;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::size_t i = first;
    while (i < len) {
        const std::size_t next = find_non_verbatim_utf8(p, i, len);
        if (next > i) {
            const std::size_t span = next - i;
            ensure_space(out, dst, end, span);
            std::memcpy(dst, p + i, span);
            dst += span;
            i = next;
        }
        if (i == len) {
            break;
        }

        const auto b0 = static_cast<std::uint8_t>(p[i]);
        if (b0 < 0x80u) {
            ensure_space(out, dst, end, 6);
            switch (b0) {
                case 0x08u:
                    dst[0] = '\\';
                    dst[1] = 'b';
                    dst += 2;
                    break;
                case 0x09u:
                    dst[0] = '\\';
                    dst[1] = 't';
                    dst += 2;
                    break;
                case 0x0Au:
                    dst[0] = '\\';
                    dst[1] = 'n';
                    dst += 2;
                    break;
                case 0x0Cu:
                    dst[0] = '\\';
                    dst[1] = 'f';
                    dst += 2;
                    break;
                case 0x0Du:
                    dst[0] = '\\';
                    dst[1] = 'r';
                    dst += 2;
                    break;
                case 0x22u:
                    dst[0] = '\\';
                    dst[1] = '"';
                    dst += 2;
                    break;
                case 0x5Cu:
                    dst[0] = '\\';
                    dst[1] = '\\';
                    dst += 2;
                    break;
                default:
                    dst[0] = '\\';
                    dst[1] = 'u';
                    dst[2] = '0';
                    dst[3] = '0';
                    dst[4] = kHex[(b0 >> 4u) & 0x0Fu];
                    dst[5] = kHex[b0 & 0x0Fu];
                    dst += 6;
                    break;
            }
            ++i;
            continue;
        }

        // Stream consecutive valid RFC 3629 multi-byte UTF-8 sequences without re-entering SWAR/DFA.
        while (i < len) {
            const auto u0 = static_cast<std::uint8_t>(p[i]);
            if (u0 < 0x80u) {
                break;
            }
            if (u0 >= 0xC2u && u0 <= 0xDFu) {
                if (i + 1 < len && (static_cast<std::uint8_t>(p[i + 1]) & 0xC0u) == 0x80u) {
                    ensure_space(out, dst, end, 2);
                    dst[0] = p[i];
                    dst[1] = p[i + 1];
                    dst += 2;
                    i += 2;
                    continue;
                }
            } else if (u0 >= 0xE0u && u0 <= 0xEFu) {
                if (i + 2 < len) {
                    const auto u1 = static_cast<std::uint8_t>(p[i + 1]);
                    const auto u2 = static_cast<std::uint8_t>(p[i + 2]);
                    if ((u1 & 0xC0u) == 0x80u && (u2 & 0xC0u) == 0x80u &&
                        !(u0 == 0xE0u && u1 < 0xA0u) && !(u0 == 0xEDu && u1 >= 0xA0u)) {
                        ensure_space(out, dst, end, 3);
                        dst[0] = p[i];
                        dst[1] = p[i + 1];
                        dst[2] = p[i + 2];
                        dst += 3;
                        i += 3;
                        continue;
                    }
                }
            } else if (u0 >= 0xF0u && u0 <= 0xF4u) {
                if (i + 3 < len) {
                    const auto u1 = static_cast<std::uint8_t>(p[i + 1]);
                    const auto u2 = static_cast<std::uint8_t>(p[i + 2]);
                    const auto u3 = static_cast<std::uint8_t>(p[i + 3]);
                    if ((u1 & 0xC0u) == 0x80u && (u2 & 0xC0u) == 0x80u &&
                        (u3 & 0xC0u) == 0x80u && !(u0 == 0xF0u && u1 < 0x90u) &&
                        !(u0 == 0xF4u && u1 >= 0x90u)) {
                        ensure_space(out, dst, end, 4);
                        dst[0] = p[i];
                        dst[1] = p[i + 1];
                        dst[2] = p[i + 2];
                        dst[3] = p[i + 3];
                        dst += 4;
                        i += 4;
                        continue;
                    }
                }
            }
            out.advance_to(dst);
            dump_escaped_from(out, s, i, false, error_handler_t::strict);
            out.append(with_colon ? "\":" : "\"", with_colon ? 2u : 1u);
            return;
        }
    }

    ensure_space(out, dst, end, with_colon ? 2u : 1u);
    *dst++ = '"';
    if (with_colon) {
        *dst++ = ':';
    }
    out.advance_to(dst);
}

inline void dump_quoted_fast_strict(DumpBuffer& out, const std::string_view s) {
    dump_quoted_strict_impl(out, s, false);
}

inline void dump_key_colon_fast_strict(DumpBuffer& out, const std::string_view s) {
    dump_quoted_strict_impl(out, s, true);
}

#if defined(YYJSON_DISABLE_UTF8_VALIDATION) && YYJSON_DISABLE_UTF8_VALIDATION
#error "neojson requires yyjson UTF-8 validation (YYJSON_DISABLE_UTF8_VALIDATION must be 0)"
#endif

inline void dump_valid_utf8_escaped_strict(DumpBuffer& out, const char* p, const std::size_t len,
                                           const bool with_colon) {
    const std::size_t extra = with_colon ? 3u : 2u;
    if (len > (std::numeric_limits<std::size_t>::max)() - 32u) {
        throw std::bad_alloc();
    }
    // Inputs from yyjson were already RFC-3629 validated at parse time; with ensure_ascii=false,
    // all bytes >= 0x80 and printable ASCII != '"' and != '\\' copy verbatim.
    out.reserve_extra(len + extra + 16u);
    char* dst = out.write_ptr();
    char* end = out.end_ptr();
    *dst++ = '"';

    static constexpr char kHex[] = "0123456789abcdef";
    std::size_t i = 0u;
    while (i < len) {
        const std::size_t next = find_non_verbatim_valid_utf8(p, i, len);
        if (next > i) {
            const std::size_t span = next - i;
            ensure_space(out, dst, end, span);
            std::memcpy(dst, p + i, span);
            dst += span;
            i = next;
        }
        if (i == len) {
            break;
        }
        const auto b0 = static_cast<std::uint8_t>(p[i]);
        ensure_space(out, dst, end, 6u);
        switch (b0) {
            case 0x08u:
                dst[0] = '\\';
                dst[1] = 'b';
                dst += 2u;
                break;
            case 0x09u:
                dst[0] = '\\';
                dst[1] = 't';
                dst += 2u;
                break;
            case 0x0Au:
                dst[0] = '\\';
                dst[1] = 'n';
                dst += 2u;
                break;
            case 0x0Cu:
                dst[0] = '\\';
                dst[1] = 'f';
                dst += 2u;
                break;
            case 0x0Du:
                dst[0] = '\\';
                dst[1] = 'r';
                dst += 2u;
                break;
            case 0x22u:
                dst[0] = '\\';
                dst[1] = '"';
                dst += 2u;
                break;
            case 0x5Cu:
                dst[0] = '\\';
                dst[1] = '\\';
                dst += 2u;
                break;
            default:
                dst[0] = '\\';
                dst[1] = 'u';
                dst[2] = '0';
                dst[3] = '0';
                dst[4] = kHex[(b0 >> 4u) & 0x0Fu];
                dst[5] = kHex[b0 & 0x0Fu];
                dst += 6u;
                break;
        }
        ++i;
    }

    ensure_space(out, dst, end, with_colon ? 2u : 1u);
    *dst++ = '"';
    if (with_colon) {
        *dst++ = ':';
    }
    out.advance_to(dst);
}

} // namespace utf8_impl

struct json_access {
    static json make_string(const char* s, const std::size_t len) {
        detail::SharedString* ptr = json::create_string(s, len);
        json j;
        j.m_type = json::value_t::string;
        j.m_value.string = ptr;
        return j;
    }

    static json make_array() noexcept {
        json j;
        j.m_type = json::value_t::array;
        j.m_storage = 3u;
        return j;
    }

    static json make_object() noexcept {
        json j;
        j.m_type = json::value_t::object;
        j.m_storage = 3u;
        return j;
    }

    static bool is_empty_container(const json& j) noexcept {
        return j.m_storage == 3u;
    }

    static json::value_t type_of(const json& j) noexcept {
        return j.m_type;
    }

    static bool bool_of(const json& j) noexcept {
        return j.m_value.boolean;
    }

    static json::number_integer_t int_of(const json& j) noexcept {
        return j.m_value.number_integer;
    }

    static json::number_unsigned_t uint_of(const json& j) noexcept {
        return j.m_value.number_unsigned;
    }

    static json::number_float_t float_of(const json& j) noexcept {
        return j.m_value.number_float;
    }

    static const json::string_t& string_of(const json& j) noexcept {
        return j.m_value.string->value;
    }

    static void detach_shared_string(json& j) {
        if (j.m_type == json::value_t::string) {
            (void)j.mutable_string_unique();
        }
    }

    static const json::array_t& array_of(const json& j) {
        return *j.const_array();
    }

    static const json::object_t& object_of(const json& j) {
        return *j.const_object();
    }

    static const json::array_t* raw_array_ptr(const json& j) noexcept {
        return j.m_value.array;
    }

    static const json::object_t* raw_object_ptr(const json& j) noexcept {
        return j.m_value.object;
    }

    static bool is_small_array(const json& j) noexcept {
        return j.m_storage == 2u && j.m_type == json::value_t::array;
    }

    static const SmallArray2& small_array_of(const json& j) noexcept {
        return *j.m_value.small_arr;
    }

    static SmallArray2* small_array_ptr(json& j) noexcept {
        return j.m_value.small_arr;
    }

    static json make_small_array(json&& v0) {
        json j;
        j.m_type = json::value_t::array;
        j.m_storage = 2u;
        j.m_value.small_arr = json::create_small_array1(std::move(v0));
        return j;
    }

    static json make_small_array(json&& v0, json&& v1) {
        json j;
        j.m_type = json::value_t::array;
        j.m_storage = 2u;
        j.m_value.small_arr = json::create_small_array2(std::move(v0), std::move(v1));
        return j;
    }

    static void drop_small_array_const_cache(SmallArray2* sa) noexcept {
        if (sa == nullptr) {
            return;
        }
        if (void* mv = sa->materialized_vec.exchange(nullptr, std::memory_order_acq_rel)) {
            auto* vec = static_cast<json::array_t*>(mv);
            json::json_value::destroy_container(vec);
        }
    }

    static bool is_small_object(const json& j) noexcept {
        return j.m_storage == 2u && j.m_type == json::value_t::object;
    }

    static const SmallObject4& small_object_of(const json& j) noexcept {
        return *j.m_value.small_obj;
    }

    static SmallObject4* small_object_ptr(json& j) noexcept {
        return j.m_value.small_obj;
    }

    static SmallObject4* create_small_object_raw(const std::size_t cap = kSmallObjCap) {
        return json::create_small_object_empty(cap);
    }

    static SmallObject4* create_small_object_raw_cap(const std::size_t exact_cap) {
        return json::create_small_object_empty_cap(exact_cap);
    }

    static SmallObject4* create_small_object_raw_for(const std::size_t n,
                                                     const std::size_t key_bytes) {
        return json::create_small_object_empty_for(n, key_bytes);
    }

    static void destroy_small_object_raw(SmallObject4*& so) noexcept {
        json::destroy_small_object(so);
    }

    static json make_from_small_object(SmallObject4* so) noexcept {
        json j;
        j.m_type = json::value_t::object;
        j.m_storage = 2u;
        j.m_value.small_obj = so;
        return j;
    }

    static void drop_small_object_const_cache(SmallObject4* so) noexcept {
        json::drop_small_object_const_cache(so);
    }

    static void small_object_append_unique(SmallObject4* so, const std::string_view key,
                                           json&& val) {
        json::small_obj_append_unique_entry(so, key, std::move(val));
    }

    static void small_object_emplace_or_assign(SmallObject4* so, const std::string_view key,
                                               json&& val) {
        const std::size_t n = so->count;
        const std::uint8_t tag = small_obj_key_tag(key);
        const std::size_t idx = json::small_obj_find_with_tag(so, key, tag);
        if (idx < n) {
            so->val(idx) = std::move(val);
            return;
        }
        json::small_obj_append_unique_entry(so, key, std::move(val));
    }

    static json make_lazy(const json::value_t type, LazyHeader* node) noexcept {
        json j;
        j.m_type = type;
        j.m_storage = 1u;
        j.m_value.lazy = node;
        return j;
    }

    static bool is_lazy(const json& j) noexcept {
        return j.m_storage == 1u;
    }

    static LazyHeader* lazy_node(const json& j) noexcept {
        return j.m_value.lazy;
    }

    static const json::array_t* const_array_of(const json& j) {
        return j.const_array();
    }

    static const json::object_t* const_object_of(const json& j) {
        return j.const_object();
    }

    static json::array_t* detach_array(json& j) {
        return j.mutable_array();
    }

    static json::object_t* detach_object(json& j) {
        return j.mutable_object();
    }

    static json::array_t* create_array_cap(const std::size_t cap) {
        auto* ptr = json::create_array();
        if (cap > 0u) {
            ptr->reserve(cap);
        }
        return ptr;
    }

    static json::object_t* create_object_cap(const std::size_t cap) {
        auto* ptr = json::create_object();
        if (cap > 0u) {
            ptr->reserve_for_build(cap);
        }
        return ptr;
    }

    static json::object_t* create_object_vec_cap(const std::size_t cap) {
        auto* ptr = json::create_object();
        if (cap > 0u) {
            ptr->reserve_vector_only(cap);
        }
        return ptr;
    }

    static json::array_t* clone_array(const json::array_t* src) {
        return json::create_array(*src);
    }

    static json::object_t* clone_object(const json::object_t* src) {
        return json::create_object(*src);
    }

    static void destroy_erased_container(void* ptr, const json::value_t type) noexcept {
        if (ptr == nullptr) {
            return;
        }
        if (type == json::value_t::array) {
            auto* arr = static_cast<json::array_t*>(ptr);
            json::json_value::destroy_container(arr);
        } else {
            auto* obj = static_cast<json::object_t*>(ptr);
            json::json_value::destroy_container(obj);
        }
    }

    static json make_from_array_ptr(json::array_t* arr) noexcept {
        json j;
        j.m_type = json::value_t::array;
        j.m_value.array = arr;
        return j;
    }

    static json make_from_object_ptr(json::object_t* obj) noexcept {
        json j;
        j.m_type = json::value_t::object;
        j.m_value.object = obj;
        return j;
    }

    static bool is_arena_array(const json& j) noexcept {
        return j.m_storage == 4u && j.m_type == json::value_t::array;
    }

    static bool is_arena_object(const json& j) noexcept {
        return j.m_storage == 4u && j.m_type == json::value_t::object;
    }

    static ArenaArray* arena_array_ptr(const json& j) noexcept {
        return j.m_value.arena_arr;
    }

    static ArenaObject* arena_object_ptr(const json& j) noexcept {
        return j.m_value.arena_obj;
    }

    static std::uint8_t storage_of(const json& j) noexcept {
        return j.m_storage;
    }

    static SharedString* shared_string_ptr(const json& j) noexcept {
        return j.m_value.string;
    }

    static json make_from_arena_array(ArenaArray* aa) noexcept {
        json j;
        j.m_type = json::value_t::array;
        j.m_storage = 4u;
        j.m_value.arena_arr = aa;
        return j;
    }

    static json make_from_arena_object(ArenaObject* ao) noexcept {
        json j;
        j.m_type = json::value_t::object;
        j.m_storage = 4u;
        j.m_value.arena_obj = ao;
        return j;
    }

    static json make_arena_string_json(SharedString* ss) noexcept {
        json j;
        j.m_type = json::value_t::string;
        j.m_storage = 4u;
        j.m_value.string = ss;
        return j;
    }

    static json make_owned_arena_string_json(SharedString* ss) noexcept {
        json j;
        j.m_type = json::value_t::string;
        j.m_value.string = ss;
        if (ss->owner_doc != nullptr) {
            shared_doc_addref(ss->owner_doc);
            j.m_storage = 5u;
        } else {
            j.m_storage = 4u;
        }
        return j;
    }

    static void init_arena_null(json* dst) noexcept {
        ::new (dst) json();
    }

    static void init_arena_bool(json* dst, const bool b) noexcept {
        ::new (dst) json();
        dst->m_type = json::value_t::boolean;
        dst->m_value.number_unsigned = 0u;
        dst->m_value.boolean = b;
    }

    static void init_arena_uint(json* dst, const json::number_unsigned_t u) noexcept {
        ::new (dst) json();
        dst->m_type = json::value_t::number_unsigned;
        dst->m_value.number_unsigned = u;
    }

    static void init_arena_sint(json* dst, const json::number_integer_t i) noexcept {
        ::new (dst) json();
        dst->m_type = json::value_t::number_integer;
        dst->m_value.number_integer = i;
    }

    static void init_arena_real(json* dst, const json::number_float_t d) noexcept {
        ::new (dst) json();
        dst->m_type = json::value_t::number_float;
        dst->m_value.number_float = d;
    }

    static void init_arena_string(json* dst, SharedString* ss) noexcept {
        ::new (dst) json();
        dst->m_type = json::value_t::string;
        dst->m_storage = 4u;
        dst->m_value.string = ss;
    }

    static void init_arena_empty_array(json* dst) noexcept {
        ::new (dst) json();
        dst->m_type = json::value_t::array;
        dst->m_storage = 3u;
    }

    static void init_arena_empty_object(json* dst) noexcept {
        ::new (dst) json();
        dst->m_type = json::value_t::object;
        dst->m_storage = 3u;
    }

    static void init_arena_array_ptr(json* dst, ArenaArray* aa) noexcept {
        ::new (dst) json();
        dst->m_type = json::value_t::array;
        dst->m_storage = 4u;
        dst->m_value.arena_arr = aa;
    }

    static void init_arena_object_ptr(json* dst, ArenaObject* ao) noexcept {
        ::new (dst) json();
        dst->m_type = json::value_t::object;
        dst->m_storage = 4u;
        dst->m_value.arena_obj = ao;
    }

    static void set_arena_array_ptr(json* dst, ArenaArray* aa) noexcept {
        dst->m_type = json::value_t::array;
        dst->m_storage = 4u;
        dst->m_value.arena_arr = aa;
    }

    static void set_arena_object_ptr(json* dst, ArenaObject* ao) noexcept {
        dst->m_type = json::value_t::object;
        dst->m_storage = 4u;
        dst->m_value.arena_obj = ao;
    }

    struct ArenaConstLeaf {
        json::value_t type;
        std::uint8_t storage;
        bool bval;
    };

    static constexpr std::array<ArenaConstLeaf, 16u> make_arena_const_leaf_table() noexcept {
        constexpr std::uint8_t kNullTag = YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE;
        constexpr std::uint8_t kFalseTag = YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE;
        constexpr std::uint8_t kTrueTag = YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE;
        constexpr std::uint8_t kArrTag = YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE;
        constexpr std::uint8_t kObjTag = YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE;
        std::array<ArenaConstLeaf, 16u> tbl{};
        for (std::size_t i = 0u; i < 16u; ++i) {
            tbl[i] = ArenaConstLeaf{json::value_t::null, 0u, false};
        }
        tbl[kNullTag] = ArenaConstLeaf{json::value_t::null, 0u, false};
        tbl[kFalseTag] = ArenaConstLeaf{json::value_t::boolean, 0u, false};
        tbl[kTrueTag] = ArenaConstLeaf{json::value_t::boolean, 0u, true};
        tbl[kArrTag] = ArenaConstLeaf{json::value_t::array, 3u, false};
        tbl[kObjTag] = ArenaConstLeaf{json::value_t::object, 3u, false};
        return tbl;
    }

    static void init_arena_const_leaf(json* dst, const std::uint8_t tag) noexcept {
        constexpr std::uint8_t kNullTag = YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE;
        constexpr std::uint8_t kFalseTag = YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE;
        constexpr std::uint8_t kTrueTag = YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE;
        constexpr std::uint8_t kArrTag = YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE;
        constexpr std::uint8_t kObjTag = YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE;
        static_assert(kNullTag < 16u && kFalseTag < 16u && kTrueTag < 16u && kArrTag < 16u &&
                          kObjTag < 16u,
                      "constant yyjson leaf tags must fit in 16-entry table");
        static_assert(kNullTag != kFalseTag && kNullTag != kTrueTag && kNullTag != kArrTag &&
                          kNullTag != kObjTag && kFalseTag != kTrueTag && kFalseTag != kArrTag &&
                          kFalseTag != kObjTag && kTrueTag != kArrTag && kTrueTag != kObjTag &&
                          kArrTag != kObjTag,
                      "constant yyjson leaf tags must be distinct");
        static constexpr std::array<ArenaConstLeaf, 16u> kConstLeafTable =
            make_arena_const_leaf_table();
        const ArenaConstLeaf e = kConstLeafTable[tag & 15u];
        ::new (dst) json();
        dst->m_type = e.type;
        dst->m_storage = e.storage;
        dst->m_value.boolean = e.bval;
    }

    // Shallowly copies an m_storage == 4u container/scalar slot into sdoc->compat_cache
    // without incrementing SharedYyDoc refcount or deep-cloning nested subtrees; safe because
    // destroy_shared_doc tears down compat_cache before destroying sdoc->arena.
    static json copy_arena_slot_raw(const json& src) noexcept {
        json j;
        j.m_type = src.m_type;
        j.m_storage = src.m_storage;
        j.m_value = src.m_value;
        return j;
    }

    static SmallArray2* create_small_array_from_elem0(json&& v0) {
        return json::create_small_array1(std::move(v0));
    }

    static SmallArray2* create_small_array_from_elem2(json&& v0, json&& v1) {
        return json::create_small_array2(std::move(v0), std::move(v1));
    }

    static json make_small_array_from_ptr(SmallArray2* sa) noexcept {
        json j;
        j.m_type = json::value_t::array;
        j.m_storage = 2u;
        j.m_value.small_arr = sa;
        return j;
    }

    static void destroy_small_array_raw(SmallArray2*& sa) noexcept {
        json::destroy_small_array(sa);
    }

    static json& small_array_slot(SmallArray2* sa, const std::size_t idx) noexcept {
        return sa->elem(idx);
    }

    static json& small_object_append_placeholder(SmallObject4* so, const std::string_view key) {
        const std::size_t n = so->count;
        small_obj_append_unique_entry_helper(so, key);
        return so->val(n);
    }

    static void small_obj_append_unique_entry_helper(SmallObject4* so,
                                                     const std::string_view key) {
        json::small_obj_append_unique_entry(so, key, json());
    }

    static void commit_cloned_owned(json& dst, json&& owned) noexcept {
        dst.m_type = std::exchange(owned.m_type, json::value_t::null);
        dst.m_storage = std::exchange(owned.m_storage, static_cast<std::uint8_t>(0u));
        dst.m_value = std::exchange(owned.m_value, json::json_value{});
    }
};

namespace {

inline bool string_is_inline(const std::string& s) noexcept {
    const char* p = s.data();
    const char* base = reinterpret_cast<const char*>(&s);
    const std::less<const char*> lt{};
    return !lt(p, base) && lt(p, base + sizeof(std::string));
}

inline SharedString& empty_arena_string_singleton() noexcept {
    struct Holder {
        SharedString ss;
        Holder() {
            ss.refcount.store(kArenaStringRef, std::memory_order_relaxed);
            ss.owner_doc = nullptr;
        }
    };
    static Holder h;
    return h.ss;
}

NEOJSON_ALWAYS_INLINE void copy_str_ge16(char* dst, const char* src,
                                         const std::size_t len) noexcept {
    if (len <= 32u) {
        std::memcpy(dst, src, 16u);
        std::memcpy(dst + len - 16u, src + len - 16u, 16u);
        return;
    }
    if (len <= 64u) {
        std::memcpy(dst, src, 16u);
        std::memcpy(dst + 16u, src + 16u, 16u);
        std::memcpy(dst + len - 32u, src + len - 32u, 16u);
        std::memcpy(dst + len - 16u, src + len - 16u, 16u);
        return;
    }
    std::memcpy(dst, src, len);
}

inline thread_local bool t_str_pool_init = false;
inline thread_local bool t_str_pool_dead = false;

struct ThreadStrPool {
    static constexpr std::size_t kNumBuckets = 8u; // 32, 64, 128, 256, 512, 1024, 2048, 4096+
    static constexpr std::array<std::uint32_t, kNumBuckets> kBucketCaps = {
        1280u, 1024u, 768u, 512u, 768u, 768u, 512u, 64u};

    struct BucketStack {
        std::vector<std::string> slots;
        std::uint32_t count = 0u;
    };

    std::array<BucketStack, kNumBuckets> buckets;
    bool dead = false;

    ThreadStrPool() noexcept {
        t_str_pool_init = true;
    }

    ~ThreadStrPool() noexcept {
        dead = true;
        t_str_pool_dead = true;
        for (auto& st : buckets) {
            st.slots.clear();
            st.slots.shrink_to_fit();
            st.count = 0u;
        }
    }

    static std::size_t bucket_for_len(const std::size_t len) noexcept {
        if (len <= 32u) {
            return 0u;
        }
#if defined(__GNUC__) || defined(__clang__)
        const unsigned lg = 63u - static_cast<unsigned>(__builtin_clzll(len - 1u));
        const std::size_t b = (lg >= 5u) ? static_cast<std::size_t>(lg - 4u) : 0u;
        return (b < kNumBuckets) ? b : (kNumBuckets - 1u);
#else
        std::size_t b = 1u;
        std::size_t cap = 64u;
        while (b + 1u < kNumBuckets && cap < len) {
            cap <<= 1u;
            ++b;
        }
        return b;
#endif
    }

    static std::size_t bucket_for_cap(const std::size_t cap) noexcept {
#if defined(__GNUC__) || defined(__clang__)
        const unsigned lg = 63u - static_cast<unsigned>(__builtin_clzll(cap));
        const std::size_t b = (lg >= 5u) ? static_cast<std::size_t>(lg - 5u) : 0u;
        return (b < kNumBuckets) ? b : (kNumBuckets - 1u);
#else
        std::size_t b = 0u;
        std::size_t thr = 64u;
        while (b + 1u < kNumBuckets && thr <= cap) {
            thr <<= 1u;
            ++b;
        }
        return b;
#endif
    }

    NEOJSON_NOINLINE void acquire_into_cold(std::string& dst, const char* p, const std::size_t len) {
        if (!dead && len >= 16u && len <= 4096u) {
            dst.reserve(std::size_t{32u} << bucket_for_len(len));
        }
        dst.assign(p, len);
    }

    NEOJSON_NOINLINE static void assign_grow_cold(std::string& dst, const char* p,
                                                  const std::size_t len) {
        dst.assign(p, len);
    }

    void release(std::string& s) noexcept {
        if (dead) {
            return;
        }
        const std::size_t cap = s.capacity();
        if (cap >= 32u && cap <= 8192u) {
            const std::size_t b = bucket_for_cap(cap);
            const std::uint32_t bcap = kBucketCaps[b];
            BucketStack& st = buckets[b];
            if (NEOJSON_UNLIKELY(st.slots.empty())) {
                try {
                    st.slots.resize(bcap);
                } catch (...) {
                    return;
                }
            }
            if (NEOJSON_LIKELY(st.count < bcap)) {
                st.slots[st.count++] = std::move(s);
            }
        }
    }
};

inline ThreadStrPool& tls_str_pool() noexcept {
    thread_local ThreadStrPool pool;
    return pool;
}

inline ThreadStrPool* try_tls_str_pool_existing() noexcept {
    return (t_str_pool_init && !t_str_pool_dead) ? &tls_str_pool() : nullptr;
}

inline ThreadStrPool* try_tls_str_pool() noexcept {
    return !t_str_pool_dead ? &tls_str_pool() : nullptr;
}

struct HeapStrBlock {
    static constexpr std::uint32_t kCap = 254u;
    HeapStrBlock* next = nullptr;
    std::uint32_t count = 0u;
    std::uint32_t pad = 0u;
    std::string* ptrs[kCap];
};

struct DocArenaChunk {
    DocArenaChunk* next = nullptr;
    std::size_t used = 0u;
    std::size_t cap = 0u;
};

struct DocArena {
    DocArenaChunk* head = nullptr;
    char* bump_ptr = nullptr;
    char* bump_end = nullptr;
    HeapStrBlock* heap_str_head = nullptr;
    std::size_t next_chunk_bytes = 65536u;

    NEOJSON_NOINLINE void* alloc_slow(const std::size_t n) {
        const std::size_t header_sz = (sizeof(DocArenaChunk) + 15u) & ~std::size_t{15u};
        const std::size_t min_need = header_sz + n;
        std::size_t req = (min_need > next_chunk_bytes) ? min_need : next_chunk_bytes;
        if (req > kLargeCacheMaxEntryBytes && min_need <= kLargeCacheMaxEntryBytes) {
            req = kLargeCacheMaxEntryBytes;
        }
        std::size_t actual_cap = 0u;
        void* mem = large_alloc_full_cap(req, actual_cap);
        if (mem == nullptr) {
            throw std::bad_alloc();
        }
        auto* chunk = ::new (mem) DocArenaChunk();
        chunk->used = header_sz + n;
        chunk->cap = actual_cap;
        chunk->next = head;
        head = chunk;
        char* base = reinterpret_cast<char*>(chunk);
        bump_ptr = base + header_sz + n;
        bump_end = base + actual_cap;
        const std::size_t doubled = req * 2u;
        next_chunk_bytes = (doubled <= kLargeCacheMaxEntryBytes && doubled > req)
                               ? doubled
                               : kLargeCacheMaxEntryBytes;
        return base + header_sz;
    }

    NEOJSON_ALWAYS_INLINE void* alloc(const std::size_t bytes) {
        const std::size_t n = (bytes + 7u) & ~std::size_t{7u};
        if (n > 0u && static_cast<std::size_t>(bump_end - bump_ptr) >= n) {
            void* p = bump_ptr;
            bump_ptr += n;
            return p;
        }
        return alloc_slow(n == 0u ? 8u : n);
    }

    NEOJSON_NOINLINE void track_heap_string(std::string* s) {
        HeapStrBlock* b = heap_str_head;
        if (b == nullptr || b->count == HeapStrBlock::kCap) {
            try {
                void* mem = alloc(sizeof(HeapStrBlock));
                b = ::new (mem) HeapStrBlock();
            } catch (...) {
                using StringT = std::string;
                s->~StringT();
                throw;
            }
            b->next = heap_str_head;
            heap_str_head = b;
        }
        b->ptrs[b->count++] = s;
    }

    NEOJSON_ALWAYS_INLINE void track_heap_string_fast(std::string* s) {
        HeapStrBlock* b = heap_str_head;
        if (NEOJSON_LIKELY(b != nullptr && b->count < HeapStrBlock::kCap)) {
            b->ptrs[b->count++] = s;
            return;
        }
        track_heap_string(s);
    }

    void construct_key_in_place(std::string* dst, const char* p, const std::size_t len,
                                ThreadStrPool* pool) {
        if (len == 0u) {
            ::new (static_cast<void*>(dst)) std::string();
            return;
        }
        if (len <= 15u) {
            ::new (static_cast<void*>(dst)) std::string(p, len);
#if !(defined(__GLIBCXX__) && defined(_GLIBCXX_USE_CXX11_ABI) && _GLIBCXX_USE_CXX11_ABI == 1) && \
    !defined(_LIBCPP_VERSION) && !defined(_MSVC_STL_VERSION)
            if (!string_is_inline(*dst)) {
                try {
                    track_heap_string_fast(dst);
                } catch (...) {
                    using StringT = std::string;
                    dst->~StringT();
                    throw;
                }
            }
#endif
            return;
        }
        if (NEOJSON_LIKELY(pool != nullptr && !pool->dead && len <= 4096u)) {
            const std::size_t b = ThreadStrPool::bucket_for_len(len);
            auto& st = pool->buckets[b];
            if (NEOJSON_LIKELY(st.count != 0u)) {
                std::string& slot = st.slots[--st.count];
                ::new (static_cast<void*>(dst)) std::string(std::move(slot));
                try {
                    const std::size_t cur_sz = dst->size();
                    if (NEOJSON_LIKELY(len <= cur_sz)) {
                        if (NEOJSON_UNLIKELY(len != cur_sz)) {
                            dst->erase(len);
                        }
                        copy_str_ge16(dst->data(), p, len);
                    } else {
                        ThreadStrPool::assign_grow_cold(*dst, p, len);
                    }
#if defined(__GLIBCXX__) && defined(_GLIBCXX_USE_CXX11_ABI) && _GLIBCXX_USE_CXX11_ABI == 1
                    track_heap_string_fast(dst);
#else
                    if (!string_is_inline(*dst)) {
                        track_heap_string_fast(dst);
                    }
#endif
                } catch (...) {
                    using StringT = std::string;
                    dst->~StringT();
                    throw;
                }
                return;
            }
        }
        ::new (static_cast<void*>(dst)) std::string();
        try {
            if (pool != nullptr) {
                pool->acquire_into_cold(*dst, p, len);
            } else {
                ThreadStrPool::assign_grow_cold(*dst, p, len);
            }
#if defined(__GLIBCXX__) && defined(_GLIBCXX_USE_CXX11_ABI) && _GLIBCXX_USE_CXX11_ABI == 1
            track_heap_string_fast(dst);
#else
            if (!string_is_inline(*dst)) {
                track_heap_string_fast(dst);
            }
#endif
        } catch (...) {
            using StringT = std::string;
            dst->~StringT();
            throw;
        }
    }

    void destroy_all() noexcept {
        using StringT = std::string;
        HeapStrBlock* b = heap_str_head;
        heap_str_head = nullptr;
        ThreadStrPool* const pool = (b != nullptr) ? try_tls_str_pool_existing() : nullptr;
        while (b != nullptr) {
            HeapStrBlock* next = b->next;
            const std::uint32_t cnt = b->count;
            if (pool != nullptr && !pool->dead) {
                for (std::uint32_t i = cnt; i-- > 0u;) {
                    pool->release(*b->ptrs[i]);
                    b->ptrs[i]->~StringT();
                }
            } else {
                for (std::uint32_t i = cnt; i-- > 0u;) {
                    b->ptrs[i]->~StringT();
                }
            }
            b = next;
        }
        DocArenaChunk* c = head;
        head = nullptr;
        bump_ptr = nullptr;
        bump_end = nullptr;
        while (c != nullptr) {
            DocArenaChunk* next = c->next;
            large_free(nullptr, c);
            c = next;
        }
    }
};

struct SharedDocExtras {
#if __cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
    std::atomic_flag extra_lock{};
#else
    std::atomic_flag extra_lock = ATOMIC_FLAG_INIT;
#endif
    std::vector<ArenaHashIndex*> arena_indices;
    std::unordered_map<const void*, std::pair<void*, value_t>> compat_cache;
};

} // namespace

struct SharedYyDoc : SharedDocHeader {
    yyjson_doc* doc = nullptr;
    std::atomic<SharedDocExtras*> extras{nullptr};
    DocArena arena;

    explicit SharedYyDoc(yyjson_doc* d) noexcept : doc(d) {}
};
static_assert(sizeof(SharedYyDoc) <= 64u, "SharedYyDoc must fit in 64-byte pooled slab");

namespace {

inline SharedDocExtras* get_or_create_doc_extras(SharedYyDoc* sdoc) {
    SharedDocExtras* ex = sdoc->extras.load(std::memory_order_acquire);
    if (ex != nullptr) {
        return ex;
    }
    void* mem = tcache_alloc(sizeof(SharedDocExtras));
    SharedDocExtras* fresh = nullptr;
    try {
        fresh = ::new (mem) SharedDocExtras();
    } catch (...) {
        tcache_free(mem, sizeof(SharedDocExtras));
        throw;
    }
    SharedDocExtras* expected = nullptr;
    if (sdoc->extras.compare_exchange_strong(
            expected, fresh, std::memory_order_release, std::memory_order_acquire)) {
        return fresh;
    }
    fresh->~SharedDocExtras();
    tcache_free(fresh, sizeof(SharedDocExtras));
    return expected;
}

inline void destroy_arena_index(ArenaHashIndex* idx) noexcept {
    if (idx == nullptr) {
        return;
    }
    if (idx->slots != nullptr && idx->cap > 0u) {
        pool_free(idx->slots, idx->cap * sizeof(ArenaHashSlot));
    }
    pool_free(idx, sizeof(ArenaHashIndex));
}

inline void destroy_shared_doc(SharedYyDoc* sdoc) noexcept {
    if (sdoc == nullptr) {
        return;
    }
    // 1. Destroy extras (compat_cache before arena memory goes away, then arena_indices).
    if (SharedDocExtras* ex = sdoc->extras.load(std::memory_order_acquire)) {
        for (auto& kv : ex->compat_cache) {
            json_access::destroy_erased_container(kv.second.first, kv.second.second);
        }
        ex->compat_cache.clear();
        for (ArenaHashIndex* idx : ex->arena_indices) {
            destroy_arena_index(idx);
        }
        ex->arena_indices.clear();
        ex->~SharedDocExtras();
        tcache_free(ex, sizeof(SharedDocExtras));
    }
    // 2. Free all DocArena heap strings and chunks back to ThreadLargeCache / OS.
    sdoc->arena.destroy_all();
    // 3. Free yyjson document (if present) and SharedYyDoc slab object last.
    yyjson_doc* d = sdoc->doc;
    sdoc->~SharedYyDoc();
    tcache_free_fixed<sizeof(SharedYyDoc)>(sdoc);
    if (d != nullptr) {
        yyjson_doc_free(d);
    }
}

struct LazyNode : LazyHeader {
    SharedYyDoc* doc;
    yyjson_val* val;
#if __cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
    std::atomic_flag init_lock{};
#else
    std::atomic_flag init_lock = ATOMIC_FLAG_INIT;
#endif
    std::atomic<std::uint8_t> lookup_probes{0u};

    LazyNode(SharedYyDoc* d, yyjson_val* v) noexcept
        : LazyHeader(), doc(d), val(v) {
        raw_len = (v != nullptr) ? unsafe_yyjson_get_len(v) : 0u;
    }
};
static_assert(sizeof(LazyNode) <= 64u, "LazyNode should fit in small pooled slab");

struct SpinGuard {
    std::atomic_flag& flag;
    explicit SpinGuard(std::atomic_flag& f) noexcept : flag(f) {
        std::uint32_t spins = 0u;
        while (flag.test_and_set(std::memory_order_acquire)) {
            if (++spins < 64u) {
                cpu_relax();
            } else {
                std::this_thread::yield();
            }
        }
    }
    ~SpinGuard() {
        flag.clear(std::memory_order_release);
    }
    SpinGuard(const SpinGuard&) = delete;
    SpinGuard& operator=(const SpinGuard&) = delete;
};

struct U8SpinGuard {
    std::atomic<std::uint8_t>& flag;
    explicit U8SpinGuard(std::atomic<std::uint8_t>& f) noexcept : flag(f) {
        std::uint32_t spins = 0u;
        while (flag.exchange(1u, std::memory_order_acquire) != 0u) {
            if (++spins < 64u) {
                cpu_relax();
            } else {
                std::this_thread::yield();
            }
        }
    }
    ~U8SpinGuard() {
        flag.store(0u, std::memory_order_release);
    }
    U8SpinGuard(const U8SpinGuard&) = delete;
    U8SpinGuard& operator=(const U8SpinGuard&) = delete;
};

NEOJSON_NOINLINE bool check_large_object_unique_sorted(yyjson_val* obj, const std::size_t len) {
    std::vector<std::string_view> keys;
    keys.reserve(len);
    yyjson_obj_iter iter = yyjson_obj_iter_with(obj);
    yyjson_val* key = nullptr;
    while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
        keys.emplace_back(unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
    }
    std::sort(keys.begin(), keys.end());
    for (std::size_t i = 1u; i < keys.size(); ++i) {
        if (keys[i - 1u] == keys[i]) {
            return false;
        }
    }
    return true;
}

NEOJSON_ALWAYS_INLINE std::uint64_t hash_bytes_seeded(const char* data, const std::size_t len,
                                                      const std::uint64_t seed) noexcept {
    std::uint64_t h = seed ^ (static_cast<std::uint64_t>(len) * 0x9e3779b97f4a7c15ULL);
    const auto* p = reinterpret_cast<const unsigned char*>(data);
    if (len <= 16u) {
        if (len >= 8u) {
            std::uint64_t a = 0u;
            std::uint64_t b = 0u;
            std::memcpy(&a, p, 8u);
            std::memcpy(&b, p + len - 8u, 8u);
            h = (h ^ a) * 0xbf58476d1ce4e5b9ULL;
            h = (rotl64(h, 27u) ^ b) * 0x94d049bb133111ebULL;
            return fmix64(h);
        }
        if (len >= 4u) {
            std::uint32_t a = 0u;
            std::uint32_t b = 0u;
            std::memcpy(&a, p, 4u);
            std::memcpy(&b, p + len - 4u, 4u);
            h ^= (static_cast<std::uint64_t>(a) << 32u) | b;
            return fmix64(h);
        }
        if (len > 0u) {
            const std::uint64_t v = (static_cast<std::uint64_t>(p[0]) << 16u) |
                                    (static_cast<std::uint64_t>(p[len >> 1u]) << 8u) |
                                    static_cast<std::uint64_t>(p[len - 1u]);
            h ^= v * 0x9e3779b97f4a7c15ULL;
            return fmix64(h);
        }
        return fmix64(h);
    }
    return hash_bytes_long(data, len, h);
}

// Precondition: len >= 16u. O(1) three-window hash for string-value interning and dup probing.
NEOJSON_ALWAYS_INLINE std::uint64_t hash_str_sample(const char* p, const std::size_t len,
                                                    const std::uint64_t seed) noexcept {
    std::uint64_t a = 0u;
    std::uint64_t b = 0u;
    std::uint64_t c = 0u;
    std::memcpy(&a, p, 8u);
    std::memcpy(&b, p + (len >> 1u) - 4u, 8u);
    std::memcpy(&c, p + len - 8u, 8u);
    std::uint64_t h = seed ^ (static_cast<std::uint64_t>(len) * 0x9e3779b97f4a7c15ULL);
    h = (h ^ a) * 0xbf58476d1ce4e5b9ULL;
    h = (rotl64(h, 27u) ^ b) * 0x94d049bb133111ebULL;
    h = (rotl64(h, 31u) ^ c) * 0x9e3779b97f4a7c15ULL;
    return fmix64(h);
}

struct DupProbeEntry {
    std::size_t hash = 0;
    const char* ptr = nullptr;
    std::size_t len = 0;
    std::uint32_t vec_idx = 0;
    std::uint32_t stamp = 0;
};
static_assert(sizeof(DupProbeEntry) == 32u, "DupProbeEntry should stay 32 bytes");

struct DupProbeTable {
    // Standard heap vector so arbitrary reader/writer threads never depend on ThreadPool state.
    std::vector<DupProbeEntry> slots;
    std::uint32_t stamp = 1u;

    void prepare(const std::size_t len) {
        std::size_t cap = 64u;
        const std::size_t need = len * 2u;
        while (cap < need) {
            cap <<= 1u;
        }
        if (slots.size() < cap) {
            slots.assign(cap, DupProbeEntry{});
            stamp = 1u;
        } else if (slots.size() > 65536u && cap <= 512u) {
            slots.assign(cap, DupProbeEntry{});
            stamp = 1u;
        } else {
            ++stamp;
            if (stamp == 0u) {
                std::fill(slots.begin(), slots.end(), DupProbeEntry{});
                stamp = 1u;
            }
        }
    }

    bool check(yyjson_val* obj, const std::size_t len) {
        prepare(len);
        std::uint64_t seed = g_hash_seed.load(std::memory_order_acquire);
        if (seed == 0u) {
            seed = hash_seed_slow();
        }
        const std::size_t mask = slots.size() - 1u;
        yyjson_obj_iter iter = yyjson_obj_iter_with(obj);
        yyjson_val* key = nullptr;
        while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
            const char* kptr = unsafe_yyjson_get_str(key);
            const std::size_t klen = unsafe_yyjson_get_len(key);
            const std::size_t h = (klen >= 16u)
                                      ? static_cast<std::size_t>(hash_str_sample(kptr, klen, seed))
                                      : static_cast<std::size_t>(hash_bytes_seeded(kptr, klen, seed));
            std::size_t pos = h & mask;
            std::size_t probes = 0u;
            for (;;) {
                auto& e = slots[pos];
                if (e.stamp != stamp) {
                    e = DupProbeEntry{h, kptr, klen, 0u, stamp};
                    break;
                }
                if (e.hash == h && e.len == klen && fast_memeq(e.ptr, kptr, klen)) {
                    return false;
                }
                if (++probes >= 64u) {
                    return check_large_object_unique_sorted(obj, len);
                }
                pos = (pos + 1u) & mask;
            }
        }
        return true;
    }
};

inline DupProbeTable& tls_dup_probe_table() {
    thread_local DupProbeTable table;
    return table;
}

NEOJSON_NOINLINE bool check_small_object_unique(yyjson_val* obj, const std::size_t len) noexcept {
    std::string_view seen[16];
    std::size_t count = 0u;
    yyjson_obj_iter iter = yyjson_obj_iter_with(obj);
    yyjson_val* key = nullptr;
    while ((key = yyjson_obj_iter_next(&iter)) != nullptr && count < len) {
        const std::string_view sv(unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
        for (std::size_t i = 0u; i < count; ++i) {
            if (seen[i].size() == sv.size() &&
                (sv.empty() || std::memcmp(seen[i].data(), sv.data(), sv.size()) == 0)) {
                return false;
            }
        }
        seen[count++] = sv;
    }
    return true;
}

NEOJSON_NOINLINE bool check_large_object_unique(yyjson_val* obj, const std::size_t len) {
    return tls_dup_probe_table().check(obj, len);
}

inline bool is_yyjson_object_unique(yyjson_val* obj) {
    const std::size_t len = unsafe_yyjson_get_len(obj);
    if (len <= 1u) {
        return true;
    }
    if (len <= 16u) {
        return check_small_object_unique(obj, len);
    }
    return check_large_object_unique(obj, len);
}

// A scalar as a node, or an empty array or object for a container (whose elements the caller adds).
json make_node(yyjson_val* val) {
    switch (yyjson_get_tag(val)) {
        case YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE:
            return json(nullptr);

        case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE:
            return json(true);

        case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE:
            return json(false);

        case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT:
            return json(static_cast<json::number_unsigned_t>(unsafe_yyjson_get_uint(val)));

        case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT:
            return json(static_cast<json::number_integer_t>(unsafe_yyjson_get_sint(val)));

        case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL:
            return json(static_cast<json::number_float_t>(unsafe_yyjson_get_real(val)));

        case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE:
        case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
            return json_access::make_string(unsafe_yyjson_get_str(val), unsafe_yyjson_get_len(val));

        case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
            return json_access::make_array();

        case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
            return json_access::make_object();

        default:
            return json(nullptr);
    }
}


struct DocRefBatch {
    SharedYyDoc* sdoc;
    std::size_t credit = 0u;

    explicit DocRefBatch(SharedYyDoc* d) noexcept : sdoc(d) {}

    ~DocRefBatch() noexcept {
        if (credit > 0u) {
            const std::uint32_t sub = static_cast<std::uint32_t>(credit);
            if (sdoc->refcount.fetch_sub(sub, std::memory_order_acq_rel) == sub) {
                destroy_shared_doc(sdoc);
            }
        }
    }

    DocRefBatch(const DocRefBatch&) = delete;
    DocRefBatch& operator=(const DocRefBatch&) = delete;

    void take(const std::size_t remaining_hint) noexcept {
        if (credit == 0u) {
            const std::size_t chunk = (remaining_hint > 0u && remaining_hint < 16u)
                                          ? remaining_hint
                                          : 16u;
            sdoc->refcount.fetch_add(static_cast<std::uint32_t>(chunk),
                                      std::memory_order_relaxed);
            credit = chunk;
        }
        --credit;
    }

    void unget() noexcept {
        ++credit;
    }
};

inline json make_lazy_child(SharedYyDoc* sdoc, yyjson_val* val, DocRefBatch& batch,
                            const std::size_t remaining_hint) {
    if (!yyjson_is_ctn(val)) {
        return make_node(val);
    }
    const std::size_t len = unsafe_yyjson_get_len(val);
    const bool is_arr = yyjson_is_arr(val);
    if (len == 0u) {
        return is_arr ? json_access::make_array() : json_access::make_object();
    }
    if (is_arr && len <= 8u) {
        yyjson_val* elems[8];
        bool all_scalar = true;
        yyjson_val* elem = unsafe_yyjson_get_first(val);
        for (std::size_t i = 0u; i < len; ++i) {
            elems[i] = elem;
            if (yyjson_is_ctn(elem)) {
                all_scalar = false;
                break;
            }
            if (i + 1u < len) {
                elem = unsafe_yyjson_get_next(elem);
            }
        }
        if (all_scalar) {
            if (len == 1u) {
                return json_access::make_small_array(make_node(elems[0]));
            }
            if (len == 2u) {
                return json_access::make_small_array(make_node(elems[0]), make_node(elems[1]));
            }
            json::array_t* arr = json_access::create_array_cap(len);
            struct ArrGuard {
                json::array_t* p;
                ~ArrGuard() {
                    if (p != nullptr) {
                        json_access::destroy_erased_container(p, value_t::array);
                    }
                }
            } guard{arr};
            for (std::size_t i = 0u; i < len; ++i) {
                arr->emplace_back(make_node(elems[i]));
            }
            guard.p = nullptr;
            return json_access::make_from_array_ptr(arr);
        }
    } else if (!is_arr && len <= 8u) {
        yyjson_val* keys[8];
        yyjson_val* vals[8];
        bool cheap_leaf = true;
        std::size_t total_key_bytes = 0u;
        yyjson_obj_iter it = yyjson_obj_iter_with(val);
        for (std::size_t i = 0u; i < len; ++i) {
            yyjson_val* k = yyjson_obj_iter_next(&it);
            yyjson_val* v = yyjson_obj_iter_get_val(k);
            const std::size_t kl = unsafe_yyjson_get_len(k);
            if (kl > 32u || (yyjson_is_ctn(v) && unsafe_yyjson_get_len(v) != 0u)) {
                cheap_leaf = false;
                break;
            }
            keys[i] = k;
            vals[i] = v;
            total_key_bytes += kl;
        }
        if (cheap_leaf) {
            if (len <= kSmallObjCap) {
                SmallObject4* so = json_access::create_small_object_raw_cap(
                    SmallObject4::cap_for_size_and_keys(len, total_key_bytes));
                struct SmallObjGuard {
                    SmallObject4* p;
                    ~SmallObjGuard() {
                        if (p != nullptr) {
                            json_access::destroy_small_object_raw(p);
                        }
                    }
                } guard{so};
                if (sdoc->tree_unique_verified.load(std::memory_order_acquire)) {
                    for (std::size_t i = 0u; i < len; ++i) {
                        const std::string_view kv(unsafe_yyjson_get_str(keys[i]),
                                                  unsafe_yyjson_get_len(keys[i]));
                        json_access::small_object_append_unique(so, kv, make_node(vals[i]));
                    }
                } else {
                    for (std::size_t i = 0u; i < len; ++i) {
                        const std::string_view kv(unsafe_yyjson_get_str(keys[i]),
                                                  unsafe_yyjson_get_len(keys[i]));
                        json_access::small_object_emplace_or_assign(so, kv, make_node(vals[i]));
                    }
                }
                guard.p = nullptr;
                return json_access::make_from_small_object(so);
            }
            json::object_t* obj = json_access::create_object_vec_cap(len);
            struct ObjGuard {
                json::object_t* p;
                ~ObjGuard() {
                    if (p != nullptr) {
                        json_access::destroy_erased_container(p, value_t::object);
                    }
                }
            } guard{obj};
            if (sdoc->tree_unique_verified.load(std::memory_order_acquire)) {
                for (std::size_t i = 0u; i < len; ++i) {
                    const std::string_view kv(unsafe_yyjson_get_str(keys[i]),
                                              unsafe_yyjson_get_len(keys[i]));
                    obj->emplace_back_unique_key(kv, make_node(vals[i]));
                }
            } else {
                for (std::size_t i = 0u; i < len; ++i) {
                    const std::string_view kv(unsafe_yyjson_get_str(keys[i]),
                                              unsafe_yyjson_get_len(keys[i]));
                    obj->emplace_or_assign_key(kv, make_node(vals[i]));
                }
            }
            guard.p = nullptr;
            return json_access::make_from_object_ptr(obj);
        }
    }
    batch.take(remaining_hint);
    void* mem = nullptr;
    try {
        mem = tcache_alloc_fixed<sizeof(LazyNode)>();
    } catch (...) {
        batch.unget();
        throw;
    }
    auto* node = new (mem) LazyNode(sdoc, val);
    return json_access::make_lazy(is_arr ? value_t::array : value_t::object, node);
}

inline void* shallow_materialize(SharedYyDoc* sdoc, yyjson_val* val, const bool known_unique) {
    const std::size_t len = unsafe_yyjson_get_len(val);
    DocRefBatch batch(sdoc);
    if (yyjson_is_arr(val)) {
        json::array_t* arr = json_access::create_array_cap(len);
        struct ArrGuard {
            json::array_t* p;
            ~ArrGuard() {
                if (p != nullptr) {
                    json_access::destroy_erased_container(p, value_t::array);
                }
            }
        } guard{arr};
        yyjson_val* cur = unsafe_yyjson_get_first(val);
        for (std::size_t i = 0u; i < len; ++i) {
            arr->emplace_back(make_lazy_child(sdoc, cur, batch, len - i));
            if (i + 1u < len) {
                cur = unsafe_yyjson_get_next(cur);
            }
        }
        guard.p = nullptr;
        return arr;
    }
    json::object_t* obj = json_access::create_object_vec_cap(len);
    struct ObjGuard {
        json::object_t* p;
        ~ObjGuard() {
            if (p != nullptr) {
                json_access::destroy_erased_container(p, value_t::object);
            }
        }
    } guard{obj};
    yyjson_obj_iter iter = yyjson_obj_iter_with(val);
    yyjson_val* key = nullptr;
    std::size_t rem = len;
    if (known_unique) {
        while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
            yyjson_val* cval = yyjson_obj_iter_get_val(key);
            const std::string_view k(unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
            obj->emplace_back_unindexed(k, make_lazy_child(sdoc, cval, batch, rem));
            --rem;
        }
    } else if (len <= detail::kOrderedMapHashThreshold) {
        while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
            yyjson_val* cval = yyjson_obj_iter_get_val(key);
            const std::string_view k(unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
            obj->emplace_or_assign_key(k, make_lazy_child(sdoc, cval, batch, rem));
            --rem;
        }
    } else {
        DupProbeTable& table = tls_dup_probe_table();
        table.prepare(len);
        const std::size_t mask = table.slots.size() - 1u;
        const std::uint32_t st = table.stamp;
        bool use_probe = true;
        while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
            yyjson_val* cval = yyjson_obj_iter_get_val(key);
            const char* kptr = unsafe_yyjson_get_str(key);
            const std::size_t klen = unsafe_yyjson_get_len(key);
            json child = make_lazy_child(sdoc, cval, batch, rem);
            if (!use_probe) {
                obj->emplace_or_assign_key(std::string_view(kptr, klen), std::move(child));
                --rem;
                continue;
            }
            const std::size_t h = hash_bytes(kptr, klen);
            std::size_t pos = h & mask;
            std::size_t probes = 0u;
            for (;;) {
                auto& e = table.slots[pos];
                if (e.stamp != st) {
                    if (obj->size() <= 0xFFFFFFFFu) {
                        const auto vec_idx = static_cast<std::uint32_t>(obj->size());
                        obj->emplace_back_unindexed(std::string_view(kptr, klen), std::move(child));
                        e = DupProbeEntry{h, kptr, klen, vec_idx, st};
                    } else {
                        use_probe = false;
                        obj->emplace_or_assign_key(std::string_view(kptr, klen), std::move(child));
                    }
                    break;
                }
                if (e.hash == h && e.len == klen &&
                    (klen == 0u || std::memcmp(e.ptr, kptr, klen) == 0)) {
                    (*obj)[e.vec_idx].second = std::move(child);
                    break;
                }
                if (++probes >= 64u) {
                    use_probe = false;
                    obj->emplace_or_assign_key(std::string_view(kptr, klen), std::move(child));
                    break;
                }
                pos = (pos + 1u) & mask;
            }
            --rem;
        }
    }
    guard.p = nullptr;
    return obj;
}

inline json lazy_make_root(yyjson_doc*& doc_ref, yyjson_val* root,
                           const bool str_pool_padded_32 = false) {
    void* smem = tcache_alloc_fixed<sizeof(SharedYyDoc)>();
    auto* sdoc = ::new (smem) SharedYyDoc(doc_ref);
    sdoc->str_pool_padded_32 = str_pool_padded_32;
    void* mem = nullptr;
    try {
        mem = tcache_alloc_fixed<sizeof(LazyNode)>();
    } catch (...) {
        sdoc->~SharedYyDoc();
        tcache_free_fixed<sizeof(SharedYyDoc)>(sdoc);
        throw;
    }
    doc_ref = nullptr;
    auto* node = ::new (mem) LazyNode(sdoc, root);
    return json_access::make_lazy(
        yyjson_is_arr(root) ? value_t::array : value_t::object, node);
}

inline void mark_own_unique(LazyHeader* hdr) noexcept {
    hdr->dedup_state.fetch_or(1u, std::memory_order_acq_rel);
}

inline void mark_own_dup(LazyHeader* hdr) noexcept {
    hdr->dedup_state.fetch_or(2u | 8u, std::memory_order_release);
}

inline bool is_yyjson_tree_unique(yyjson_val* root) {
    yyjson_val* const end = unsafe_yyjson_get_next(root);
    for (yyjson_val* cur = root; cur < end; ++cur) {
        if (unsafe_yyjson_is_obj(cur) && !is_yyjson_object_unique(cur)) {
            return false;
        }
    }
    return true;
}

struct KeyInternSlot {
    const char* ptr = nullptr;
    std::uint32_t hash32 = 0u;
    std::uint32_t doc_stamp = 0u;
    std::uint32_t len = 0u;
    std::uint32_t key_id = 0u;
};
static_assert(sizeof(KeyInternSlot) == 24u, "KeyInternSlot must stay 24 bytes");

struct KeySeenToken {
    std::uint64_t token = 0u;
    std::uint32_t out_idx = 0u;
};

struct KeyInternTable {
    static constexpr std::uint32_t kUnindexedKeyId = 0xFFFFFFFFu;

    std::vector<KeyInternSlot> slots;
    std::vector<KeySeenToken> seen;
    std::uint32_t doc_stamp = 1u;
    std::uint32_t obj_stamp = 1u;
    std::size_t used = 0u;
    std::uint32_t next_key_id = 1u;

    void begin_doc() {
        used = 0u;
        next_key_id = 1u;
        if (slots.empty()) {
            slots.assign(1024u, KeyInternSlot{});
            seen.assign(512u, KeySeenToken{});
            doc_stamp = 1u;
            obj_stamp = 1u;
            return;
        }
        if (slots.size() > 65536u) {
            slots.assign(4096u, KeyInternSlot{});
            doc_stamp = 1u;
        } else {
            ++doc_stamp;
            if (doc_stamp == 0u) {
                std::fill(slots.begin(), slots.end(), KeyInternSlot{});
                doc_stamp = 1u;
            }
        }
        if (seen.size() > 65536u) {
            seen.assign(4096u, KeySeenToken{});
        } else if (doc_stamp == 1u) {
            std::fill(seen.begin(), seen.end(), KeySeenToken{});
        }
        obj_stamp = 1u;
    }

    std::uint64_t next_obj_token() {
        ++obj_stamp;
        if (obj_stamp == 0u) {
            ++doc_stamp;
            if (doc_stamp == 0u) {
                std::fill(slots.begin(), slots.end(), KeyInternSlot{});
                doc_stamp = 1u;
            }
            std::fill(seen.begin(), seen.end(), KeySeenToken{});
            obj_stamp = 1u;
        }
        return (static_cast<std::uint64_t>(doc_stamp) << 32u) | obj_stamp;
    }

    NEOJSON_NOINLINE void rehash(const std::size_t new_cap, const std::uint64_t seed) {
        std::vector<KeyInternSlot> fresh(new_cap, KeyInternSlot{});
        const std::size_t mask = new_cap - 1u;
        for (const KeyInternSlot& s : slots) {
            if (s.doc_stamp == doc_stamp) {
                const std::uint64_t h = hash_bytes_seeded(s.ptr, s.len, seed);
                std::size_t pos = static_cast<std::size_t>(h) & mask;
                while (fresh[pos].doc_stamp == doc_stamp) {
                    pos = (pos + 1u) & mask;
                }
                fresh[pos] = s;
            }
        }
        slots.swap(fresh);
    }

    NEOJSON_NOINLINE void grow_seen(const std::uint32_t needed_id) {
        std::size_t new_sz = seen.empty() ? 512u : seen.size() * 2u;
        while (new_sz <= needed_id) {
            new_sz *= 2u;
        }
        seen.resize(new_sz, KeySeenToken{});
    }

    NEOJSON_NOINLINE std::uint32_t intern_miss(const char* kp, const std::size_t kl,
                                             const std::uint64_t h, const std::uint64_t seed) {
        if ((used + 1u) * 2u >= slots.size()) {
            rehash(slots.size() * 2u, seed);
        }
        const std::size_t mask = slots.size() - 1u;
        const std::uint32_t h32 = static_cast<std::uint32_t>(h >> 32u);
        std::size_t pos = static_cast<std::size_t>(h) & mask;
        while (slots[pos].doc_stamp == doc_stamp) {
            if (slots[pos].hash32 == h32 && slots[pos].len == kl &&
                fast_memeq(slots[pos].ptr, kp, kl)) {
                return slots[pos].key_id;
            }
            pos = (pos + 1u) & mask;
        }
        const std::uint32_t id = next_key_id++;
        if (NEOJSON_UNLIKELY(id == kUnindexedKeyId)) {
            return kUnindexedKeyId;
        }
        if (id >= seen.size()) {
            grow_seen(id);
        }
        slots[pos] = KeyInternSlot{kp, h32, doc_stamp, static_cast<std::uint32_t>(kl), id};
        ++used;
        return id;
    }

    NEOJSON_ALWAYS_INLINE std::uint32_t intern(const char* kp, const std::size_t kl,
                                             const std::uint64_t seed) {
        if (kl == 0u) {
            return 0u;
        }
        if (NEOJSON_UNLIKELY(kl > 0xFFFFFFFFu)) {
            return kUnindexedKeyId;
        }
        const std::uint64_t h = hash_bytes_seeded(kp, kl, seed);
        const std::uint32_t h32 = static_cast<std::uint32_t>(h >> 32u);
        const std::size_t mask = slots.size() - 1u;
        std::size_t pos = static_cast<std::size_t>(h) & mask;
        for (;;) {
            const KeyInternSlot& s = slots[pos];
            if (s.doc_stamp != doc_stamp) {
                return intern_miss(kp, kl, h, seed);
            }
            if (s.hash32 == h32 && s.len == kl && fast_memeq(s.ptr, kp, kl)) {
                return s.key_id;
            }
            pos = (pos + 1u) & mask;
        }
    }
};

NEOJSON_ALWAYS_INLINE SharedString* alloc_arena_string_direct_inline(
    SharedYyDoc* sdoc, ThreadStrPool* pool, const char* p, const std::size_t len) {
    assert(len >= 16u);
    void* mem = sdoc->arena.alloc(sizeof(SharedString));
    SharedString* ss = nullptr;
    if (NEOJSON_LIKELY(pool != nullptr && !pool->dead && len <= 4096u)) {
        const std::size_t b = ThreadStrPool::bucket_for_len(len);
        auto& st = pool->buckets[b];
        if (NEOJSON_LIKELY(st.count != 0u)) {
            std::string& slot = st.slots[--st.count];
            ss = ::new (mem) SharedString(std::move(slot));
            ss->refcount.store(kArenaStringRef, std::memory_order_relaxed);
            ss->owner_doc = sdoc;
            try {
                const std::size_t cur_sz = ss->value.size();
                if (NEOJSON_LIKELY(len <= cur_sz)) {
                    if (NEOJSON_UNLIKELY(len != cur_sz)) {
                        ss->value.erase(len);
                    }
                    copy_str_ge16(ss->value.data(), p, len);
                } else {
                    ThreadStrPool::assign_grow_cold(ss->value, p, len);
                }
#if defined(__GLIBCXX__) && defined(_GLIBCXX_USE_CXX11_ABI) && _GLIBCXX_USE_CXX11_ABI == 1
                sdoc->arena.track_heap_string_fast(&ss->value);
#else
                if (!string_is_inline(ss->value)) {
                    sdoc->arena.track_heap_string_fast(&ss->value);
                }
#endif
            } catch (...) {
                ss->~SharedString();
                throw;
            }
            return ss;
        }
    }
    ss = ::new (mem) SharedString();
    ss->refcount.store(kArenaStringRef, std::memory_order_relaxed);
    ss->owner_doc = sdoc;
    try {
        if (pool != nullptr) {
            pool->acquire_into_cold(ss->value, p, len);
        } else {
            ThreadStrPool::assign_grow_cold(ss->value, p, len);
        }
#if defined(__GLIBCXX__) && defined(_GLIBCXX_USE_CXX11_ABI) && _GLIBCXX_USE_CXX11_ABI == 1
        sdoc->arena.track_heap_string_fast(&ss->value);
#else
        if (!string_is_inline(ss->value)) {
            sdoc->arena.track_heap_string_fast(&ss->value);
        }
#endif
    } catch (...) {
        ss->~SharedString();
        throw;
    }
    return ss;
}

NEOJSON_NOINLINE SharedString* alloc_arena_string_direct(SharedYyDoc* sdoc, ThreadStrPool* pool,
                                                       const char* p, const std::size_t len) {
    return alloc_arena_string_direct_inline(sdoc, pool, p, len);
}

NEOJSON_ALWAYS_INLINE std::uint64_t hash_long_str_fast(
    const std::uint64_t a, const std::uint64_t e, const std::size_t len) noexcept {
    std::uint64_t h =
        (a ^ (e * 0x9e3779b97f4a7c15ULL) ^ (static_cast<std::uint64_t>(len) << 48u)) *
        0xbf58476d1ce4e5b9ULL;
    h ^= h >> 32u;
    return h;
}

struct LongStrSlot {
    SharedString* ss = nullptr;
    const char* raw_ptr = nullptr;
    std::uint32_t hash32 = 0u;
    std::uint32_t stamp_and_len = 0u;
};
static_assert(sizeof(LongStrSlot) == 24u, "LongStrSlot must stay 24 bytes");

struct alignas(64) LongStrSet {
    LongStrSlot w[2]{};
    std::uint8_t next_way = 0u;
};
static_assert(sizeof(LongStrSet) == 64u, "LongStrSet must stay 64 bytes");

struct LongStrInternCache {
    static constexpr std::size_t kMaxSets = 4096u;
    static constexpr std::size_t kMaxInternLen = 16384u;
    static_assert(kMaxInternLen <= 65535u, "kMaxInternLen must fit in uint16_t");

    alignas(64) std::array<LongStrSet, kMaxSets> sets{};
    std::size_t set_mask = 1023u;
    std::uint16_t doc_stamp = 1u;

    void begin_doc(const std::size_t dat_read) noexcept {
        set_mask = (dat_read <= 1048576u) ? 1023u : 4095u;
        ++doc_stamp;
        if (NEOJSON_UNLIKELY(doc_stamp == 0u)) {
            sets.fill(LongStrSet{});
            doc_stamp = 1u;
        }
    }
};

#if defined(__SSE2__) || NEOJSON_X86_SIMD
struct alignas(16) ShortMaskTable16 {
    std::uint8_t row[16][16]{};
    constexpr ShortMaskTable16() noexcept {
        for (int n = 0; n < 16; ++n) {
            for (int j = 0; j < n; ++j) {
                row[n][j] = 0xFFu;
            }
        }
    }
};
static constexpr ShortMaskTable16 kShortMask16{};
#endif

struct alignas(32) ShortStrSlot {
    std::uint64_t w0 = 0u;
    std::uint64_t w1 = 0u;
    SharedString* ss = nullptr;
    std::uint64_t stamp_and_len = 0u;
};
static_assert(sizeof(ShortStrSlot) == 32u, "ShortStrSlot must stay 32 bytes");

struct ShortStrInternCache {
    static constexpr std::size_t kMaxSlots = 4096u;

    alignas(64) std::array<ShortStrSlot, kMaxSlots> slots{};
    std::size_t slot_mask = 1023u;
    std::uint32_t doc_stamp = 1u;

    void begin_doc(const std::size_t dat_read) noexcept {
        slot_mask = (dat_read <= 1048576u) ? 1023u : 4095u;
        ++doc_stamp;
        if (NEOJSON_UNLIKELY(doc_stamp == 0u)) {
            slots.fill(ShortStrSlot{});
            doc_stamp = 1u;
        }
    }
};

inline constexpr std::size_t kMaxDirectDfsDepth = 32u;

struct ArenaWorkItem {
    yyjson_val* yy_ctn;
    json* dst_slot;
};

struct ShapeKeyDesc {
    std::uint32_t byte_off = 0u;
    std::uint16_t len = 0u;
    std::uint16_t mask16 = 0u;
};
static_assert(sizeof(ShapeKeyDesc) == 8u, "ShapeKeyDesc must stay 8 bytes");

struct ShapeEntry {
    std::uint32_t doc_stamp = 0u;
    std::uint32_t count = 0u;
    std::uint32_t descs_off = 0u;
    std::uint16_t key0_len = 0u;
    std::uint16_t max_key_len = 0u;
    std::uint64_t key0_w0 = 0u;
    const std::string* keys = nullptr;
};
static_assert(sizeof(ShapeEntry) == 32u, "ShapeEntry must stay 32 bytes");

struct ArenaShapeCache {
    static constexpr std::size_t kNumSets = 32u;
    static constexpr std::size_t kWays = 4u;
    static constexpr std::size_t kMaxCachedObjKeys = 64u;

    std::array<ShapeEntry, kNumSets * kWays> entries{};
    std::array<std::uint8_t, kNumSets> next_way{};
    std::vector<char> key_bytes;
    std::size_t key_bytes_len = 0u;
    std::vector<ShapeKeyDesc> key_descs;
    std::uint32_t doc_stamp = 1u;

    void begin_doc() {
        key_bytes.clear();
        key_bytes_len = 0u;
        key_descs.clear();
        ++doc_stamp;
        if (NEOJSON_UNLIKELY(doc_stamp == 0u)) {
            entries.fill(ShapeEntry{});
            next_way.fill(0u);
            doc_stamp = 1u;
        }
    }

    NEOJSON_ALWAYS_INLINE static std::size_t compute_set(const std::size_t len, const char* p0,
                                                         const std::size_t l0) noexcept {
        const std::uint32_t c0 = (l0 > 0u) ? static_cast<std::uint8_t>(p0[0]) : 0u;
        const std::uint32_t clast = (l0 > 0u) ? static_cast<std::uint8_t>(p0[l0 - 1u]) : 0u;
        std::uint32_t sh = static_cast<std::uint32_t>(len) * 2654435761u ^
                           static_cast<std::uint32_t>(l0) * 40503u ^
                           (((c0 << 8u) | clast) * 2246822519u);
        sh ^= sh >> 16u;
        return static_cast<std::size_t>((sh ^ (sh >> 8u)) & (kNumSets - 1u));
    }

    NEOJSON_NOINLINE void record(const std::size_t len, const std::string* dst_keys) {
        std::size_t total_bytes = 0u;
        std::size_t max_kl = 0u;
        for (std::size_t i = 0u; i < len; ++i) {
            const std::size_t kl = dst_keys[i].size();
            if (NEOJSON_UNLIKELY(kl > 65535u)) {
                return;
            }
            if (kl > max_kl) {
                max_kl = kl;
            }
            total_bytes += kl;
        }
        if (NEOJSON_UNLIKELY(key_descs.size() + len > 0xFFFFFFFFu ||
                             key_bytes_len + total_bytes + 32u > 0xFFFFFFFFu)) {
            return;
        }
        if (key_descs.capacity() < 512u) {
            key_descs.reserve(512u);
        }
        if (key_bytes.capacity() < 4096u) {
            key_bytes.reserve(4096u);
        }
        const std::size_t l0 = dst_keys[0].size();
        const char* const p0 = dst_keys[0].data();
        std::uint64_t k0_w0 = 0u;
        const std::size_t k0_copy = (l0 >= 8u) ? 8u : l0;
        if (k0_copy > 0u) {
            std::memcpy(&k0_w0, p0, k0_copy);
        }
        const std::size_t set_idx = compute_set(len, p0, l0);
        const std::size_t base = set_idx * kWays;
        std::size_t slot_idx = base;
        bool found_free = false;
        for (std::size_t w = 0u; w < kWays; ++w) {
            if (entries[base + w].doc_stamp != doc_stamp) {
                slot_idx = base + w;
                found_free = true;
                break;
            }
        }
        if (!found_free) {
            slot_idx = base + (next_way[set_idx]++ & (kWays - 1u));
        }
        key_bytes.resize(key_bytes_len);
        const auto d_off = static_cast<std::uint32_t>(key_descs.size());
        for (std::size_t i = 0u; i < len; ++i) {
            const std::string& kstr = dst_keys[i];
            const std::size_t kl = kstr.size();
            const std::uint16_t mask16 =
                static_cast<std::uint16_t>((kl >= 16u) ? 0xFFFFu : ((1u << kl) - 1u));
            key_descs.push_back(ShapeKeyDesc{
                static_cast<std::uint32_t>(key_bytes.size()),
                static_cast<std::uint16_t>(kl),
                mask16,
            });
            key_bytes.insert(key_bytes.end(), kstr.data(), kstr.data() + kl);
        }
        key_bytes_len = key_bytes.size();
        key_bytes.resize(key_bytes_len + 32u, '\0');
        entries[slot_idx] = ShapeEntry{
            doc_stamp,
            static_cast<std::uint32_t>(len),
            d_off,
            static_cast<std::uint16_t>(l0),
            static_cast<std::uint16_t>(max_kl),
            k0_w0,
            dst_keys,
        };
    }
};

struct ArenaMaterializeScratch {
    std::vector<ArenaWorkItem> workstack;
    ArenaShapeCache shape_cache;
    yyjson_val* dfs_vals[kMaxDirectDfsDepth + 1u][ArenaShapeCache::kMaxCachedObjKeys];
    std::uint8_t dfs_slot_idx[kMaxDirectDfsDepth + 1u][ArenaShapeCache::kMaxCachedObjKeys];
};

struct ArenaMatTLS {
    KeyInternTable key_intern;
    LongStrInternCache long_cache;
    ShortStrInternCache short_cache;
    ArenaMaterializeScratch scratch;
};

inline ArenaMatTLS& tls_arena_mat() {
    thread_local std::unique_ptr<ArenaMatTLS> tls;
    if (NEOJSON_UNLIKELY(tls == nullptr)) {
        tls = std::make_unique<ArenaMatTLS>();
    }
    return *tls;
}

struct ArenaMatCtx {
    SharedYyDoc* sdoc;
    KeyInternTable& intern;
    ArenaMaterializeScratch& scratch;
    SharedString* empty_str;
    LongStrInternCache& long_cache;
    ShortStrInternCache& short_cache;
    ThreadStrPool* str_pool;
    std::uint64_t seed;
    std::size_t dat_read;
    std::size_t depth = 0u;
    bool str_pool_padded_32 = false;
    bool saw_dup = false;
};

struct MatDepthGuard {
    std::size_t& d;
    explicit MatDepthGuard(std::size_t& v) noexcept : d(v) {
        ++d;
    }
    ~MatDepthGuard() noexcept {
        --d;
    }
    MatDepthGuard(const MatDepthGuard&) = delete;
    MatDepthGuard& operator=(const MatDepthGuard&) = delete;
};

NEOJSON_NOINLINE SharedString* alloc_arena_string_short_miss(
    SharedYyDoc* sdoc, const char* p, const std::size_t len, ShortStrSlot& slot,
    const std::uint64_t w0, const std::uint64_t w1, const std::uint64_t stamp_and_len) {
    const std::size_t n = (len <= 15u) ? len : 0u;
    void* mem = sdoc->arena.alloc(sizeof(SharedString));
    auto* ss = ::new (mem) SharedString(p, n);
    ss->refcount.store(kArenaStringRef, std::memory_order_relaxed);
    ss->owner_doc = sdoc;
#if !(defined(__GLIBCXX__) && defined(_GLIBCXX_USE_CXX11_ABI) && _GLIBCXX_USE_CXX11_ABI == 1) && \
    !defined(_LIBCPP_VERSION) && !defined(_MSVC_STL_VERSION)
    if (!string_is_inline(ss->value)) {
        sdoc->arena.track_heap_string_fast(&ss->value);
    }
#endif
    slot = ShortStrSlot{w0, w1, ss, stamp_and_len};
    return ss;
}

NEOJSON_NOINLINE SharedString* alloc_arena_string_short(ArenaMatCtx& ctx, const char* p,
                                                        const std::size_t len) {
    assert(len >= 1u && len <= 15u);
    std::uint64_t w0 = 0u;
    std::uint64_t w1 = 0u;
#if defined(__SSE2__) || NEOJSON_X86_SIMD
    if (NEOJSON_LIKELY(ctx.str_pool_padded_32)) {
        const __m128i v = _mm_and_si128(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)),
            _mm_load_si128(reinterpret_cast<const __m128i*>(kShortMask16.row[len & 15u])));
        w0 = static_cast<std::uint64_t>(_mm_cvtsi128_si64(v));
        w1 = static_cast<std::uint64_t>(_mm_cvtsi128_si64(_mm_srli_si128(v, 8)));
    } else
#endif
    {
        std::uint64_t buf[2] = {0u, 0u};
        std::memcpy(buf, p, len);
        w0 = buf[0];
        w1 = buf[1];
    }
    const std::uint64_t h =
        (w0 ^ (w1 * 0x9e3779b97f4a7c15ULL) ^ (static_cast<std::uint64_t>(len) << 48u)) *
        0xbf58476d1ce4e5b9ULL;
    ShortStrInternCache& sc = ctx.short_cache;
    ShortStrSlot& slot = sc.slots[static_cast<std::size_t>(h >> 32u) & sc.slot_mask];
    const std::uint64_t expected_sl =
        (static_cast<std::uint64_t>(sc.doc_stamp) << 8u) | static_cast<std::uint64_t>(len);
    if (slot.stamp_and_len == expected_sl && slot.w0 == w0 && slot.w1 == w1) {
        return slot.ss;
    }
    return alloc_arena_string_short_miss(ctx.sdoc, p, len, slot, w0, w1, expected_sl);
}

NEOJSON_ALWAYS_INLINE bool long_str_eq_after_ae(const char* a_ptr, const char* b_ptr,
                                                 const std::size_t len,
                                                 const std::uint64_t b_first8,
                                                 const std::uint64_t b_last8) noexcept {
    assert(len >= 16u);
    std::uint64_t af = 0u;
    std::uint64_t al = 0u;
    std::memcpy(&af, a_ptr, 8u);
    std::memcpy(&al, a_ptr + len - 8u, 8u);
    if (((af ^ b_first8) | (al ^ b_last8)) != 0u) {
        return false;
    }
    if (len <= 16u) {
        return true;
    }
    if (len <= 32u) {
        std::uint64_t a1 = 0u, b1 = 0u, a2 = 0u, b2 = 0u;
        std::memcpy(&a1, a_ptr + 8u, 8u);
        std::memcpy(&b1, b_ptr + 8u, 8u);
        std::memcpy(&a2, a_ptr + len - 16u, 8u);
        std::memcpy(&b2, b_ptr + len - 16u, 8u);
        return ((a1 ^ b1) | (a2 ^ b2)) == 0u;
    }
    if (len <= 48u) {
        std::uint64_t a1 = 0u, b1 = 0u, a2 = 0u, b2 = 0u;
        std::uint64_t a3 = 0u, b3 = 0u, a4 = 0u, b4 = 0u;
        std::memcpy(&a1, a_ptr + 8u, 8u);
        std::memcpy(&b1, b_ptr + 8u, 8u);
        std::memcpy(&a2, a_ptr + 16u, 8u);
        std::memcpy(&b2, b_ptr + 16u, 8u);
        std::memcpy(&a3, a_ptr + len - 24u, 8u);
        std::memcpy(&b3, b_ptr + len - 24u, 8u);
        std::memcpy(&a4, a_ptr + len - 16u, 8u);
        std::memcpy(&b4, b_ptr + len - 16u, 8u);
        return ((a1 ^ b1) | (a2 ^ b2) | (a3 ^ b3) | (a4 ^ b4)) == 0u;
    }
    return std::memcmp(a_ptr + 8u, b_ptr + 8u, len - 16u) == 0;
}

NEOJSON_NOINLINE SharedString* alloc_arena_string_long_miss(ArenaMatCtx& ctx, const char* p,
                                                            const std::size_t len,
                                                            LongStrSlot& slot,
                                                            const std::uint32_t h32,
                                                            const std::uint32_t stamp_and_len) {
    SharedString* ss = alloc_arena_string_direct_inline(ctx.sdoc, ctx.str_pool, p, len);
    slot = LongStrSlot{ss, p, h32, stamp_and_len};
    return ss;
}

NEOJSON_NOINLINE SharedString* alloc_arena_string_long(ArenaMatCtx& ctx, const char* p,
                                                       const std::size_t len) {
    if (NEOJSON_UNLIKELY(len > LongStrInternCache::kMaxInternLen)) {
        return alloc_arena_string_direct(ctx.sdoc, ctx.str_pool, p, len);
    }
    std::uint64_t a = 0u;
    std::uint64_t e = 0u;
    std::memcpy(&a, p, 8u);
    std::memcpy(&e, p + len - 8u, 8u);
    const std::uint64_t h = hash_long_str_fast(a, e, len);
    const std::uint32_t h32 = static_cast<std::uint32_t>(h >> 32u);
    LongStrInternCache& lc = ctx.long_cache;
    LongStrSet& set = lc.sets[static_cast<std::size_t>(h) & lc.set_mask];
    const std::uint32_t expected_sl =
        (static_cast<std::uint32_t>(lc.doc_stamp) << 16u) | static_cast<std::uint32_t>(len);
    if (set.w[0].stamp_and_len == expected_sl && set.w[0].hash32 == h32 &&
        long_str_eq_after_ae(set.w[0].raw_ptr, p, len, a, e)) {
        return set.w[0].ss;
    }
    if (set.w[1].stamp_and_len == expected_sl && set.w[1].hash32 == h32 &&
        long_str_eq_after_ae(set.w[1].raw_ptr, p, len, a, e)) {
        return set.w[1].ss;
    }
    LongStrSlot* victim = nullptr;
    if ((set.w[0].stamp_and_len >> 16u) != lc.doc_stamp) {
        victim = &set.w[0];
    } else if ((set.w[1].stamp_and_len >> 16u) != lc.doc_stamp) {
        victim = &set.w[1];
    } else {
        victim = &set.w[set.next_way++ & 1u];
    }
    return alloc_arena_string_long_miss(ctx, p, len, *victim, h32, expected_sl);
}

NEOJSON_ALWAYS_INLINE bool is_non_str_leaf_yyval(const yyjson_val* v) noexcept {
    return !unsafe_yyjson_is_str(v) && (!yyjson_is_ctn(v) || unsafe_yyjson_get_len(v) == 0u);
}

NEOJSON_ALWAYS_INLINE void init_arena_non_str_leaf(json* dst, yyjson_val* val) noexcept {
    const std::uint8_t tag = yyjson_get_tag(val);
    if ((tag & YYJSON_TYPE_MASK) == YYJSON_TYPE_NUM) {
        if (tag == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT)) {
            json_access::init_arena_uint(
                dst, static_cast<json::number_unsigned_t>(unsafe_yyjson_get_uint(val)));
        } else if (tag == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL)) {
            json_access::init_arena_real(
                dst, static_cast<json::number_float_t>(unsafe_yyjson_get_real(val)));
        } else {
            json_access::init_arena_sint(
                dst, static_cast<json::number_integer_t>(unsafe_yyjson_get_sint(val)));
        }
        return;
    }
    json_access::init_arena_const_leaf(dst, tag);
}

NEOJSON_ALWAYS_INLINE void construct_arena_leaf(ArenaMatCtx& ctx, json* dst, yyjson_val* val) {
    if (unsafe_yyjson_is_str(val)) {
        const std::size_t kl = unsafe_yyjson_get_len(val);
        if (kl == 0u) {
            json_access::init_arena_string(dst, ctx.empty_str);
            return;
        }
        const char* kp = unsafe_yyjson_get_str(val);
        SharedString* ss = (kl <= 15u) ? alloc_arena_string_short(ctx, kp, kl)
                                       : alloc_arena_string_long(ctx, kp, kl);
        json_access::init_arena_string(dst, ss);
        return;
    }
    init_arena_non_str_leaf(dst, val);
}

NEOJSON_ALWAYS_INLINE void construct_arena_scalar(ArenaMatCtx& ctx, json* dst, yyjson_val* val) {
    construct_arena_leaf(ctx, dst, val);
}

NEOJSON_NOINLINE ArenaArray* try_build_inline_leaf_arena_array(ArenaMatCtx& ctx, yyjson_val* val,
                                                               const std::size_t len) {
    if (len == 2u) {
        yyjson_val* e0 = unsafe_yyjson_get_first(val);
        yyjson_val* e1 = e0 + 1u;
        if (yyjson_get_tag(e0) == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL) &&
            yyjson_get_tag(e1) == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL)) {
            void* mem = ctx.sdoc->arena.alloc(sizeof(ArenaArray) + 2u * sizeof(json));
            auto* aa = ::new (mem) ArenaArray();
            aa->count = 2u;
            aa->doc = ctx.sdoc;
            json* out = aa->elems();
            json_access::init_arena_real(
                &out[0], static_cast<json::number_float_t>(unsafe_yyjson_get_real(e0)));
            json_access::init_arena_real(
                &out[1], static_cast<json::number_float_t>(unsafe_yyjson_get_real(e1)));
            return aa;
        }
        if (yyjson_get_tag(e0) == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT) &&
            yyjson_get_tag(e1) == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT)) {
            void* mem = ctx.sdoc->arena.alloc(sizeof(ArenaArray) + 2u * sizeof(json));
            auto* aa = ::new (mem) ArenaArray();
            aa->count = 2u;
            aa->doc = ctx.sdoc;
            json* out = aa->elems();
            json_access::init_arena_uint(
                &out[0], static_cast<json::number_unsigned_t>(unsafe_yyjson_get_uint(e0)));
            json_access::init_arena_uint(
                &out[1], static_cast<json::number_unsigned_t>(unsafe_yyjson_get_uint(e1)));
            return aa;
        }
        if (yyjson_is_ctn(e0)) {
            return nullptr;
        }
        e1 = unsafe_yyjson_get_next(e0);
        if (yyjson_is_ctn(e1)) {
            return nullptr;
        }
        void* mem = ctx.sdoc->arena.alloc(sizeof(ArenaArray) + 2u * sizeof(json));
        auto* aa = ::new (mem) ArenaArray();
        aa->count = 2u;
        aa->doc = ctx.sdoc;
        json* out = aa->elems();
        construct_arena_scalar(ctx, &out[0], e0);
        construct_arena_scalar(ctx, &out[1], e1);
        return aa;
    }
    yyjson_val* e0 = unsafe_yyjson_get_first(val);
    if (yyjson_is_ctn(e0)) {
        return nullptr;
    }
    void* mem = ctx.sdoc->arena.alloc(sizeof(ArenaArray) + sizeof(json));
    auto* aa = ::new (mem) ArenaArray();
    aa->count = 1u;
    aa->doc = ctx.sdoc;
    construct_arena_scalar(ctx, aa->elems(), e0);
    return aa;
}

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
alignas(64) static constexpr std::uint64_t kU64LenMask[9] = {
    0x0000000000000000ULL, 0x00000000000000FFULL, 0x000000000000FFFFULL,
    0x0000000000FFFFFFULL, 0x00000000FFFFFFFFULL, 0x000000FFFFFFFFFFULL,
    0x0000FFFFFFFFFFFFULL, 0x00FFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
};
#endif

NEOJSON_ALWAYS_INLINE std::uint64_t load_key0_w0(const bool padded32, const char* p,
                                                 const std::size_t n) noexcept {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (NEOJSON_LIKELY(padded32)) {
        std::uint64_t w = 0u;
        std::memcpy(&w, p, 8u);
        return w & kU64LenMask[(n >= 8u) ? 8u : n];
    }
#else
    (void)padded32;
#endif
    std::uint64_t w = 0u;
    const std::size_t k = (n >= 8u) ? 8u : n;
    if (k > 0u) {
        std::memcpy(&w, p, k);
    }
    return w;
}

#if defined(__SSE2__) || NEOJSON_X86_SIMD
alignas(64) static constexpr std::uint32_t kKeyLenMask32[33] = {
    0x00000000u, 0x00000001u, 0x00000003u, 0x00000007u, 0x0000000Fu, 0x0000001Fu, 0x0000003Fu,
    0x0000007Fu, 0x000000FFu, 0x000001FFu, 0x000003FFu, 0x000007FFu, 0x00000FFFu, 0x00001FFFu,
    0x00003FFFu, 0x00007FFFu, 0x0000FFFFu, 0x0001FFFFu, 0x0003FFFFu, 0x0007FFFFu, 0x000FFFFFu,
    0x001FFFFFu, 0x003FFFFFu, 0x007FFFFFu, 0x00FFFFFFu, 0x01FFFFFFu, 0x03FFFFFFu, 0x07FFFFFFu,
    0x0FFFFFFFu, 0x1FFFFFFFu, 0x3FFFFFFFu, 0x7FFFFFFFu, 0xFFFFFFFFu,
};

NEOJSON_ALWAYS_INLINE bool shape_key_eq_padded32(const char* a, const char* b,
                                                 const std::size_t n) noexcept {
    assert(n <= 32u);
    const __m128i va0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a));
    const __m128i vb0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b));
    const __m128i va1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + 16u));
    const __m128i vb1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + 16u));
    const std::uint32_t eq0 =
        static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(va0, vb0)));
    const std::uint32_t eq1 =
        static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(va1, vb1)));
    const std::uint32_t eq = eq0 | (eq1 << 16u);
    return ((~eq) & kKeyLenMask32[n]) == 0u;
}
#endif

NEOJSON_NOINLINE bool shape_key_eq_slow(const char* a, const char* b,
                                        const std::size_t n) noexcept {
    return fast_memeq(a, b, n);
}

NEOJSON_ALWAYS_INLINE bool shape_key_eq_ctx(const bool padded32, const char* yy_s,
                                            const char* shape_s, const std::size_t n) noexcept {
#if defined(__SSE2__) || NEOJSON_X86_SIMD
    if (NEOJSON_LIKELY(padded32 && n <= 32u)) {
        return shape_key_eq_padded32(yy_s, shape_s, n);
    }
#else
    (void)padded32;
#endif
    return shape_key_eq_slow(yy_s, shape_s, n);
}

NEOJSON_ALWAYS_INLINE bool shape_key_eq_desc(const bool padded32, const char* yy_s,
                                             const std::size_t yy_len, const char* key_bytes,
                                             const ShapeKeyDesc d) noexcept {
    if (NEOJSON_UNLIKELY(yy_len != d.len)) {
        return false;
    }
#if defined(__SSE2__) || NEOJSON_X86_SIMD
    if (NEOJSON_LIKELY(padded32)) {
        const char* const shape_s = key_bytes + d.byte_off;
        const __m128i va0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(yy_s));
        const __m128i vb0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(shape_s));
        const std::uint32_t eq0 =
            static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(va0, vb0)));
        if (NEOJSON_UNLIKELY(((~eq0) & d.mask16) != 0u)) {
            return false;
        }
        if (NEOJSON_LIKELY(d.len <= 16u)) {
            return true;
        }
        if (NEOJSON_LIKELY(d.len <= 32u)) {
            const __m128i va1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(yy_s + 16u));
            const __m128i vb1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(shape_s + 16u));
            const std::uint32_t eq1 =
                static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(va1, vb1)));
            return ((~eq1) & kKeyLenMask32[d.len - 16u]) == 0u;
        }
        return shape_key_eq_slow(yy_s + 16u, shape_s + 16u, d.len - 16u);
    }
#else
    (void)padded32;
#endif
    return shape_key_eq_slow(yy_s, key_bytes + d.byte_off, d.len);
}

struct ShapeBuildRes {
    ArenaObject* ao = nullptr;
    ShapeEntry se{};
};

NEOJSON_NOINLINE ArenaArray* build_arena_array_node(ArenaMatCtx& ctx, yyjson_val* val);
NEOJSON_NOINLINE ArenaObject* build_arena_object_node(ArenaMatCtx& ctx, yyjson_val* val,
                                                      bool* own_unique_out, bool known_unique);

NEOJSON_NOINLINE void init_arena_nonempty_ctn(ArenaMatCtx& ctx, json* dst, yyjson_val* cv,
                                              const std::size_t clen) {
    if (clen <= 2u && yyjson_is_arr(cv)) {
        if (ArenaArray* leaf = try_build_inline_leaf_arena_array(ctx, cv, clen)) {
            json_access::init_arena_array_ptr(dst, leaf);
            return;
        }
    }
    if (NEOJSON_LIKELY(ctx.depth < kMaxDirectDfsDepth)) {
        MatDepthGuard dguard(ctx.depth);
        if (yyjson_is_arr(cv)) {
            json_access::init_arena_array_ptr(dst, build_arena_array_node(ctx, cv));
        } else {
            json_access::init_arena_object_ptr(
                dst, build_arena_object_node(ctx, cv, nullptr, false));
        }
        return;
    }
    json_access::init_arena_null(dst);
    ctx.scratch.workstack.push_back(ArenaWorkItem{cv, dst});
}

inline void init_arena_slot(ArenaMatCtx& ctx, json* dst, yyjson_val* cv) {
    if (!yyjson_is_ctn(cv)) {
        construct_arena_scalar(ctx, dst, cv);
        return;
    }
    const std::size_t clen = unsafe_yyjson_get_len(cv);
    if (clen == 0u) {
        if (yyjson_is_arr(cv)) {
            json_access::init_arena_empty_array(dst);
        } else {
            json_access::init_arena_empty_object(dst);
        }
        return;
    }
    init_arena_nonempty_ctn(ctx, dst, cv, clen);
}

NEOJSON_NOINLINE ArenaObject* try_build_arena_object_with_entry(ArenaMatCtx& ctx, yyjson_val* val,
                                                                const ShapeEntry& se,
                                                                const bool check_key0) {
    const std::size_t len = se.count;
    if (NEOJSON_UNLIKELY(len == 0u || len > ArenaShapeCache::kMaxCachedObjKeys)) {
        return nullptr;
    }
    const char* const kb = ctx.scratch.shape_cache.key_bytes.data();
    const ShapeKeyDesc* const descs = ctx.scratch.shape_cache.key_descs.data() + se.descs_off;
    const bool padded = ctx.str_pool_padded_32;
    yyjson_val* const k0 = unsafe_yyjson_get_first(val);
    if (check_key0 &&
        NEOJSON_UNLIKELY(!shape_key_eq_desc(
            padded, unsafe_yyjson_get_str(k0), unsafe_yyjson_get_len(k0), kb, descs[0]))) {
        return nullptr;
    }
    const std::string* const se_keys = se.keys;

    if (val->uni.ofs == (2u * len + 1u) * sizeof(yyjson_val)) {
#if defined(__SSE2__) || NEOJSON_X86_SIMD
        if (NEOJSON_LIKELY(padded && se.max_key_len <= 16u)) {
            for (std::size_t i = 1u; i < len; ++i) {
                yyjson_val* const ck = val + 1u + 2u * i;
                const ShapeKeyDesc d = descs[i];
                if (NEOJSON_UNLIKELY(unsafe_yyjson_get_len(ck) != d.len)) {
                    return nullptr;
                }
                const __m128i va = _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(unsafe_yyjson_get_str(ck)));
                const __m128i vb =
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(kb + d.byte_off));
                const std::uint32_t eq =
                    static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(va, vb)));
                if (NEOJSON_UNLIKELY(((~eq) & d.mask16) != 0u)) {
                    return nullptr;
                }
            }
        } else
#endif
        {
            for (std::size_t i = 1u; i < len; ++i) {
                yyjson_val* const ck = val + 1u + 2u * i;
                if (NEOJSON_UNLIKELY(!shape_key_eq_desc(
                        padded, unsafe_yyjson_get_str(ck), unsafe_yyjson_get_len(ck), kb,
                        descs[i]))) {
                    return nullptr;
                }
            }
        }
        void* mem = ctx.sdoc->arena.alloc(sizeof(ArenaObject) + len * sizeof(json));
        auto* ao = ::new (mem) ArenaObject();
        ao->count = static_cast<std::uint32_t>(len);
        ao->doc = ctx.sdoc;
        ao->keys_ptr = se_keys;
        json* out = ao->vals();
        for (std::size_t i = 0u; i < len; ++i) {
            construct_arena_leaf(ctx, &out[i], val + 2u + 2u * i);
        }
        return ao;
    }

    if (len >= 2u && !yyjson_is_ctn(k0 + 1u)) {
        yyjson_val* const k1 = k0 + 2u;
        if (NEOJSON_UNLIKELY(!shape_key_eq_desc(
                padded, unsafe_yyjson_get_str(k1), unsafe_yyjson_get_len(k1), kb, descs[1]))) {
            return nullptr;
        }
    }

    char* const ao_start = ctx.sdoc->arena.bump_ptr;
    void* mem = ctx.sdoc->arena.alloc(sizeof(ArenaObject) + len * sizeof(json));
    char* const ao_end = ctx.sdoc->arena.bump_ptr;
    auto* ao = ::new (mem) ArenaObject();
    ao->count = static_cast<std::uint32_t>(len);
    ao->doc = ctx.sdoc;
    ao->keys_ptr = se_keys;
    json* const out = ao->vals();

    assert(ctx.depth <= kMaxDirectDfsDepth);
    yyjson_val** const p_cv = ctx.scratch.dfs_vals[ctx.depth];
    std::uint8_t* const p_idx = ctx.scratch.dfs_slot_idx[ctx.depth];
    std::size_t pending_cnt = 0u;

    yyjson_val* ck = nullptr;
    yyjson_val* const cv0 = k0 + 1u;
    if (NEOJSON_LIKELY(!yyjson_is_ctn(cv0))) {
        construct_arena_leaf(ctx, &out[0], cv0);
        ck = cv0 + 1u;
    } else if (unsafe_yyjson_get_len(cv0) == 0u) {
        if (yyjson_is_arr(cv0)) {
            json_access::init_arena_empty_array(&out[0]);
        } else {
            json_access::init_arena_empty_object(&out[0]);
        }
        ck = cv0 + 1u;
    } else {
        p_idx[0] = 0u;
        p_cv[0] = cv0;
        pending_cnt = 1u;
        ck = unsafe_yyjson_get_next(cv0);
    }

    for (std::size_t i = 1u; i < len; ++i) {
        if (NEOJSON_UNLIKELY(!shape_key_eq_desc(
                padded, unsafe_yyjson_get_str(ck), unsafe_yyjson_get_len(ck), kb, descs[i]))) {
            if (mem == static_cast<void*>(ao_start) && ctx.sdoc->arena.bump_ptr == ao_end) {
                ctx.sdoc->arena.bump_ptr = ao_start;
            }
            return nullptr;
        }
        yyjson_val* const cv = ck + 1u;
        if (NEOJSON_LIKELY(!yyjson_is_ctn(cv))) {
            construct_arena_leaf(ctx, &out[i], cv);
            if (i + 1u < len) {
                ck = cv + 1u;
            }
        } else if (unsafe_yyjson_get_len(cv) == 0u) {
            if (yyjson_is_arr(cv)) {
                json_access::init_arena_empty_array(&out[i]);
            } else {
                json_access::init_arena_empty_object(&out[i]);
            }
            if (i + 1u < len) {
                ck = cv + 1u;
            }
        } else {
            p_idx[pending_cnt] = static_cast<std::uint8_t>(i);
            p_cv[pending_cnt] = cv;
            ++pending_cnt;
            if (i + 1u < len) {
                ck = unsafe_yyjson_get_next(cv);
            }
        }
    }

    for (std::size_t p = 0u; p < pending_cnt; ++p) {
        yyjson_val* const cval = p_cv[p];
        init_arena_nonempty_ctn(ctx, &out[p_idx[p]], cval, unsafe_yyjson_get_len(cval));
    }
    return ao;
}

NEOJSON_NOINLINE ShapeBuildRes try_build_from_shape_cache_rest(
    ArenaMatCtx& ctx, yyjson_val* val, const ShapeEntry* ways, const std::size_t start_w,
    const std::uint32_t stamp, const std::size_t len, const char* p0, const std::size_t l0,
    const std::uint64_t k0_w0) {
    const char* const kb = ctx.scratch.shape_cache.key_bytes.data();
    const ShapeKeyDesc* const kdescs = ctx.scratch.shape_cache.key_descs.data();
    const bool padded = ctx.str_pool_padded_32;
    for (std::size_t w = start_w; w < ArenaShapeCache::kWays; ++w) {
        const ShapeEntry& se = ways[w];
        if (se.doc_stamp == stamp && se.count == len && se.key0_len == l0 &&
            se.key0_w0 == k0_w0 &&
            (l0 <= 8u || shape_key_eq_ctx(padded, p0, kb + kdescs[se.descs_off].byte_off, l0))) {
            if (ArenaObject* ao = try_build_arena_object_with_entry(ctx, val, se, false)) {
                return ShapeBuildRes{ao, se};
            }
        }
    }
    return ShapeBuildRes{};
}

NEOJSON_ALWAYS_INLINE ShapeBuildRes find_and_build_shape(ArenaMatCtx& ctx, yyjson_val* val,
                                                         const std::size_t len, const char* p0,
                                                         const std::size_t l0) {
    const std::size_t set_idx = ArenaShapeCache::compute_set(len, p0, l0);
    const std::uint32_t stamp = ctx.scratch.shape_cache.doc_stamp;
    const ShapeEntry* ways = &ctx.scratch.shape_cache.entries[set_idx * ArenaShapeCache::kWays];
    const char* const kb = ctx.scratch.shape_cache.key_bytes.data();
    const ShapeKeyDesc* const kdescs = ctx.scratch.shape_cache.key_descs.data();
    const bool padded = ctx.str_pool_padded_32;
    const std::uint64_t k0_w0 = load_key0_w0(padded, p0, l0);
    const ShapeEntry* cand = nullptr;
    std::size_t cand_w = 0u;
    for (std::size_t w = 0u; w < ArenaShapeCache::kWays; ++w) {
        const ShapeEntry& se = ways[w];
        if (se.doc_stamp == stamp && se.count == len && se.key0_len == l0 &&
            se.key0_w0 == k0_w0 &&
            (l0 <= 8u || shape_key_eq_ctx(padded, p0, kb + kdescs[se.descs_off].byte_off, l0))) {
            cand = &se;
            cand_w = w;
            break;
        }
    }
    if (cand != nullptr) {
        if (ArenaObject* ao = try_build_arena_object_with_entry(ctx, val, *cand, false)) {
            return ShapeBuildRes{ao, *cand};
        }
        if (NEOJSON_UNLIKELY(cand_w + 1u < ArenaShapeCache::kWays)) {
            return try_build_from_shape_cache_rest(ctx, val, ways, cand_w + 1u, stamp, len, p0, l0,
                                                   k0_w0);
        }
    }
    return ShapeBuildRes{};
}

NEOJSON_NOINLINE ShapeBuildRes try_build_from_shape_cache(ArenaMatCtx& ctx, yyjson_val* val,
                                                        const std::size_t len, const char* p0,
                                                        const std::size_t l0) {
    return find_and_build_shape(ctx, val, len, p0, l0);
}

NEOJSON_NOINLINE void build_arena_array_obj2_run(ArenaMatCtx& ctx, json* out, const std::size_t len,
                                                 std::size_t& i_io, yyjson_val*& cur_io,
                                                 const ShapeEntry& cached_se) {
    const ShapeKeyDesc* const descs = ctx.scratch.shape_cache.key_descs.data() + cached_se.descs_off;
    const ShapeKeyDesc d0 = descs[0];
    const ShapeKeyDesc d1 = descs[1];
    const char* const kb = ctx.scratch.shape_cache.key_bytes.data();
    const bool padded = ctx.str_pool_padded_32;
#if defined(__SSE2__) || NEOJSON_X86_SIMD
    const bool fast_sse = padded && (cached_se.max_key_len <= 16u);
    const __m128i k0v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(kb + d0.byte_off));
    const __m128i k1v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(kb + d1.byte_off));
    auto key_eq = [&](yyjson_val* ck, const ShapeKeyDesc d, const __m128i kv) noexcept -> bool {
        if (NEOJSON_LIKELY(fast_sse)) {
            if (NEOJSON_UNLIKELY(unsafe_yyjson_get_len(ck) != d.len)) {
                return false;
            }
            const __m128i av = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(unsafe_yyjson_get_str(ck)));
            const std::uint32_t eq =
                static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(av, kv)));
            return ((~eq) & d.mask16) == 0u;
        }
        return shape_key_eq_desc(padded, unsafe_yyjson_get_str(ck), unsafe_yyjson_get_len(ck), kb,
                                 d);
    };
#else
    const int k0v = 0;
    const int k1v = 0;
    auto key_eq = [&](yyjson_val* ck, const ShapeKeyDesc d, const int) noexcept -> bool {
        return shape_key_eq_desc(padded, unsafe_yyjson_get_str(ck), unsafe_yyjson_get_len(ck), kb,
                                 d);
    };
#endif
    static_assert(sizeof(ArenaObject) + 2u * sizeof(json) == 64u,
                  "2-entry ArenaObject leaf must be 64 bytes");
    std::size_t i = i_io;
    yyjson_val* cur = cur_io;
    char* bp = ctx.sdoc->arena.bump_ptr;
    char* bend = ctx.sdoc->arena.bump_end;

    auto emit_ao2 = [&](yyjson_val* cv0, yyjson_val* cv1) {
        ++i;
        void* omem = nullptr;
        if (NEOJSON_LIKELY(static_cast<std::size_t>(bend - bp) >= 64u)) {
            omem = bp;
            bp += 64u;
        } else {
            ctx.sdoc->arena.bump_ptr = bp;
            omem = ctx.sdoc->arena.alloc_slow(64u);
            bp = ctx.sdoc->arena.bump_ptr;
            bend = ctx.sdoc->arena.bump_end;
        }
        auto* next_ao = ::new (omem) ArenaObject();
        next_ao->count = 2u;
        next_ao->doc = ctx.sdoc;
        next_ao->keys_ptr = cached_se.keys;
        json* lout = next_ao->vals();
        init_arena_non_str_leaf(&lout[0], cv0);
        init_arena_non_str_leaf(&lout[1], cv1);
        json_access::init_arena_object_ptr(&out[i], next_ao);
        cur = cv1 + 1u;
    };

    while (i + 1u < len &&
           yyjson_get_tag(cur) == (YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE) &&
           unsafe_yyjson_get_len(cur) == 2u &&
           cur->uni.ofs == 5u * sizeof(yyjson_val)) {
        yyjson_val* ck0 = cur + 1u;
        yyjson_val* cv0 = cur + 2u;
        yyjson_val* ck1 = cur + 3u;
        yyjson_val* cv1 = cur + 4u;
        if (unsafe_yyjson_is_str(cv0) || unsafe_yyjson_is_str(cv1)) {
            break;
        }
        if (NEOJSON_UNLIKELY(!key_eq(ck0, d0, k0v) || !key_eq(ck1, d1, k1v))) {
            break;
        }
        emit_ao2(cv0, cv1);
    }
    ctx.sdoc->arena.bump_ptr = bp;
    i_io = i;
    cur_io = cur;
}

NEOJSON_NOINLINE void build_arena_array_obj3_run(ArenaMatCtx& ctx, json* out, const std::size_t len,
                                                 std::size_t& i_io, yyjson_val*& cur_io,
                                                 const ShapeEntry& cached_se) {
    const ShapeKeyDesc* const descs = ctx.scratch.shape_cache.key_descs.data() + cached_se.descs_off;
    const ShapeKeyDesc d0 = descs[0];
    const ShapeKeyDesc d1 = descs[1];
    const ShapeKeyDesc d2 = descs[2];
    const char* const kb = ctx.scratch.shape_cache.key_bytes.data();
    const bool padded = ctx.str_pool_padded_32;
#if defined(__SSE2__) || NEOJSON_X86_SIMD
    const bool fast_sse = padded && (cached_se.max_key_len <= 16u);
    const __m128i k0v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(kb + d0.byte_off));
    const __m128i k1v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(kb + d1.byte_off));
    const __m128i k2v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(kb + d2.byte_off));
    auto key_eq = [&](yyjson_val* ck, const ShapeKeyDesc d, const __m128i kv) noexcept -> bool {
        if (NEOJSON_LIKELY(fast_sse)) {
            if (NEOJSON_UNLIKELY(unsafe_yyjson_get_len(ck) != d.len)) {
                return false;
            }
            const __m128i av = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(unsafe_yyjson_get_str(ck)));
            const std::uint32_t eq =
                static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(av, kv)));
            return ((~eq) & d.mask16) == 0u;
        }
        return shape_key_eq_desc(padded, unsafe_yyjson_get_str(ck), unsafe_yyjson_get_len(ck), kb,
                                 d);
    };
#else
    const int k0v = 0;
    const int k1v = 0;
    const int k2v = 0;
    auto key_eq = [&](yyjson_val* ck, const ShapeKeyDesc d, const int) noexcept -> bool {
        return shape_key_eq_desc(padded, unsafe_yyjson_get_str(ck), unsafe_yyjson_get_len(ck), kb,
                                 d);
    };
#endif
    static_assert(sizeof(ArenaObject) + 3u * sizeof(json) == 80u,
                  "3-entry ArenaObject leaf must be 80 bytes");
    std::size_t i = i_io;
    yyjson_val* cur = cur_io;
    char* bp = ctx.sdoc->arena.bump_ptr;
    char* bend = ctx.sdoc->arena.bump_end;

    auto emit_ao3 = [&](yyjson_val* cv0, yyjson_val* cv1, yyjson_val* cv2) {
        ++i;
        void* omem = nullptr;
        if (NEOJSON_LIKELY(static_cast<std::size_t>(bend - bp) >= 80u)) {
            omem = bp;
            bp += 80u;
        } else {
            ctx.sdoc->arena.bump_ptr = bp;
            omem = ctx.sdoc->arena.alloc_slow(80u);
            bp = ctx.sdoc->arena.bump_ptr;
            bend = ctx.sdoc->arena.bump_end;
        }
        auto* next_ao = ::new (omem) ArenaObject();
        next_ao->count = 3u;
        next_ao->doc = ctx.sdoc;
        next_ao->keys_ptr = cached_se.keys;
        json* lout = next_ao->vals();
        init_arena_non_str_leaf(&lout[0], cv0);
        init_arena_non_str_leaf(&lout[1], cv1);
        init_arena_non_str_leaf(&lout[2], cv2);
        json_access::init_arena_object_ptr(&out[i], next_ao);
        cur = cv2 + 1u;
    };

    while (i + 1u < len &&
           yyjson_get_tag(cur) == (YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE) &&
           unsafe_yyjson_get_len(cur) == 3u &&
           cur->uni.ofs == 7u * sizeof(yyjson_val)) {
        yyjson_val* ck0 = cur + 1u;
        yyjson_val* cv0 = cur + 2u;
        yyjson_val* ck1 = cur + 3u;
        yyjson_val* cv1 = cur + 4u;
        yyjson_val* ck2 = cur + 5u;
        yyjson_val* cv2 = cur + 6u;
        if (unsafe_yyjson_is_str(cv0) || unsafe_yyjson_is_str(cv1) ||
            unsafe_yyjson_is_str(cv2)) {
            break;
        }
        if (NEOJSON_UNLIKELY(!key_eq(ck0, d0, k0v) || !key_eq(ck1, d1, k1v) ||
                             !key_eq(ck2, d2, k2v))) {
            break;
        }
        emit_ao3(cv0, cv1, cv2);
    }
    ctx.sdoc->arena.bump_ptr = bp;
    i_io = i;
    cur_io = cur;
}

NEOJSON_NOINLINE ArenaArray* build_arena_array_node(ArenaMatCtx& ctx, yyjson_val* val) {
    const std::size_t len = unsafe_yyjson_get_len(val);
    void* mem = ctx.sdoc->arena.alloc(sizeof(ArenaArray) + len * sizeof(json));
    auto* aa = ::new (mem) ArenaArray();
    aa->count = static_cast<std::uint32_t>(len);
    aa->doc = ctx.sdoc;
    json* out = aa->elems();
    yyjson_val* cur = unsafe_yyjson_get_first(val);
    for (std::size_t i = 0u; i < len; ++i) {
        if (!yyjson_is_ctn(cur)) {
            if (NEOJSON_LIKELY(!unsafe_yyjson_is_str(cur))) {
                init_arena_non_str_leaf(&out[i], cur);
            } else {
                construct_arena_leaf(ctx, &out[i], cur);
            }
            if (i + 1u < len) {
                cur = cur + 1u;
            }
        } else {
            const std::size_t clen = unsafe_yyjson_get_len(cur);
            if (clen == 0u) {
                if (yyjson_is_arr(cur)) {
                    json_access::init_arena_empty_array(&out[i]);
                } else {
                    json_access::init_arena_empty_object(&out[i]);
                }
                if (i + 1u < len) {
                    cur = cur + 1u;
                }
            } else if (clen <= 2u && yyjson_is_arr(cur)) {
                if (clen == 2u && len >= 4u) {
                    yyjson_val* e0 = cur + 1u;
                    yyjson_val* e1 = e0 + 1u;
                    if (yyjson_get_tag(e0) == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL) &&
                        yyjson_get_tag(e1) == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL)) {
                        static_assert(sizeof(ArenaArray) + 2u * sizeof(json) == 48u,
                                      "2-element ArenaArray must fit in 48 bytes");
                        char* bp = ctx.sdoc->arena.bump_ptr;
                        char* bend = ctx.sdoc->arena.bump_end;
                        for (;;) {
                            void* lmem = nullptr;
                            if (static_cast<std::size_t>(bend - bp) >= 48u) {
                                lmem = bp;
                                bp += 48u;
                            } else {
                                ctx.sdoc->arena.bump_ptr = bp;
                                lmem = ctx.sdoc->arena.alloc_slow(48u);
                                bp = ctx.sdoc->arena.bump_ptr;
                                bend = ctx.sdoc->arena.bump_end;
                            }
                            auto* leaf = ::new (lmem) ArenaArray();
                            leaf->count = 2u;
                            leaf->doc = ctx.sdoc;
                            json* lout = leaf->elems();
                            json_access::init_arena_real(
                                &lout[0],
                                static_cast<json::number_float_t>(unsafe_yyjson_get_real(e0)));
                            json_access::init_arena_real(
                                &lout[1],
                                static_cast<json::number_float_t>(unsafe_yyjson_get_real(e1)));
                            json_access::init_arena_array_ptr(&out[i], leaf);
                            ++i;
                            if (i >= len) {
                                ctx.sdoc->arena.bump_ptr = bp;
                                return aa;
                            }
                            cur = e1 + 1u;
                            if (!yyjson_is_arr(cur) || unsafe_yyjson_get_len(cur) != 2u) {
                                ctx.sdoc->arena.bump_ptr = bp;
                                --i;
                                break;
                            }
                            e0 = cur + 1u;
                            e1 = e0 + 1u;
                            if (yyjson_get_tag(e0) != (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL) ||
                                yyjson_get_tag(e1) != (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL)) {
                                ctx.sdoc->arena.bump_ptr = bp;
                                --i;
                                break;
                            }
                        }
                        continue;
                    }
                }
                if (ArenaArray* leaf = try_build_inline_leaf_arena_array(ctx, cur, clen)) {
                    json_access::init_arena_array_ptr(&out[i], leaf);
                    if (i + 1u < len) {
                        cur = cur + clen + 1u;
                    }
                } else if (NEOJSON_LIKELY(ctx.depth < kMaxDirectDfsDepth)) {
                    MatDepthGuard dguard(ctx.depth);
                    ArenaArray* child_aa = build_arena_array_node(ctx, cur);
                    json_access::init_arena_array_ptr(&out[i], child_aa);
                    if (i + 1u < len) {
                        cur = unsafe_yyjson_get_next(cur);
                    }
                } else {
                    json_access::init_arena_null(&out[i]);
                    ctx.scratch.workstack.push_back(ArenaWorkItem{cur, &out[i]});
                    if (i + 1u < len) {
                        cur = unsafe_yyjson_get_next(cur);
                    }
                }
            } else if (NEOJSON_LIKELY(ctx.depth < kMaxDirectDfsDepth) && yyjson_is_obj(cur) &&
                       clen <= ArenaShapeCache::kMaxCachedObjKeys) {
                yyjson_val* k0 = unsafe_yyjson_get_first(cur);
                ShapeBuildRes res{};
                {
                    MatDepthGuard dguard(ctx.depth);
                    res = try_build_from_shape_cache(ctx, cur, clen, unsafe_yyjson_get_str(k0),
                                                     unsafe_yyjson_get_len(k0));
                    if (res.ao == nullptr) {
                        res.ao = build_arena_object_node(ctx, cur, nullptr, false);
                        json_access::init_arena_object_ptr(&out[i], res.ao);
                        if (i + 1u < len) {
                            cur = unsafe_yyjson_get_next(cur);
                        }
                        continue;
                    }
                }
                const ShapeEntry cached_se = res.se;
                json_access::init_arena_object_ptr(&out[i], res.ao);
                if (i + 1u >= len) {
                    return aa;
                }
                cur = unsafe_yyjson_get_next(cur);
                if (cached_se.count == 2u) {
                    build_arena_array_obj2_run(ctx, out, len, i, cur, cached_se);
                    if (i + 1u >= len) {
                        return aa;
                    }
                } else if (cached_se.count == 3u) {
                    build_arena_array_obj3_run(ctx, out, len, i, cur, cached_se);
                    if (i + 1u >= len) {
                        return aa;
                    }
                }
                {
                    MatDepthGuard dguard(ctx.depth);
                    while (i + 1u < len &&
                           yyjson_get_tag(cur) == (YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE) &&
                           unsafe_yyjson_get_len(cur) == cached_se.count) {
                        ArenaObject* next_ao =
                            try_build_arena_object_with_entry(ctx, cur, cached_se, true);
                        if (next_ao == nullptr) {
                            break;
                        }
                        ++i;
                        json_access::init_arena_object_ptr(&out[i], next_ao);
                        if (i + 1u >= len) {
                            return aa;
                        }
                        cur = unsafe_yyjson_get_next(cur);
                    }
                }
                continue;
            } else if (NEOJSON_LIKELY(ctx.depth < kMaxDirectDfsDepth)) {
                MatDepthGuard dguard(ctx.depth);
                if (yyjson_is_arr(cur)) {
                    ArenaArray* child_aa = build_arena_array_node(ctx, cur);
                    json_access::init_arena_array_ptr(&out[i], child_aa);
                } else {
                    ArenaObject* child_ao = build_arena_object_node(ctx, cur, nullptr, false);
                    json_access::init_arena_object_ptr(&out[i], child_ao);
                }
                if (i + 1u < len) {
                    cur = unsafe_yyjson_get_next(cur);
                }
            } else {
                json_access::init_arena_null(&out[i]);
                ctx.scratch.workstack.push_back(ArenaWorkItem{cur, &out[i]});
                if (i + 1u < len) {
                    cur = unsafe_yyjson_get_next(cur);
                }
            }
        }
    }
    return aa;
}

NEOJSON_NOINLINE ArenaObject* build_arena_medium_object_slow(ArenaMatCtx& ctx, yyjson_val* val,
                                                             const std::size_t len,
                                                             bool* own_unique_out) {
    yyjson_val* small_keys[ArenaShapeCache::kMaxCachedObjKeys];
    yyjson_val* small_vals[ArenaShapeCache::kMaxCachedObjKeys];
    std::vector<yyjson_val*> large_keys;
    std::vector<yyjson_val*> large_vals;
    yyjson_val** kbuf = small_keys;
    yyjson_val** vbuf = small_vals;
    if (len > ArenaShapeCache::kMaxCachedObjKeys) {
        large_keys.resize(len);
        large_vals.resize(len);
        kbuf = large_keys.data();
        vbuf = large_vals.data();
    }

    bool local_unique = true;
    std::size_t out_n = 0u;
    yyjson_obj_iter iter = yyjson_obj_iter_with(val);
    yyjson_val* k = nullptr;
    const std::uint64_t obj_tok = ctx.intern.next_obj_token();
    while ((k = yyjson_obj_iter_next(&iter)) != nullptr) {
        const char* kp = unsafe_yyjson_get_str(k);
        const std::size_t kl = unsafe_yyjson_get_len(k);
        const std::uint32_t key_id = ctx.intern.intern(kp, kl, ctx.seed);
        yyjson_val* cv = yyjson_obj_iter_get_val(k);
        if (NEOJSON_UNLIKELY(key_id == KeyInternTable::kUnindexedKeyId)) {
            bool found = false;
            for (std::size_t j = 0u; j < out_n; ++j) {
                if (unsafe_yyjson_get_len(kbuf[j]) == kl &&
                    fast_memeq(unsafe_yyjson_get_str(kbuf[j]), kp, kl)) {
                    vbuf[j] = cv;
                    local_unique = false;
                    ctx.saw_dup = true;
                    found = true;
                    break;
                }
            }
            if (!found) {
                kbuf[out_n] = k;
                vbuf[out_n] = cv;
                ++out_n;
            }
            continue;
        }
        KeySeenToken& st = ctx.intern.seen[key_id];
        if (st.token == obj_tok) {
            vbuf[st.out_idx] = cv;
            local_unique = false;
            ctx.saw_dup = true;
        } else {
            st.token = obj_tok;
            st.out_idx = static_cast<std::uint32_t>(out_n);
            kbuf[out_n] = k;
            vbuf[out_n] = cv;
            ++out_n;
        }
    }
    if (own_unique_out != nullptr) {
        *own_unique_out = local_unique;
    }

    static_assert(alignof(std::string) <= alignof(json) && alignof(json) >= alignof(ArenaObject),
                  "ArenaObject contiguous keys must stay aligned");
    void* mem = ctx.sdoc->arena.alloc(sizeof(ArenaObject) + out_n * sizeof(json) +
                                      out_n * sizeof(std::string));
    auto* ao = ::new (mem) ArenaObject();
    ao->count = static_cast<std::uint32_t>(out_n);
    ao->doc = ctx.sdoc;
    json* out = ao->vals();
    std::string* dst_keys = reinterpret_cast<std::string*>(
        reinterpret_cast<unsigned char*>(ao) + sizeof(ArenaObject) + out_n * sizeof(json));
    ao->keys_ptr = dst_keys;
    for (std::size_t i = 0u; i < out_n; ++i) {
        yyjson_val* ky = kbuf[i];
        ctx.sdoc->arena.construct_key_in_place(&dst_keys[i], unsafe_yyjson_get_str(ky),
                                               unsafe_yyjson_get_len(ky), ctx.str_pool);
    }
    for (std::size_t i = 0u; i < out_n; ++i) {
        init_arena_slot(ctx, &out[i], vbuf[i]);
    }
    if (local_unique && out_n >= 1u && out_n <= ArenaShapeCache::kMaxCachedObjKeys) {
        ctx.scratch.shape_cache.record(out_n, dst_keys);
    }
    return ao;
}

NEOJSON_NOINLINE ArenaObject* build_arena_large_unique_object(ArenaMatCtx& ctx, yyjson_val* val,
                                                              const std::size_t len) {
    void* mem = ctx.sdoc->arena.alloc(sizeof(ArenaObject) + len * sizeof(json) +
                                      len * sizeof(std::string));
    auto* ao = ::new (mem) ArenaObject();
    ao->count = static_cast<std::uint32_t>(len);
    ao->doc = ctx.sdoc;
    json* out = ao->vals();
    std::string* dst_keys = reinterpret_cast<std::string*>(
        reinterpret_cast<unsigned char*>(ao) + sizeof(ArenaObject) + len * sizeof(json));
    ao->keys_ptr = dst_keys;
    yyjson_obj_iter iter = yyjson_obj_iter_with(val);
    yyjson_val* k = nullptr;
    std::size_t idx = 0u;
    while ((k = yyjson_obj_iter_next(&iter)) != nullptr) {
        ctx.sdoc->arena.construct_key_in_place(&dst_keys[idx], unsafe_yyjson_get_str(k),
                                               unsafe_yyjson_get_len(k), ctx.str_pool);
        init_arena_slot(ctx, &out[idx], yyjson_obj_iter_get_val(k));
        ++idx;
    }
    return ao;
}

NEOJSON_NOINLINE ArenaObject* build_arena_object_miss(ArenaMatCtx& ctx, yyjson_val* val,
                                                      const std::size_t len,
                                                      bool* own_unique_out,
                                                      const bool known_unique) {
    if (known_unique && len > ArenaShapeCache::kMaxCachedObjKeys) {
        if (own_unique_out != nullptr) {
            *own_unique_out = true;
        }
        return build_arena_large_unique_object(ctx, val, len);
    }

    if (len > 16u) {
        return build_arena_medium_object_slow(ctx, val, len, own_unique_out);
    }

    bool local_unique = true;
    yyjson_val* small_keys[16];
    yyjson_val* small_vals[16];
    std::size_t out_n = 0u;
    yyjson_obj_iter iter = yyjson_obj_iter_with(val);
    yyjson_val* k = nullptr;
    while ((k = yyjson_obj_iter_next(&iter)) != nullptr) {
        const char* kp = unsafe_yyjson_get_str(k);
        const std::size_t kl = unsafe_yyjson_get_len(k);
        yyjson_val* cv = yyjson_obj_iter_get_val(k);
        bool found = false;
        for (std::size_t j = 0u; j < out_n; ++j) {
            yyjson_val* prev_k = small_keys[j];
            if (unsafe_yyjson_get_len(prev_k) == kl &&
                fast_memeq(unsafe_yyjson_get_str(prev_k), kp, kl)) {
                small_vals[j] = cv;
                local_unique = false;
                ctx.saw_dup = true;
                found = true;
                break;
            }
        }
        if (!found) {
            small_keys[out_n] = k;
            small_vals[out_n] = cv;
            ++out_n;
        }
    }
    if (own_unique_out != nullptr) {
        *own_unique_out = local_unique;
    }

    void* mem = ctx.sdoc->arena.alloc(sizeof(ArenaObject) + out_n * sizeof(json) +
                                      out_n * sizeof(std::string));
    auto* ao = ::new (mem) ArenaObject();
    ao->count = static_cast<std::uint32_t>(out_n);
    ao->doc = ctx.sdoc;
    json* out = ao->vals();
    std::string* dst_keys = reinterpret_cast<std::string*>(
        reinterpret_cast<unsigned char*>(ao) + sizeof(ArenaObject) + out_n * sizeof(json));
    ao->keys_ptr = dst_keys;
    for (std::size_t i = 0u; i < out_n; ++i) {
        yyjson_val* ky = small_keys[i];
        ctx.sdoc->arena.construct_key_in_place(&dst_keys[i], unsafe_yyjson_get_str(ky),
                                               unsafe_yyjson_get_len(ky), ctx.str_pool);
    }
    for (std::size_t i = 0u; i < out_n; ++i) {
        init_arena_slot(ctx, &out[i], small_vals[i]);
    }
    if (local_unique && out_n >= 1u) {
        ctx.scratch.shape_cache.record(out_n, dst_keys);
    }
    return ao;
}

NEOJSON_NOINLINE ArenaObject* build_arena_object_node(ArenaMatCtx& ctx, yyjson_val* val,
                                                      bool* own_unique_out,
                                                      const bool known_unique) {
    const std::size_t len = unsafe_yyjson_get_len(val);
    if (own_unique_out != nullptr) {
        *own_unique_out = true;
    }

    if (NEOJSON_LIKELY(len >= 1u && len <= ArenaShapeCache::kMaxCachedObjKeys)) {
        yyjson_val* k0 = unsafe_yyjson_get_first(val);
        const char* p0 = unsafe_yyjson_get_str(k0);
        const std::size_t l0 = unsafe_yyjson_get_len(k0);
        if (ArenaObject* ao = find_and_build_shape(ctx, val, len, p0, l0).ao) {
            return ao;
        }
    }

    return build_arena_object_miss(ctx, val, len, own_unique_out, known_unique);
}

inline const void* materialize_doc_arena(SharedYyDoc* sdoc, yyjson_val* root,
                                         const value_t root_type, const bool root_already_unique,
                                         bool& root_own_unique, bool& saw_dup) {
    const std::size_t dat_read = (sdoc->doc != nullptr) ? sdoc->doc->dat_read : 0u;
    if (sdoc->arena.head == nullptr && sdoc->doc != nullptr) {
        const std::size_t vc = yyjson_doc_get_val_count(sdoc->doc);
        std::size_t est = vc * 28u + 4096u;
        if (est < 65536u) {
            est = 65536u;
        } else if (est > kLargeCacheMaxEntryBytes) {
            est = kLargeCacheMaxEntryBytes;
        }
        sdoc->arena.next_chunk_bytes = est;
    }

    std::uint64_t seed = g_hash_seed.load(std::memory_order_acquire);
    if (seed == 0u) {
        seed = hash_seed_slow();
    }
    ArenaMatTLS& tls = tls_arena_mat();
    tls.key_intern.begin_doc();
    tls.long_cache.begin_doc(dat_read);
    tls.short_cache.begin_doc(dat_read);
    tls.scratch.shape_cache.begin_doc();
    tls.scratch.workstack.clear();
    struct ScratchGuard {
        ArenaMaterializeScratch& s;
        ~ScratchGuard() {
            s.workstack.clear();
        }
    } scratch_guard{tls.scratch};
    ArenaMaterializeScratch& scratch = tls.scratch;

    ArenaMatCtx ctx{
        sdoc,
        tls.key_intern,
        scratch,
        &empty_arena_string_singleton(),
        tls.long_cache,
        tls.short_cache,
        try_tls_str_pool(),
        seed,
        dat_read,
        0u,
        sdoc->str_pool_padded_32,
        false,
    };

    root_own_unique = true;

    const void* root_ptr = nullptr;
    if (root_type == value_t::array) {
        root_ptr = build_arena_array_node(ctx, root);
    } else {
        root_ptr = build_arena_object_node(ctx, root, &root_own_unique, root_already_unique);
    }

    while (!scratch.workstack.empty()) {
        const ArenaWorkItem item = scratch.workstack.back();
        scratch.workstack.pop_back();
        ctx.depth = 0u;
        if (yyjson_is_arr(item.yy_ctn)) {
            ArenaArray* child_aa = build_arena_array_node(ctx, item.yy_ctn);
            json_access::set_arena_array_ptr(item.dst_slot, child_aa);
        } else {
            ArenaObject* child_ao =
                build_arena_object_node(ctx, item.yy_ctn, nullptr, false);
            json_access::set_arena_object_ptr(item.dst_slot, child_ao);
        }
    }
    saw_dup = ctx.saw_dup;
    return root_ptr;
}

} // namespace

void destroy_shared_doc_slow(SharedDocHeader* hdr) noexcept {
    destroy_shared_doc(static_cast<SharedYyDoc*>(hdr));
}

const void* lazy_materialize_arena(LazyHeader* hdr, const value_t type) {
    auto* node = static_cast<LazyNode*>(hdr);
    void* existing = node->arena_view.load(std::memory_order_acquire);
    if (existing != nullptr) {
        return existing;
    }
    U8SpinGuard guard(node->doc->arena_lock);
    existing = node->arena_view.load(std::memory_order_relaxed);
    if (existing == nullptr) {
        const bool root_already_unique =
            (type == value_t::object) &&
            ((node->dedup_state.load(std::memory_order_acquire) & 1u) != 0u);
        bool root_own_unique = true;
        bool saw_dup = false;
        const void* built = materialize_doc_arena(node->doc, node->val, type, root_already_unique,
                                                  root_own_unique, saw_dup);
        if (type == value_t::object) {
            if (root_own_unique) {
                mark_own_unique(node);
            } else {
                mark_own_dup(node);
            }
        }
        if (!saw_dup && node->val == yyjson_doc_get_root(node->doc->doc)) {
            node->doc->tree_unique_verified.store(true, std::memory_order_release);
        }
        existing = const_cast<void*>(built);
        node->arena_view.store(existing, std::memory_order_release);
    }
    return existing;
}

ArenaHashIndex* arena_obj_build_index_slow(const ArenaObject* ao) noexcept {
    if (ao == nullptr || ao->doc == nullptr || ao->count <= kOrderedMapHashThreshold) {
        return nullptr;
    }
    try {
        auto* sdoc = static_cast<SharedYyDoc*>(ao->doc);
        SharedDocExtras* ex = get_or_create_doc_extras(sdoc);
        SpinGuard guard(ex->extra_lock);
        if (ArenaHashIndex* existing = ao->index.load(std::memory_order_relaxed)) {
            return existing;
        }
        const std::size_t n = ao->count;
        std::size_t cap = 32u;
        while (cap < n * 2u) {
            cap <<= 1u;
        }
        auto* slots = static_cast<ArenaHashSlot*>(pool_alloc(cap * sizeof(ArenaHashSlot)));
        ArenaHashIndex* idx = nullptr;
        try {
            idx = static_cast<ArenaHashIndex*>(pool_alloc(sizeof(ArenaHashIndex)));
            ::new (idx) ArenaHashIndex{cap, slots};
            ex->arena_indices.reserve(ex->arena_indices.size() + 1u);
        } catch (...) {
            if (idx != nullptr) {
                pool_free(idx, sizeof(ArenaHashIndex));
            }
            pool_free(slots, cap * sizeof(ArenaHashSlot));
            throw;
        }
        std::fill_n(slots, cap, ArenaHashSlot{0u, 0u});
        const std::size_t mask = cap - 1u;
        const std::string* ks = ao->keys();
        for (std::size_t i = 0u; i < n; ++i) {
            const std::uint64_t h = hash_bytes(ks[i].data(), ks[i].size());
            const std::uint32_t h32 = static_cast<std::uint32_t>(h >> 32u);
            std::size_t pos = static_cast<std::size_t>(h) & mask;
            while (slots[pos].idx_plus_one != 0u) {
                pos = (pos + 1u) & mask;
            }
            slots[pos] = ArenaHashSlot{static_cast<std::uint32_t>(i + 1u), h32};
        }
        ex->arena_indices.push_back(idx);
        ao->index.store(idx, std::memory_order_release);
        return idx;
    } catch (...) {
        return nullptr;
    }
}

namespace {

inline json clone_arena_leaf_or_placeholder(const json& src, bool& is_nested_ctn) {
    is_nested_ctn = false;
    if (json_access::storage_of(src) == 4u) {
        if (json_access::type_of(src) == value_t::string) {
            return json_access::make_owned_arena_string_json(json_access::shared_string_ptr(src));
        }
        is_nested_ctn = true;
        return json(nullptr);
    }
    // Scalar (0u) or empty container (3u) or escaped string (5u)
    if (json_access::type_of(src) == value_t::string) {
        return json_access::make_owned_arena_string_json(json_access::shared_string_ptr(src));
    }
    return src;
}

} // namespace

void arena_clone_to_owned(json& dst, const json& src) {
    json owned;
    std::vector<std::pair<const json*, json*>> workstack;
    workstack.emplace_back(&src, &owned);

    while (!workstack.empty()) {
        const auto item = workstack.back();
        workstack.pop_back();
        const json* cur_src = item.first;
        json* cur_dst = item.second;

        if (json_access::is_arena_array(*cur_src)) {
            const ArenaArray* aa = json_access::arena_array_ptr(*cur_src);
            const std::size_t n = aa->count;
            const json* elems = aa->elems();
            if (n == 0u) {
                *cur_dst = json_access::make_array();
            } else if (n == 1u) {
                bool nested = false;
                SmallArray2* sa = json_access::create_small_array_from_elem0(
                    clone_arena_leaf_or_placeholder(elems[0], nested));
                try {
                    if (nested) {
                        workstack.emplace_back(&elems[0], &json_access::small_array_slot(sa, 0u));
                    }
                } catch (...) {
                    json_access::destroy_small_array_raw(sa);
                    throw;
                }
                *cur_dst = json_access::make_small_array_from_ptr(sa);
            } else if (n == 2u) {
                bool nested0 = false;
                bool nested1 = false;
                json v0 = clone_arena_leaf_or_placeholder(elems[0], nested0);
                json v1 = clone_arena_leaf_or_placeholder(elems[1], nested1);
                SmallArray2* sa =
                    json_access::create_small_array_from_elem2(std::move(v0), std::move(v1));
                try {
                    if (nested0) {
                        workstack.emplace_back(&elems[0], &json_access::small_array_slot(sa, 0u));
                    }
                    if (nested1) {
                        workstack.emplace_back(&elems[1], &json_access::small_array_slot(sa, 1u));
                    }
                } catch (...) {
                    json_access::destroy_small_array_raw(sa);
                    throw;
                }
                *cur_dst = json_access::make_small_array_from_ptr(sa);
            } else {
                json::array_t* arr = json_access::create_array_cap(n);
                struct ArrGuard {
                    json::array_t* p;
                    ~ArrGuard() {
                        if (p != nullptr) {
                            json_access::destroy_erased_container(p, value_t::array);
                        }
                    }
                } guard{arr};
                for (std::size_t i = 0u; i < n; ++i) {
                    bool nested = false;
                    arr->push_back(clone_arena_leaf_or_placeholder(elems[i], nested));
                    if (nested) {
                        workstack.emplace_back(&elems[i], &(*arr)[i]);
                    }
                }
                guard.p = nullptr;
                *cur_dst = json_access::make_from_array_ptr(arr);
            }
        } else {
            const ArenaObject* ao = json_access::arena_object_ptr(*cur_src);
            const std::size_t n = ao->count;
            const std::string* ks = ao->keys();
            const json* vs = ao->vals();
            if (n == 0u) {
                *cur_dst = json_access::make_object();
            } else {
                SmallObject4* so = nullptr;
                if (n <= kSmallObjMaxCap) {
                    std::size_t total_key_bytes = 0u;
                    for (std::size_t i = 0u; i < n; ++i) {
                        total_key_bytes += ks[i].size();
                    }
                    so = json_access::create_small_object_raw_for(n, total_key_bytes);
                }
                if (so != nullptr) {
                    struct SmallObjGuard {
                        SmallObject4* p;
                        ~SmallObjGuard() {
                            if (p != nullptr) {
                                json_access::destroy_small_object_raw(p);
                            }
                        }
                    } guard{so};
                    for (std::size_t i = 0u; i < n; ++i) {
                        bool nested = false;
                        json val = clone_arena_leaf_or_placeholder(vs[i], nested);
                        json_access::small_object_append_unique(so, ks[i], std::move(val));
                        if (nested) {
                            workstack.emplace_back(&vs[i], &so->val(i));
                        }
                    }
                    guard.p = nullptr;
                    *cur_dst = json_access::make_from_small_object(so);
                } else {
                    json::object_t* obj = json_access::create_object_vec_cap(n);
                    struct ObjGuard {
                        json::object_t* p;
                        ~ObjGuard() {
                            if (p != nullptr) {
                                json_access::destroy_erased_container(p, value_t::object);
                            }
                        }
                    } guard{obj};
                    for (std::size_t i = 0u; i < n; ++i) {
                        bool nested = false;
                        json val = clone_arena_leaf_or_placeholder(vs[i], nested);
                        obj->emplace_back_unindexed(ks[i], std::move(val));
                        if (nested) {
                            workstack.emplace_back(&vs[i], &(*obj)[i].second);
                        }
                    }
                    guard.p = nullptr;
                    *cur_dst = json_access::make_from_object_ptr(obj);
                }
            }
        }
    }

    json_access::commit_cloned_owned(dst, std::move(owned));
}

const void* arena_compat_array(const ArenaArray* aa) {
    auto* sdoc = static_cast<SharedYyDoc*>(aa->doc);
    SharedDocExtras* ex = get_or_create_doc_extras(sdoc);
    {
        SpinGuard guard(ex->extra_lock);
        const auto it = ex->compat_cache.find(aa);
        if (it != ex->compat_cache.end()) {
            return it->second.first;
        }
    }
    const std::size_t n = aa->count;
    const json* elems = aa->elems();
    json::array_t* arr = json_access::create_array_cap(n);
    struct ArrGuard {
        json::array_t* p;
        ~ArrGuard() {
            if (p != nullptr) {
                json_access::destroy_erased_container(p, value_t::array);
            }
        }
    } arr_guard{arr};
    for (std::size_t i = 0u; i < n; ++i) {
        const json& e = elems[i];
        if (e.is_string()) {
            // Detach strings to m_storage == 0u so compat_cache never holds a refcount cycle to sdoc.
            const auto& s = json_access::string_of(e);
            arr->push_back(json_access::make_string(s.data(), s.size()));
        } else {
            arr->push_back(json_access::copy_arena_slot_raw(e));
        }
    }
    SpinGuard lock_guard(ex->extra_lock);
    const auto [it, inserted] =
        ex->compat_cache.emplace(static_cast<const void*>(aa),
                                 std::make_pair(static_cast<void*>(arr), value_t::array));
    if (inserted) {
        arr_guard.p = nullptr;
        return arr;
    }
    return it->second.first;
}

const void* arena_compat_object(const ArenaObject* ao) {
    auto* sdoc = static_cast<SharedYyDoc*>(ao->doc);
    SharedDocExtras* ex = get_or_create_doc_extras(sdoc);
    {
        SpinGuard guard(ex->extra_lock);
        const auto it = ex->compat_cache.find(ao);
        if (it != ex->compat_cache.end()) {
            return it->second.first;
        }
    }
    const std::size_t n = ao->count;
    const std::string* ks = ao->keys();
    const json* vs = ao->vals();
    json::object_t* obj = json_access::create_object_vec_cap(n);
    struct ObjGuard {
        json::object_t* p;
        ~ObjGuard() {
            if (p != nullptr) {
                json_access::destroy_erased_container(p, value_t::object);
            }
        }
    } obj_guard{obj};
    for (std::size_t i = 0u; i < n; ++i) {
        const json& v = vs[i];
        if (v.is_string()) {
            // Detach strings to m_storage == 0u so compat_cache never holds a refcount cycle to sdoc.
            const auto& s = json_access::string_of(v);
            obj->emplace_back_unindexed(ks[i], json_access::make_string(s.data(), s.size()));
        } else {
            obj->emplace_back_unindexed(ks[i], json_access::copy_arena_slot_raw(v));
        }
    }
    SpinGuard lock_guard(ex->extra_lock);
    const auto [it, inserted] =
        ex->compat_cache.emplace(static_cast<const void*>(ao),
                                 std::make_pair(static_cast<void*>(obj), value_t::object));
    if (inserted) {
        obj_guard.p = nullptr;
        return obj;
    }
    return it->second.first;
}

void lazy_release(LazyHeader* hdr, const value_t type) noexcept {
    if (hdr == nullptr) {
        return;
    }
    auto* node = static_cast<LazyNode*>(hdr);
    if (node->refcount.load(std::memory_order_acquire) == 1u ||
        node->refcount.fetch_sub(1u, std::memory_order_acq_rel) == 1u) {
        void* mat = node->materialized.load(std::memory_order_acquire);
        SharedYyDoc* sdoc = node->doc;
        node->~LazyNode();
        tcache_free_fixed<sizeof(LazyNode)>(node);
        if (mat != nullptr) {
            json_access::destroy_erased_container(mat, type);
        }
        shared_doc_release(sdoc);
    }
}

const void* lazy_materialize_const(LazyHeader* hdr, const value_t type) {
    auto* node = static_cast<LazyNode*>(hdr);
    void* existing = node->materialized.load(std::memory_order_acquire);
    if (existing != nullptr) {
        return existing;
    }
    SpinGuard guard(node->init_lock);
    existing = node->materialized.load(std::memory_order_relaxed);
    if (existing == nullptr) {
        const bool known_unique =
            (type == value_t::object) &&
            ((node->dedup_state.load(std::memory_order_acquire) & 1u) != 0u ||
             node->doc->tree_unique_verified.load(std::memory_order_acquire));
        existing = shallow_materialize(node->doc, node->val, known_unique);
        if (type == value_t::object && !known_unique &&
            static_cast<const json::object_t*>(existing)->size() == node->raw_len) {
            mark_own_unique(node);
        }
        node->materialized.store(existing, std::memory_order_release);
    }
    return existing;
}

void* lazy_detach_mutable(LazyHeader* hdr, const value_t type) {
    auto* node = static_cast<LazyNode*>(hdr);
    if (node->refcount.load(std::memory_order_acquire) == 1u) {
        void* mat = node->materialized.exchange(nullptr, std::memory_order_acq_rel);
        if (mat == nullptr) {
            const bool known_unique =
                (type == value_t::object) &&
                ((node->dedup_state.load(std::memory_order_acquire) & 1u) != 0u ||
                 node->doc->tree_unique_verified.load(std::memory_order_acquire));
            mat = shallow_materialize(node->doc, node->val, known_unique);
        }
        SharedYyDoc* sdoc = node->doc;
        node->~LazyNode();
        tcache_free_fixed<sizeof(LazyNode)>(node);
        shared_doc_release(sdoc);
        return mat;
    }
    const void* shared = lazy_materialize_const(hdr, type);
    void* copy = (type == value_t::array)
                     ? static_cast<void*>(
                           json_access::clone_array(static_cast<const json::array_t*>(shared)))
                     : static_cast<void*>(
                           json_access::clone_object(static_cast<const json::object_t*>(shared)));
    if (node->refcount.fetch_sub(1u, std::memory_order_acq_rel) == 1u) {
        json_access::destroy_erased_container(copy, type);
        void* mat = node->materialized.exchange(nullptr, std::memory_order_acq_rel);
        SharedYyDoc* sdoc = node->doc;
        node->~LazyNode();
        tcache_free_fixed<sizeof(LazyNode)>(node);
        shared_doc_release(sdoc);
        return mat;
    }
    return copy;
}

std::size_t lazy_size(LazyHeader* hdr, const value_t type) noexcept {
    auto* node = static_cast<LazyNode*>(hdr);
    if (type == value_t::array) {
        return node->raw_len;
    }
    void* av = node->arena_view.load(std::memory_order_acquire);
    if (av != nullptr) {
        return static_cast<const ArenaObject*>(av)->count;
    }
    void* mat = node->materialized.load(std::memory_order_acquire);
    if (mat != nullptr) {
        return static_cast<const json::object_t*>(mat)->size();
    }
    if (node->doc->tree_unique_verified.load(std::memory_order_acquire) ||
        (node->dedup_state.load(std::memory_order_acquire) & 1u) != 0u) {
        return node->raw_len;
    }
    try {
        if (is_yyjson_object_unique(node->val)) {
            mark_own_unique(node);
            return node->raw_len;
        }
        mark_own_dup(node);
        const auto* obj =
            static_cast<const json::object_t*>(lazy_materialize_const(hdr, value_t::object));
        return obj->size();
    } catch (...) {
        std::terminate();
    }
}

bool lazy_contains(LazyHeader* hdr, const std::string_view key) {
    auto* node = static_cast<LazyNode*>(hdr);
    void* av = node->arena_view.load(std::memory_order_acquire);
    if (av != nullptr) {
        const auto* ao = static_cast<const ArenaObject*>(av);
        return arena_obj_find(ao, key) < ao->count;
    }
    void* mat = node->materialized.load(std::memory_order_acquire);
    if (mat != nullptr) {
        return static_cast<const json::object_t*>(mat)->contains(key);
    }
    const std::size_t len = node->raw_len;
    if (len > 16u) {
        const std::uint8_t prev = node->lookup_probes.load(std::memory_order_relaxed);
        if (prev >= 3u ||
            node->lookup_probes.fetch_add(1u, std::memory_order_relaxed) >= 2u) {
            const auto* ao =
                static_cast<const ArenaObject*>(lazy_materialize_arena(hdr, value_t::object));
            return arena_obj_find(ao, key) < ao->count;
        }
    }
    yyjson_obj_iter iter = yyjson_obj_iter_with(node->val);
    yyjson_val* k = nullptr;
    while ((k = yyjson_obj_iter_next(&iter)) != nullptr) {
        const std::size_t klen = unsafe_yyjson_get_len(k);
        if (klen == key.size() &&
            (klen == 0u || std::memcmp(unsafe_yyjson_get_str(k), key.data(), klen) == 0)) {
            return true;
        }
    }
    return false;
}

void* lazy_clear(LazyHeader* hdr, const value_t type) noexcept {
    auto* node = static_cast<LazyNode*>(hdr);
    if (node->refcount.load(std::memory_order_acquire) == 1u) {
        if (void* mat = node->materialized.exchange(nullptr, std::memory_order_acq_rel)) {
            if (type == value_t::array) {
                static_cast<json::array_t*>(mat)->clear();
            } else {
                static_cast<json::object_t*>(mat)->clear();
            }
            SharedYyDoc* sdoc = node->doc;
            node->~LazyNode();
            tcache_free_fixed<sizeof(LazyNode)>(node);
            shared_doc_release(sdoc);
            return mat;
        }
    }
    try {
        void* empty = (type == value_t::array)
                          ? static_cast<void*>(json_access::create_array_cap(0u))
                          : static_cast<void*>(json_access::create_object_cap(0u));
        lazy_release(hdr, type);
        return empty;
    } catch (...) {
        std::terminate();
    }
}

namespace {

struct ArrView {
    enum class Kind : std::uint8_t { Empty, Small, Arena, Eager } kind = Kind::Empty;
    const void* ptr = nullptr;
    std::size_t n = 0u;

    const json& at(const std::size_t i) const noexcept {
        switch (kind) {
            case Kind::Small:
                return static_cast<const SmallArray2*>(ptr)->elem(i);
            case Kind::Arena:
                return static_cast<const ArenaArray*>(ptr)->elems()[i];
            case Kind::Eager:
            case Kind::Empty:
            default:
                return (*static_cast<const json::array_t*>(ptr))[i];
        }
    }
};

inline ArrView resolve_arr_view(const json& j) {
    const std::uint8_t st = json_access::storage_of(j);
    if (st == 3u) {
        return ArrView{};
    }
    if (st == 2u) {
        const auto* sa = &json_access::small_array_of(j);
        return ArrView{ArrView::Kind::Small, sa, sa->count};
    }
    if (st == 4u) {
        const auto* aa = json_access::arena_array_ptr(j);
        return ArrView{ArrView::Kind::Arena, aa, aa->count};
    }
    if (st == 1u) {
        LazyHeader* hdr = json_access::lazy_node(j);
        void* av = hdr->arena_view.load(std::memory_order_acquire);
        if (av == nullptr) {
            if (void* mat = hdr->materialized.load(std::memory_order_acquire)) {
                const auto* arr = static_cast<const json::array_t*>(mat);
                return ArrView{ArrView::Kind::Eager, arr, arr->size()};
            }
            av = const_cast<void*>(lazy_materialize_arena(hdr, value_t::array));
        }
        const auto* aa = static_cast<const ArenaArray*>(av);
        return ArrView{ArrView::Kind::Arena, aa, aa->count};
    }
    const auto* arr = json_access::raw_array_ptr(j);
    return ArrView{ArrView::Kind::Eager, arr, arr->size()};
}

struct ObjView {
    enum class Kind : std::uint8_t { Empty, Small, Arena, Eager } kind = Kind::Empty;
    const void* ptr = nullptr;
    std::size_t n = 0u;

    std::string_view key_sv(const std::size_t i) const noexcept {
        switch (kind) {
            case Kind::Small:
                return static_cast<const SmallObject4*>(ptr)->key_view(i);
            case Kind::Arena:
                return std::string_view(static_cast<const ArenaObject*>(ptr)->keys()[i]);
            case Kind::Eager:
            case Kind::Empty:
            default:
                return std::string_view((*static_cast<const json::object_t*>(ptr))[i].first);
        }
    }

    const json& val(const std::size_t i) const noexcept {
        switch (kind) {
            case Kind::Small:
                return static_cast<const SmallObject4*>(ptr)->val(i);
            case Kind::Arena:
                return static_cast<const ArenaObject*>(ptr)->vals()[i];
            case Kind::Eager:
            case Kind::Empty:
            default:
                return (*static_cast<const json::object_t*>(ptr))[i].second;
        }
    }
};

inline ObjView resolve_obj_view(const json& j) {
    const std::uint8_t st = json_access::storage_of(j);
    if (st == 3u) {
        return ObjView{};
    }
    if (st == 2u) {
        const auto* so = &json_access::small_object_of(j);
        return ObjView{ObjView::Kind::Small, so, so->count};
    }
    if (st == 4u) {
        const auto* ao = json_access::arena_object_ptr(j);
        return ObjView{ObjView::Kind::Arena, ao, ao->count};
    }
    if (st == 1u) {
        LazyHeader* hdr = json_access::lazy_node(j);
        void* av = hdr->arena_view.load(std::memory_order_acquire);
        if (av == nullptr) {
            if (void* mat = hdr->materialized.load(std::memory_order_acquire)) {
                const auto* obj = static_cast<const json::object_t*>(mat);
                return ObjView{ObjView::Kind::Eager, obj, obj->size()};
            }
            av = const_cast<void*>(lazy_materialize_arena(hdr, value_t::object));
        }
        const auto* ao = static_cast<const ArenaObject*>(av);
        return ObjView{ObjView::Kind::Arena, ao, ao->count};
    }
    const auto* obj = json_access::raw_object_ptr(j);
    return ObjView{ObjView::Kind::Eager, obj, obj->size()};
}

inline bool array_view_equal(const json& lhs, const json& rhs) {
    const ArrView lv = resolve_arr_view(lhs);
    const ArrView rv = resolve_arr_view(rhs);
    if (lv.n != rv.n) {
        return false;
    }
    for (std::size_t i = 0u; i < lv.n; ++i) {
        if (!(lv.at(i) == rv.at(i))) {
            return false;
        }
    }
    return true;
}

inline bool array_view_less(const json& lhs, const json& rhs) {
    const ArrView lv = resolve_arr_view(lhs);
    const ArrView rv = resolve_arr_view(rhs);
    const std::size_t common = (std::min)(lv.n, rv.n);
    for (std::size_t i = 0u; i < common; ++i) {
        const json& le = lv.at(i);
        const json& re = rv.at(i);
        if (le < re) {
            return true;
        }
        if (re < le) {
            return false;
        }
    }
    return lv.n < rv.n;
}

inline bool object_view_equal(const json& lhs, const json& rhs) {
    const ObjView lv = resolve_obj_view(lhs);
    const ObjView rv = resolve_obj_view(rhs);
    if (lv.n != rv.n) {
        return false;
    }
    for (std::size_t i = 0u; i < lv.n; ++i) {
        if (!(lv.key_sv(i) == rv.key_sv(i) && lv.val(i) == rv.val(i))) {
            return false;
        }
    }
    return true;
}

inline bool object_view_less(const json& lhs, const json& rhs) {
    const ObjView lv = resolve_obj_view(lhs);
    const ObjView rv = resolve_obj_view(rhs);
    const std::size_t common = (std::min)(lv.n, rv.n);
    for (std::size_t i = 0u; i < common; ++i) {
        const std::string_view lk = lv.key_sv(i);
        const std::string_view rk = rv.key_sv(i);
        if (lk < rk) {
            return true;
        }
        if (rk < lk) {
            return false;
        }
        const json& lval = lv.val(i);
        const json& rval = rv.val(i);
        if (lval < rval) {
            return true;
        }
        if (rval < lval) {
            return false;
        }
    }
    return lv.n < rv.n;
}

} // namespace

bool lazy_equal(const json& lhs, const json& rhs) noexcept {
    const auto lhs_type = json_access::type_of(lhs);
    if (lhs_type != json_access::type_of(rhs)) {
        return false;
    }
    if (json_access::is_lazy(lhs) && json_access::is_lazy(rhs) &&
        json_access::lazy_node(lhs) == json_access::lazy_node(rhs)) {
        return true;
    }
    try {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        if (lhs_type == value_t::array) {
            return array_view_equal(lhs, rhs);
        }
        if (lhs_type == value_t::object) {
            return object_view_equal(lhs, rhs);
        }
        return false;
    } catch (...) {
        std::terminate();
    }
}

bool lazy_less(const json& lhs, const json& rhs) noexcept {
    const auto lhs_type = json_access::type_of(lhs);
    const auto rhs_type = json_access::type_of(rhs);
    if (lhs_type != rhs_type) {
        return operator<(lhs_type, rhs_type);
    }
    if (json_access::is_lazy(lhs) && json_access::is_lazy(rhs) &&
        json_access::lazy_node(lhs) == json_access::lazy_node(rhs)) {
        return false;
    }
    try {
        if (lhs_type == value_t::array) {
            return array_view_less(lhs, rhs);
        }
        if (lhs_type == value_t::object) {
            return object_view_less(lhs, rhs);
        }
        return false;
    } catch (...) {
        std::terminate();
    }
}

void lazy_materialize_deep(json& root) {
    if (root.is_string()) {
        json_access::detach_shared_string(root);
        return;
    }
    if (!root.is_array() && !root.is_object()) {
        return;
    }
    std::vector<json*> stack;
    stack.push_back(&root);
    while (!stack.empty()) {
        json* cur = stack.back();
        stack.pop_back();
        if (cur->is_array()) {
            if (json_access::is_small_array(*cur)) {
                auto* sa = json_access::small_array_ptr(*cur);
                json_access::drop_small_array_const_cache(sa);
                for (std::size_t i = 0u; i < sa->count; ++i) {
                    json& elem = sa->elem(i);
                    if (elem.is_string()) {
                        json_access::detach_shared_string(elem);
                    } else if (elem.is_array() || elem.is_object()) {
                        stack.push_back(&elem);
                    }
                }
            } else if (!json_access::is_empty_container(*cur)) {
                auto* arr = json_access::detach_array(*cur);
                for (auto& elem : *arr) {
                    if (elem.is_string()) {
                        json_access::detach_shared_string(elem);
                    } else if (elem.is_array() || elem.is_object()) {
                        stack.push_back(&elem);
                    }
                }
            }
        } else if (cur->is_object()) {
            if (json_access::is_small_object(*cur)) {
                auto* so = json_access::small_object_ptr(*cur);
                json_access::drop_small_object_const_cache(so);
                for (std::size_t i = 0u; i < so->count; ++i) {
                    json& val = so->val(i);
                    if (val.is_string()) {
                        json_access::detach_shared_string(val);
                    } else if (val.is_array() || val.is_object()) {
                        stack.push_back(&val);
                    }
                }
            } else if (!json_access::is_empty_container(*cur)) {
                auto* obj = json_access::detach_object(*cur);
                for (auto& kv : *obj) {
                    if (kv.second.is_string()) {
                        json_access::detach_shared_string(kv.second);
                    } else if (kv.second.is_array() || kv.second.is_object()) {
                        stack.push_back(&kv.second);
                    }
                }
            }
        }
    }
}

void* lazy_steal_owned_materialized(LazyHeader* hdr, const value_t /*type*/) noexcept {
    if (hdr == nullptr) {
        return nullptr;
    }
    auto* node = static_cast<LazyNode*>(hdr);
    void* mat = node->materialized.exchange(nullptr, std::memory_order_acq_rel);
    if (mat == nullptr) {
        return nullptr;
    }
    SharedYyDoc* sdoc = node->doc;
    node->~LazyNode();
    tcache_free_fixed<sizeof(LazyNode)>(node);
    shared_doc_release(sdoc);
    return mat;
}

namespace {

void dump_compact_strict(DumpBuffer& out, const json& val);
void dump_value(DumpBuffer& out, const json& val, bool pretty_print, bool ensure_ascii,
                error_handler_t error_handler, unsigned int indent_step,
                std::string& indent_string, unsigned int current_indent);
json materialize_yyjson(yyjson_val* root, std::size_t max_depth, bool& depth_exceeded);

NEOJSON_ALWAYS_INLINE void copy_short_noesc_bytes(char* dst, const char* src,
                                                  const std::size_t len) noexcept {
    if (len <= 8u) {
        if (len >= 4u) {
            std::memcpy(dst, src, 4u);
            std::memcpy(dst + len - 4u, src + len - 4u, 4u);
        } else if (len != 0u) {
            dst[0] = src[0];
            dst[len >> 1u] = src[len >> 1u];
            dst[len - 1u] = src[len - 1u];
        }
        return;
    }
    if (len <= 16u) {
        std::memcpy(dst, src, 8u);
        std::memcpy(dst + len - 8u, src + len - 8u, 8u);
        return;
    }
    std::memcpy(dst, src, 16u);
    std::memcpy(dst + len - 16u, src + len - 16u, 16u);
}

NEOJSON_NOINLINE char* flush_stage_to_string(std::string& out, char* stage, char* cur) {
    const std::size_t n = static_cast<std::size_t>(cur - stage);
    if (n != 0u) {
        out.append(stage, n);
    }
    return stage;
}

NEOJSON_NOINLINE char* write_long_noesc_flat(std::string& out, char* stage, char* cur,
                                             const char* sp, const std::size_t sl,
                                             const char term) {
    cur = flush_stage_to_string(out, stage, cur);
    out.push_back('"');
    out.append(sp, sl);
    const char tail[2] = {'"', term};
    out.append(tail, 2u);
    return stage;
}

NEOJSON_NOINLINE char* write_valid_utf8_escaped_flat(std::string& out, char* stage, char* cur,
                                                      char* const end, const char* p,
                                                      const std::size_t len, const char term) {
    auto flush = [&]() {
        const std::size_t n = static_cast<std::size_t>(cur - stage);
        if (n != 0u) {
            out.append(stage, n);
            cur = stage;
        }
    };
    auto ensure = [&](const std::size_t need) {
        if (NEOJSON_UNLIKELY(static_cast<std::size_t>(end - cur) < need)) {
            flush();
        }
    };
    auto append_raw = [&](const char* sp, const std::size_t sn) {
        if (NEOJSON_LIKELY(sn <= 256u)) {
            ensure(sn);
            if (sn != 0u) {
                std::memcpy(cur, sp, sn);
                cur += sn;
            }
        } else {
            flush();
            out.append(sp, sn);
        }
    };

    std::size_t i = utf8_impl::find_non_verbatim_valid_utf8(p, 0u, len);
    if (i == len) {
        if (len <= 256u) {
            ensure(len + 3u);
            *cur++ = '"';
            if (len != 0u) {
                std::memcpy(cur, p, len);
                cur += len;
            }
            *cur++ = '"';
            *cur++ = term;
        } else {
            flush();
            out.push_back('"');
            out.append(p, len);
            const char tail[2] = {'"', term};
            out.append(tail, 2u);
        }
        return cur;
    }

    ensure(1u);
    *cur++ = '"';
    if (i > 0u) {
        append_raw(p, i);
    }
    static constexpr char kHex[] = "0123456789abcdef";
    while (i < len) {
        const auto b0 = static_cast<std::uint8_t>(p[i]);
        ensure(6u);
        switch (b0) {
            case 0x08u:
                cur[0] = '\\';
                cur[1] = 'b';
                cur += 2u;
                break;
            case 0x09u:
                cur[0] = '\\';
                cur[1] = 't';
                cur += 2u;
                break;
            case 0x0Au:
                cur[0] = '\\';
                cur[1] = 'n';
                cur += 2u;
                break;
            case 0x0Cu:
                cur[0] = '\\';
                cur[1] = 'f';
                cur += 2u;
                break;
            case 0x0Du:
                cur[0] = '\\';
                cur[1] = 'r';
                cur += 2u;
                break;
            case 0x22u:
                cur[0] = '\\';
                cur[1] = '"';
                cur += 2u;
                break;
            case 0x5Cu:
                cur[0] = '\\';
                cur[1] = '\\';
                cur += 2u;
                break;
            default:
                cur[0] = '\\';
                cur[1] = 'u';
                cur[2] = '0';
                cur[3] = '0';
                cur[4] = kHex[(b0 >> 4u) & 0x0Fu];
                cur[5] = kHex[b0 & 0x0Fu];
                cur += 6u;
                break;
        }
        ++i;
        if (i < len) {
            const std::size_t next = utf8_impl::find_non_verbatim_valid_utf8(p, i, len);
            if (next > i) {
                append_raw(p + i, next - i);
                i = next;
            }
        }
    }
    ensure(2u);
    *cur++ = '"';
    *cur++ = term;
    return cur;
}

struct NumPairRunResult {
    char* cur;
    const yyjson_val* val;
    std::size_t ctn_len;
};

NEOJSON_ALWAYS_INLINE char* write_yyjson_num_real_first(char* cur, const yyjson_val* v) noexcept {
    const std::uint8_t tag = unsafe_yyjson_get_tag(v);
    if (NEOJSON_LIKELY(tag == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL))) {
        return write_f64_neojson_fast(cur, unsafe_yyjson_get_real(v));
    }
    if (tag == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT)) {
        return write_u64_fast(cur, static_cast<std::uint64_t>(unsafe_yyjson_get_uint(v)));
    }
    return write_i64_fast(cur, static_cast<std::int64_t>(unsafe_yyjson_get_sint(v)));
}

NEOJSON_NOINLINE NumPairRunResult write_num_pair_run_flat(std::string& out, char* stage, char* cur,
                                                          char* const end, const yyjson_val* val,
                                                          std::size_t ctn_len) {
    for (;;) {
        if (NEOJSON_UNLIKELY(static_cast<std::size_t>(end - cur) < 104u)) {
            cur = flush_stage_to_string(out, stage, cur);
        }
        *cur++ = '[';
        cur = write_yyjson_num_real_first(cur, val + 1);
        *cur++ = ',';
        cur = write_yyjson_num_real_first(cur, val + 2);
        cur[0] = ']';
        cur[1] = ',';
        cur += 2u;
        val += 3;
        if (--ctn_len == 0u) {
            return NumPairRunResult{cur, val, 0u};
        }
        if (unsafe_yyjson_get_tag(val) != (YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE) ||
            unsafe_yyjson_get_len(val) != 2u || !unsafe_yyjson_is_num(val + 1) ||
            !unsafe_yyjson_is_num(val + 2)) {
            return NumPairRunResult{cur, val, ctn_len};
        }
    }
}

NEOJSON_NOINLINE void push_heap_ctx(std::vector<std::size_t>& heap_ctx, const std::size_t tagged) {
    heap_ctx.push_back(tagged);
}

NEOJSON_NOINLINE std::size_t pop_heap_ctx(std::vector<std::size_t>& heap_ctx) {
    const std::size_t tagged = heap_ctx.back();
    heap_ctx.pop_back();
    return tagged;
}

template <bool SkipDupCheck>
NEOJSON_NOINLINE bool dump_yyjson_direct_flat_impl(std::string& out, const yyjson_val* root,
                                                   const std::size_t reserve_hint,
                                                   bool& saw_dup) {
    if constexpr (SkipDupCheck) {
        (void)saw_dup;
    }
    const yyjson_type root_type = unsafe_yyjson_get_type(root);
    const std::size_t root_len = unsafe_yyjson_get_len(root);
    if (root_len == 0u) {
        out.assign(root_type == YYJSON_TYPE_OBJ ? "{}" : "[]", 2u);
        return true;
    }
    if constexpr (!SkipDupCheck) {
        if (root_type == YYJSON_TYPE_OBJ &&
            !is_yyjson_object_unique(const_cast<yyjson_val*>(root))) {
            saw_dup = true;
            return false;
        }
    }
    if (reserve_hint != 0u) {
        out.reserve(reserve_hint);
    }

    static constexpr std::size_t kStageSize = 8192u;
    static constexpr std::size_t kMaxInlineDepth = 64u;
    char stage[kStageSize];
    char* cur = stage;
    char* const end = stage + kStageSize;
    std::size_t inline_ctx[kMaxInlineDepth];
    std::vector<std::size_t> heap_ctx;
    std::size_t depth = 0u;

    bool ctn_obj = (root_type == YYJSON_TYPE_OBJ);
    std::size_t ctn_len = root_len;
    *cur++ = static_cast<char>('[' | (ctn_obj ? 32 : 0));
    const yyjson_val* val = root + 1;

    for (;;) {
        if (NEOJSON_UNLIKELY(static_cast<std::size_t>(end - cur) < 128u)) {
            cur = flush_stage_to_string(out, stage, cur);
        }
        if (ctn_obj) {
            const char* kp = unsafe_yyjson_get_str(val);
            const std::size_t kl = unsafe_yyjson_get_len(val);
            if (NEOJSON_LIKELY(unsafe_yyjson_get_subtype(val) == YYJSON_SUBTYPE_NOESC)) {
                if (NEOJSON_LIKELY(kl <= 32u)) {
                    *cur++ = '"';
                    copy_short_noesc_bytes(cur, kp, kl);
                    cur += kl;
                    cur[0] = '"';
                    cur[1] = ':';
                    cur += 2u;
                } else if (NEOJSON_LIKELY(kl <= 256u)) {
                    if (NEOJSON_UNLIKELY(static_cast<std::size_t>(end - cur) < kl + 96u)) {
                        cur = flush_stage_to_string(out, stage, cur);
                    }
                    *cur++ = '"';
                    std::memcpy(cur, kp, kl);
                    cur += kl;
                    cur[0] = '"';
                    cur[1] = ':';
                    cur += 2u;
                } else {
                    cur = write_long_noesc_flat(out, stage, cur, kp, kl, ':');
                }
            } else {
                cur = write_valid_utf8_escaped_flat(out, stage, cur, end, kp, kl, ':');
                if (NEOJSON_UNLIKELY(static_cast<std::size_t>(end - cur) < 96u)) {
                    cur = flush_stage_to_string(out, stage, cur);
                }
            }
            ++val;
        }

        const std::uint8_t tag = unsafe_yyjson_get_tag(val);
        const std::uint8_t t = tag & YYJSON_TYPE_MASK;
        if (t == YYJSON_TYPE_STR) {
            const char* sp = unsafe_yyjson_get_str(val);
            const std::size_t sl = unsafe_yyjson_get_len(val);
            if (NEOJSON_LIKELY(unsafe_yyjson_get_subtype(val) == YYJSON_SUBTYPE_NOESC)) {
                if (NEOJSON_LIKELY(sl <= 32u)) {
                    *cur++ = '"';
                    copy_short_noesc_bytes(cur, sp, sl);
                    cur += sl;
                    cur[0] = '"';
                    cur[1] = ',';
                    cur += 2u;
                } else if (NEOJSON_LIKELY(sl <= 256u)) {
                    if (NEOJSON_UNLIKELY(static_cast<std::size_t>(end - cur) < sl + 4u)) {
                        cur = flush_stage_to_string(out, stage, cur);
                    }
                    *cur++ = '"';
                    std::memcpy(cur, sp, sl);
                    cur += sl;
                    cur[0] = '"';
                    cur[1] = ',';
                    cur += 2u;
                } else {
                    cur = write_long_noesc_flat(out, stage, cur, sp, sl, ',');
                }
            } else {
                cur = write_valid_utf8_escaped_flat(out, stage, cur, end, sp, sl, ',');
            }
            ++val;
        } else if (t == YYJSON_TYPE_NUM) {
            if (NEOJSON_LIKELY(tag == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT))) {
                cur = write_u64_fast(cur, static_cast<std::uint64_t>(unsafe_yyjson_get_uint(val)));
            } else if (tag == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT)) {
                cur = write_i64_fast(cur, static_cast<std::int64_t>(unsafe_yyjson_get_sint(val)));
            } else {
                cur = write_f64_neojson_outlined(cur, unsafe_yyjson_get_real(val));
            }
            *cur++ = ',';
            ++val;
        } else if ((t & (YYJSON_TYPE_ARR & YYJSON_TYPE_OBJ)) ==
                   (YYJSON_TYPE_ARR & YYJSON_TYPE_OBJ)) {
            const std::size_t clen = unsafe_yyjson_get_len(val);
            const bool child_obj = (t == YYJSON_TYPE_OBJ);
            if (clen == 0u) {
                cur[0] = static_cast<char>('[' | (child_obj ? 32 : 0));
                cur[1] = static_cast<char>(']' | (child_obj ? 32 : 0));
                cur[2] = ',';
                cur += 3u;
                ++val;
            } else if (!child_obj && !ctn_obj && clen == 2u && unsafe_yyjson_is_num(val + 1) &&
                       unsafe_yyjson_is_num(val + 2)) {
                const NumPairRunResult res =
                    write_num_pair_run_flat(out, stage, cur, end, val, ctn_len);
                cur = res.cur;
                val = res.val;
                ctn_len = res.ctn_len;
                if (ctn_len == 0u) {
                    goto close_ctn;
                }
                continue;
            } else {
                if constexpr (!SkipDupCheck) {
                    if (child_obj && !is_yyjson_object_unique(const_cast<yyjson_val*>(val))) {
                        saw_dup = true;
                        return false;
                    }
                }
                const std::size_t tagged = ((ctn_len - 1u) << 1u) | (ctn_obj ? 1u : 0u);
                if (NEOJSON_LIKELY(depth < kMaxInlineDepth)) {
                    inline_ctx[depth++] = tagged;
                } else {
                    push_heap_ctx(heap_ctx, tagged);
                    ++depth;
                }
                *cur++ = static_cast<char>('[' | (child_obj ? 32 : 0));
                ctn_obj = child_obj;
                ctn_len = clen;
                ++val;
                continue;
            }
        } else if (t == YYJSON_TYPE_BOOL) {
            if (unsafe_yyjson_get_subtype(val) == YYJSON_SUBTYPE_TRUE) {
                std::memcpy(cur, "true,", 5u);
                cur += 5u;
            } else {
                std::memcpy(cur, "false,", 6u);
                cur += 6u;
            }
            ++val;
        } else {
            std::memcpy(cur, "null,", 5u);
            cur += 5u;
            ++val;
        }

        if (--ctn_len != 0u) {
            continue;
        }

    close_ctn:
        for (;;) {
            const char close_ch = static_cast<char>(']' | (ctn_obj ? 32 : 0));
            if (NEOJSON_LIKELY(cur > stage)) {
                cur[-1] = close_ch;
            } else {
                out.back() = close_ch;
            }
            if (depth == 0u) {
                const std::size_t n = static_cast<std::size_t>(cur - stage);
                if (n != 0u) {
                    out.append(stage, n);
                }
                return true;
            }
            std::size_t tagged = 0u;
            if (NEOJSON_LIKELY(depth <= kMaxInlineDepth)) {
                tagged = inline_ctx[--depth];
            } else {
                --depth;
                tagged = pop_heap_ctx(heap_ctx);
            }
            ctn_len = tagged >> 1u;
            ctn_obj = (tagged & 1u) != 0u;
            if (NEOJSON_UNLIKELY(cur >= end)) {
                cur = flush_stage_to_string(out, stage, cur);
            }
            *cur++ = ',';
            if (ctn_len != 0u) {
                break;
            }
        }
    }
}

inline bool dump_yyjson_direct_flat(std::string& out, const yyjson_val* root,
                                    const std::size_t reserve_hint, const bool skip_dup_check,
                                    bool& saw_dup) {
    if (NEOJSON_LIKELY(skip_dup_check)) {
        return dump_yyjson_direct_flat_impl<true>(out, root, reserve_hint, saw_dup);
    }
    return dump_yyjson_direct_flat_impl<false>(out, root, reserve_hint, saw_dup);
}

NEOJSON_NOINLINE void dump_yyjson_dup_obj_compact_strict(DumpBuffer& out, yyjson_val* val) {
    bool ignored = false;
    json tmp = materialize_yyjson(val, (std::numeric_limits<std::size_t>::max)(), ignored);
    dump_compact_strict(out, tmp);
}

NEOJSON_NOINLINE void dump_yyjson_dup_obj_value(
    DumpBuffer& out, yyjson_val* val, const bool pretty_print, const bool ensure_ascii,
    const error_handler_t error_handler, const unsigned int indent_step,
    std::string& indent_string, const unsigned int current_indent) {
    bool ignored = false;
    json tmp = materialize_yyjson(val, (std::numeric_limits<std::size_t>::max)(), ignored);
    dump_value(out, tmp, pretty_print, ensure_ascii, error_handler, indent_step, indent_string,
               current_indent);
}

inline void dump_eager_array_compact_strict(DumpBuffer& out, const json::array_t& arr) {
    if (arr.empty()) {
        out.append("[]", 2);
        return;
    }
    out.push('[');
    for (std::size_t i = 0; i < arr.size(); ++i) {
        if (i > 0) {
            out.push(',');
        }
        dump_compact_strict(out, arr[i]);
    }
    out.push(']');
}

inline void dump_eager_object_compact_strict(DumpBuffer& out, const json::object_t& obj) {
    if (obj.empty()) {
        out.append("{}", 2);
        return;
    }
    out.push('{');
    for (std::size_t i = 0; i < obj.size(); ++i) {
        if (i > 0) {
            out.push(',');
        }
        utf8_impl::dump_key_colon_fast_strict(out, obj[i].first);
        dump_compact_strict(out, obj[i].second);
    }
    out.push('}');
}

inline void dump_yyjson_string_compact_strict(DumpBuffer& out, yyjson_val* val) {
    const char* sp = unsafe_yyjson_get_str(val);
    const std::size_t sl = unsafe_yyjson_get_len(val);
    if (unsafe_yyjson_get_subtype(val) == YYJSON_SUBTYPE_NOESC) {
        out.reserve_extra(sl + 2u);
        out.push_unchecked('"');
        out.append_unchecked(sp, sl);
        out.push_unchecked('"');
    } else {
        utf8_impl::dump_valid_utf8_escaped_strict(out, sp, sl, false);
    }
}

inline void dump_yyjson_key_colon_compact_strict(DumpBuffer& out, yyjson_val* key) {
    const char* kp = unsafe_yyjson_get_str(key);
    const std::size_t kl = unsafe_yyjson_get_len(key);
    if (unsafe_yyjson_get_subtype(key) == YYJSON_SUBTYPE_NOESC) {
        out.reserve_extra(kl + 3u);
        out.push_unchecked('"');
        out.append_unchecked(kp, kl);
        out.push_unchecked('"');
        out.push_unchecked(':');
    } else {
        utf8_impl::dump_valid_utf8_escaped_strict(out, kp, kl, true);
    }
}

void dump_yyjson_val_compact_strict(DumpBuffer& out, SharedYyDoc* sdoc, yyjson_val* val,
                                    bool& saw_dup, yyjson_val* preverified_obj = nullptr) {
    switch (yyjson_get_tag(val)) {
        case YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE:
            out.append("null", 4);
            return;

        case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE:
            out.append("true", 4);
            return;

        case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE:
            out.append("false", 5);
            return;

        case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT:
            append_uint(out, static_cast<json::number_unsigned_t>(unsafe_yyjson_get_uint(val)));
            return;

        case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT:
            append_int(out, static_cast<json::number_integer_t>(unsafe_yyjson_get_sint(val)));
            return;

        case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL:
            dtoa_impl::append_float(out, static_cast<json::number_float_t>(unsafe_yyjson_get_real(val)));
            return;

        case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE:
        case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
            dump_yyjson_string_compact_strict(out, val);
            return;

        case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE: {
            const std::size_t len = unsafe_yyjson_get_len(val);
            if (len == 0u) {
                out.append("[]", 2);
                return;
            }
            out.push('[');
            yyjson_val* cur = unsafe_yyjson_get_first(val);
            for (std::size_t i = 0u; i < len; ++i) {
                if (i > 0u) {
                    out.push(',');
                }
                dump_yyjson_val_compact_strict(out, sdoc, cur, saw_dup, nullptr);
                if (i + 1u < len) {
                    cur = unsafe_yyjson_get_next(cur);
                }
            }
            out.push(']');
            return;
        }

        case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE: {
            const std::size_t len = unsafe_yyjson_get_len(val);
            if (len == 0u) {
                out.append("{}", 2);
                return;
            }
            if (val != preverified_obj &&
                !sdoc->tree_unique_verified.load(std::memory_order_acquire) &&
                !is_yyjson_object_unique(val)) {
                saw_dup = true;
                dump_yyjson_dup_obj_compact_strict(out, val);
                return;
            }
            out.push('{');
            yyjson_obj_iter iter = yyjson_obj_iter_with(val);
            yyjson_val* key = nullptr;
            bool first = true;
            while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
                if (!first) {
                    out.push(',');
                }
                first = false;
                dump_yyjson_key_colon_compact_strict(out, key);
                dump_yyjson_val_compact_strict(out, sdoc, yyjson_obj_iter_get_val(key), saw_dup,
                                               nullptr);
            }
            out.push('}');
            return;
        }

        default:
            out.append("null", 4);
            return;
    }
}

inline void dump_arena_array_compact_strict(DumpBuffer& out, const ArenaArray* aa) {
    const std::size_t n = aa->count;
    if (n == 0u) {
        out.append("[]", 2);
        return;
    }
    out.push('[');
    const json* elems = aa->elems();
    for (std::size_t i = 0u; i < n; ++i) {
        if (i > 0u) {
            out.push(',');
        }
        dump_compact_strict(out, elems[i]);
    }
    out.push(']');
}

inline void dump_arena_object_compact_strict(DumpBuffer& out, const ArenaObject* ao) {
    const std::size_t n = ao->count;
    if (n == 0u) {
        out.append("{}", 2);
        return;
    }
    out.push('{');
    const std::string* ks = ao->keys();
    const json* vs = ao->vals();
    for (std::size_t i = 0u; i < n; ++i) {
        if (i > 0u) {
            out.push(',');
        }
        utf8_impl::dump_key_colon_fast_strict(out, ks[i]);
        dump_compact_strict(out, vs[i]);
    }
    out.push('}');
}

void dump_yyjson_node_compact_strict(DumpBuffer& out, LazyNode* node, const value_t type) {
    void* mat = node->materialized.load(std::memory_order_acquire);
    if (mat != nullptr) {
        if (type == value_t::array) {
            dump_eager_array_compact_strict(out, *static_cast<const json::array_t*>(mat));
        } else {
            dump_eager_object_compact_strict(out, *static_cast<const json::object_t*>(mat));
        }
        return;
    }
    if (void* av = node->arena_view.load(std::memory_order_acquire)) {
        if (type == value_t::array) {
            dump_arena_array_compact_strict(out, static_cast<const ArenaArray*>(av));
        } else {
            dump_arena_object_compact_strict(out, static_cast<const ArenaObject*>(av));
        }
        return;
    }
    if (type == value_t::object &&
        !node->doc->tree_unique_verified.load(std::memory_order_acquire) &&
        (node->dedup_state.load(std::memory_order_acquire) & 1u) == 0u &&
        !is_yyjson_object_unique(node->val)) {
        mark_own_dup(node);
        const auto* obj = static_cast<const json::object_t*>(
            lazy_materialize_const(node, value_t::object));
        dump_eager_object_compact_strict(out, *obj);
        return;
    }
    bool saw_dup = false;
    dump_yyjson_val_compact_strict(out, node->doc, node->val, saw_dup,
                                   type == value_t::object ? node->val : nullptr);
    if (type == value_t::object) {
        mark_own_unique(node);
    }
    if (!saw_dup && node->val == yyjson_doc_get_root(node->doc->doc)) {
        node->doc->tree_unique_verified.store(true, std::memory_order_release);
    }
}

inline void dump_eager_array_value(DumpBuffer& out, const json::array_t& arr,
                                   const bool pretty_print, const bool ensure_ascii,
                                   const error_handler_t error_handler,
                                   const unsigned int indent_step, std::string& indent_string,
                                   const unsigned int current_indent) {
    if (arr.empty()) {
        out.append("[]", 2);
        return;
    }
    if (pretty_print) {
        out.append("[\n", 2);
        const unsigned int new_indent = current_indent + indent_step;
        if (indent_string.size() < new_indent) {
            indent_string.resize(
                (std::max)(indent_string.size() * 2, static_cast<std::size_t>(new_indent)), ' ');
        }
        for (std::size_t i = 0; i < arr.size(); ++i) {
            out.append(indent_string.data(), new_indent);
            dump_value(out, arr[i], true, ensure_ascii, error_handler, indent_step, indent_string,
                       new_indent);
            if (i + 1 < arr.size()) {
                out.append(",\n", 2);
            } else {
                out.push('\n');
            }
        }
        out.append(indent_string.data(), current_indent);
        out.push(']');
    } else {
        out.push('[');
        for (std::size_t i = 0; i < arr.size(); ++i) {
            if (i > 0) {
                out.push(',');
            }
            dump_value(out, arr[i], false, ensure_ascii, error_handler, indent_step, indent_string,
                       current_indent);
        }
        out.push(']');
    }
}

inline void dump_eager_object_value(DumpBuffer& out, const json::object_t& obj,
                                    const bool pretty_print, const bool ensure_ascii,
                                    const error_handler_t error_handler,
                                    const unsigned int indent_step, std::string& indent_string,
                                    const unsigned int current_indent) {
    if (obj.empty()) {
        out.append("{}", 2);
        return;
    }
    if (pretty_print) {
        out.append("{\n", 2);
        const unsigned int new_indent = current_indent + indent_step;
        if (indent_string.size() < new_indent) {
            indent_string.resize(
                (std::max)(indent_string.size() * 2, static_cast<std::size_t>(new_indent)), ' ');
        }
        for (std::size_t i = 0; i < obj.size(); ++i) {
            out.append(indent_string.data(), new_indent);
            out.push('"');
            utf8_impl::dump_escaped(out, obj[i].first, ensure_ascii, error_handler);
            out.append("\": ", 3);
            dump_value(out, obj[i].second, true, ensure_ascii, error_handler, indent_step,
                       indent_string, new_indent);
            if (i + 1 < obj.size()) {
                out.append(",\n", 2);
            } else {
                out.push('\n');
            }
        }
        out.append(indent_string.data(), current_indent);
        out.push('}');
    } else {
        out.push('{');
        for (std::size_t i = 0; i < obj.size(); ++i) {
            if (i > 0) {
                out.push(',');
            }
            out.push('"');
            utf8_impl::dump_escaped(out, obj[i].first, ensure_ascii, error_handler);
            out.append("\":", 2);
            dump_value(out, obj[i].second, false, ensure_ascii, error_handler, indent_step,
                       indent_string, current_indent);
        }
        out.push('}');
    }
}

void dump_yyjson_val(DumpBuffer& out, SharedYyDoc* sdoc, yyjson_val* val,
                     const bool pretty_print, const bool ensure_ascii,
                     const error_handler_t error_handler, const unsigned int indent_step,
                     std::string& indent_string, const unsigned int current_indent, bool& saw_dup,
                     yyjson_val* preverified_obj = nullptr) {
    switch (yyjson_get_tag(val)) {
        case YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE:
            out.append("null", 4);
            return;

        case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE:
            out.append("true", 4);
            return;

        case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE:
            out.append("false", 5);
            return;

        case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT:
            append_uint(out, static_cast<json::number_unsigned_t>(unsafe_yyjson_get_uint(val)));
            return;

        case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT:
            append_int(out, static_cast<json::number_integer_t>(unsafe_yyjson_get_sint(val)));
            return;

        case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL:
            dtoa_impl::append_float(out, static_cast<json::number_float_t>(unsafe_yyjson_get_real(val)));
            return;

        case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE:
        case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC: {
            const char* sp = unsafe_yyjson_get_str(val);
            const std::size_t sl = unsafe_yyjson_get_len(val);
            if (!ensure_ascii && unsafe_yyjson_get_subtype(val) == YYJSON_SUBTYPE_NOESC) {
                out.reserve_extra(sl + 2u);
                out.push_unchecked('"');
                out.append_unchecked(sp, sl);
                out.push_unchecked('"');
            } else {
                out.push('"');
                utf8_impl::dump_escaped(out, std::string_view(sp, sl), ensure_ascii, error_handler);
                out.push('"');
            }
            return;
        }

        case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE: {
            const std::size_t len = unsafe_yyjson_get_len(val);
            if (len == 0u) {
                out.append("[]", 2);
                return;
            }
            yyjson_val* cur = unsafe_yyjson_get_first(val);
            if (pretty_print) {
                out.append("[\n", 2);
                const unsigned int new_indent = current_indent + indent_step;
                if (indent_string.size() < new_indent) {
                    indent_string.resize(
                        (std::max)(indent_string.size() * 2, static_cast<std::size_t>(new_indent)),
                        ' ');
                }
                for (std::size_t i = 0u; i < len; ++i) {
                    out.append(indent_string.data(), new_indent);
                    dump_yyjson_val(out, sdoc, cur, true, ensure_ascii, error_handler, indent_step,
                                    indent_string, new_indent, saw_dup, nullptr);
                    if (i + 1u < len) {
                        out.append(",\n", 2);
                        cur = unsafe_yyjson_get_next(cur);
                    } else {
                        out.push('\n');
                    }
                }
                out.append(indent_string.data(), current_indent);
                out.push(']');
            } else {
                out.push('[');
                for (std::size_t i = 0u; i < len; ++i) {
                    if (i > 0u) {
                        out.push(',');
                    }
                    dump_yyjson_val(out, sdoc, cur, false, ensure_ascii, error_handler,
                                    indent_step, indent_string, current_indent, saw_dup, nullptr);
                    if (i + 1u < len) {
                        cur = unsafe_yyjson_get_next(cur);
                    }
                }
                out.push(']');
            }
            return;
        }

        case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE: {
            const std::size_t len = unsafe_yyjson_get_len(val);
            if (len == 0u) {
                out.append("{}", 2);
                return;
            }
            if (val != preverified_obj &&
                !sdoc->tree_unique_verified.load(std::memory_order_acquire) &&
                !is_yyjson_object_unique(val)) {
                saw_dup = true;
                dump_yyjson_dup_obj_value(out, val, pretty_print, ensure_ascii, error_handler,
                                          indent_step, indent_string, current_indent);
                return;
            }
            yyjson_obj_iter iter = yyjson_obj_iter_with(val);
            yyjson_val* key = nullptr;
            if (pretty_print) {
                out.append("{\n", 2);
                const unsigned int new_indent = current_indent + indent_step;
                if (indent_string.size() < new_indent) {
                    indent_string.resize(
                        (std::max)(indent_string.size() * 2, static_cast<std::size_t>(new_indent)),
                        ' ');
                }
                std::size_t idx = 0u;
                while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
                    out.append(indent_string.data(), new_indent);
                    const char* kp = unsafe_yyjson_get_str(key);
                    const std::size_t kl = unsafe_yyjson_get_len(key);
                    if (!ensure_ascii && unsafe_yyjson_get_subtype(key) == YYJSON_SUBTYPE_NOESC) {
                        out.reserve_extra(kl + 4u);
                        out.push_unchecked('"');
                        out.append_unchecked(kp, kl);
                        out.append_unchecked("\": ", 3);
                    } else {
                        out.push('"');
                        utf8_impl::dump_escaped(out, std::string_view(kp, kl), ensure_ascii,
                                                error_handler);
                        out.append("\": ", 3);
                    }
                    dump_yyjson_val(out, sdoc, yyjson_obj_iter_get_val(key), true, ensure_ascii,
                                    error_handler, indent_step, indent_string, new_indent,
                                    saw_dup, nullptr);
                    if (idx + 1u < len) {
                        out.append(",\n", 2);
                    } else {
                        out.push('\n');
                    }
                    ++idx;
                }
                out.append(indent_string.data(), current_indent);
                out.push('}');
            } else {
                out.push('{');
                bool first = true;
                while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
                    if (!first) {
                        out.push(',');
                    }
                    first = false;
                    const char* kp = unsafe_yyjson_get_str(key);
                    const std::size_t kl = unsafe_yyjson_get_len(key);
                    if (!ensure_ascii && unsafe_yyjson_get_subtype(key) == YYJSON_SUBTYPE_NOESC) {
                        out.reserve_extra(kl + 3u);
                        out.push_unchecked('"');
                        out.append_unchecked(kp, kl);
                        out.append_unchecked("\":", 2);
                    } else {
                        out.push('"');
                        utf8_impl::dump_escaped(out, std::string_view(kp, kl), ensure_ascii,
                                                error_handler);
                        out.append("\":", 2);
                    }
                    dump_yyjson_val(out, sdoc, yyjson_obj_iter_get_val(key), false, ensure_ascii,
                                    error_handler, indent_step, indent_string, current_indent,
                                    saw_dup, nullptr);
                }
                out.push('}');
            }
            return;
        }

        default:
            out.append("null", 4);
            return;
    }
}

inline void dump_arena_array_value(DumpBuffer& out, const ArenaArray* aa,
                                   const bool pretty_print, const bool ensure_ascii,
                                   const error_handler_t error_handler,
                                   const unsigned int indent_step, std::string& indent_string,
                                   const unsigned int current_indent) {
    const std::size_t n = aa->count;
    if (n == 0u) {
        out.append("[]", 2);
        return;
    }
    const json* elems = aa->elems();
    if (pretty_print) {
        out.append("[\n", 2);
        const unsigned int new_indent = current_indent + indent_step;
        if (indent_string.size() < new_indent) {
            indent_string.resize(
                (std::max)(indent_string.size() * 2, static_cast<std::size_t>(new_indent)), ' ');
        }
        for (std::size_t i = 0u; i < n; ++i) {
            out.append(indent_string.data(), new_indent);
            dump_value(out, elems[i], true, ensure_ascii, error_handler, indent_step,
                       indent_string, new_indent);
            if (i + 1u < n) {
                out.append(",\n", 2);
            } else {
                out.push('\n');
            }
        }
        out.append(indent_string.data(), current_indent);
        out.push(']');
    } else {
        out.push('[');
        for (std::size_t i = 0u; i < n; ++i) {
            if (i > 0u) {
                out.push(',');
            }
            dump_value(out, elems[i], false, ensure_ascii, error_handler, indent_step,
                       indent_string, current_indent);
        }
        out.push(']');
    }
}

inline void dump_arena_object_value(DumpBuffer& out, const ArenaObject* ao,
                                    const bool pretty_print, const bool ensure_ascii,
                                    const error_handler_t error_handler,
                                    const unsigned int indent_step, std::string& indent_string,
                                    const unsigned int current_indent) {
    const std::size_t n = ao->count;
    if (n == 0u) {
        out.append("{}", 2);
        return;
    }
    const std::string* ks = ao->keys();
    const json* vs = ao->vals();
    if (pretty_print) {
        out.append("{\n", 2);
        const unsigned int new_indent = current_indent + indent_step;
        if (indent_string.size() < new_indent) {
            indent_string.resize(
                (std::max)(indent_string.size() * 2, static_cast<std::size_t>(new_indent)), ' ');
        }
        for (std::size_t i = 0u; i < n; ++i) {
            out.append(indent_string.data(), new_indent);
            out.push('"');
            utf8_impl::dump_escaped(out, ks[i], ensure_ascii, error_handler);
            out.append("\": ", 3);
            dump_value(out, vs[i], true, ensure_ascii, error_handler, indent_step, indent_string,
                       new_indent);
            if (i + 1u < n) {
                out.append(",\n", 2);
            } else {
                out.push('\n');
            }
        }
        out.append(indent_string.data(), current_indent);
        out.push('}');
    } else {
        out.push('{');
        for (std::size_t i = 0u; i < n; ++i) {
            if (i > 0u) {
                out.push(',');
            }
            out.push('"');
            utf8_impl::dump_escaped(out, ks[i], ensure_ascii, error_handler);
            out.append("\":", 2);
            dump_value(out, vs[i], false, ensure_ascii, error_handler, indent_step, indent_string,
                       current_indent);
        }
        out.push('}');
    }
}

void dump_yyjson_node(DumpBuffer& out, LazyNode* node, const value_t type,
                      const bool pretty_print, const bool ensure_ascii,
                      const error_handler_t error_handler, const unsigned int indent_step,
                      std::string& indent_string, const unsigned int current_indent) {
    void* mat = node->materialized.load(std::memory_order_acquire);
    if (mat != nullptr) {
        if (type == value_t::array) {
            dump_eager_array_value(out, *static_cast<const json::array_t*>(mat), pretty_print,
                                   ensure_ascii, error_handler, indent_step, indent_string,
                                   current_indent);
        } else {
            dump_eager_object_value(out, *static_cast<const json::object_t*>(mat), pretty_print,
                                    ensure_ascii, error_handler, indent_step, indent_string,
                                    current_indent);
        }
        return;
    }
    if (void* av = node->arena_view.load(std::memory_order_acquire)) {
        if (type == value_t::array) {
            dump_arena_array_value(out, static_cast<const ArenaArray*>(av), pretty_print,
                                   ensure_ascii, error_handler, indent_step, indent_string,
                                   current_indent);
        } else {
            dump_arena_object_value(out, static_cast<const ArenaObject*>(av), pretty_print,
                                    ensure_ascii, error_handler, indent_step, indent_string,
                                    current_indent);
        }
        return;
    }
    if (type == value_t::object &&
        !node->doc->tree_unique_verified.load(std::memory_order_acquire) &&
        (node->dedup_state.load(std::memory_order_acquire) & 1u) == 0u &&
        !is_yyjson_object_unique(node->val)) {
        mark_own_dup(node);
        const auto* obj = static_cast<const json::object_t*>(
            lazy_materialize_const(node, value_t::object));
        dump_eager_object_value(out, *obj, pretty_print, ensure_ascii, error_handler, indent_step,
                                indent_string, current_indent);
        return;
    }
    bool saw_dup = false;
    dump_yyjson_val(out, node->doc, node->val, pretty_print, ensure_ascii, error_handler,
                    indent_step, indent_string, current_indent, saw_dup,
                    type == value_t::object ? node->val : nullptr);
    if (type == value_t::object) {
        mark_own_unique(node);
    }
    if (!saw_dup && node->val == yyjson_doc_get_root(node->doc->doc)) {
        node->doc->tree_unique_verified.store(true, std::memory_order_release);
    }
}

inline void dump_small_array_compact_strict(DumpBuffer& out, const SmallArray2& sa) {
    out.push('[');
    for (std::size_t i = 0u; i < sa.count; ++i) {
        if (i > 0u) {
            out.push(',');
        }
        dump_compact_strict(out, sa.elem(i));
    }
    out.push(']');
}

inline void dump_small_array_value(DumpBuffer& out, const SmallArray2& sa,
                                   const bool pretty_print, const bool ensure_ascii,
                                   const error_handler_t error_handler,
                                   const unsigned int indent_step, std::string& indent_string,
                                   const unsigned int current_indent) {
    if (sa.count == 0u) {
        out.append("[]", 2);
        return;
    }
    if (pretty_print) {
        out.append("[\n", 2);
        const unsigned int new_indent = current_indent + indent_step;
        if (indent_string.size() < new_indent) {
            indent_string.resize(
                (std::max)(indent_string.size() * 2, static_cast<std::size_t>(new_indent)), ' ');
        }
        for (std::size_t i = 0u; i < sa.count; ++i) {
            out.append(indent_string.data(), new_indent);
            dump_value(out, sa.elem(i), true, ensure_ascii, error_handler, indent_step,
                       indent_string, new_indent);
            if (i + 1u < sa.count) {
                out.append(",\n", 2);
            } else {
                out.push('\n');
            }
        }
        out.append(indent_string.data(), current_indent);
        out.push(']');
    } else {
        out.push('[');
        for (std::size_t i = 0u; i < sa.count; ++i) {
            if (i > 0u) {
                out.push(',');
            }
            dump_value(out, sa.elem(i), false, ensure_ascii, error_handler, indent_step,
                       indent_string, current_indent);
        }
        out.push(']');
    }
}

inline void dump_small_object_compact_strict(DumpBuffer& out, const SmallObject4& so) {
    if (so.count == 0u) {
        out.append("{}", 2);
        return;
    }
    out.push('{');
    for (std::size_t i = 0u; i < so.count; ++i) {
        if (i > 0u) {
            out.push(',');
        }
        utf8_impl::dump_key_colon_fast_strict(out, so.key_view(i));
        dump_compact_strict(out, so.val(i));
    }
    out.push('}');
}

inline void dump_small_object_value(DumpBuffer& out, const SmallObject4& so,
                                    const bool pretty_print, const bool ensure_ascii,
                                    const error_handler_t error_handler,
                                    const unsigned int indent_step, std::string& indent_string,
                                    const unsigned int current_indent) {
    if (so.count == 0u) {
        out.append("{}", 2);
        return;
    }
    if (pretty_print) {
        out.append("{\n", 2);
        const unsigned int new_indent = current_indent + indent_step;
        if (indent_string.size() < new_indent) {
            indent_string.resize(
                (std::max)(indent_string.size() * 2, static_cast<std::size_t>(new_indent)), ' ');
        }
        for (std::size_t i = 0u; i < so.count; ++i) {
            out.append(indent_string.data(), new_indent);
            out.push('"');
            utf8_impl::dump_escaped(out, so.key_view(i), ensure_ascii, error_handler);
            out.append("\": ", 3);
            dump_value(out, so.val(i), true, ensure_ascii, error_handler, indent_step,
                       indent_string, new_indent);
            if (i + 1u < so.count) {
                out.append(",\n", 2);
            } else {
                out.push('\n');
            }
        }
        out.append(indent_string.data(), current_indent);
        out.push('}');
    } else {
        out.push('{');
        for (std::size_t i = 0u; i < so.count; ++i) {
            if (i > 0u) {
                out.push(',');
            }
            out.push('"');
            utf8_impl::dump_escaped(out, so.key_view(i), ensure_ascii, error_handler);
            out.append("\":", 2);
            dump_value(out, so.val(i), false, ensure_ascii, error_handler, indent_step,
                       indent_string, current_indent);
        }
        out.push('}');
    }
}

void dump_compact_strict(DumpBuffer& out, const json& val) {
    switch (json_access::type_of(val)) {
        case value_t::null:
            out.append("null", 4);
            return;

        case value_t::boolean:
            if (json_access::bool_of(val)) {
                out.append("true", 4);
            } else {
                out.append("false", 5);
            }
            return;

        case value_t::number_integer:
            append_int(out, json_access::int_of(val));
            return;

        case value_t::number_unsigned:
            append_uint(out, json_access::uint_of(val));
            return;

        case value_t::number_float:
            dtoa_impl::append_float(out, json_access::float_of(val));
            return;

        case value_t::string:
            utf8_impl::dump_quoted_fast_strict(out, json_access::string_of(val));
            return;

        case value_t::array:
            if (json_access::is_lazy(val)) {
                dump_yyjson_node_compact_strict(
                    out, static_cast<LazyNode*>(json_access::lazy_node(val)), value_t::array);
            } else if (json_access::is_arena_array(val)) {
                dump_arena_array_compact_strict(out, json_access::arena_array_ptr(val));
            } else if (json_access::is_small_array(val)) {
                dump_small_array_compact_strict(out, json_access::small_array_of(val));
            } else if (json_access::is_empty_container(val)) {
                out.append("[]", 2);
            } else {
                dump_eager_array_compact_strict(out, *json_access::raw_array_ptr(val));
            }
            return;

        case value_t::object:
            if (json_access::is_lazy(val)) {
                dump_yyjson_node_compact_strict(
                    out, static_cast<LazyNode*>(json_access::lazy_node(val)), value_t::object);
            } else if (json_access::is_arena_object(val)) {
                dump_arena_object_compact_strict(out, json_access::arena_object_ptr(val));
            } else if (json_access::is_small_object(val)) {
                dump_small_object_compact_strict(out, json_access::small_object_of(val));
            } else if (json_access::is_empty_container(val)) {
                out.append("{}", 2);
            } else {
                dump_eager_object_compact_strict(out, *json_access::raw_object_ptr(val));
            }
            return;

        case value_t::discarded:
            out.append("<discarded>", 11);
            return;

        case value_t::binary:
        default:
            return;
    }
}

void dump_value(DumpBuffer& out, const json& val, const bool pretty_print,
                const bool ensure_ascii, const error_handler_t error_handler,
                const unsigned int indent_step, std::string& indent_string,
                const unsigned int current_indent) {
    switch (json_access::type_of(val)) {
        case value_t::null:
            out.append("null", 4);
            return;

        case value_t::boolean:
            if (json_access::bool_of(val)) {
                out.append("true", 4);
            } else {
                out.append("false", 5);
            }
            return;

        case value_t::number_integer:
            append_int(out, json_access::int_of(val));
            return;

        case value_t::number_unsigned:
            append_uint(out, json_access::uint_of(val));
            return;

        case value_t::number_float:
            dtoa_impl::append_float(out, json_access::float_of(val));
            return;

        case value_t::string:
            out.push('"');
            utf8_impl::dump_escaped(out, json_access::string_of(val), ensure_ascii, error_handler);
            out.push('"');
            return;

        case value_t::array:
            if (json_access::is_lazy(val)) {
                dump_yyjson_node(out, static_cast<LazyNode*>(json_access::lazy_node(val)),
                                 value_t::array, pretty_print, ensure_ascii, error_handler,
                                 indent_step, indent_string, current_indent);
            } else if (json_access::is_arena_array(val)) {
                dump_arena_array_value(out, json_access::arena_array_ptr(val), pretty_print,
                                       ensure_ascii, error_handler, indent_step, indent_string,
                                       current_indent);
            } else if (json_access::is_small_array(val)) {
                dump_small_array_value(out, json_access::small_array_of(val), pretty_print,
                                       ensure_ascii, error_handler, indent_step, indent_string,
                                       current_indent);
            } else if (json_access::is_empty_container(val)) {
                out.append("[]", 2);
            } else {
                dump_eager_array_value(out, *json_access::raw_array_ptr(val), pretty_print,
                                       ensure_ascii, error_handler, indent_step, indent_string,
                                       current_indent);
            }
            return;

        case value_t::object:
            if (json_access::is_lazy(val)) {
                dump_yyjson_node(out, static_cast<LazyNode*>(json_access::lazy_node(val)),
                                 value_t::object, pretty_print, ensure_ascii, error_handler,
                                 indent_step, indent_string, current_indent);
            } else if (json_access::is_arena_object(val)) {
                dump_arena_object_value(out, json_access::arena_object_ptr(val), pretty_print,
                                        ensure_ascii, error_handler, indent_step, indent_string,
                                        current_indent);
            } else if (json_access::is_small_object(val)) {
                dump_small_object_value(out, json_access::small_object_of(val), pretty_print,
                                        ensure_ascii, error_handler, indent_step, indent_string,
                                        current_indent);
            } else if (json_access::is_empty_container(val)) {
                out.append("{}", 2);
            } else {
                dump_eager_object_value(out, *json_access::raw_object_ptr(val), pretty_print,
                                        ensure_ascii, error_handler, indent_step, indent_string,
                                        current_indent);
            }
            return;

        case value_t::discarded:
            out.append("<discarded>", 11);
            return;

        case value_t::binary:
        default:
            return;
    }
}

std::string_view member_key_view(yyjson_val* key) {
    return std::string_view(unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
}

// Lists the keys of the parsed object `val` (`len` members) in document order in `keys`, and the positions 0..len-1
// ordered by key, with the position as a tie-break, in `order`. The cost is len*log(len) key comparisons whatever
// the keys are (a hash table could degrade to a scan on keys that collide).
void sort_member_keys(yyjson_val* val, const std::size_t len, std::vector<yyjson_val*>& keys,
                      std::vector<std::size_t>& order) {
    keys.reserve(len);
    {
        yyjson_obj_iter iter = yyjson_obj_iter_with(val);
        yyjson_val* key = nullptr;
        while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
            keys.push_back(key);
        }
    }

    order.resize(len);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&keys](const std::size_t a, const std::size_t b) {
        const int c = member_key_view(keys[a]).compare(member_key_view(keys[b]));
        return c != 0 ? c < 0 : a < b;
    });
}

// For every member of the parsed object `val` (`len` members), the position of the first member with the same key.
std::vector<std::size_t> first_occurrences_sorted(yyjson_val* val, const std::size_t len) {
    std::vector<yyjson_val*> keys;
    std::vector<std::size_t> order;
    sort_member_keys(val, len, keys, order);

    std::vector<std::size_t> first(len);
    for (std::size_t run = 0; run < len;) {
        std::size_t end = run + 1;
        while (end < len && member_key_view(keys[order[end]]) == member_key_view(keys[order[run]])) {
            ++end;
        }
        for (std::size_t i = run; i < end; ++i) {
            first[order[i]] = order[run];
        }
        run = end;
    }
    return first;
}

// Builds the node tree of a parsed document in a single pass while enforcing `max_depth` (same semantics as
// `within_depth`). Keeps its own explicit container stack so C stack usage is constant with nesting depth.
json materialize_yyjson(yyjson_val* root, const std::size_t max_depth, bool& depth_exceeded) {
    depth_exceeded = false;
    if (!yyjson_is_ctn(root)) {
        return make_node(root);
    }

    struct frame {
        bool is_array;
        void* container;
        yyjson_val* next;
        std::size_t left;
    };

    json result = make_node(root);
    std::vector<frame> open;
    open.reserve(32);

    // Starts filling `node`, which was made from the container `val`.
    auto open_container = [&open, max_depth](json& node, yyjson_val* val) -> bool {
        if (open.size() >= max_depth) {
            return false;
        }
        const std::size_t len = unsafe_yyjson_get_len(val);
        if (len == 0) {
            return true;
        }
        if (yyjson_is_arr(val)) {
            auto* arr = node.get_ptr<json::array_t*>();
            arr->reserve(len);
            open.push_back({true, arr, unsafe_yyjson_get_first(val), len});
        } else {
            auto* obj = node.get_ptr<json::object_t*>();
            obj->reserve_for_build(len);
            open.push_back({false, obj, unsafe_yyjson_get_first(val), len});
        }
        return true;
    };

    if (!open_container(result, root)) {
        depth_exceeded = true;
        return json(json::value_t::discarded);
    }
    while (!open.empty()) {
        frame& top = open.back();
        if (top.left == 0) {
            open.pop_back();
            continue;
        }
        --top.left;

        json* child = nullptr;
        yyjson_val* child_val = nullptr;
        if (top.is_array) {
            child_val = top.next;
            top.next = unsafe_yyjson_get_next(child_val);
            auto* arr = static_cast<json::array_t*>(top.container);
            child = &arr->emplace_back(make_node(child_val));
        } else {
            yyjson_val* key = top.next;
            child_val = yyjson_obj_iter_get_val(key);
            top.next = unsafe_yyjson_get_next(child_val);
            const std::string_view k(unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
            auto* obj = static_cast<json::object_t*>(top.container);
            child = &obj->emplace_or_assign_key(k, make_node(child_val));
        }
        // `top` may be invalid from here on: opening the child grows the stack.
        if (yyjson_is_ctn(child_val)) {
            if (!open_container(*child, child_val)) {
                depth_exceeded = true;
                return json(json::value_t::discarded);
            }
        }
    }
    return result;
}

// Whether the arrays and objects of the parsed document nest at most `max_depth` levels deep. The outermost array
// or object is level 1, an empty array or object counts as a level, a single value has no level. Iterative: the
// stack holds one entry per open array or object and never grows past max_depth entries.
bool within_depth(yyjson_doc* doc, yyjson_val* root, const std::size_t max_depth) {
    if (!yyjson_is_ctn(root)) {
        return true;
    }
    // Every level is a value of the document, so a document with no more values than max_depth cannot exceed it.
    if (yyjson_doc_get_val_count(doc) <= max_depth) {
        return true;
    }

    struct frame {
        yyjson_val* next;
        std::size_t left;
    };
    std::vector<frame> open;

    auto enter = [&open, max_depth](yyjson_val* ctn) {
        if (open.size() >= max_depth) {
            return false;
        }
        const std::size_t len = unsafe_yyjson_get_len(ctn);
        const std::size_t children = yyjson_is_obj(ctn) ? 2 * len : len;
        if (children != 0) {
            open.push_back({unsafe_yyjson_get_first(ctn), children});
        }
        return true;
    };

    if (!enter(root)) {
        return false;
    }
    while (!open.empty()) {
        frame& top = open.back();
        if (top.left == 0) {
            open.pop_back();
            continue;
        }
        yyjson_val* v = top.next;
        top.next = unsafe_yyjson_get_next(v);
        --top.left;
        if (yyjson_is_ctn(v) && !enter(v)) {
            return false;
        }
    }
    return true;
}

// Builds the tree of a parse with a callback. It does what nlohmann's SAX parser for callbacks does, call for call:
// a key inserts a member holding a discarded value (an existing member of that key takes the discarded value and keeps
// its position); a value that is kept replaces it; a value that is dropped leaves it in the object; and whenever an
// object ends, the first discarded child of its parent, if there is one, is removed.
//
// Doing that literally costs time in proportion to the size of the parent for every key and every object that ends.
// Here, a key finds an existing member through the sorted keys of its object (for objects above kObjectIndexThreshold
// members; smaller ones are scanned), and the children that may be discarded are listed in a heap, so the first
// one is found without a scan. A removed child stays in place and is dropped when its parent ends, just before the
// callback sees the parent, so positions do not move while the parent is being filled. Nothing else can observe the
// parent before that.
class dom_callback_builder {
public:
    dom_callback_builder(json& r, const json::parser_callback_t& cb)
        : root(r), callback(cb) {
        keep_stack.push_back(true);
    }

    // Feeds the parsed document to the builder in document order. It keeps its own stack of the open arrays and
    // objects, so the call stack does not grow with the nesting of the document.
    void parse_tree(yyjson_val* root) {
        // An open array or object: `next` is its next element (arrays) or next key (objects), `left` counts what is
        // still to be fed, `index` is the position in the document of the next member of an object.
        struct frame {
            bool is_array;
            yyjson_val* next;
            std::size_t left;
            std::size_t index;
        };
        std::vector<frame> open;

        auto visit = [this, &open](yyjson_val* val) {
            switch (yyjson_get_tag(val)) {
                case YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE:
                    handle_value(nullptr);
                    break;

                case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE:
                    handle_value(true);
                    break;

                case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE:
                    handle_value(false);
                    break;

                case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT:
                    handle_value(static_cast<json::number_unsigned_t>(unsafe_yyjson_get_uint(val)));
                    break;

                case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT:
                    handle_value(static_cast<json::number_integer_t>(unsafe_yyjson_get_sint(val)));
                    break;

                case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL:
                    handle_value(static_cast<json::number_float_t>(unsafe_yyjson_get_real(val)));
                    break;

                case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE:
                case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC: {
                    json::string_t s(unsafe_yyjson_get_str(val), unsafe_yyjson_get_len(val));
                    handle_value(s);
                    break;
                }

                case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE: {
                    const std::size_t len = unsafe_yyjson_get_len(val);
                    start_array(len);
                    open.push_back({true, unsafe_yyjson_get_first(val), len, 0});
                    break;
                }

                case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE: {
                    const std::size_t len = unsafe_yyjson_get_len(val);
                    start_object(val, len);
                    open.push_back({false, unsafe_yyjson_get_first(val), len, 0});
                    break;
                }

                default:
                    handle_value(nullptr);
                    break;
            }
        };

        visit(root);
        while (!open.empty()) {
            frame& top = open.back();
            if (top.left == 0) {
                const bool is_array = top.is_array;
                open.pop_back();
                if (is_array) {
                    end_array();
                } else {
                    end_object();
                }
                continue;
            }
            --top.left;
            if (top.is_array) {
                yyjson_val* elem = top.next;
                top.next = unsafe_yyjson_get_next(elem);
                visit(elem);
            } else {
                yyjson_val* k = top.next;
                yyjson_val* v = yyjson_obj_iter_get_val(k);
                top.next = unsafe_yyjson_get_next(v);
                const std::size_t member = top.index++;
                json::string_t key_str(unsafe_yyjson_get_str(k), unsafe_yyjson_get_len(k));
                key(key_str, member);
                visit(v);
            }
        }
    }

private:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    // The repeated keys of one object above kObjectIndexThreshold members, resolved before the members arrive.
    struct key_index {
        // For each member of the document: the position in the document of the first member with the same key.
        std::vector<std::size_t> first_of;
        // For each first occurrence: the position in the object being built of the member with that key; kNone while
        // there is none (the key was not stored yet, or its member was removed).
        std::vector<std::size_t> slot_of;
        // For each member of the object being built: the first occurrence of its key.
        std::vector<std::size_t> first_of_slot;
    };

    // An array or object being built. `node` is null when the container is not stored (it, or a container around it,
    // was dropped); then nothing else is used.
    struct level {
        json* node = nullptr;
        // Position of the child that was added last or whose key was inserted last.
        std::size_t last = kNone;
        // Positions that may hold a discarded child, as a min-heap. A position can be listed more than once and can
        // have stopped holding a discarded child; the child is looked at when the position is taken from the heap.
        std::vector<std::size_t> pending;
        // Positions of children that are removed but still in place.
        std::vector<bool> removed;
        std::size_t removed_count = 0;
        std::unique_ptr<key_index> index;
    };

    static bool is_removed(const level& l, const std::size_t pos) {
        return pos < l.removed.size() && l.removed[pos];
    }

    static std::size_t child_count(const level& l) {
        return l.node->is_array() ? l.node->get_ptr<json::array_t*>()->size()
                                  : l.node->get_ptr<json::object_t*>()->size();
    }

    static json& child_at(level& l, const std::size_t pos) {
        return l.node->is_array() ? (*l.node->get_ptr<json::array_t*>())[pos]
                                  : (*l.node->get_ptr<json::object_t*>())[pos].second;
    }

    static void list_pending(level& l, const std::size_t pos) {
        l.pending.push_back(pos);
        std::push_heap(l.pending.begin(), l.pending.end(), std::greater<std::size_t>());
    }

    // Lists the child at `l.last` if it is discarded now.
    static void list_pending_if_discarded(level& l) {
        if (l.last != kNone && l.last < child_count(l) && child_at(l, l.last).is_discarded()) {
            list_pending(l, l.last);
        }
    }

    // Removes the first discarded child of `l`, if there is one.
    static void remove_first_discarded(level& l) {
        while (!l.pending.empty()) {
            std::pop_heap(l.pending.begin(), l.pending.end(), std::greater<std::size_t>());
            const std::size_t pos = l.pending.back();
            l.pending.pop_back();
            if (pos >= child_count(l) || is_removed(l, pos) || !child_at(l, pos).is_discarded()) {
                continue;
            }
            if (l.removed.size() <= pos) {
                l.removed.resize(child_count(l), false);
            }
            l.removed[pos] = true;
            ++l.removed_count;
            if (l.index != nullptr) {
                l.index->slot_of[l.index->first_of_slot[pos]] = kNone;
            }
            return;
        }
    }

    // Drops the removed children of `l` from its container.
    static void drop_removed(level& l) {
        if (l.removed_count == 0) {
            return;
        }
        if (l.node->is_array()) {
            json::array_t& arr = *l.node->get_ptr<json::array_t*>();
            json::array_t kept;
            kept.reserve(arr.size() - l.removed_count);
            for (std::size_t i = 0; i < arr.size(); ++i) {
                if (!is_removed(l, i)) {
                    kept.push_back(std::move(arr[i]));
                }
            }
            arr = std::move(kept);
        } else {
            json::object_t& obj = *l.node->get_ptr<json::object_t*>();
            json::object_t kept;
            kept.reserve(obj.size() - l.removed_count);
            for (std::size_t i = 0; i < obj.size(); ++i) {
                if (!is_removed(l, i)) {
                    kept.emplace_back(std::piecewise_construct, std::forward_as_tuple(obj[i].first),
                                      std::forward_as_tuple(std::move(obj[i].second)));
                }
            }
            obj = std::move(kept);
        }
        l.removed.clear();
        l.removed_count = 0;
    }

    void start_object(yyjson_val* val, std::size_t len) {
        const bool keep = callback(static_cast<int>(ref_stack.size()),
                                   json::parse_event_t::object_start, discarded);
        keep_stack.push_back(keep);

        auto stored = handle_value(json::value_t::object, true);
        ref_stack.emplace_back();
        level& l = ref_stack.back();
        l.node = stored.second;
        if (l.node != nullptr) {
            l.node->get_ptr<json::object_t*>()->reserve(len);
            if (len > kObjectIndexThreshold) {
                l.index = std::make_unique<key_index>();
                l.index->first_of = first_occurrences_sorted(val, len);
                l.index->slot_of.assign(len, kNone);
                l.index->first_of_slot.reserve(len);
            }
        }
    }

    void key(json::string_t& val, const std::size_t member) {
        json k(val);

        const bool keep =
            callback(static_cast<int>(ref_stack.size()), json::parse_event_t::key, k);
        key_keep_stack.push_back(keep);

        if (keep && ref_stack.back().node != nullptr) {
            level& l = ref_stack.back();
            json::object_t& obj = *l.node->get_ptr<json::object_t*>();
            std::size_t pos = kNone;
            if (l.index != nullptr) {
                pos = l.index->slot_of[l.index->first_of[member]];
            } else {
                for (std::size_t i = 0; i < obj.size(); ++i) {
                    if (!is_removed(l, i) && obj[i].first == val) {
                        pos = i;
                        break;
                    }
                }
            }
            if (pos == kNone) {
                obj.emplace_back(std::piecewise_construct, std::forward_as_tuple(val),
                                 std::forward_as_tuple(discarded));
                pos = obj.size() - 1;
                if (l.index != nullptr) {
                    const std::size_t first = l.index->first_of[member];
                    l.index->slot_of[first] = pos;
                    l.index->first_of_slot.push_back(first);
                }
            } else {
                obj[pos].second = discarded;
            }
            l.last = pos;
            list_pending(l, pos);
            object_element = &obj[pos].second;
        }
    }

    void end_object() {
        {
            level& l = ref_stack.back();
            if (l.node != nullptr) {
                drop_removed(l);
                if (!callback(static_cast<int>(ref_stack.size()) - 1,
                              json::parse_event_t::object_end, *l.node)) {
                    *l.node = discarded;
                }
            }
        }

        ref_stack.pop_back();
        keep_stack.pop_back();

        if (!ref_stack.empty() && ref_stack.back().node != nullptr &&
            ref_stack.back().node->is_structured()) {
            list_pending_if_discarded(ref_stack.back());
            remove_first_discarded(ref_stack.back());
        }
    }

    void start_array(std::size_t len) {
        const bool keep = callback(static_cast<int>(ref_stack.size()),
                                   json::parse_event_t::array_start, discarded);
        keep_stack.push_back(keep);

        auto stored = handle_value(json::value_t::array, true);
        ref_stack.emplace_back();
        ref_stack.back().node = stored.second;
        if (stored.second != nullptr) {
            stored.second->get_ptr<json::array_t*>()->reserve(len);
        }
    }

    void end_array() {
        bool keep = true;

        {
            level& l = ref_stack.back();
            if (l.node != nullptr) {
                drop_removed(l);
                keep = callback(static_cast<int>(ref_stack.size()) - 1,
                                json::parse_event_t::array_end, *l.node);
                if (!keep) {
                    *l.node = discarded;
                }
            }
        }

        ref_stack.pop_back();
        keep_stack.pop_back();

        if (!ref_stack.empty() && ref_stack.back().node != nullptr) {
            level& parent = ref_stack.back();
            if (!keep && parent.node->is_array()) {
                json::array_t& arr = *parent.node->get_ptr<json::array_t*>();
                arr.pop_back();
                // The child that was taken out was not removed, so no removed position lies past the new end.
                if (parent.removed.size() > arr.size()) {
                    parent.removed.resize(arr.size());
                }
                parent.last = kNone;
            } else {
                list_pending_if_discarded(parent);
            }
        }
    }

    template <typename Value>
    std::pair<bool, json*> handle_value(Value&& v, const bool skip_callback = false) {
        if (!keep_stack.back()) {
            return {false, nullptr};
        }

        auto value = json(std::forward<Value>(v));

        const bool keep =
            skip_callback ||
            callback(static_cast<int>(ref_stack.size()), json::parse_event_t::value, value);

        if (!keep) {
            return {false, nullptr};
        }

        if (ref_stack.empty()) {
            root = std::move(value);
            return {true, &root};
        }

        level& l = ref_stack.back();
        if (l.node == nullptr) {
            return {false, nullptr};
        }

        if (l.node->is_array()) {
            json::array_t& arr = *l.node->get_ptr<json::array_t*>();
            arr.emplace_back(std::move(value));
            l.last = arr.size() - 1;
            if (arr.back().is_discarded()) {
                list_pending(l, l.last);
            }
            return {true, &arr.back()};
        }

        const bool store_element = key_keep_stack.back();
        key_keep_stack.pop_back();

        if (!store_element) {
            return {false, nullptr};
        }

        *object_element = std::move(value);
        if (object_element->is_discarded()) {
            list_pending(l, l.last);
        }
        return {true, object_element};
    }

    json& root;
    std::vector<level> ref_stack{};
    std::vector<bool> keep_stack{};
    std::vector<bool> key_keep_stack{};
    json* object_element = nullptr;
    const json::parser_callback_t& callback;
    json discarded = json::value_t::discarded;
};

struct diag_position_t {
    std::size_t chars_read_total = 0;
    std::size_t chars_read_current_line = 0;
    std::size_t lines_read = 0;
};

enum class diag_token_type {
    uninitialized,
    literal_true,
    literal_false,
    literal_null,
    value_string,
    value_unsigned,
    value_integer,
    value_float,
    begin_array,
    begin_object,
    end_array,
    end_object,
    name_separator,
    value_separator,
    parse_error,
    end_of_input,
    literal_or_value
};

const char* diag_token_type_name(const diag_token_type t) noexcept {
    switch (t) {
        case diag_token_type::uninitialized:
            return "<uninitialized>";
        case diag_token_type::literal_true:
            return "true literal";
        case diag_token_type::literal_false:
            return "false literal";
        case diag_token_type::literal_null:
            return "null literal";
        case diag_token_type::value_string:
            return "string literal";
        case diag_token_type::value_unsigned:
        case diag_token_type::value_integer:
        case diag_token_type::value_float:
            return "number literal";
        case diag_token_type::begin_array:
            return "'['";
        case diag_token_type::begin_object:
            return "'{'";
        case diag_token_type::end_array:
            return "']'";
        case diag_token_type::end_object:
            return "'}'";
        case diag_token_type::name_separator:
            return "':'";
        case diag_token_type::value_separator:
            return "','";
        case diag_token_type::parse_error:
            return "<parse error>";
        case diag_token_type::end_of_input:
            return "end of input";
        case diag_token_type::literal_or_value:
            return "'[', '{', or a literal";
        default:
            return "unknown token";
    }
}

class diagnostic_lexer {
public:
    using char_type = char;
    using char_int_type = std::char_traits<char>::int_type;

    diagnostic_lexer(std::string_view input, bool ignore_comments) noexcept
        : input_(input), ignore_comments_(ignore_comments) {}

    constexpr json::number_float_t get_number_float() const noexcept {
        return value_float_;
    }

    constexpr diag_position_t get_position() const noexcept {
        return position_;
    }

    constexpr std::size_t accepted_prefix_size() const noexcept {
        return current_ == '\0' ? cursor_ - 1 : cursor_;
    }

    std::string get_token_string() const {
        std::string result;
        for (const auto c : token_string_) {
            if (static_cast<unsigned char>(c) <= '\x1F') {
                std::array<char, 9> cs{{}};
                static_cast<void>(std::snprintf(cs.data(), cs.size(), "<U+%.4X>",
                                                static_cast<unsigned char>(c)));
                result += cs.data();
            } else {
                result.push_back(c);
            }
        }
        return result;
    }

    constexpr const char* get_error_message() const noexcept {
        return error_message_;
    }

    diag_token_type scan() {
        if (position_.chars_read_total == 0 && !skip_bom()) {
            error_message_ = "invalid BOM; must be 0xEF 0xBB 0xBF if given";
            return diag_token_type::parse_error;
        }

        skip_whitespace();

        while (ignore_comments_ && current_ == '/') {
            if (!scan_comment()) {
                return diag_token_type::parse_error;
            }
            skip_whitespace();
        }

        switch (current_) {
            case '[':
                return diag_token_type::begin_array;
            case ']':
                return diag_token_type::end_array;
            case '{':
                return diag_token_type::begin_object;
            case '}':
                return diag_token_type::end_object;
            case ':':
                return diag_token_type::name_separator;
            case ',':
                return diag_token_type::value_separator;

            case 't':
                return scan_literal("true", 4, diag_token_type::literal_true);
            case 'f':
                return scan_literal("false", 5, diag_token_type::literal_false);
            case 'n':
                return scan_literal("null", 4, diag_token_type::literal_null);

            case '\"':
                return scan_string();

            case '-':
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                return scan_number();

            case '\0':
            case std::char_traits<char>::eof():
                return diag_token_type::end_of_input;

            default:
                error_message_ = "invalid literal";
                return diag_token_type::parse_error;
        }
    }

private:
    char_int_type get_character() noexcept {
        if (cursor_ < input_.size()) {
            return std::char_traits<char>::to_int_type(input_[cursor_++]);
        }
        return std::char_traits<char>::eof();
    }

    char_int_type get() {
        ++position_.chars_read_total;
        ++position_.chars_read_current_line;

        if (next_unget_) {
            next_unget_ = false;
        } else {
            current_ = get_character();
        }

        if (current_ != std::char_traits<char>::eof()) {
            token_string_.push_back(std::char_traits<char>::to_char_type(current_));
        }

        if (current_ == '\n') {
            ++position_.lines_read;
            position_.chars_read_current_line = 0;
        }

        return current_;
    }

    void unget() {
        next_unget_ = true;
        --position_.chars_read_total;

        if (position_.chars_read_current_line == 0) {
            if (position_.lines_read > 0) {
                --position_.lines_read;
            }
        } else {
            --position_.chars_read_current_line;
        }

        if (current_ != std::char_traits<char>::eof()) {
            assert(!token_string_.empty());
            token_string_.pop_back();
        }
    }

    void reset() noexcept {
        token_buffer_.clear();
        token_string_.clear();
        token_string_.push_back(std::char_traits<char>::to_char_type(current_));
    }

    void add(char_int_type c) {
        token_buffer_.push_back(static_cast<char>(c));
    }

    bool skip_bom() {
        if (get() == 0xEF) {
            return get() == 0xBB && get() == 0xBF;
        }
        unget();
        return true;
    }

    void skip_whitespace() {
        do {
            get();
        } while (current_ == ' ' || current_ == '\t' || current_ == '\n' || current_ == '\r');
    }

    int get_codepoint() {
        assert(current_ == 'u');
        int codepoint = 0;

        for (const auto factor : {12u, 8u, 4u, 0u}) {
            get();
            if (current_ >= '0' && current_ <= '9') {
                codepoint += static_cast<int>((static_cast<unsigned int>(current_) - 0x30u)
                                              << factor);
            } else if (current_ >= 'A' && current_ <= 'F') {
                codepoint += static_cast<int>((static_cast<unsigned int>(current_) - 0x37u)
                                              << factor);
            } else if (current_ >= 'a' && current_ <= 'f') {
                codepoint += static_cast<int>((static_cast<unsigned int>(current_) - 0x57u)
                                              << factor);
            } else {
                return -1;
            }
        }
        return codepoint;
    }

    bool next_byte_in_range(std::initializer_list<char_int_type> ranges) {
        for (auto range = ranges.begin(); range != ranges.end(); ++range) {
            get();
            if (*range <= current_ && current_ <= *(++range)) {
                // valid continuation byte
            } else {
                error_message_ = "invalid string: ill-formed UTF-8 byte";
                return false;
            }
        }
        return true;
    }

    diag_token_type scan_string() {
        reset();
        assert(current_ == '\"');

        while (true) {
            switch (get()) {
                case std::char_traits<char>::eof():
                    error_message_ = "invalid string: missing closing quote";
                    return diag_token_type::parse_error;

                case '\"':
                    return diag_token_type::value_string;

                case '\\': {
                    switch (get()) {
                        case '\"':
                        case '\\':
                        case '/':
                        case 'b':
                        case 'f':
                        case 'n':
                        case 'r':
                        case 't':
                            break;

                        case 'u': {
                            const int codepoint1 = get_codepoint();
                            if (codepoint1 == -1) {
                                error_message_ =
                                    "invalid string: '\\u' must be followed by 4 hex digits";
                                return diag_token_type::parse_error;
                            }

                            if (0xD800 <= codepoint1 && codepoint1 <= 0xDBFF) {
                                if (get() == '\\' && get() == 'u') {
                                    const int codepoint2 = get_codepoint();
                                    if (codepoint2 == -1) {
                                        error_message_ =
                                            "invalid string: '\\u' must be followed by 4 hex digits";
                                        return diag_token_type::parse_error;
                                    }
                                    if (!(0xDC00 <= codepoint2 && codepoint2 <= 0xDFFF)) {
                                        error_message_ =
                                            "invalid string: surrogate U+D800..U+DBFF must be "
                                            "followed by U+DC00..U+DFFF";
                                        return diag_token_type::parse_error;
                                    }
                                } else {
                                    error_message_ =
                                        "invalid string: surrogate U+D800..U+DBFF must be "
                                        "followed by U+DC00..U+DFFF";
                                    return diag_token_type::parse_error;
                                }
                            } else if (0xDC00 <= codepoint1 && codepoint1 <= 0xDFFF) {
                                error_message_ =
                                    "invalid string: surrogate U+DC00..U+DFFF must follow "
                                    "U+D800..U+DBFF";
                                return diag_token_type::parse_error;
                            }
                            break;
                        }

                        default:
                            error_message_ = "invalid string: forbidden character after backslash";
                            return diag_token_type::parse_error;
                    }
                    break;
                }

                case 0x00:
                    error_message_ =
                        "invalid string: control character U+0000 (NUL) must be escaped to \\u0000";
                    return diag_token_type::parse_error;
                case 0x01:
                    error_message_ =
                        "invalid string: control character U+0001 (SOH) must be escaped to \\u0001";
                    return diag_token_type::parse_error;
                case 0x02:
                    error_message_ =
                        "invalid string: control character U+0002 (STX) must be escaped to \\u0002";
                    return diag_token_type::parse_error;
                case 0x03:
                    error_message_ =
                        "invalid string: control character U+0003 (ETX) must be escaped to \\u0003";
                    return diag_token_type::parse_error;
                case 0x04:
                    error_message_ =
                        "invalid string: control character U+0004 (EOT) must be escaped to \\u0004";
                    return diag_token_type::parse_error;
                case 0x05:
                    error_message_ =
                        "invalid string: control character U+0005 (ENQ) must be escaped to \\u0005";
                    return diag_token_type::parse_error;
                case 0x06:
                    error_message_ =
                        "invalid string: control character U+0006 (ACK) must be escaped to \\u0006";
                    return diag_token_type::parse_error;
                case 0x07:
                    error_message_ =
                        "invalid string: control character U+0007 (BEL) must be escaped to \\u0007";
                    return diag_token_type::parse_error;
                case 0x08:
                    error_message_ =
                        "invalid string: control character U+0008 (BS) must be escaped to \\u0008 "
                        "or \\b";
                    return diag_token_type::parse_error;
                case 0x09:
                    error_message_ =
                        "invalid string: control character U+0009 (HT) must be escaped to \\u0009 "
                        "or \\t";
                    return diag_token_type::parse_error;
                case 0x0A:
                    error_message_ =
                        "invalid string: control character U+000A (LF) must be escaped to \\u000A "
                        "or \\n";
                    return diag_token_type::parse_error;
                case 0x0B:
                    error_message_ =
                        "invalid string: control character U+000B (VT) must be escaped to \\u000B";
                    return diag_token_type::parse_error;
                case 0x0C:
                    error_message_ =
                        "invalid string: control character U+000C (FF) must be escaped to \\u000C "
                        "or \\f";
                    return diag_token_type::parse_error;
                case 0x0D:
                    error_message_ =
                        "invalid string: control character U+000D (CR) must be escaped to \\u000D "
                        "or \\r";
                    return diag_token_type::parse_error;
                case 0x0E:
                    error_message_ =
                        "invalid string: control character U+000E (SO) must be escaped to \\u000E";
                    return diag_token_type::parse_error;
                case 0x0F:
                    error_message_ =
                        "invalid string: control character U+000F (SI) must be escaped to \\u000F";
                    return diag_token_type::parse_error;
                case 0x10:
                    error_message_ =
                        "invalid string: control character U+0010 (DLE) must be escaped to \\u0010";
                    return diag_token_type::parse_error;
                case 0x11:
                    error_message_ =
                        "invalid string: control character U+0011 (DC1) must be escaped to \\u0011";
                    return diag_token_type::parse_error;
                case 0x12:
                    error_message_ =
                        "invalid string: control character U+0012 (DC2) must be escaped to \\u0012";
                    return diag_token_type::parse_error;
                case 0x13:
                    error_message_ =
                        "invalid string: control character U+0013 (DC3) must be escaped to \\u0013";
                    return diag_token_type::parse_error;
                case 0x14:
                    error_message_ =
                        "invalid string: control character U+0014 (DC4) must be escaped to \\u0014";
                    return diag_token_type::parse_error;
                case 0x15:
                    error_message_ =
                        "invalid string: control character U+0015 (NAK) must be escaped to \\u0015";
                    return diag_token_type::parse_error;
                case 0x16:
                    error_message_ =
                        "invalid string: control character U+0016 (SYN) must be escaped to \\u0016";
                    return diag_token_type::parse_error;
                case 0x17:
                    error_message_ =
                        "invalid string: control character U+0017 (ETB) must be escaped to \\u0017";
                    return diag_token_type::parse_error;
                case 0x18:
                    error_message_ =
                        "invalid string: control character U+0018 (CAN) must be escaped to \\u0018";
                    return diag_token_type::parse_error;
                case 0x19:
                    error_message_ =
                        "invalid string: control character U+0019 (EM) must be escaped to \\u0019";
                    return diag_token_type::parse_error;
                case 0x1A:
                    error_message_ =
                        "invalid string: control character U+001A (SUB) must be escaped to \\u001A";
                    return diag_token_type::parse_error;
                case 0x1B:
                    error_message_ =
                        "invalid string: control character U+001B (ESC) must be escaped to \\u001B";
                    return diag_token_type::parse_error;
                case 0x1C:
                    error_message_ =
                        "invalid string: control character U+001C (FS) must be escaped to \\u001C";
                    return diag_token_type::parse_error;
                case 0x1D:
                    error_message_ =
                        "invalid string: control character U+001D (GS) must be escaped to \\u001D";
                    return diag_token_type::parse_error;
                case 0x1E:
                    error_message_ =
                        "invalid string: control character U+001E (RS) must be escaped to \\u001E";
                    return diag_token_type::parse_error;
                case 0x1F:
                    error_message_ =
                        "invalid string: control character U+001F (US) must be escaped to \\u001F";
                    return diag_token_type::parse_error;

                case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
                case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E:
                case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
                case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C:
                case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43:
                case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A:
                case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51:
                case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58:
                case 0x59: case 0x5A: case 0x5B: case 0x5D: case 0x5E: case 0x5F: case 0x60:
                case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
                case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E:
                case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:
                case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C:
                case 0x7D: case 0x7E: case 0x7F:
                    break;

                case 0xC2: case 0xC3: case 0xC4: case 0xC5: case 0xC6: case 0xC7: case 0xC8:
                case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF:
                case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6:
                case 0xD7: case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD:
                case 0xDE: case 0xDF:
                    if (!next_byte_in_range({0x80, 0xBF})) {
                        return diag_token_type::parse_error;
                    }
                    break;

                case 0xE0:
                    if (!next_byte_in_range({0xA0, 0xBF, 0x80, 0xBF})) {
                        return diag_token_type::parse_error;
                    }
                    break;

                case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7:
                case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xEE: case 0xEF:
                    if (!next_byte_in_range({0x80, 0xBF, 0x80, 0xBF})) {
                        return diag_token_type::parse_error;
                    }
                    break;

                case 0xED:
                    if (!next_byte_in_range({0x80, 0x9F, 0x80, 0xBF})) {
                        return diag_token_type::parse_error;
                    }
                    break;

                case 0xF0:
                    if (!next_byte_in_range({0x90, 0xBF, 0x80, 0xBF, 0x80, 0xBF})) {
                        return diag_token_type::parse_error;
                    }
                    break;

                case 0xF1: case 0xF2: case 0xF3:
                    if (!next_byte_in_range({0x80, 0xBF, 0x80, 0xBF, 0x80, 0xBF})) {
                        return diag_token_type::parse_error;
                    }
                    break;

                case 0xF4:
                    if (!next_byte_in_range({0x80, 0x8F, 0x80, 0xBF, 0x80, 0xBF})) {
                        return diag_token_type::parse_error;
                    }
                    break;

                default:
                    error_message_ = "invalid string: ill-formed UTF-8 byte";
                    return diag_token_type::parse_error;
            }
        }
    }

    bool scan_comment() {
        switch (get()) {
            case '/': {
                while (true) {
                    switch (get()) {
                        case '\n':
                        case '\r':
                        case std::char_traits<char>::eof():
                        case '\0':
                            return true;
                        default:
                            break;
                    }
                }
            }

            case '*': {
                while (true) {
                    switch (get()) {
                        case std::char_traits<char>::eof():
                        case '\0':
                            error_message_ = "invalid comment; missing closing '*/'";
                            return false;

                        case '*': {
                            switch (get()) {
                                case '/':
                                    return true;
                                default:
                                    unget();
                                    continue;
                            }
                        }

                        default:
                            continue;
                    }
                }
            }

            default:
                error_message_ = "invalid comment; expecting '/' or '*' after '/'";
                return false;
        }
    }

    diag_token_type scan_number() {
        reset();
        diag_token_type number_type = diag_token_type::value_unsigned;

        switch (current_) {
            case '-':
                add(current_);
                goto scan_number_minus;
            case '0':
                add(current_);
                goto scan_number_zero;
            case '1': case '2': case '3': case '4': case '5':
            case '6': case '7': case '8': case '9':
                add(current_);
                goto scan_number_any1;
            default:
                assert(false);
                return diag_token_type::parse_error;
        }

    scan_number_minus:
        number_type = diag_token_type::value_integer;
        switch (get()) {
            case '0':
                add(current_);
                goto scan_number_zero;
            case '1': case '2': case '3': case '4': case '5':
            case '6': case '7': case '8': case '9':
                add(current_);
                goto scan_number_any1;
            default:
                error_message_ = "invalid number; expected digit after '-'";
                return diag_token_type::parse_error;
        }

    scan_number_zero:
        switch (get()) {
            case '.':
                add('.');
                goto scan_number_decimal1;
            case 'e':
            case 'E':
                add(current_);
                goto scan_number_exponent;
            default:
                goto scan_number_done;
        }

    scan_number_any1:
        switch (get()) {
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                add(current_);
                goto scan_number_any1;
            case '.':
                add('.');
                goto scan_number_decimal1;
            case 'e':
            case 'E':
                add(current_);
                goto scan_number_exponent;
            default:
                goto scan_number_done;
        }

    scan_number_decimal1:
        number_type = diag_token_type::value_float;
        switch (get()) {
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                add(current_);
                goto scan_number_decimal2;
            default:
                error_message_ = "invalid number; expected digit after '.'";
                return diag_token_type::parse_error;
        }

    scan_number_decimal2:
        switch (get()) {
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                add(current_);
                goto scan_number_decimal2;
            case 'e':
            case 'E':
                add(current_);
                goto scan_number_exponent;
            default:
                goto scan_number_done;
        }

    scan_number_exponent:
        number_type = diag_token_type::value_float;
        switch (get()) {
            case '+':
            case '-':
                add(current_);
                goto scan_number_sign;
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                add(current_);
                goto scan_number_any2;
            default:
                error_message_ = "invalid number; expected '+', '-', or digit after exponent";
                return diag_token_type::parse_error;
        }

    scan_number_sign:
        switch (get()) {
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                add(current_);
                goto scan_number_any2;
            default:
                error_message_ = "invalid number; expected digit after exponent sign";
                return diag_token_type::parse_error;
        }

    scan_number_any2:
        switch (get()) {
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                add(current_);
                goto scan_number_any2;
            default:
                goto scan_number_done;
        }

    scan_number_done:
        unget();

        char* endptr = nullptr;
        errno = 0;

        if (number_type == diag_token_type::value_unsigned) {
            const auto x = std::strtoull(token_buffer_.c_str(), &endptr, 10);
            if (errno == 0) {
                const auto vu = static_cast<json::number_unsigned_t>(x);
                if (vu == x) {
                    return diag_token_type::value_unsigned;
                }
            }
        } else if (number_type == diag_token_type::value_integer) {
            const auto x = std::strtoll(token_buffer_.c_str(), &endptr, 10);
            if (errno == 0) {
                const auto vi = static_cast<json::number_integer_t>(x);
                if (vi == x) {
                    return diag_token_type::value_integer;
                }
            }
        }

        value_float_ = std::strtod(token_buffer_.c_str(), &endptr);
        return diag_token_type::value_float;
    }

    diag_token_type scan_literal(const char* literal_text, const std::size_t length,
                                 diag_token_type return_type) {
        assert(std::char_traits<char>::to_char_type(current_) == literal_text[0]);
        for (std::size_t i = 1; i < length; ++i) {
            if (std::char_traits<char>::to_char_type(get()) != literal_text[i]) {
                error_message_ = "invalid literal";
                return diag_token_type::parse_error;
            }
        }
        return return_type;
    }

    std::string_view input_;
    std::size_t cursor_ = 0;
    const bool ignore_comments_ = false;
    char_int_type current_ = std::char_traits<char>::eof();
    bool next_unget_ = false;
    diag_position_t position_{};
    std::vector<char> token_string_{};
    std::string token_buffer_{};
    const char* error_message_ = "";
    json::number_float_t value_float_ = 0.0;
};

std::string nesting_limit_message(const std::size_t max_depth) {
    return "nesting limit exceeded; arrays and objects may be nested at most " +
           std::to_string(max_depth) + " levels deep";
}

class diagnostic_parser {
public:
    // Besides syntax errors, the scanner reports the first array or object that is nested deeper than `max_depth`
    // levels (see within_depth), at the position of its opening bracket.
    diagnostic_parser(std::string_view input, bool ignore_comments, std::size_t max_depth)
        : lexer_(input, ignore_comments), max_depth_(max_depth) {
        get_token();
    }

    bool diagnose(bool allow_exceptions) {
        const bool ok = sax_parse_internal(allow_exceptions);
        if (!ok) {
            return false;
        }
        if (get_token() != diag_token_type::end_of_input) {
            return fail_parse_error(
                allow_exceptions,
                exception_message(diag_token_type::end_of_input, "value"));
        }
        return true;
    }

    constexpr std::size_t accepted_prefix_size() const noexcept {
        return lexer_.accepted_prefix_size();
    }

private:
    diag_token_type get_token() {
        return last_token_ = lexer_.scan();
    }

    bool fail_parse_error(bool allow_exceptions, const std::string& msg) {
        if (allow_exceptions) {
            const auto pos = lexer_.get_position();
            throw json::parse_error::create(101, pos.chars_read_total, pos.lines_read + 1,
                                            pos.chars_read_current_line, msg);
        }
        return false;
    }

    bool fail_nesting_limit(bool allow_exceptions) {
        if (allow_exceptions) {
            const auto pos = lexer_.get_position();
            throw json::parse_error::create(kNestingLimitErrorId, pos.chars_read_total,
                                            pos.lines_read + 1, pos.chars_read_current_line,
                                            nesting_limit_message(max_depth_));
        }
        return false;
    }

    bool sax_parse_internal(bool allow_exceptions) {
        std::vector<bool> states;
        bool skip_to_state_evaluation = false;

        while (true) {
            if (!skip_to_state_evaluation) {
                switch (last_token_) {
                    case diag_token_type::begin_object: {
                        // `states` holds the open non-empty containers; this one is level states.size() + 1.
                        if (states.size() >= max_depth_) {
                            return fail_nesting_limit(allow_exceptions);
                        }
                        if (get_token() == diag_token_type::end_object) {
                            break;
                        }
                        if (last_token_ != diag_token_type::value_string) {
                            return fail_parse_error(
                                allow_exceptions,
                                exception_message(diag_token_type::value_string, "object key"));
                        }
                        if (get_token() != diag_token_type::name_separator) {
                            return fail_parse_error(
                                allow_exceptions,
                                exception_message(diag_token_type::name_separator,
                                                  "object separator"));
                        }
                        states.push_back(false);
                        get_token();
                        continue;
                    }

                    case diag_token_type::begin_array: {
                        if (states.size() >= max_depth_) {
                            return fail_nesting_limit(allow_exceptions);
                        }
                        if (get_token() == diag_token_type::end_array) {
                            break;
                        }
                        states.push_back(true);
                        continue;
                    }

                    case diag_token_type::value_float: {
                        const auto res = lexer_.get_number_float();
                        if (!std::isfinite(res)) {
                            if (allow_exceptions) {
                                throw json::out_of_range::create(
                                    406, "number overflow parsing '" + lexer_.get_token_string() +
                                             "'");
                            }
                            return false;
                        }
                        break;
                    }

                    case diag_token_type::literal_false:
                    case diag_token_type::literal_null:
                    case diag_token_type::literal_true:
                    case diag_token_type::value_integer:
                    case diag_token_type::value_string:
                    case diag_token_type::value_unsigned:
                        break;

                    case diag_token_type::parse_error:
                        return fail_parse_error(
                            allow_exceptions,
                            exception_message(diag_token_type::uninitialized, "value"));

                    case diag_token_type::end_of_input: {
                        if (lexer_.get_position().chars_read_total == 1) {
                            return fail_parse_error(
                                allow_exceptions,
                                "attempting to parse an empty input; check that your input string "
                                "or stream contains the expected JSON");
                        }
                        return fail_parse_error(
                            allow_exceptions,
                            exception_message(diag_token_type::literal_or_value, "value"));
                    }

                    case diag_token_type::uninitialized:
                    case diag_token_type::end_array:
                    case diag_token_type::end_object:
                    case diag_token_type::name_separator:
                    case diag_token_type::value_separator:
                    case diag_token_type::literal_or_value:
                    default:
                        return fail_parse_error(
                            allow_exceptions,
                            exception_message(diag_token_type::literal_or_value, "value"));
                }
            } else {
                skip_to_state_evaluation = false;
            }

            if (states.empty()) {
                return true;
            }

            if (states.back()) {
                if (get_token() == diag_token_type::value_separator) {
                    get_token();
                    continue;
                }
                if (last_token_ == diag_token_type::end_array) {
                    states.pop_back();
                    skip_to_state_evaluation = true;
                    continue;
                }
                return fail_parse_error(
                    allow_exceptions,
                    exception_message(diag_token_type::end_array, "array"));
            }

            if (get_token() == diag_token_type::value_separator) {
                if (get_token() != diag_token_type::value_string) {
                    return fail_parse_error(
                        allow_exceptions,
                        exception_message(diag_token_type::value_string, "object key"));
                }
                if (get_token() != diag_token_type::name_separator) {
                    return fail_parse_error(
                        allow_exceptions,
                        exception_message(diag_token_type::name_separator, "object separator"));
                }
                get_token();
                continue;
            }

            if (last_token_ == diag_token_type::end_object) {
                states.pop_back();
                skip_to_state_evaluation = true;
                continue;
            }

            return fail_parse_error(
                allow_exceptions,
                exception_message(diag_token_type::end_object, "object"));
        }
    }

    std::string exception_message(const diag_token_type expected,
                                  const std::string& context) const {
        std::string error_msg = "syntax error ";
        if (!context.empty()) {
            error_msg += "while parsing " + context + " ";
        }
        error_msg += "- ";
        if (last_token_ == diag_token_type::parse_error) {
            error_msg += std::string(lexer_.get_error_message()) + "; last read: '" +
                         lexer_.get_token_string() + "'";
        } else {
            error_msg += std::string("unexpected ") + diag_token_type_name(last_token_);
        }
        if (expected != diag_token_type::uninitialized) {
            error_msg += std::string("; expected ") + diag_token_type_name(expected);
        }
        return error_msg;
    }

    diagnostic_lexer lexer_;
    std::size_t max_depth_;
    diag_token_type last_token_ = diag_token_type::uninitialized;
};

bool try_dump_yyjson_direct(const json& val, std::string& out) {
    if (!json_access::is_lazy(val)) {
        return false;
    }
    const value_t type = json_access::type_of(val);
    if (type != value_t::array && type != value_t::object) {
        return false;
    }
    auto* node = static_cast<LazyNode*>(json_access::lazy_node(val));
    if (node->materialized.load(std::memory_order_acquire) != nullptr) {
        return false;
    }
    const bool doc_verified = node->doc->tree_unique_verified.load(std::memory_order_acquire);
    const std::uint8_t state = doc_verified ? 4u : node->dedup_state.load(std::memory_order_acquire);
    if (!doc_verified && (state & (2u | 8u)) != 0u) {
        return false;
    }
    const bool skip_dup_check = doc_verified || ((state & 4u) != 0u);
    const std::size_t est = (node->val == yyjson_doc_get_root(node->doc->doc))
                                ? yyjson_doc_get_read_size(node->doc->doc)
                                : 0u;
    bool saw_dup = false;
    if (!dump_yyjson_direct_flat(out, node->val, est, skip_dup_check, saw_dup)) {
        if (saw_dup) {
            node->dedup_state.fetch_or(8u, std::memory_order_release);
        }
        out.clear();
        return false;
    }
    if (!skip_dup_check && !saw_dup) {
        const std::uint8_t bits = (type == value_t::object) ? (1u | 4u) : 4u;
        node->dedup_state.fetch_or(bits, std::memory_order_release);
        if (node->val == yyjson_doc_get_root(node->doc->doc)) {
            node->doc->tree_unique_verified.store(true, std::memory_order_release);
        }
    }
    return true;
}

NEOJSON_NOINLINE std::string dump_compact_strict_fallback(const json& val) {
    DumpBuffer buf;
    dump_compact_strict(buf, val);
    return buf.take_string();
}

NEOJSON_NOINLINE std::string dump_formatted_fallback(const json& val, const int indent,
                                                     const char indent_char,
                                                     const bool ensure_ascii,
                                                     const error_handler_t error_handler) {
    DumpBuffer buf;
    if (indent >= 0) {
        std::string indent_string(512, indent_char);
        dump_value(buf, val, true, ensure_ascii, error_handler, static_cast<unsigned int>(indent),
                   indent_string, 0);
    } else {
        std::string indent_string;
        dump_value(buf, val, false, ensure_ascii, error_handler, 0, indent_string, 0);
    }
    return buf.take_string();
}

#if NEOJSON_X86_SIMD && defined(__SIZEOF_INT128__)

inline constexpr double kPow10Dbl23[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

struct Pow10Sig128 {
    std::uint64_t hi;
    std::uint64_t lo;
};

// Bit-exact normalized 128-bit significands for 10^-27 .. 10^+27 matching yyjson.
inline constexpr Pow10Sig128 kPow10Sig55[55] = {
    {0x9E74D1B791E07E48ULL, 0x775EA264CF55347DULL}, // 10^-27
    {0xC612062576589DDAULL, 0x95364AFE032A819DULL}, // 10^-26
    {0xF79687AED3EEC551ULL, 0x3A83DDBD83F52204ULL}, // 10^-25
    {0x9ABE14CD44753B52ULL, 0xC4926A9672793542ULL}, // 10^-24
    {0xC16D9A0095928A27ULL, 0x75B7053C0F178293ULL}, // 10^-23
    {0xF1C90080BAF72CB1ULL, 0x5324C68B12DD6338ULL}, // 10^-22
    {0x971DA05074DA7BEEULL, 0xD3F6FC16EBCA5E03ULL}, // 10^-21
    {0xBCE5086492111AEAULL, 0x88F4BB1CA6BCF584ULL}, // 10^-20
    {0xEC1E4A7DB69561A5ULL, 0x2B31E9E3D06C32E5ULL}, // 10^-19
    {0x9392EE8E921D5D07ULL, 0x3AFF322E62439FCFULL}, // 10^-18
    {0xB877AA3236A4B449ULL, 0x09BEFEB9FAD487C2ULL}, // 10^-17
    {0xE69594BEC44DE15BULL, 0x4C2EBE687989A9B3ULL}, // 10^-16
    {0x901D7CF73AB0ACD9ULL, 0x0F9D37014BF60A10ULL}, // 10^-15
    {0xB424DC35095CD80FULL, 0x538484C19EF38C94ULL}, // 10^-14
    {0xE12E13424BB40E13ULL, 0x2865A5F206B06FB9ULL}, // 10^-13
    {0x8CBCCC096F5088CBULL, 0xF93F87B7442E45D3ULL}, // 10^-12
    {0xAFEBFF0BCB24AAFEULL, 0xF78F69A51539D748ULL}, // 10^-11
    {0xDBE6FECEBDEDD5BEULL, 0xB573440E5A884D1BULL}, // 10^-10
    {0x89705F4136B4A597ULL, 0x31680A88F8953030ULL}, // 10^-9
    {0xABCC77118461CEFCULL, 0xFDC20D2B36BA7C3DULL}, // 10^-8
    {0xD6BF94D5E57A42BCULL, 0x3D32907604691B4CULL}, // 10^-7
    {0x8637BD05AF6C69B5ULL, 0xA63F9A49C2C1B10FULL}, // 10^-6
    {0xA7C5AC471B478423ULL, 0x0FCF80DC33721D53ULL}, // 10^-5
    {0xD1B71758E219652BULL, 0xD3C36113404EA4A8ULL}, // 10^-4
    {0x83126E978D4FDF3BULL, 0x645A1CAC083126E9ULL}, // 10^-3
    {0xA3D70A3D70A3D70AULL, 0x3D70A3D70A3D70A3ULL}, // 10^-2
    {0xCCCCCCCCCCCCCCCCULL, 0xCCCCCCCCCCCCCCCCULL}, // 10^-1
    {0x8000000000000000ULL, 0x0000000000000000ULL}, // 10^0
    {0xA000000000000000ULL, 0x0000000000000000ULL}, // 10^1
    {0xC800000000000000ULL, 0x0000000000000000ULL}, // 10^2
    {0xFA00000000000000ULL, 0x0000000000000000ULL}, // 10^3
    {0x9C40000000000000ULL, 0x0000000000000000ULL}, // 10^4
    {0xC350000000000000ULL, 0x0000000000000000ULL}, // 10^5
    {0xF424000000000000ULL, 0x0000000000000000ULL}, // 10^6
    {0x9896800000000000ULL, 0x0000000000000000ULL}, // 10^7
    {0xBEBC200000000000ULL, 0x0000000000000000ULL}, // 10^8
    {0xEE6B280000000000ULL, 0x0000000000000000ULL}, // 10^9
    {0x9502F90000000000ULL, 0x0000000000000000ULL}, // 10^10
    {0xBA43B74000000000ULL, 0x0000000000000000ULL}, // 10^11
    {0xE8D4A51000000000ULL, 0x0000000000000000ULL}, // 10^12
    {0x9184E72A00000000ULL, 0x0000000000000000ULL}, // 10^13
    {0xB5E620F480000000ULL, 0x0000000000000000ULL}, // 10^14
    {0xE35FA931A0000000ULL, 0x0000000000000000ULL}, // 10^15
    {0x8E1BC9BF04000000ULL, 0x0000000000000000ULL}, // 10^16
    {0xB1A2BC2EC5000000ULL, 0x0000000000000000ULL}, // 10^17
    {0xDE0B6B3A76400000ULL, 0x0000000000000000ULL}, // 10^18
    {0x8AC7230489E80000ULL, 0x0000000000000000ULL}, // 10^19
    {0xAD78EBC5AC620000ULL, 0x0000000000000000ULL}, // 10^20
    {0xD8D726B7177A8000ULL, 0x0000000000000000ULL}, // 10^21
    {0x878678326EAC9000ULL, 0x0000000000000000ULL}, // 10^22
    {0xA968163F0A57B400ULL, 0x0000000000000000ULL}, // 10^23
    {0xD3C21BCECCEDA100ULL, 0x0000000000000000ULL}, // 10^24
    {0x84595161401484A0ULL, 0x0000000000000000ULL}, // 10^25
    {0xA56FA5B99019A5C8ULL, 0x0000000000000000ULL}, // 10^26
    {0xCECB8F27F4200F3AULL, 0x0000000000000000ULL}  // 10^27
};

inline constexpr std::int8_t kHexDecodeLut[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
     0, 1, 2, 3, 4, 5, 6, 7, 8, 9,-1,-1,-1,-1,-1,-1,
    -1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

NEOJSON_ALWAYS_INLINE __attribute__((target("avx2"))) bool decode_hex4_fast(
    const char* p, std::uint32_t& out) noexcept {
    const int d0 = kHexDecodeLut[static_cast<unsigned char>(p[0])];
    const int d1 = kHexDecodeLut[static_cast<unsigned char>(p[1])];
    const int d2 = kHexDecodeLut[static_cast<unsigned char>(p[2])];
    const int d3 = kHexDecodeLut[static_cast<unsigned char>(p[3])];
    if (NEOJSON_UNLIKELY((d0 | d1 | d2 | d3) < 0)) {
        return false;
    }
    out = static_cast<std::uint32_t>((d0 << 12) | (d1 << 8) | (d2 << 4) | d3);
    return true;
}

NEOJSON_ALWAYS_INLINE __attribute__((target("avx2"))) bool is_made_of_8_digits_swar(
    const std::uint64_t v) noexcept {
    return (((v & 0xF0F0F0F0F0F0F0F0ULL) |
             (((v + 0x0606060606060606ULL) & 0xF0F0F0F0F0F0F0F0ULL) >> 4u)) ==
            0x3333333333333333ULL);
}

NEOJSON_ALWAYS_INLINE __attribute__((target("avx2"))) std::uint32_t parse_8_digits_swar(
    std::uint64_t v) noexcept {
    constexpr std::uint64_t mask = 0x000000FF000000FFULL;
    constexpr std::uint64_t mul1 = 0x000F424000000064ULL;
    constexpr std::uint64_t mul2 = 0x0000271000000001ULL;
    v -= 0x3030303030303030ULL;
    v = (v * 10u) + (v >> 8u);
    const std::uint64_t v1 = (v & mask) * mul1;
    const std::uint64_t v2 = ((v >> 16u) & mask) * mul2;
    return static_cast<std::uint32_t>((v1 + v2) >> 32u);
}

NEOJSON_ALWAYS_INLINE __attribute__((target("avx2"))) bool is_made_of_4_digits_swar(
    const std::uint32_t v) noexcept {
    return (((v & 0xF0F0F0F0u) | (((v + 0x06060606u) & 0xF0F0F0F0u) >> 4u)) == 0x33333333u);
}

NEOJSON_ALWAYS_INLINE __attribute__((target("avx2"))) std::uint32_t parse_4_digits_swar(
    std::uint32_t v) noexcept {
    v -= 0x30303030u;
    v = (v * 10u) + (v >> 8u);
    return ((v & 0x00FF00FFu) * 0x00640001u) >> 16u;
}

NEOJSON_NOINLINE const char* parse_number_yyjson_fallback(const char* p, const char* end,
                                                          yyjson_val* out) noexcept {
    const std::size_t avail = static_cast<std::size_t>(end - p);
    const std::size_t max_len = (std::min)(avail, std::size_t{120u});
    auto is_num_char = [](const unsigned char ch) noexcept {
        return (static_cast<unsigned>(ch - '0') <= 9u) || ch == '-' || ch == '+' || ch == '.' ||
               ch == 'e' || ch == 'E';
    };
    std::size_t n = 0u;
    while (n < max_len && is_num_char(static_cast<unsigned char>(p[n]))) {
        ++n;
    }
    if (n == 0u || (n < avail && is_num_char(static_cast<unsigned char>(p[n])))) {
        return nullptr;
    }
    char tmp[128];
    std::memcpy(tmp, p, n);
    tmp[n] = '\0';
    yyjson_read_err err{};
    const char* consumed = yyjson_read_number(tmp, out, 0u, nullptr, &err);
    if (consumed == nullptr || static_cast<std::size_t>(consumed - tmp) != n) {
        return nullptr;
    }
    return p + n;
}

NEOJSON_ALWAYS_INLINE __attribute__((target("avx2"))) bool try_eisel_lemire_avx2(
    const bool neg, const std::uint64_t sig, const std::int32_t exp10, yyjson_val* out) noexcept {
    if (NEOJSON_UNLIKELY(exp10 < -27 || exp10 > 27)) {
        return false;
    }
    const Pow10Sig128& p10 = kPow10Sig55[exp10 + 27];
    std::int32_t exp2 = static_cast<std::int32_t>(
                            (static_cast<std::int64_t>(exp10) * 217706LL + (1024LL << 16) -
                             4128768LL) >>
                            16) -
                        1024;
    const int lz = __builtin_clzll(sig);
    const std::uint64_t sig1 = sig << static_cast<unsigned>(lz);
    exp2 -= lz;

    __extension__ using u128_t = unsigned __int128;
    const u128_t prod = static_cast<u128_t>(sig1) * p10.hi;
    std::uint64_t hi = static_cast<std::uint64_t>(prod >> 64u);
    const std::uint64_t lo = static_cast<std::uint64_t>(prod);
    const std::uint64_t bits = hi & 0x1FFULL;
    bool exact = (bits - 1u) < 0x1FEULL;
    if (NEOJSON_UNLIKELY(!exact)) {
        const u128_t prod2 = static_cast<u128_t>(sig1) * p10.lo;
        const std::uint64_t hi2 = static_cast<std::uint64_t>(prod2 >> 64u);
        const std::uint64_t add = lo + hi2;
        if (add + 1u > 1u) {
            hi += (add < lo) ? 1ULL : 0ULL;
            exact = true;
        }
    }
    if (NEOJSON_LIKELY(exact)) {
        const std::uint32_t lz2 = (hi < (1ULL << 63u)) ? 1u : 0u;
        hi <<= lz2;
        exp2 -= static_cast<std::int32_t>(lz2);
        exp2 += 64;
        const bool round_up = (hi & (1ULL << 10u)) != 0u;
        hi += round_up ? (1ULL << 10u) : 0ULL;
        if (NEOJSON_UNLIKELY(hi < (1ULL << 10u))) {
            hi = (1ULL << 63u);
            exp2 += 1;
        }
        hi >>= 11u;
        exp2 += 11 + 52 + 1023;
        if (NEOJSON_LIKELY(exp2 > 0 && exp2 < 2047)) {
            const std::uint64_t raw = (static_cast<std::uint64_t>(neg) << 63u) |
                                      (static_cast<std::uint64_t>(exp2) << 52u) |
                                      (hi & 0x000FFFFFFFFFFFFFULL);
            out->tag = YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL;
            out->uni.u64 = raw;
            return true;
        }
    }
    return false;
}

NEOJSON_NOINLINE __attribute__((target("avx2"))) const char* parse_number_exp_avx2(
    const char* const start, const char* p, const char* const end, const bool neg,
    const std::uint64_t sig, std::int32_t exp10, yyjson_val* out) noexcept {
    if (NEOJSON_LIKELY(p < end && (*p == 'e' || *p == 'E'))) {
        ++p;
        bool exp_neg = false;
        if (p < end && (*p == '-' || *p == '+')) {
            exp_neg = (*p == '-');
            ++p;
        }
        if (NEOJSON_UNLIKELY(p >= end ||
                             static_cast<unsigned>(static_cast<unsigned char>(*p) - '0') > 9u)) {
            return nullptr;
        }
        while (p < end && *p == '0') {
            ++p;
        }
        std::int32_t e = 0;
        int edigits = 0;
        while (p < end) {
            const unsigned d = static_cast<unsigned>(static_cast<unsigned char>(*p) - '0');
            if (d > 9u) {
                break;
            }
            if (NEOJSON_UNLIKELY(++edigits > 4)) {
                return parse_number_yyjson_fallback(start, end, out);
            }
            e = e * 10 + static_cast<std::int32_t>(d);
            ++p;
        }
        if (NEOJSON_UNLIKELY(e > 300)) {
            return parse_number_yyjson_fallback(start, end, out);
        }
        exp10 += exp_neg ? -e : e;
    }

    if (NEOJSON_UNLIKELY(sig == 0u)) {
        out->tag = YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL;
        out->uni.u64 = (static_cast<std::uint64_t>(neg) << 63u);
        return p;
    }
    if (sig < (1ULL << 53u) && exp10 >= -22 && exp10 <= 22) {
        double d = static_cast<double>(sig);
        d = (exp10 < 0) ? (d / kPow10Dbl23[-exp10]) : (d * kPow10Dbl23[exp10]);
        out->tag = YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL;
        out->uni.f64 = neg ? -d : d;
        return p;
    }
    if (NEOJSON_LIKELY(try_eisel_lemire_avx2(neg, sig, exp10, out))) {
        return p;
    }
    return parse_number_yyjson_fallback(start, end, out);
}

NEOJSON_ALWAYS_INLINE __attribute__((target("avx2"))) const char* finish_float_noexp_avx2(
    const char* const start, const char* const p, const char* const end, const bool neg,
    const std::uint64_t sig, const std::int32_t exp10, yyjson_val* out) noexcept {
    if (NEOJSON_UNLIKELY(sig == 0u)) {
        out->tag = YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL;
        out->uni.u64 = (static_cast<std::uint64_t>(neg) << 63u);
        return p;
    }
    if (sig < (1ULL << 53u) && exp10 >= -22) {
        const double d = static_cast<double>(sig) / kPow10Dbl23[-exp10];
        out->tag = YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL;
        out->uni.f64 = neg ? -d : d;
        return p;
    }
    if (NEOJSON_LIKELY(try_eisel_lemire_avx2(neg, sig, exp10, out))) {
        return p;
    }
    return parse_number_yyjson_fallback(start, end, out);
}

NEOJSON_ALWAYS_INLINE __attribute__((target("avx2"))) const char* parse_number_core_avx2(
    const char* p, const char* const end, yyjson_val* out) noexcept {
    const char* const start = p;
    const bool neg = (*p == '-');
    p += neg ? 1 : 0;
    if (NEOJSON_UNLIKELY(p >= end)) {
        return nullptr;
    }

    std::uint64_t sig = 0u;
    int digits = 0;
    bool sig_started = false;

    if (*p == '0') {
        ++p;
        if (p < end) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (NEOJSON_UNLIKELY(static_cast<unsigned>(c - '0') <= 9u)) {
                return nullptr;
            }
            if (c == '.') {
                goto core_frac;
            }
            if (c == 'e' || c == 'E') {
                return parse_number_exp_avx2(start, p, end, neg, 0u, 0, out);
            }
        }
        out->tag = YYJSON_TYPE_NUM | (neg ? YYJSON_SUBTYPE_SINT : YYJSON_SUBTYPE_UINT);
        out->uni.u64 = 0u;
        return p;
    }

    {
        const unsigned d0 = static_cast<unsigned>(static_cast<unsigned char>(*p) - '0');
        if (NEOJSON_UNLIKELY(d0 > 9u)) {
            return nullptr;
        }
        sig = d0;
        sig_started = true;
        ++p;
        digits = 1;
        if (p < end) {
            const unsigned d1 = static_cast<unsigned>(static_cast<unsigned char>(*p) - '0');
            if (d1 <= 9u) {
                sig = sig * 10u + d1;
                ++p;
                digits = 2;
                if (p < end) {
                    const unsigned d2 = static_cast<unsigned>(static_cast<unsigned char>(*p) - '0');
                    if (d2 <= 9u) {
                        sig = sig * 10u + d2;
                        ++p;
                        digits = 3;
                        if (p < end &&
                            static_cast<unsigned>(static_cast<unsigned char>(*p) - '0') <= 9u) {
                            while (end - p >= 8 && digits <= 10) {
                                std::uint64_t w = 0u;
                                std::memcpy(&w, p, 8u);
                                if (!is_made_of_8_digits_swar(w)) {
                                    break;
                                }
                                sig = sig * 100000000ULL + parse_8_digits_swar(w);
                                p += 8;
                                digits += 8;
                            }
                            if (end - p >= 4 && digits <= 14) {
                                std::uint32_t w4 = 0u;
                                std::memcpy(&w4, p, 4u);
                                if (is_made_of_4_digits_swar(w4)) {
                                    sig = sig * 10000ULL + parse_4_digits_swar(w4);
                                    p += 4;
                                    digits += 4;
                                }
                            }
                            while (p < end) {
                                const unsigned d =
                                    static_cast<unsigned>(static_cast<unsigned char>(*p) - '0');
                                if (d > 9u) {
                                    break;
                                }
                                if (NEOJSON_UNLIKELY(digits >= 18)) {
                                    return parse_number_yyjson_fallback(start, end, out);
                                }
                                sig = sig * 10ULL + d;
                                ++p;
                                ++digits;
                            }
                        }
                    }
                }
            }
        }
        if (p >= end || (*p != '.' && *p != 'e' && *p != 'E')) {
            if (!neg) {
                out->tag = YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT;
                out->uni.u64 = sig;
            } else {
                out->tag = YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT;
                out->uni.i64 = -static_cast<std::int64_t>(sig);
            }
            return p;
        }
        if (NEOJSON_UNLIKELY(*p != '.')) {
            return parse_number_exp_avx2(start, p, end, neg, sig, 0, out);
        }
    }

core_frac:
    std::int32_t exp10 = 0;
    ++p;
    if (NEOJSON_UNLIKELY(p >= end ||
                         static_cast<unsigned>(static_cast<unsigned char>(*p) - '0') > 9u)) {
        return nullptr;
    }
    if (!sig_started) {
        while (p < end && *p == '0') {
            ++p;
            --exp10;
        }
    }
    while (end - p >= 8 && digits <= 11) {
        std::uint64_t w = 0u;
        std::memcpy(&w, p, 8u);
        if (!is_made_of_8_digits_swar(w)) {
            break;
        }
        sig = sig * 100000000ULL + parse_8_digits_swar(w);
        p += 8;
        digits += 8;
        exp10 -= 8;
    }
    if (end - p >= 4 && digits <= 15) {
        std::uint32_t w4 = 0u;
        std::memcpy(&w4, p, 4u);
        if (is_made_of_4_digits_swar(w4)) {
            sig = sig * 10000ULL + parse_4_digits_swar(w4);
            p += 4;
            digits += 4;
            exp10 -= 4;
        }
    }
    while (p < end) {
        const unsigned d = static_cast<unsigned>(static_cast<unsigned char>(*p) - '0');
        if (d > 9u) {
            break;
        }
        if (NEOJSON_UNLIKELY(digits >= 19)) {
            return parse_number_yyjson_fallback(start, end, out);
        }
        sig = sig * 10ULL + d;
        ++p;
        ++digits;
        --exp10;
    }
    if (NEOJSON_UNLIKELY(p < end && (*p == 'e' || *p == 'E'))) {
        return parse_number_exp_avx2(start, p, end, neg, sig, exp10, out);
    }
    return finish_float_noexp_avx2(start, p, end, neg, sig, exp10, out);
}

NEOJSON_NOINLINE __attribute__((target("avx2"))) const char* parse_number_slow_dispatch_avx2(
    const char* p, const char* const end, yyjson_val* const out) noexcept {
    return parse_number_core_avx2(p, end, out);
}

NEOJSON_ALWAYS_INLINE __attribute__((target("avx2"))) const char* parse_number_inline_int_avx2(
    const char* p, const char* const end, yyjson_val* const out) noexcept {
    const char* const start = p;
    const bool neg = (*p == '-');
    if (neg) {
        ++p;
        if (NEOJSON_UNLIKELY(p >= end)) {
            return nullptr;
        }
    }
    if (*p == '0') {
        ++p;
        if (p < end) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (NEOJSON_UNLIKELY(static_cast<unsigned>(c - '0') <= 9u || c == '.' || c == 'e' ||
                                 c == 'E')) {
                return parse_number_slow_dispatch_avx2(start, end, out);
            }
        }
        out->tag = YYJSON_TYPE_NUM | (neg ? YYJSON_SUBTYPE_SINT : YYJSON_SUBTYPE_UINT);
        out->uni.u64 = 0u;
        return p;
    }
    const unsigned d0 = static_cast<unsigned>(static_cast<unsigned char>(*p) - '0');
    if (NEOJSON_UNLIKELY(d0 > 9u)) {
        return nullptr;
    }
    std::uint64_t sig = d0;
    ++p;
    int digits = 1;
    if (end - p >= 8) {
        std::uint64_t w = 0u;
        std::memcpy(&w, p, 8u);
        if (is_made_of_8_digits_swar(w)) {
            sig = sig * 100000000ULL + parse_8_digits_swar(w);
            p += 8;
            digits += 8;
            if (end - p >= 8) {
                std::uint64_t w2 = 0u;
                std::memcpy(&w2, p, 8u);
                if (is_made_of_8_digits_swar(w2)) {
                    sig = sig * 100000000ULL + parse_8_digits_swar(w2);
                    p += 8;
                    digits += 8;
                }
            }
            if (digits <= 14 && end - p >= 4) {
                std::uint32_t w4 = 0u;
                std::memcpy(&w4, p, 4u);
                if (is_made_of_4_digits_swar(w4)) {
                    sig = sig * 10000ULL + parse_4_digits_swar(w4);
                    p += 4;
                    digits += 4;
                }
            }
        }
    }
    while (p < end) {
        const unsigned d = static_cast<unsigned>(static_cast<unsigned char>(*p) - '0');
        if (d > 9u) {
            if (NEOJSON_UNLIKELY(*p == '.' || *p == 'e' || *p == 'E')) {
                return parse_number_slow_dispatch_avx2(start, end, out);
            }
            break;
        }
        if (NEOJSON_UNLIKELY(digits >= 18)) {
            return parse_number_slow_dispatch_avx2(start, end, out);
        }
        sig = sig * 10ULL + d;
        ++p;
        ++digits;
    }
    if (!neg) {
        out->tag = YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT;
        out->uni.u64 = sig;
    } else {
        out->tag = YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT;
        out->uni.i64 = -static_cast<std::int64_t>(sig);
    }
    return p;
}

NEOJSON_NOINLINE __attribute__((target("avx2"))) const char* skip_ws_avx2_slow(
    const char* p, const char* const end) noexcept {
    if (p < end && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') {
        return p;
    }
    const __m256i v_sp = _mm256_set1_epi8(' ');
    const __m256i v_nl = _mm256_set1_epi8('\n');
    const __m256i v_cr = _mm256_set1_epi8('\r');
    const __m256i v_tb = _mm256_set1_epi8('\t');
    while (end - p >= 32) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
        const __m256i eq1 = _mm256_or_si256(_mm256_cmpeq_epi8(v, v_sp), _mm256_cmpeq_epi8(v, v_nl));
        const __m256i eq2 = _mm256_or_si256(_mm256_cmpeq_epi8(v, v_cr), _mm256_cmpeq_epi8(v, v_tb));
        const std::uint32_t non_ws =
            ~static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_or_si256(eq1, eq2)));
        if (non_ws != 0u) {
            return p + static_cast<unsigned>(__builtin_ctz(non_ws));
        }
        p += 32;
    }
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) {
        ++p;
    }
    return p;
}

} // namespace

alignas(32) extern std::uint8_t kSimdQuote32[32];
alignas(32) extern std::uint8_t kSimdBslash32[32];
alignas(32) extern std::uint8_t kSimd1F_32[32];
alignas(32) extern std::uint8_t kSimd20_32[32];
alignas(32) extern std::uint8_t kSimdE0_32[32];

alignas(32) std::uint8_t kSimdQuote32[32] = {
    0x22u, 0x22u, 0x22u, 0x22u, 0x22u, 0x22u, 0x22u, 0x22u,
    0x22u, 0x22u, 0x22u, 0x22u, 0x22u, 0x22u, 0x22u, 0x22u,
    0x22u, 0x22u, 0x22u, 0x22u, 0x22u, 0x22u, 0x22u, 0x22u,
    0x22u, 0x22u, 0x22u, 0x22u, 0x22u, 0x22u, 0x22u, 0x22u,
};
alignas(32) std::uint8_t kSimdBslash32[32] = {
    0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu,
    0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu,
    0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu,
    0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu, 0x5Cu,
};
alignas(32) std::uint8_t kSimd1F_32[32] = {
    0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu,
    0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu,
    0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu,
    0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu,
};
alignas(32) std::uint8_t kSimd20_32[32] = {
    0x20u, 0x20u, 0x20u, 0x20u, 0x20u, 0x20u, 0x20u, 0x20u,
    0x20u, 0x20u, 0x20u, 0x20u, 0x20u, 0x20u, 0x20u, 0x20u,
    0x20u, 0x20u, 0x20u, 0x20u, 0x20u, 0x20u, 0x20u, 0x20u,
    0x20u, 0x20u, 0x20u, 0x20u, 0x20u, 0x20u, 0x20u, 0x20u,
};
alignas(32) std::uint8_t kSimdE0_32[32] = {
    0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u,
    0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u,
    0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u,
    0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u, 0xE0u,
};

namespace {
alignas(32) static constexpr std::uint8_t kSimd0F_32[32] = {
    0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu,
    0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu,
    0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu,
    0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu, 0x0Fu,
};
alignas(32) static constexpr std::uint8_t kSimdDF_32[32] = {
    0xDFu, 0xDFu, 0xDFu, 0xDFu, 0xDFu, 0xDFu, 0xDFu, 0xDFu,
    0xDFu, 0xDFu, 0xDFu, 0xDFu, 0xDFu, 0xDFu, 0xDFu, 0xDFu,
    0xDFu, 0xDFu, 0xDFu, 0xDFu, 0xDFu, 0xDFu, 0xDFu, 0xDFu,
    0xDFu, 0xDFu, 0xDFu, 0xDFu, 0xDFu, 0xDFu, 0xDFu, 0xDFu,
};
alignas(32) static constexpr std::uint8_t kSimdEF_32[32] = {
    0xEFu, 0xEFu, 0xEFu, 0xEFu, 0xEFu, 0xEFu, 0xEFu, 0xEFu,
    0xEFu, 0xEFu, 0xEFu, 0xEFu, 0xEFu, 0xEFu, 0xEFu, 0xEFu,
    0xEFu, 0xEFu, 0xEFu, 0xEFu, 0xEFu, 0xEFu, 0xEFu, 0xEFu,
    0xEFu, 0xEFu, 0xEFu, 0xEFu, 0xEFu, 0xEFu, 0xEFu, 0xEFu,
};
alignas(32) static constexpr std::uint8_t kSimd80_32[32] = {
    0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x80u,
    0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x80u,
    0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x80u,
    0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x80u,
};

NEOJSON_ALWAYS_INLINE __attribute__((target("avx2"))) const char* skip_ws_inline_avx2(
    const char* p, const char* const end) noexcept {
    if (NEOJSON_LIKELY(p < end && static_cast<unsigned char>(*p) > 0x20u)) {
        return p;
    }
    return skip_ws_avx2_slow(p, end);
}

NEOJSON_ALWAYS_INLINE __attribute__((target("avx2"))) const char* skip_spaces_once_avx2(
    const char* p, const char* const end) noexcept {
    if (NEOJSON_LIKELY(end - p >= 32)) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
        const __m256i v_sp = _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimd20_32));
        const std::uint32_t non_sp =
            ~static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, v_sp)));
        if (NEOJSON_LIKELY(non_sp != 0u)) {
            p += static_cast<unsigned>(__builtin_ctz(non_sp));
            if (NEOJSON_LIKELY(static_cast<unsigned char>(*p) > 0x20u)) {
                return p;
            }
            return skip_ws_avx2_slow(p, end);
        }
        return skip_ws_avx2_slow(p + 32, end);
    }
    return skip_ws_avx2_slow(p, end);
}

struct StrParseRes {
    const char* p;
    char* str_cur;
};

struct ParseRunCtx {
    const char* p;
    yyjson_val* val_hdr;
    yyjson_val* val;
    yyjson_val* val_end;
    yyjson_val* ctn;
    char* str_pool;
    std::size_t val_cap;
    std::size_t ctn_len;
    std::size_t depth_rem;
};

NEOJSON_NOINLINE bool grow_parse_vals_ptr(ParseRunCtx* const st, const std::size_t kHdrVals,
                                          const std::size_t need) noexcept {
    const std::size_t old_bytes = (kHdrVals + st->val_cap) * sizeof(yyjson_val);
    const std::size_t new_cap = st->val_cap + (st->val_cap >> 1u) + need;
    if (new_cap <= st->val_cap) {
        return false;
    }
    const std::size_t new_bytes = (kHdrVals + new_cap) * sizeof(yyjson_val);
    const std::size_t val_ofs_bytes = static_cast<std::size_t>(
        reinterpret_cast<const char*>(st->val) - reinterpret_cast<const char*>(st->val_hdr));
    const std::size_t ctn_ofs_bytes = static_cast<std::size_t>(
        reinterpret_cast<const char*>(st->ctn) - reinterpret_cast<const char*>(st->val_hdr));
    auto* next_hdr = static_cast<yyjson_val*>(
        large_realloc_bounded(nullptr, st->val_hdr, old_bytes, new_bytes));
    if (NEOJSON_UNLIKELY(next_hdr == nullptr)) {
        return false;
    }
    auto* const next_bytes = reinterpret_cast<char*>(next_hdr);
    st->val_hdr = next_hdr;
    st->val = reinterpret_cast<yyjson_val*>(next_bytes + val_ofs_bytes);
    st->ctn = reinterpret_cast<yyjson_val*>(next_bytes + ctn_ofs_bytes);
    st->val_cap = new_cap;
    st->val_end = next_hdr + kHdrVals + new_cap - 4u;
    return true;
}

alignas(32) static constexpr std::uint8_t kUtf8PrefixMask[64] = {
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
};

NEOJSON_ALWAYS_INLINE __attribute__((target("avx2"))) bool utf8_chunk_valid_avx2(
    const __m256i curr) noexcept {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i v_shift = _mm256_permute2x128_si256(curr, curr, 0x08);
    const __m256i prev1 = _mm256_alignr_epi8(curr, v_shift, 15);
    const __m256i prev2 = _mm256_alignr_epi8(curr, v_shift, 14);
    const __m256i prev3 = _mm256_alignr_epi8(curr, v_shift, 13);

    const __m256i v_0f = _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimd0F_32));
    const __m256i hi1 = _mm256_and_si256(_mm256_srli_epi16(prev1, 4), v_0f);
    const __m256i lo1 = _mm256_and_si256(prev1, v_0f);
    const __m256i hi2 = _mm256_and_si256(_mm256_srli_epi16(curr, 4), v_0f);

    const __m256i lut_hi1 = _mm256_setr_epi8(
        0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
        static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80),
        0x11, 0x01, 0x0D, 0x61,
        0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
        static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80),
        0x11, 0x01, 0x0D, 0x61);
    const __m256i lut_lo1 = _mm256_setr_epi8(
        static_cast<char>(0xB7), static_cast<char>(0x93), static_cast<char>(0x83), static_cast<char>(0x83),
        static_cast<char>(0xC3), static_cast<char>(0xE3), static_cast<char>(0xE3), static_cast<char>(0xE3),
        static_cast<char>(0xE3), static_cast<char>(0xE3), static_cast<char>(0xE3), static_cast<char>(0xE3),
        static_cast<char>(0xE3), static_cast<char>(0xEB), static_cast<char>(0xE3), static_cast<char>(0xE3),
        static_cast<char>(0xB7), static_cast<char>(0x93), static_cast<char>(0x83), static_cast<char>(0x83),
        static_cast<char>(0xC3), static_cast<char>(0xE3), static_cast<char>(0xE3), static_cast<char>(0xE3),
        static_cast<char>(0xE3), static_cast<char>(0xE3), static_cast<char>(0xE3), static_cast<char>(0xE3),
        static_cast<char>(0xE3), static_cast<char>(0xEB), static_cast<char>(0xE3), static_cast<char>(0xE3));
    const __m256i lut_hi2 = _mm256_setr_epi8(
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        static_cast<char>(0xB6), static_cast<char>(0xD6), static_cast<char>(0xDA), static_cast<char>(0xDA),
        0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        static_cast<char>(0xB6), static_cast<char>(0xD6), static_cast<char>(0xDA), static_cast<char>(0xDA),
        0x01, 0x01, 0x01, 0x01);

    const __m256i sc = _mm256_and_si256(
        _mm256_and_si256(_mm256_shuffle_epi8(lut_hi1, hi1), _mm256_shuffle_epi8(lut_lo1, lo1)),
        _mm256_shuffle_epi8(lut_hi2, hi2));
    const __m256i must23 = _mm256_or_si256(
        _mm256_subs_epu8(prev2, _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimdDF_32))),
        _mm256_subs_epu8(prev3, _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimdEF_32))));
    const __m256i must23_80 = _mm256_and_si256(
        _mm256_cmpgt_epi8(must23, zero),
        _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimd80_32)));
    const __m256i err = _mm256_xor_si256(sc, must23_80);
    return _mm256_testz_si256(err, err) != 0;
}

NEOJSON_ALWAYS_INLINE __attribute__((target("avx2"))) void validate_utf8_step_avx2(
    const __m256i p, const __m256i c, const __m256i zero, const __m256i v_0f, const __m256i v_df,
    const __m256i v_ef, const __m256i v_80, const __m256i lut_hi1, const __m256i lut_lo1,
    const __m256i lut_hi2, __m256i& err) noexcept {
    const __m256i v_shift = _mm256_permute2x128_si256(p, c, 0x21);
    const __m256i prev1 = _mm256_alignr_epi8(c, v_shift, 15);
    const __m256i prev2 = _mm256_alignr_epi8(c, v_shift, 14);
    const __m256i prev3 = _mm256_alignr_epi8(c, v_shift, 13);
    const __m256i hi1 = _mm256_and_si256(_mm256_srli_epi16(prev1, 4), v_0f);
    const __m256i lo1 = _mm256_and_si256(prev1, v_0f);
    const __m256i hi2 = _mm256_and_si256(_mm256_srli_epi16(c, 4), v_0f);
    const __m256i sc = _mm256_and_si256(
        _mm256_and_si256(_mm256_shuffle_epi8(lut_hi1, hi1), _mm256_shuffle_epi8(lut_lo1, lo1)),
        _mm256_shuffle_epi8(lut_hi2, hi2));
    const __m256i must23 =
        _mm256_or_si256(_mm256_subs_epu8(prev2, v_df), _mm256_subs_epu8(prev3, v_ef));
    const __m256i must23_80 = _mm256_and_si256(_mm256_cmpgt_epi8(must23, zero), v_80);
    err = _mm256_or_si256(err, _mm256_xor_si256(sc, must23_80));
}

NEOJSON_NOINLINE __attribute__((target("avx2"))) bool validate_utf8_avx2(
    const char* const s, const std::size_t len) noexcept {
    if (len == 0u) {
        return true;
    }
    const __m256i zero = _mm256_setzero_si256();
    const __m256i v_0f = _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimd0F_32));
    const __m256i v_df = _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimdDF_32));
    const __m256i v_ef = _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimdEF_32));
    const __m256i v_80 = _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimd80_32));
    const __m256i lut_hi1 = _mm256_setr_epi8(
        0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
        static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80),
        0x11, 0x01, 0x0D, 0x61,
        0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
        static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80),
        0x11, 0x01, 0x0D, 0x61);
    const __m256i lut_lo1 = _mm256_setr_epi8(
        static_cast<char>(0xB7), static_cast<char>(0x93), static_cast<char>(0x83), static_cast<char>(0x83),
        static_cast<char>(0xC3), static_cast<char>(0xE3), static_cast<char>(0xE3), static_cast<char>(0xE3),
        static_cast<char>(0xE3), static_cast<char>(0xE3), static_cast<char>(0xE3), static_cast<char>(0xE3),
        static_cast<char>(0xE3), static_cast<char>(0xEB), static_cast<char>(0xE3), static_cast<char>(0xE3),
        static_cast<char>(0xB7), static_cast<char>(0x93), static_cast<char>(0x83), static_cast<char>(0x83),
        static_cast<char>(0xC3), static_cast<char>(0xE3), static_cast<char>(0xE3), static_cast<char>(0xE3),
        static_cast<char>(0xE3), static_cast<char>(0xE3), static_cast<char>(0xE3), static_cast<char>(0xE3),
        static_cast<char>(0xE3), static_cast<char>(0xEB), static_cast<char>(0xE3), static_cast<char>(0xE3));
    const __m256i lut_hi2 = _mm256_setr_epi8(
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        static_cast<char>(0xB6), static_cast<char>(0xD6), static_cast<char>(0xDA), static_cast<char>(0xDA),
        0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        static_cast<char>(0xB6), static_cast<char>(0xD6), static_cast<char>(0xDA), static_cast<char>(0xDA),
        0x01, 0x01, 0x01, 0x01);

    __m256i prev = zero;
    __m256i err = zero;

    std::size_t i = 0u;
    for (; i + 32u <= len; i += 32u) {
        const __m256i curr = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + i));
        if (_mm256_movemask_epi8(_mm256_or_si256(prev, curr)) != 0) {
            validate_utf8_step_avx2(prev, curr, zero, v_0f, v_df, v_ef, v_80, lut_hi1, lut_lo1,
                                    lut_hi2, err);
        }
        prev = curr;
    }
    const std::size_t rem = len & 31u;
    if (rem > 0u) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + i));
        const __m256i pmask =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(kUtf8PrefixMask + 32u - rem));
        const __m256i curr = _mm256_and_si256(v, pmask);
        validate_utf8_step_avx2(prev, curr, zero, v_0f, v_df, v_ef, v_80, lut_hi1, lut_lo1,
                                lut_hi2, err);
    } else if (_mm256_movemask_epi8(prev) != 0) {
        validate_utf8_step_avx2(prev, zero, zero, v_0f, v_df, v_ef, v_80, lut_hi1, lut_lo1,
                                lut_hi2, err);
    }
    return _mm256_testz_si256(err, err) != 0;
}

NEOJSON_NOINLINE __attribute__((target("avx2"))) bool decode_unicode_escape_avx2(
    const char*& p, const char* const end, char*& dst) noexcept {
    if (NEOJSON_UNLIKELY(end - p < 4)) {
        return false;
    }
    std::uint32_t cp = 0u;
    if (NEOJSON_UNLIKELY(!decode_hex4_fast(p, cp))) {
        return false;
    }
    p += 4;
    if (cp <= 0x7Fu) {
        *dst++ = static_cast<char>(cp);
    } else if (cp <= 0x7FFu) {
        dst[0] = static_cast<char>(0xC0u | (cp >> 6u));
        dst[1] = static_cast<char>(0x80u | (cp & 0x3Fu));
        dst += 2;
    } else if (cp >= 0xD800u && cp <= 0xDBFFu) {
        if (NEOJSON_UNLIKELY(end - p < 6 || p[0] != '\\' || p[1] != 'u')) {
            return false;
        }
        std::uint32_t cp2 = 0u;
        if (NEOJSON_UNLIKELY(!decode_hex4_fast(p + 2, cp2) || cp2 < 0xDC00u ||
                             cp2 > 0xDFFFu)) {
            return false;
        }
        p += 6;
        const std::uint32_t full =
            0x10000u + (((cp - 0xD800u) << 10u) | (cp2 - 0xDC00u));
        dst[0] = static_cast<char>(0xF0u | (full >> 18u));
        dst[1] = static_cast<char>(0x80u | ((full >> 12u) & 0x3Fu));
        dst[2] = static_cast<char>(0x80u | ((full >> 6u) & 0x3Fu));
        dst[3] = static_cast<char>(0x80u | (full & 0x3Fu));
        dst += 4;
    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
        return false;
    } else {
        dst[0] = static_cast<char>(0xE0u | (cp >> 12u));
        dst[1] = static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu));
        dst[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
        dst += 3;
    }
    return true;
}

struct alignas(64) SimpleEscTable {
    char ch[256]{};
    constexpr SimpleEscTable() noexcept {
        ch[static_cast<unsigned char>('"')] = '"';
        ch[static_cast<unsigned char>('\\')] = '\\';
        ch[static_cast<unsigned char>('/')] = '/';
        ch[static_cast<unsigned char>('b')] = '\b';
        ch[static_cast<unsigned char>('f')] = '\f';
        ch[static_cast<unsigned char>('n')] = '\n';
        ch[static_cast<unsigned char>('r')] = '\r';
        ch[static_cast<unsigned char>('t')] = '\t';
    }
};
static constexpr SimpleEscTable kSimpleEsc{};

NEOJSON_NOINLINE __attribute__((target("avx2"))) StrParseRes parse_string_slow_avx2(
    const char* p, const char* const end, char* const dst_start, char* dst,
    yyjson_val* const out, const bool has_esc_init = false) noexcept {
    bool has_esc = has_esc_init;
    if (p < end) {
        const unsigned char b0 = static_cast<unsigned char>(*p);
        if (NEOJSON_UNLIKELY(b0 < 0x20u)) {
            return {nullptr, dst_start};
        }
        if (b0 == '\\') {
            goto handle_escape;
        }
    }

    for (;;) {
        if (end - p >= 32) {
            const __m256i v_quote =
                _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimdQuote32));
            const __m256i v_bslash =
                _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimdBslash32));
            const __m256i v_e0 = _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimdE0_32));
            const __m256i v_zero = _mm256_setzero_si256();
            do {
                const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), v);
                const __m256i q_or_b = _mm256_or_si256(
                    _mm256_cmpeq_epi8(v, v_quote), _mm256_cmpeq_epi8(v, v_bslash));
                const __m256i ctrl = _mm256_cmpeq_epi8(_mm256_and_si256(v, v_e0), v_zero);
                const std::uint32_t mask = static_cast<std::uint32_t>(
                    _mm256_movemask_epi8(_mm256_or_si256(q_or_b, ctrl)));
                if (mask == 0u) {
                    if (_mm256_movemask_epi8(v) != 0) {
                        if (NEOJSON_UNLIKELY(!utf8_chunk_valid_avx2(v))) {
                            return {nullptr, dst_start};
                        }
                        const unsigned char c29 = static_cast<unsigned char>(p[29]);
                        const unsigned char c30 = static_cast<unsigned char>(p[30]);
                        const unsigned char c31 = static_cast<unsigned char>(p[31]);
                        std::size_t adv = 32u;
                        adv = (c29 >= 0xF0u) ? 29u : adv;
                        adv = (c30 >= 0xE0u) ? 30u : adv;
                        adv = (c31 >= 0xC0u) ? 31u : adv;
                        p += adv;
                        dst += adv;
                    } else {
                        p += 32;
                        dst += 32;
                    }
                    continue;
                }
                const unsigned n = static_cast<unsigned>(__builtin_ctz(mask));
                if (n > 0u) {
                    const __m256i pmask = _mm256_loadu_si256(
                        reinterpret_cast<const __m256i*>(kUtf8PrefixMask + 32u - n));
                    const __m256i v_pre = _mm256_and_si256(v, pmask);
                    if (_mm256_movemask_epi8(v_pre) != 0 &&
                        NEOJSON_UNLIKELY(!utf8_chunk_valid_avx2(v_pre))) {
                        return {nullptr, dst_start};
                    }
                    p += n;
                    dst += n;
                }
                if (NEOJSON_LIKELY(*p == '"')) {
                    *dst = '\0';
                    const std::size_t slen = static_cast<std::size_t>(dst - dst_start);
                    out->tag = (static_cast<std::uint64_t>(slen) << 8u) | YYJSON_TYPE_STR |
                               (has_esc ? YYJSON_SUBTYPE_NONE : YYJSON_SUBTYPE_NOESC);
                    out->uni.str = dst_start;
                    return {p + 1, dst + 1};
                }
                if (NEOJSON_UNLIKELY(*p != '\\')) {
                    return {nullptr, dst_start};
                }
                goto handle_escape;
            } while (end - p >= 32);
        }

        while (p < end) {
            const unsigned char b = static_cast<unsigned char>(*p);
            if (b == '"') {
                *dst = '\0';
                const std::size_t slen = static_cast<std::size_t>(dst - dst_start);
                out->tag = (static_cast<std::uint64_t>(slen) << 8u) | YYJSON_TYPE_STR |
                           (has_esc ? YYJSON_SUBTYPE_NONE : YYJSON_SUBTYPE_NOESC);
                out->uni.str = dst_start;
                return {p + 1, dst + 1};
            }
            if (b == '\\') {
                goto handle_escape;
            }
            if (b >= 0x80u) {
                do {
                    const unsigned char b0 = static_cast<unsigned char>(*p);
                    if (NEOJSON_LIKELY(end - p >= 4)) {
                        std::uint32_t u = 0u;
                        std::memcpy(&u, p, 4u);
                        if (b0 >= 0xE1u && b0 <= 0xEFu) {
                            if (b0 == 0xEDu) {
                                if (NEOJSON_UNLIKELY((u & 0x00C0E000u) != 0x00808000u)) {
                                    return {nullptr, dst_start};
                                }
                            } else if (NEOJSON_UNLIKELY((u & 0x00C0C000u) != 0x00808000u)) {
                                return {nullptr, dst_start};
                            }
                            std::memcpy(dst, &u, 4u);
                            dst += 3;
                            p += 3;
                        } else if (b0 == 0xE0u) {
                            if (NEOJSON_UNLIKELY((u & 0x00C0E000u) != 0x0080A000u)) {
                                return {nullptr, dst_start};
                            }
                            std::memcpy(dst, &u, 4u);
                            dst += 3;
                            p += 3;
                        } else if (b0 >= 0xC2u && b0 <= 0xDFu) {
                            if (NEOJSON_UNLIKELY((u & 0x0000C000u) != 0x00008000u)) {
                                return {nullptr, dst_start};
                            }
                            std::memcpy(dst, &u, 2u);
                            dst += 2;
                            p += 2;
                        } else if (b0 >= 0xF0u && b0 <= 0xF4u) {
                            const unsigned char b1 =
                                static_cast<unsigned char>((u >> 8u) & 0xFFu);
                            if (NEOJSON_UNLIKELY((u & 0xC0C00000u) != 0x80800000u)) {
                                return {nullptr, dst_start};
                            }
                            if (b0 == 0xF0u) {
                                if (b1 < 0x90u || b1 > 0xBFu) {
                                    return {nullptr, dst_start};
                                }
                            } else if (b0 == 0xF4u) {
                                if ((u & 0x0000F000u) != 0x00008000u) {
                                    return {nullptr, dst_start};
                                }
                            } else if ((b1 & 0xC0u) != 0x80u) {
                                return {nullptr, dst_start};
                            }
                            std::memcpy(dst, &u, 4u);
                            dst += 4;
                            p += 4;
                        } else {
                            return {nullptr, dst_start};
                        }
                    } else if (b0 >= 0xC2u && b0 <= 0xDFu) {
                        if (NEOJSON_UNLIKELY(end - p < 2 ||
                                             (static_cast<unsigned char>(p[1]) & 0xC0u) != 0x80u)) {
                            return {nullptr, dst_start};
                        }
                        dst[0] = p[0];
                        dst[1] = p[1];
                        dst += 2;
                        p += 2;
                    } else if (b0 >= 0xE0u && b0 <= 0xEFu) {
                        if (NEOJSON_UNLIKELY(end - p < 3)) {
                            return {nullptr, dst_start};
                        }
                        const unsigned char b1 = static_cast<unsigned char>(p[1]);
                        const unsigned char b2 = static_cast<unsigned char>(p[2]);
                        if ((b2 & 0xC0u) != 0x80u) {
                            return {nullptr, dst_start};
                        }
                        if (b0 == 0xE0u) {
                            if (b1 < 0xA0u || b1 > 0xBFu) {
                                return {nullptr, dst_start};
                            }
                        } else if (b0 == 0xEDu) {
                            if (b1 < 0x80u || b1 > 0x9Fu) {
                                return {nullptr, dst_start};
                            }
                        } else if ((b1 & 0xC0u) != 0x80u) {
                            return {nullptr, dst_start};
                        }
                        dst[0] = p[0];
                        dst[1] = p[1];
                        dst[2] = p[2];
                        dst += 3;
                        p += 3;
                    } else {
                        return {nullptr, dst_start};
                    }
                } while (p < end && static_cast<unsigned char>(*p) >= 0x80u);
                continue;
            }
            if (NEOJSON_UNLIKELY(b < 0x20u)) {
                return {nullptr, dst_start};
            }
            *dst++ = static_cast<char>(b);
            ++p;
        }
        return {nullptr, dst_start};

    handle_escape:
        has_esc = true;
        ++p;
        if (NEOJSON_UNLIKELY(p >= end)) {
            return {nullptr, dst_start};
        }
        const unsigned char esc = static_cast<unsigned char>(*p++);
        const char dec = kSimpleEsc.ch[esc];
        if (NEOJSON_LIKELY(dec != 0)) {
            *dst++ = dec;
        } else if (esc == 'u') {
            if (NEOJSON_UNLIKELY(!decode_unicode_escape_avx2(p, end, dst))) {
                return {nullptr, dst_start};
            }
        } else {
            return {nullptr, dst_start};
        }
    }
}

NEOJSON_ALWAYS_INLINE __attribute__((target("avx2"))) bool parse_string_key_fast_avx2(
    const char*& p_io, const char* const end, char*& str_cur_io, yyjson_val* const out) noexcept {
    const char* p = p_io;
    char* const dst_start = str_cur_io;
    if (NEOJSON_LIKELY(end - p >= 32)) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst_start), v);
        const std::uint32_t q_mask = static_cast<std::uint32_t>(_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(v, _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimdQuote32)))));
        const __m256i ok_vec = _mm256_andnot_si256(
            _mm256_cmpeq_epi8(v, _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimdBslash32))),
            _mm256_cmpgt_epi8(v, _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimd1F_32))));
        const std::uint32_t bad_mask = ~static_cast<std::uint32_t>(_mm256_movemask_epi8(ok_vec));
        if (NEOJSON_LIKELY(q_mask != 0u)) {
            const std::uint32_t pre_mask = (q_mask ^ (q_mask - 1u)) >> 1u;
            if (NEOJSON_LIKELY((bad_mask & pre_mask) == 0u)) {
                const unsigned n = static_cast<unsigned>(__builtin_ctz(q_mask));
                dst_start[n] = '\0';
                out->tag = (static_cast<std::uint64_t>(n) << 8u) | YYJSON_TYPE_STR |
                           YYJSON_SUBTYPE_NOESC;
                out->uni.str = dst_start;
                p_io = p + n + 1u;
                str_cur_io = dst_start + n + 1u;
                return true;
            }
        }
        if (bad_mask != 0u) {
            const unsigned b = static_cast<unsigned>(__builtin_ctz(bad_mask));
            const StrParseRes sr =
                parse_string_slow_avx2(p + b, end, dst_start, dst_start + b, out, false);
            p_io = sr.p;
            str_cur_io = sr.str_cur;
            return sr.p != nullptr;
        }
        p += 32;
        char* dst = dst_start + 32;
        while (end - p >= 32) {
            const __m256i vc = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), vc);
            const std::uint32_t qm = static_cast<std::uint32_t>(_mm256_movemask_epi8(
                _mm256_cmpeq_epi8(
                    vc, _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimdQuote32)))));
            const __m256i okc = _mm256_andnot_si256(
                _mm256_cmpeq_epi8(
                    vc, _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimdBslash32))),
                _mm256_cmpgt_epi8(
                    vc, _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimd1F_32))));
            const std::uint32_t bm = ~static_cast<std::uint32_t>(_mm256_movemask_epi8(okc));
            if (NEOJSON_LIKELY(qm != 0u)) {
                const std::uint32_t prem = (qm ^ (qm - 1u)) >> 1u;
                if (NEOJSON_LIKELY((bm & prem) == 0u)) {
                    const unsigned m = static_cast<unsigned>(__builtin_ctz(qm));
                    dst[m] = '\0';
                    const std::size_t slen = static_cast<std::size_t>((dst + m) - dst_start);
                    out->tag = (static_cast<std::uint64_t>(slen) << 8u) | YYJSON_TYPE_STR |
                               YYJSON_SUBTYPE_NOESC;
                    out->uni.str = dst_start;
                    p_io = p + m + 1u;
                    str_cur_io = dst + m + 1u;
                    return true;
                }
            }
            if (bm != 0u) {
                const unsigned b = static_cast<unsigned>(__builtin_ctz(bm));
                const StrParseRes sr =
                    parse_string_slow_avx2(p + b, end, dst_start, dst + b, out, false);
                p_io = sr.p;
                str_cur_io = sr.str_cur;
                return sr.p != nullptr;
            }
            p += 32;
            dst += 32;
        }
        const StrParseRes sr = parse_string_slow_avx2(p, end, dst_start, dst, out, false);
        p_io = sr.p;
        str_cur_io = sr.str_cur;
        return sr.p != nullptr;
    }
    const StrParseRes sr = parse_string_slow_avx2(p, end, dst_start, dst_start, out, false);
    p_io = sr.p;
    str_cur_io = sr.str_cur;
    return sr.p != nullptr;
}

NEOJSON_ALWAYS_INLINE __attribute__((target("avx2"))) bool parse_string_val_fast_avx2(
    const char*& p_io, const char* const end, char*& str_cur_io, yyjson_val* const out) noexcept {
    const char* p = p_io;
    char* const dst_start = str_cur_io;
    char* dst = dst_start;
    if (NEOJSON_LIKELY(end - p >= 32)) {
        std::uint32_t hi_accum = 0u;
        std::uint8_t subtype = YYJSON_SUBTYPE_NOESC;
        do {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), v);
            const std::uint32_t q_mask = static_cast<std::uint32_t>(_mm256_movemask_epi8(
                _mm256_cmpeq_epi8(
                    v, _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimdQuote32)))));
            const __m256i bad_vec = _mm256_or_si256(
                _mm256_cmpeq_epi8(
                    v, _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimdBslash32))),
                _mm256_cmpeq_epi8(
                    _mm256_and_si256(
                        v, _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimdE0_32))),
                    _mm256_setzero_si256()));
            const std::uint32_t bad_mask =
                static_cast<std::uint32_t>(_mm256_movemask_epi8(bad_vec));
            if (NEOJSON_LIKELY(q_mask != 0u)) {
                const std::uint32_t pre_mask = (q_mask ^ (q_mask - 1u)) >> 1u;
                if (NEOJSON_LIKELY((bad_mask & pre_mask) == 0u)) {
                    const unsigned n = static_cast<unsigned>(__builtin_ctz(q_mask));
                    const std::uint32_t cur_hi =
                        static_cast<std::uint32_t>(_mm256_movemask_epi8(v)) & pre_mask;
                    dst[n] = '\0';
                    const std::size_t slen = static_cast<std::size_t>((dst + n) - dst_start);
                    if (NEOJSON_UNLIKELY((hi_accum | cur_hi) != 0u) &&
                        NEOJSON_UNLIKELY(!validate_utf8_avx2(dst_start, slen))) {
                        return false;
                    }
                    out->tag =
                        (static_cast<std::uint64_t>(slen) << 8u) | YYJSON_TYPE_STR | subtype;
                    out->uni.str = dst_start;
                    p_io = p + n + 1u;
                    str_cur_io = dst + n + 1u;
                    return true;
                }
            }
            if (bad_mask != 0u) {
                const unsigned b = static_cast<unsigned>(__builtin_ctz(bad_mask));
                const std::uint32_t pre_b = (bad_mask ^ (bad_mask - 1u)) >> 1u;
                hi_accum |= static_cast<std::uint32_t>(_mm256_movemask_epi8(v)) & pre_b;
                p += b;
                dst += b;
                if (*p == '\\' && p + 1 < end) {
                    const char ec = kSimpleEsc.ch[static_cast<unsigned char>(p[1])];
                    if (NEOJSON_LIKELY(ec != 0)) {
                        *dst++ = ec;
                        p += 2;
                        subtype = YYJSON_SUBTYPE_NONE;
                        continue;
                    }
                }
                if (NEOJSON_UNLIKELY(hi_accum != 0u) &&
                    NEOJSON_UNLIKELY(!validate_utf8_avx2(
                        dst_start, static_cast<std::size_t>(dst - dst_start)))) {
                    return false;
                }
                const StrParseRes sr = parse_string_slow_avx2(
                    p, end, dst_start, dst, out, subtype == YYJSON_SUBTYPE_NONE);
                p_io = sr.p;
                str_cur_io = sr.str_cur;
                return sr.p != nullptr;
            }
            hi_accum |= static_cast<std::uint32_t>(_mm256_movemask_epi8(v));
            p += 32;
            dst += 32;
        } while (end - p >= 32);
        if (NEOJSON_UNLIKELY(hi_accum != 0u)) {
            p = p_io;
            dst = dst_start;
            subtype = YYJSON_SUBTYPE_NOESC;
        }
        const StrParseRes sr = parse_string_slow_avx2(
            p, end, dst_start, dst, out, subtype == YYJSON_SUBTYPE_NONE);
        p_io = sr.p;
        str_cur_io = sr.str_cur;
        return sr.p != nullptr;
    }
    const StrParseRes sr = parse_string_slow_avx2(p, end, dst_start, dst, out, false);
    p_io = sr.p;
    str_cur_io = sr.str_cur;
    return sr.p != nullptr;
}

NEOJSON_NOINLINE __attribute__((target("avx2"))) int parse_num_pair_run_avx2(
    ParseRunCtx* const st, const char* const end, const std::size_t kHdrVals) noexcept {
    const char* p = st->p;
    yyjson_val* val = st->val;
    yyjson_val* val_end = st->val_end;
    std::size_t ctn_len = st->ctn_len;

    while (p + 4 < end &&
           (p[1] == '-' || static_cast<unsigned>(static_cast<unsigned char>(p[1]) - '0') <= 9u)) {
        if (NEOJSON_UNLIKELY(val >= val_end)) {
            st->val = val;
            if (NEOJSON_UNLIKELY(!grow_parse_vals_ptr(st, kHdrVals, 8u))) {
                return 0;
            }
            val = st->val;
            val_end = st->val_end;
        }
        const char* p1 = parse_number_core_avx2(p + 1, end, val + 1);
        if (NEOJSON_UNLIKELY(p1 == nullptr)) {
            return 0;
        }
        if (NEOJSON_UNLIKELY(p1 < end && static_cast<unsigned char>(*p1) <= 0x20u)) {
            p1 = skip_ws_avx2_slow(p1, end);
        }
        if (p1 < end && *p1 == ',') {
            const char* p2s = p1 + 1;
            if (NEOJSON_UNLIKELY(p2s < end && static_cast<unsigned char>(*p2s) <= 0x20u)) {
                p2s = skip_ws_avx2_slow(p2s, end);
            }
            if (p2s < end &&
                (*p2s == '-' ||
                 static_cast<unsigned>(static_cast<unsigned char>(*p2s) - '0') <= 9u)) {
                const char* p2 = parse_number_core_avx2(p2s, end, val + 2);
                if (NEOJSON_UNLIKELY(p2 == nullptr)) {
                    return 0;
                }
                if (NEOJSON_UNLIKELY(p2 < end && static_cast<unsigned char>(*p2) <= 0x20u)) {
                    p2 = skip_ws_avx2_slow(p2, end);
                }
                if (p2 < end && *p2 == ']') {
                    val[0].tag = (2ULL << 8u) | YYJSON_TYPE_ARR;
                    val[0].uni.ofs = 3u * sizeof(yyjson_val);
                    val += 3;
                    ++ctn_len;
                    p = p2 + 1;
                    if (NEOJSON_UNLIKELY(p < end && static_cast<unsigned char>(*p) <= 0x20u)) {
                        p = skip_ws_avx2_slow(p, end);
                    }
                    if (p < end && *p == ',') {
                        const char* pn = p + 1;
                        if (NEOJSON_UNLIKELY(pn < end &&
                                             static_cast<unsigned char>(*pn) <= 0x20u)) {
                            pn = skip_ws_avx2_slow(pn, end);
                        }
                        p = pn;
                        if (pn + 4 < end && *pn == '[' &&
                            (pn[1] == '-' ||
                             static_cast<unsigned>(static_cast<unsigned char>(pn[1]) - '0') <=
                                 9u)) {
                            continue;
                        }
                        st->p = p;
                        st->val = val;
                        st->val_end = val_end;
                        st->ctn_len = ctn_len;
                        return 1;
                    }
                    st->p = p;
                    st->val = val;
                    st->val_end = val_end;
                    st->ctn_len = ctn_len;
                    return 2;
                }
            }
        }
        st->p = p1;
        st->val = val;
        st->val_end = val_end;
        st->ctn_len = ctn_len;
        return 3;
    }
    st->p = p;
    st->val = val;
    st->val_end = val_end;
    st->ctn_len = ctn_len;
    return 1;
}

NEOJSON_NOINLINE __attribute__((target("avx2"))) yyjson_doc* parse_native_simd_avx2(
    const char* const data, const std::size_t len, const std::size_t max_depth,
    ParseRunCtx* const ctx) noexcept {
    const char* p = data;
    const char* const end = data + len;

    p = skip_ws_inline_avx2(p, end);
    if (p >= end || (*p != '{' && *p != '[')) {
        return nullptr;
    }

    static constexpr std::size_t kHdrVals =
        (sizeof(yyjson_doc) + sizeof(yyjson_val) - 1u) / sizeof(yyjson_val);
    const std::size_t init_cap = (len / 8u) + 64u;
    auto* const val_hdr_init = static_cast<yyjson_val*>(
        large_malloc_bounded(nullptr, (kHdrVals + init_cap) * sizeof(yyjson_val)));
    if (NEOJSON_UNLIKELY(val_hdr_init == nullptr)) {
        return nullptr;
    }
    char* const str_pool_init = static_cast<char*>(large_malloc_bounded(nullptr, len + 64u));
    if (NEOJSON_UNLIKELY(str_pool_init == nullptr)) {
        large_free(nullptr, val_hdr_init);
        return nullptr;
    }
    ctx->val_hdr = val_hdr_init;
    ctx->str_pool = str_pool_init;
    ctx->val_cap = init_cap;
    ctx->depth_rem = max_depth - 1u;

    yyjson_val* val = val_hdr_init + kHdrVals;
    yyjson_val* val_end = val + init_cap - 4u;
    yyjson_val* ctn = val++;
    std::size_t ctn_len = 0u;
    char* str_cur = str_pool_init;

    ctn->uni.ofs = 0u;
    if (*p == '{') {
        ctn->tag = YYJSON_TYPE_OBJ;
        p = skip_ws_inline_avx2(p + 1, end);
        if (NEOJSON_UNLIKELY(p >= end)) {
            goto fail;
        }
        if (*p == '}') {
            ++p;
            goto obj_end;
        }
        goto obj_key_begin_no_ws;
    } else {
        ctn->tag = YYJSON_TYPE_ARR;
        p = skip_ws_inline_avx2(p + 1, end);
        if (NEOJSON_UNLIKELY(p >= end)) {
            goto fail;
        }
        if (*p == ']') {
            ++p;
            goto arr_end;
        }
        goto arr_val_begin_no_ws;
    }

obj_key_begin_spaces:
    if (NEOJSON_LIKELY(end - p >= 32)) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
        const std::uint32_t non_sp = ~static_cast<std::uint32_t>(_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(
                v, _mm256_load_si256(reinterpret_cast<const __m256i*>(kSimd20_32)))));
        if (NEOJSON_LIKELY(non_sp != 0u)) {
            p += static_cast<unsigned>(__builtin_ctz(non_sp));
            if (NEOJSON_LIKELY(*p == '"')) {
                goto obj_key_got_quote;
            }
            p = skip_ws_inline_avx2(p, end);
            if (NEOJSON_UNLIKELY(p >= end)) {
                goto fail;
            }
            goto obj_key_begin_no_ws;
        }
        p = skip_ws_avx2_slow(p + 32, end);
    } else {
        p = skip_ws_avx2_slow(p, end);
    }
    if (NEOJSON_UNLIKELY(p >= end)) {
        goto fail;
    }
    goto obj_key_begin_no_ws;
obj_key_begin:
    p = skip_ws_inline_avx2(p, end);
    if (NEOJSON_UNLIKELY(p >= end)) {
        goto fail;
    }
obj_key_begin_no_ws:
    if (NEOJSON_UNLIKELY(*p != '"')) {
        goto fail;
    }
obj_key_got_quote:
    if (NEOJSON_UNLIKELY(val >= val_end)) {
        ctx->val = val;
        ctx->ctn = ctn;
        if (NEOJSON_UNLIKELY(!grow_parse_vals_ptr(ctx, kHdrVals, 8u))) {
            goto fail;
        }
        val = ctx->val;
        val_end = ctx->val_end;
        ctn = ctx->ctn;
    }
    ++p;
    if (NEOJSON_UNLIKELY(!parse_string_key_fast_avx2(p, end, str_cur, val++))) {
        goto fail;
    }
    if (NEOJSON_LIKELY(end - p >= 3 && p[0] == ':' && p[1] == ' ')) {
        p += 2;
        if (NEOJSON_UNLIKELY(static_cast<unsigned char>(*p) <= 0x20u)) {
            p = skip_ws_avx2_slow(p, end);
            if (NEOJSON_UNLIKELY(p >= end)) {
                goto fail;
            }
        }
    } else {
        if (p >= end || *p != ':') {
            p = skip_ws_avx2_slow(p, end);
            if (NEOJSON_UNLIKELY(p >= end || *p != ':')) {
                goto fail;
            }
        }
        ++p;
        if (p < end && static_cast<unsigned char>(*p) <= 0x20u) {
            p = skip_ws_avx2_slow(p, end);
        }
        if (NEOJSON_UNLIKELY(p >= end)) {
            goto fail;
        }
    }

    {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '"') {
            ++p;
            if (NEOJSON_UNLIKELY(!parse_string_val_fast_avx2(p, end, str_cur, val++))) {
                goto fail;
            }
            ++ctn_len;
        } else if ((c | 0x20u) == 0x7bu) {
            if (NEOJSON_UNLIKELY(ctx->depth_rem == 0u)) {
                goto fail;
            }
            if ((c & 0x20u) == 0u) {
                if (p + 1 < end && p[1] == ']') {
                    val->tag = YYJSON_TYPE_ARR;
                    val->uni.ofs = sizeof(yyjson_val);
                    ++val;
                    ++ctn_len;
                    p += 2;
                } else {
                    if (p + 1 < end && p[1] == '\n') {
                        p = skip_spaces_once_avx2(p + 2, end);
                    } else {
                        p = skip_ws_inline_avx2(p + 1, end);
                    }
                    if (NEOJSON_UNLIKELY(p >= end)) {
                        goto fail;
                    }
                    if (*p == ']') {
                        val->tag = YYJSON_TYPE_ARR;
                        val->uni.ofs = sizeof(yyjson_val);
                        ++val;
                        ++ctn_len;
                        ++p;
                    } else {
                        ctn->tag =
                            (static_cast<std::uint64_t>(ctn_len + 1u) << 8u) | YYJSON_TYPE_OBJ;
                        yyjson_val* const child = val++;
                        child->uni.ofs = static_cast<std::size_t>(
                            reinterpret_cast<const char*>(child) -
                            reinterpret_cast<const char*>(ctn));
                        child->tag = YYJSON_TYPE_ARR;
                        ctn = child;
                        ctn_len = 0u;
                        --ctx->depth_rem;
                        goto arr_val_begin_no_ws;
                    }
                }
            } else {
                if (p + 1 < end && p[1] == '}') {
                    val->tag = YYJSON_TYPE_OBJ;
                    val->uni.ofs = sizeof(yyjson_val);
                    ++val;
                    ++ctn_len;
                    p += 2;
                } else {
                    if (p + 1 < end && p[1] == '\n') {
                        p = skip_spaces_once_avx2(p + 2, end);
                    } else {
                        p = skip_ws_inline_avx2(p + 1, end);
                    }
                    if (NEOJSON_UNLIKELY(p >= end)) {
                        goto fail;
                    }
                    if (*p == '}') {
                        val->tag = YYJSON_TYPE_OBJ;
                        val->uni.ofs = sizeof(yyjson_val);
                        ++val;
                        ++ctn_len;
                        ++p;
                    } else {
                        ctn->tag =
                            (static_cast<std::uint64_t>(ctn_len + 1u) << 8u) | YYJSON_TYPE_OBJ;
                        yyjson_val* const child = val++;
                        child->uni.ofs = static_cast<std::size_t>(
                            reinterpret_cast<const char*>(child) -
                            reinterpret_cast<const char*>(ctn));
                        child->tag = YYJSON_TYPE_OBJ;
                        ctn = child;
                        ctn_len = 0u;
                        --ctx->depth_rem;
                        goto obj_key_begin_no_ws;
                    }
                }
            }
        } else if (static_cast<unsigned>(static_cast<unsigned>(c) - static_cast<unsigned>('0')) <=
                       9u ||
                   c == '-') {
            p = parse_number_inline_int_avx2(p, end, val++);
            if (NEOJSON_UNLIKELY(p == nullptr)) {
                goto fail;
            }
            ++ctn_len;
        } else {
            if (c == 'f') {
                if (NEOJSON_UNLIKELY(end - p < 5 || std::memcmp(p + 1, "alse", 4u) != 0)) {
                    goto fail;
                }
                val++->tag = YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE;
                p += 5;
                ++ctn_len;
            } else if (c == 'n') {
                if (NEOJSON_UNLIKELY(end - p < 4 || std::memcmp(p, "null", 4u) != 0)) {
                    goto fail;
                }
                val++->tag = YYJSON_TYPE_NULL;
                p += 4;
                ++ctn_len;
            } else if (c == 't') {
                if (NEOJSON_UNLIKELY(end - p < 4 || std::memcmp(p, "true", 4u) != 0)) {
                    goto fail;
                }
                val++->tag = YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE;
                p += 4;
                ++ctn_len;
            } else {
                goto fail;
            }
        }
    }

obj_val_end:
    if (NEOJSON_LIKELY(p < end)) {
        if (*p == ',') {
            if (p + 1 < end && p[1] == '\n') {
                p += 2;
                goto obj_key_begin_spaces;
            }
            ++p;
            goto obj_key_begin;
        }
        if (*p == '}') {
            ++p;
            goto obj_end;
        }
        if (*p == '\n') {
            p = skip_spaces_once_avx2(p + 1, end);
        } else {
            p = skip_ws_inline_avx2(p, end);
        }
        if (NEOJSON_LIKELY(p < end)) {
            if (*p == '}') {
                ++p;
                goto obj_end;
            }
            if (*p == ',') {
                ++p;
                goto obj_key_begin;
            }
        }
    }
    goto fail;

obj_end: {
    const std::size_t parent_ofs = ctn->uni.ofs;
    ctn->tag = (static_cast<std::uint64_t>(ctn_len) << 8u) | YYJSON_TYPE_OBJ;
    ctn->uni.ofs = static_cast<std::size_t>(
        reinterpret_cast<const char*>(val) - reinterpret_cast<const char*>(ctn));
    if (NEOJSON_UNLIKELY(parent_ofs == 0u)) {
        goto doc_end;
    }
    ctn = reinterpret_cast<yyjson_val*>(reinterpret_cast<char*>(ctn) - parent_ofs);
    const std::uint64_t ptag = ctn->tag;
    ctn_len = static_cast<std::size_t>(ptag >> 8u);
    ++ctx->depth_rem;
    if (static_cast<std::uint8_t>(ptag) == YYJSON_TYPE_OBJ) {
        goto obj_val_end;
    }
    goto arr_val_end;
}

arr_val_begin_spaces:
    p = skip_spaces_once_avx2(p, end);
    if (NEOJSON_UNLIKELY(p >= end)) {
        goto fail;
    }
    goto arr_val_begin_no_ws;
arr_val_begin:
    p = skip_ws_inline_avx2(p, end);
    if (NEOJSON_UNLIKELY(p >= end)) {
        goto fail;
    }
arr_val_begin_no_ws:
    if (NEOJSON_UNLIKELY(val >= val_end)) {
        ctx->val = val;
        ctx->ctn = ctn;
        if (NEOJSON_UNLIKELY(!grow_parse_vals_ptr(ctx, kHdrVals, 8u))) {
            goto fail;
        }
        val = ctx->val;
        val_end = ctx->val_end;
        ctn = ctx->ctn;
    }

    {
        const unsigned char c = static_cast<unsigned char>(*p);
        if ((c | 0x20u) == 0x7bu) {
            if (NEOJSON_UNLIKELY(ctx->depth_rem == 0u)) {
                goto fail;
            }
            if ((c & 0x20u) != 0u) {
                if (p + 1 < end && p[1] == '}') {
                    val->tag = YYJSON_TYPE_OBJ;
                    val->uni.ofs = sizeof(yyjson_val);
                    ++val;
                    ++ctn_len;
                    p += 2;
                } else {
                    if (p + 1 < end && p[1] == '\n') {
                        p = skip_spaces_once_avx2(p + 2, end);
                    } else {
                        p = skip_ws_inline_avx2(p + 1, end);
                    }
                    if (NEOJSON_UNLIKELY(p >= end)) {
                        goto fail;
                    }
                    if (*p == '}') {
                        val->tag = YYJSON_TYPE_OBJ;
                        val->uni.ofs = sizeof(yyjson_val);
                        ++val;
                        ++ctn_len;
                        ++p;
                    } else {
                        ctn->tag =
                            (static_cast<std::uint64_t>(ctn_len + 1u) << 8u) | YYJSON_TYPE_ARR;
                        yyjson_val* const child = val++;
                        child->uni.ofs = static_cast<std::size_t>(
                            reinterpret_cast<const char*>(child) -
                            reinterpret_cast<const char*>(ctn));
                        child->tag = YYJSON_TYPE_OBJ;
                        ctn = child;
                        ctn_len = 0u;
                        --ctx->depth_rem;
                        goto obj_key_begin_no_ws;
                    }
                }
            } else {
                if (p + 1 < end && p[1] == ']') {
                    val->tag = YYJSON_TYPE_ARR;
                    val->uni.ofs = sizeof(yyjson_val);
                    ++val;
                    ++ctn_len;
                    p += 2;
                } else if (p + 4 < end &&
                           (p[1] == '-' ||
                            static_cast<unsigned>(static_cast<unsigned char>(p[1]) - '0') <= 9u)) {
                    ctx->p = p;
                    ctx->val = val;
                    ctx->val_end = val_end;
                    ctx->ctn = ctn;
                    ctx->ctn_len = ctn_len;
                    const int pair_rc = parse_num_pair_run_avx2(ctx, end, kHdrVals);
                    val = ctx->val;
                    val_end = ctx->val_end;
                    ctn = ctx->ctn;
                    ctn_len = ctx->ctn_len;
                    p = ctx->p;
                    if (NEOJSON_UNLIKELY(pair_rc == 0)) {
                        goto fail;
                    }
                    if (pair_rc == 1) {
                        goto arr_val_begin;
                    }
                    if (pair_rc == 2) {
                        goto arr_val_end;
                    }
                    ctn->tag =
                        (static_cast<std::uint64_t>(ctn_len + 1u) << 8u) | YYJSON_TYPE_ARR;
                    yyjson_val* const child = val;
                    child->uni.ofs = static_cast<std::size_t>(
                        reinterpret_cast<const char*>(child) - reinterpret_cast<const char*>(ctn));
                    child->tag = YYJSON_TYPE_ARR;
                    ctn = child;
                    ctn_len = 1u;
                    val += 2;
                    --ctx->depth_rem;
                    goto arr_val_end;
                } else {
                    if (p + 1 < end && p[1] == '\n') {
                        p = skip_spaces_once_avx2(p + 2, end);
                    } else {
                        p = skip_ws_inline_avx2(p + 1, end);
                    }
                    if (NEOJSON_UNLIKELY(p >= end)) {
                        goto fail;
                    }
                    if (*p == ']') {
                        val->tag = YYJSON_TYPE_ARR;
                        val->uni.ofs = sizeof(yyjson_val);
                        ++val;
                        ++ctn_len;
                        ++p;
                    } else {
                        ctn->tag =
                            (static_cast<std::uint64_t>(ctn_len + 1u) << 8u) | YYJSON_TYPE_ARR;
                        yyjson_val* const child = val++;
                        child->uni.ofs = static_cast<std::size_t>(
                            reinterpret_cast<const char*>(child) -
                            reinterpret_cast<const char*>(ctn));
                        child->tag = YYJSON_TYPE_ARR;
                        ctn = child;
                        ctn_len = 0u;
                        --ctx->depth_rem;
                        goto arr_val_begin_no_ws;
                    }
                }
            }
        } else if (static_cast<unsigned>(static_cast<unsigned>(c) - static_cast<unsigned>('0')) <=
                       9u ||
                   c == '-') {
            p = parse_number_inline_int_avx2(p, end, val++);
            if (NEOJSON_UNLIKELY(p == nullptr)) {
                goto fail;
            }
            ++ctn_len;
        } else if (c == '"') {
            ++p;
            if (NEOJSON_UNLIKELY(!parse_string_val_fast_avx2(p, end, str_cur, val++))) {
                goto fail;
            }
            ++ctn_len;
        } else {
            if (c == 'n') {
                if (NEOJSON_UNLIKELY(end - p < 4 || std::memcmp(p, "null", 4u) != 0)) {
                    goto fail;
                }
                val++->tag = YYJSON_TYPE_NULL;
                p += 4;
                ++ctn_len;
            } else if (c == 'f') {
                if (NEOJSON_UNLIKELY(end - p < 5 || std::memcmp(p + 1, "alse", 4u) != 0)) {
                    goto fail;
                }
                val++->tag = YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE;
                p += 5;
                ++ctn_len;
            } else if (c == 't') {
                if (NEOJSON_UNLIKELY(end - p < 4 || std::memcmp(p, "true", 4u) != 0)) {
                    goto fail;
                }
                val++->tag = YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE;
                p += 4;
                ++ctn_len;
            } else {
                goto fail;
            }
        }
    }

arr_val_end:
    if (NEOJSON_LIKELY(p < end)) {
        if (*p == ',') {
            if (p + 1 < end && p[1] == '\n') {
                p += 2;
                goto arr_val_begin_spaces;
            }
            ++p;
            goto arr_val_begin;
        }
        if (*p == ']') {
            ++p;
            goto arr_end;
        }
        if (*p == '\n') {
            p = skip_spaces_once_avx2(p + 1, end);
        } else {
            p = skip_ws_inline_avx2(p, end);
        }
        if (NEOJSON_LIKELY(p < end)) {
            if (*p == ']') {
                ++p;
                goto arr_end;
            }
            if (*p == ',') {
                ++p;
                goto arr_val_begin;
            }
        }
    }
    goto fail;

arr_end: {
    const std::size_t parent_ofs = ctn->uni.ofs;
    ctn->tag = (static_cast<std::uint64_t>(ctn_len) << 8u) | YYJSON_TYPE_ARR;
    ctn->uni.ofs = static_cast<std::size_t>(
        reinterpret_cast<const char*>(val) - reinterpret_cast<const char*>(ctn));
    if (NEOJSON_UNLIKELY(parent_ofs == 0u)) {
        goto doc_end;
    }
    ctn = reinterpret_cast<yyjson_val*>(reinterpret_cast<char*>(ctn) - parent_ofs);
    const std::uint64_t ptag = ctn->tag;
    ctn_len = static_cast<std::size_t>(ptag >> 8u);
    ++ctx->depth_rem;
    if (static_cast<std::uint8_t>(ptag) == YYJSON_TYPE_OBJ) {
        goto obj_val_end;
    }
    goto arr_val_end;
}

doc_end: {
    p = skip_ws_inline_avx2(p, end);
    if (NEOJSON_UNLIKELY(p != end)) {
        goto fail;
    }
    std::memset(str_cur, 0, 32u);
    yyjson_val* const val_hdr = ctx->val_hdr;
    auto* const doc = reinterpret_cast<yyjson_doc*>(val_hdr);
    yyjson_val* const root = val_hdr + kHdrVals;
    doc->root = root;
    doc->alc = yyjson_parse_alc();
    doc->dat_read = len;
    doc->val_read = static_cast<std::size_t>(val - root);
    doc->str_pool = ctx->str_pool;
    return doc;
}

fail:
    large_free(nullptr, ctx->str_pool);
    large_free(nullptr, ctx->val_hdr);
    return nullptr;
}

#endif // NEOJSON_X86_SIMD && defined(__SIZEOF_INT128__)

inline yyjson_doc* try_parse_native_simd_doc(const std::string_view s,
                                             const std::size_t max_depth) {
#if NEOJSON_X86_SIMD && defined(__SIZEOF_INT128__)
    if (NEOJSON_LIKELY(has_avx2() && s.size() >= 32u && max_depth > 0u &&
                       static_cast<unsigned char>(s[0]) != 0xEFu)) {
        ParseRunCtx ctx{};
        return parse_native_simd_avx2(s.data(), s.size(), max_depth, &ctx);
    }
#else
    (void)s;
    (void)max_depth;
#endif
    return nullptr;
}

} // namespace
} // namespace detail

const json::array_t& json::empty_array_singleton() noexcept {
    static const array_t instance{};
    return instance;
}

const json::object_t& json::empty_object_singleton() noexcept {
    static const object_t instance{};
    return instance;
}

json::string_t json::dump(const int indent, const char indent_char, const bool ensure_ascii,
                          const error_handler_t error_handler) const {
    if (NEOJSON_LIKELY(indent < 0 && !ensure_ascii && error_handler == error_handler_t::strict)) {
        std::string direct_out;
        if (NEOJSON_LIKELY(detail::try_dump_yyjson_direct(*this, direct_out))) {
            return direct_out;
        }
        return detail::dump_compact_strict_fallback(*this);
    }
    return detail::dump_formatted_fallback(*this, indent, indent_char, ensure_ascii, error_handler);
}

std::ostream& operator<<(std::ostream& o, const json& j) {
    const bool pretty_print = (o.width() > 0);
    const auto indentation = (pretty_print ? o.width() : 0);
    o.width(0);
    const char fill_char = o.fill();
    o << j.dump(pretty_print ? static_cast<int>(indentation) : -1, fill_char);
    return o;
}

json json::parse(std::string_view s, parser_callback_t cb, const bool allow_exceptions,
                 const bool ignore_comments, const parse_options& options) {
    yyjson_read_flag flg = YYJSON_READ_ALLOW_BOM;
    if (ignore_comments) {
        flg |= YYJSON_READ_ALLOW_COMMENTS;
    }

    auto fail_nesting = [&]() -> json {
        if (!allow_exceptions) {
            return json(value_t::discarded);
        }
        detail::diagnostic_parser diag(s, ignore_comments, options.max_depth);
        (void)diag.diagnose(true);
        throw parse_error::create(detail::kNestingLimitErrorId, 0,
                                  detail::nesting_limit_message(options.max_depth));
    };

    auto finish_doc = [&](yyjson_doc*& doc, const bool padded_32) -> json {
        yyjson_val* root = yyjson_doc_get_root(doc);
        if (cb) {
            if (!detail::within_depth(doc, root, options.max_depth)) {
                return fail_nesting();
            }
            json result;
            detail::dom_callback_builder builder(result, cb);
            builder.parse_tree(root);
            if (result.is_discarded()) {
                result = nullptr;
            }
            return result;
        }
        if (!yyjson_is_ctn(root) || unsafe_yyjson_get_len(root) == 0u) {
            return detail::make_node(root);
        }
        return detail::lazy_make_root(doc, root, padded_32);
    };

    auto parse_with_nul_scan = [&]() -> json {
        detail::diagnostic_parser diag(s, ignore_comments, options.max_depth);
        if (!diag.diagnose(allow_exceptions)) {
            if (allow_exceptions) {
                throw other_error::create(
                    599, "internal error: diagnostic scanner and yyjson disagreed");
            }
            return json(value_t::discarded);
        }
        const std::string_view prefix = s.substr(0, diag.accepted_prefix_size());
        std::string clean_prefix;
        const char* data = prefix.data();
        const std::size_t size = prefix.size();
        if (prefix.find('\0') != std::string_view::npos) {
            clean_prefix.assign(prefix);
            std::replace(clean_prefix.begin(), clean_prefix.end(), '\0', '\n');
            data = clean_prefix.data();
        }
        yyjson_read_err err{};
        yyjson_doc* doc = yyjson_read_opts_max_depth(
            const_cast<char*>(data), size, flg, &detail::yyjson_parse_alc(),
            options.max_depth, &err);
        if (doc == nullptr) {
            if (err.code == YYJSON_READ_ERROR_DEPTH) {
                return fail_nesting();
            }
            if (allow_exceptions) {
                throw other_error::create(
                    599, "internal error: diagnostic scanner and yyjson disagreed");
            }
            return json(value_t::discarded);
        }
        struct DocGuard {
            yyjson_doc* d;
            ~DocGuard() {
                if (d != nullptr) {
                    yyjson_doc_free(d);
                }
            }
        } guard{doc};
        return finish_doc(guard.d, false);
    };

    // Only pre-scan for NUL when comments are enabled (raw NUL inside comments would not fail yyjson).
    if (ignore_comments && s.find('\0') != std::string_view::npos) {
        return parse_with_nul_scan();
    }

    yyjson_read_err err{};
    yyjson_doc* doc = (!ignore_comments)
                          ? detail::try_parse_native_simd_doc(s, options.max_depth)
                          : nullptr;
    const bool native_simd_padded = (doc != nullptr);
    if (doc == nullptr) {
        // yyjson_read_opts_max_depth does not mutate the input buffer when YYJSON_READ_INSITU is not set.
        doc = yyjson_read_opts_max_depth(
            const_cast<char*>(s.data()), s.size(), flg, &detail::yyjson_parse_alc(),
            options.max_depth, &err);
    }
    if (doc == nullptr) {
        if (!ignore_comments && err.code != YYJSON_READ_ERROR_DEPTH &&
            s.find('\0') != std::string_view::npos) {
            return parse_with_nul_scan();
        }
        if (err.code == YYJSON_READ_ERROR_DEPTH) {
            return fail_nesting();
        }
        if (!allow_exceptions) {
            return json(value_t::discarded);
        }
        detail::diagnostic_parser diag(s, ignore_comments, options.max_depth);
        (void)diag.diagnose(true);
        throw other_error::create(599, "internal error: diagnostic scanner and yyjson disagreed");
    }

    struct DocGuard {
        yyjson_doc* d;
        ~DocGuard() {
            if (d != nullptr) {
                yyjson_doc_free(d);
            }
        }
    } guard{doc};

    return finish_doc(guard.d, native_simd_padded);
}

json json::parse(std::istream& i, parser_callback_t cb, const bool allow_exceptions,
                 const bool ignore_comments, const parse_options& options) {
    std::string buf((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
    return parse(std::string_view(buf), cb, allow_exceptions, ignore_comments, options);
}

bool json::accept(std::string_view s, const bool ignore_comments, const parse_options& options) {
    yyjson_read_flag flg = YYJSON_READ_ALLOW_BOM;
    if (ignore_comments) {
        flg |= YYJSON_READ_ALLOW_COMMENTS;
    }

    auto accept_with_nul_scan = [&]() -> bool {
        detail::diagnostic_parser diag(s, ignore_comments, options.max_depth);
        if (!diag.diagnose(false)) {
            return false;
        }
        const std::string_view prefix = s.substr(0, diag.accepted_prefix_size());
        std::string clean_prefix;
        const char* data = prefix.data();
        const std::size_t size = prefix.size();
        if (prefix.find('\0') != std::string_view::npos) {
            clean_prefix.assign(prefix);
            std::replace(clean_prefix.begin(), clean_prefix.end(), '\0', '\n');
            data = clean_prefix.data();
        }
        yyjson_read_err err{};
        yyjson_doc* doc = yyjson_read_opts_max_depth(
            const_cast<char*>(data), size, flg, &detail::yyjson_parse_alc(),
            options.max_depth, &err);
        if (doc == nullptr) {
            return false;
        }
        yyjson_doc_free(doc);
        return true;
    };

    if (ignore_comments && s.find('\0') != std::string_view::npos) {
        return accept_with_nul_scan();
    }

    yyjson_read_err err{};
    yyjson_doc* doc = (!ignore_comments)
                          ? detail::try_parse_native_simd_doc(s, options.max_depth)
                          : nullptr;
    if (doc == nullptr) {
        doc = yyjson_read_opts_max_depth(
            const_cast<char*>(s.data()), s.size(), flg, &detail::yyjson_parse_alc(),
            options.max_depth, &err);
    }
    if (doc == nullptr) {
        if (!ignore_comments && err.code != YYJSON_READ_ERROR_DEPTH &&
            s.find('\0') != std::string_view::npos) {
            return accept_with_nul_scan();
        }
        return false;
    }
    yyjson_doc_free(doc);
    return true;
}

bool json::accept(std::istream& i, const bool ignore_comments, const parse_options& options) {
    std::string buf((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
    return accept(std::string_view(buf), ignore_comments, options);
}

} // namespace neojson
