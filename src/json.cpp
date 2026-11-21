#include "neojson/json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
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

inline constexpr std::size_t kSlabSize = 65536;
inline constexpr std::uintptr_t kSlabMask = ~std::uintptr_t{kSlabSize - 1};
inline constexpr std::uint32_t kNumSizeClasses = 16;
inline constexpr std::size_t kMaxPooledSize = 4096u;
inline constexpr std::uint32_t kMaxCachedEmptySlabs = 4;
inline constexpr std::uint32_t kReclaimScanLimit = 8;
inline constexpr std::array<std::uint32_t, kNumSizeClasses> kSizeClasses = {{
    16u,   32u,   48u,   64u,   96u,   128u,  192u,  256u,
    384u,  512u,  768u,  1024u, 1536u, 2048u, 3072u, 4096u
}};

struct FreeNode {
    FreeNode* next;
};

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
    SlabHeader* prev;
    SlabHeader* next;
    FreeNode* local_free;
    std::uint32_t class_idx;
    std::uint32_t slot_size;
    std::uint32_t slot_count;
    std::uint32_t bump_off;
    std::uint32_t local_used;
    bool in_full;
    char cache_pad[16];

    std::atomic<ThreadPool*> owner;
    std::atomic<std::uint32_t> remote_lock;
    FreeNode* remote_free;
    std::atomic<std::uint32_t> remote_count;
    std::uint32_t orphan_live;

    SlabHeader(const std::uint32_t cls, const std::uint32_t ssz, const std::uint32_t scnt,
               ThreadPool* own, const std::uint32_t bump, const std::uint32_t used,
               const std::uint32_t orphan) noexcept
        : prev(nullptr),
          next(nullptr),
          local_free(nullptr),
          class_idx(cls),
          slot_size(ssz),
          slot_count(scnt),
          bump_off(bump),
          local_used(used),
          in_full(false),
          cache_pad{},
          owner(own),
          remote_lock(0),
          remote_free(nullptr),
          remote_count(0),
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

inline constexpr std::size_t kSlabHeaderBytes = (sizeof(SlabHeader) + 63u) & ~std::size_t{63};
static_assert(kSlabHeaderBytes % 64u == 0u, "slab header must preserve slot alignment");
static_assert((kSlabSize - kSlabHeaderBytes) / kMaxPooledSize >= 15u,
              "largest pooled class must fit at least 15 slots per 64 KiB slab");

constexpr std::array<std::uint8_t, 256> make_class_lut() noexcept {
    std::array<std::uint8_t, 256> lut{};
    for (std::size_t q = 0; q < 256u; ++q) {
        const std::uint32_t req = static_cast<std::uint32_t>((q + 1u) * 16u);
        std::uint8_t cls = 0;
        while (cls + 1u < kNumSizeClasses && kSizeClasses[cls] < req) {
            ++cls;
        }
        lut[q] = cls;
    }
    return lut;
}

inline constexpr std::array<std::uint8_t, 256> kClassLut = make_class_lut();

inline std::uint32_t class_for_size(const std::size_t n) noexcept {
    if (n == 0u) {
        return 0u;
    }
    assert(n <= kMaxPooledSize);
    return static_cast<std::uint32_t>(kClassLut[(n - 1u) >> 4u]);
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
    if (slot_size > sizeof(FreeNode)) {
        ASAN_POISON_MEMORY_REGION(static_cast<char*>(p) + sizeof(FreeNode),
                                  slot_size - sizeof(FreeNode));
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
    FreeNode* drained = s->remote_free;
    s->remote_free = nullptr;
    const std::uint32_t count = s->remote_count.load(std::memory_order_relaxed);
    s->remote_count.store(0u, std::memory_order_relaxed);
    s->unlock_remote();

    if (drained != nullptr) {
        FreeNode* tail = drained;
        while (tail->next != nullptr) {
            tail = tail->next;
        }
        tail->next = s->local_free;
        s->local_free = drained;
        assert(s->local_used >= count);
        s->local_used -= count;
    }
}

struct SlabBin {
    SlabHeader* head = nullptr;
    SlabHeader* full_head = nullptr;
    SlabHeader* full_tail = nullptr;
};

struct ThreadPool {
    std::array<SlabBin, kNumSizeClasses> bins{};
    SlabHeader* empty_slabs = nullptr;
    std::uint32_t empty_count = 0;

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
        empty_count = 0;

        auto release_list = [](SlabHeader* cur) noexcept {
            while (cur != nullptr) {
                SlabHeader* next = cur->next;
                cur->lock_remote();
                const std::uint32_t rc = cur->remote_count.load(std::memory_order_relaxed);
                assert(cur->local_used >= rc);
                const std::uint32_t live = cur->local_used - rc;
                cur->remote_free = nullptr;
                cur->remote_count.store(0u, std::memory_order_relaxed);
                cur->orphan_live = live;
                cur->owner.store(nullptr, std::memory_order_release);
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

    void* alloc(const std::uint32_t cls) {
        SlabBin& bin = bins[cls];
        while (bin.head != nullptr) {
            SlabHeader* s = bin.head;
            if (FreeNode* node = s->local_free) {
                s->local_free = node->next;
                ++s->local_used;
                unpoison_slot(node, s->slot_size);
                return node;
            }
            if (s->bump_off + s->slot_size <= static_cast<std::uint32_t>(kSlabSize)) {
                void* p = reinterpret_cast<char*>(s) + s->bump_off;
                s->bump_off += s->slot_size;
                ++s->local_used;
                unpoison_slot(p, s->slot_size);
                return p;
            }
            drain_remote(s);
            if (FreeNode* node = s->local_free) {
                s->local_free = node->next;
                ++s->local_used;
                unpoison_slot(node, s->slot_size);
                return node;
            }
            list_erase(bin.head, s);
            s->in_full = true;
            queue_push_back(bin.full_head, bin.full_tail, s);
        }

        for (std::uint32_t scan = 0; scan < kReclaimScanLimit && bin.full_head != nullptr; ++scan) {
            SlabHeader* s = bin.full_head;
            if (s->remote_count.load(std::memory_order_acquire) != 0u) {
                drain_remote(s);
                if (FreeNode* node = s->local_free) {
                    s->local_free = node->next;
                    ++s->local_used;
                    queue_erase(bin.full_head, bin.full_tail, s);
                    s->in_full = false;
                    list_push(bin.head, s);
                    unpoison_slot(node, s->slot_size);
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

        const std::uint32_t ssz = kSizeClasses[cls];
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
        auto* s = ::new (slab_mem) SlabHeader(
            cls, ssz, scnt, this, static_cast<std::uint32_t>(kSlabHeaderBytes) + ssz, 1u, 0u);
        poison_region(static_cast<char*>(slab_mem) + kSlabHeaderBytes + ssz,
                      kSlabSize - kSlabHeaderBytes - ssz);
        list_push(bin.head, s);
        return static_cast<char*>(slab_mem) + kSlabHeaderBytes;
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
    const std::uint32_t ssz = kSizeClasses[cls];
    const std::uint32_t scnt = static_cast<std::uint32_t>((kSlabSize - kSlabHeaderBytes) / ssz);
    void* mem = alloc_aligned_64k();
    ::new (mem) SlabHeader(cls, ssz, scnt, nullptr, static_cast<std::uint32_t>(kSlabSize), 0u, 1u);
    poison_region(static_cast<char*>(mem) + kSlabHeaderBytes + ssz,
                  kSlabSize - kSlabHeaderBytes - ssz);
    return static_cast<char*>(mem) + kSlabHeaderBytes;
}

inline void remote_or_orphan_free(SlabHeader* s, void* p) noexcept {
    s->lock_remote();
    if (s->owner.load(std::memory_order_acquire) != nullptr) {
        auto* node = ::new (p) FreeNode{s->remote_free};
        poison_slot_tail(p, s->slot_size);
        s->remote_free = node;
        const std::uint32_t rc = s->remote_count.load(std::memory_order_relaxed);
        s->remote_count.store(rc + 1u, std::memory_order_release);
        s->unlock_remote();
        return;
    }
    assert(s->orphan_live > 0u);
    const std::uint32_t rem = --s->orphan_live;
    if (rem > 0u) {
        poison_region(p, s->slot_size);
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
    SlabHeader* s = slab_from_ptr(p);
    ThreadPool* const cur = t_pool;
    if (cur != nullptr && s->owner.load(std::memory_order_relaxed) == cur) {
        auto* node = ::new (p) FreeNode{s->local_free};
        poison_slot_tail(p, s->slot_size);
        s->local_free = node;
        --s->local_used;
        SlabBin& bin = cur->bins[s->class_idx];
        if (s->in_full) {
            queue_erase(bin.full_head, bin.full_tail, s);
            s->in_full = false;
            list_push(bin.head, s);
        } else if (s->local_used == 0u && (bin.head != s || s->next != nullptr)) {
            list_erase(bin.head, s);
            cur->recycle_empty_slab(s);
        }
        return;
    }
    remote_or_orphan_free(s, p);
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

void* large_malloc_ephemeral(void* /*ctx*/, const std::size_t size) noexcept {
    return large_malloc_impl(size, true);
}

void* large_realloc_ephemeral(void* /*ctx*/, void* ptr, const std::size_t old_size,
                              const std::size_t new_size) noexcept {
    return large_realloc_impl(ptr, old_size, new_size, true);
}

const yyjson_alc& yyjson_ephemeral_alc() noexcept {
    static const yyjson_alc alc{&large_malloc_ephemeral, &large_realloc_ephemeral, &large_free, nullptr};
    return alc;
}

} // namespace

class DumpBuffer {
public:
    DumpBuffer() noexcept : data_(inline_buf_), size_(0), cap_(sizeof(inline_buf_)) {}

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
        reserve_extra(n);
        append_unchecked(p, n);
    }

    std::string to_string() const {
        return std::string(data_, size_);
    }

private:
    void grow(std::size_t need);

    char inline_buf_[4096];
    char* data_;
    std::size_t size_;
    std::size_t cap_;
};

void DumpBuffer::grow(const std::size_t need) {
    constexpr std::size_t kMaxSize = (std::numeric_limits<std::size_t>::max)();
    if (need > kMaxSize - size_) {
        throw std::bad_alloc();
    }
    const std::size_t required = size_ + need;
    std::size_t new_cap = (cap_ <= kMaxSize / 2) ? cap_ * 2 : kMaxSize;
    if (new_cap < 8192) {
        new_cap = 8192;
    }
    if (new_cap < required) {
        new_cap = required;
    }
    std::size_t actual_cap = new_cap;
    if (data_ == inline_buf_) {
        auto* p = static_cast<char*>(large_alloc_full_cap(new_cap, actual_cap));
        if (p == nullptr) {
            throw std::bad_alloc();
        }
        if (size_ != 0) {
            std::memcpy(p, inline_buf_, size_);
        }
        data_ = p;
    } else {
        auto* p = static_cast<char*>(large_realloc_full_cap(data_, size_, new_cap, actual_cap));
        if (p == nullptr) {
            throw std::bad_alloc();
        }
        data_ = p;
    }
    cap_ = actual_cap;
}

namespace {

inline void append_uint(DumpBuffer& out, std::uint64_t v) {
    static constexpr char kDigits100[201] =
        "0001020304050607080910111213141516171819"
        "2021222324252627282930313233343536373839"
        "4041424344454647484950515253545556575859"
        "6061626364656667686970717273747576777879"
        "8081828384858687888990919293949596979899";
    if (v < 100u) {
        out.reserve_extra(2);
        char* p = out.write_ptr();
        if (v < 10u) {
            *p++ = static_cast<char>('0' + v);
        } else {
            const char* d = kDigits100 + static_cast<std::size_t>(v) * 2u;
            p[0] = d[0];
            p[1] = d[1];
            p += 2;
        }
        out.advance_to(p);
        return;
    }
    char buf[24];
    char* p = buf + sizeof(buf);
    while (v >= 100u) {
        const auto q = v / 100u;
        const auto r = static_cast<std::size_t>((v - q * 100u) * 2u);
        v = q;
        p -= 2;
        p[0] = kDigits100[r];
        p[1] = kDigits100[r + 1];
    }
    if (v >= 10u) {
        const char* d = kDigits100 + static_cast<std::size_t>(v) * 2u;
        p -= 2;
        p[0] = d[0];
        p[1] = d[1];
    } else {
        *--p = static_cast<char>('0' + v);
    }
    out.append(p, static_cast<std::size_t>((buf + sizeof(buf)) - p));
}

inline void append_int(DumpBuffer& out, const std::int64_t v) {
    if (v < 0) {
        out.push('-');
        const auto u = static_cast<std::uint64_t>(-(v + 1)) + 1u;
        append_uint(out, u);
    } else {
        append_uint(out, static_cast<std::uint64_t>(v));
    }
}

} // namespace

namespace dtoa_impl {

inline void append_float(DumpBuffer& out, const double val) {
    out.reserve_extra(40u);
    char* p = out.write_ptr();
    const std::size_t n = yyjson_write_f64_neojson(p, val);
    out.advance_to(p + n);
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

inline std::size_t find_non_verbatim_utf8(const char* p, std::size_t pos,
                                          const std::size_t len) noexcept {
    while (pos + 8 <= len) {
        std::uint64_t w = 0;
        std::memcpy(&w, p + pos, 8);
        if (has_non_verbatim_utf8_8(w)) {
            break;
        }
        pos += 8;
    }
    while (pos < len && kNeedsEscapeUtf8[static_cast<std::uint8_t>(p[pos])] == 0u) {
        ++pos;
    }
    return pos;
}

inline std::size_t find_non_verbatim_ascii(const char* p, std::size_t pos,
                                           const std::size_t len) noexcept {
    while (pos < len && kNeedsEscapeAscii[static_cast<std::uint8_t>(p[pos])] == 0u) {
        ++pos;
    }
    return pos;
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
        while (i + 8 <= len) {
            std::uint64_t w = 0;
            std::memcpy(&w, p + i, 8);
            if (has_non_verbatim_utf8_8(w)) {
                break;
            }
            ensure_space(out, dst, end, 8);
            std::memcpy(dst, p + i, 8);
            dst += 8;
            i += 8;
        }
        while (i < len && kNeedsEscapeUtf8[static_cast<std::uint8_t>(p[i])] == 0u) {
            ensure_space(out, dst, end, 1);
            *dst++ = p[i++];
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

inline bool has_non_verbatim_valid_utf8_8(const std::uint64_t w) noexcept {
    constexpr std::uint64_t kHi = 0x8080808080808080ULL;
    constexpr std::uint64_t kLo = 0x0101010101010101ULL;
    constexpr std::uint64_t kSp = 0x2020202020202020ULL;
    const std::uint64_t has_ctrl = (w - kSp) & ~w & kHi;
    const std::uint64_t q = w ^ 0x2222222222222222ULL;
    const std::uint64_t b = w ^ 0x5C5C5C5C5C5C5C5CULL;
    const std::uint64_t has_quote = (q - kLo) & ~q & kHi;
    const std::uint64_t has_bs = (b - kLo) & ~b & kHi;
    return (has_ctrl | has_quote | has_bs) != 0;
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
        while (i + 8u <= len) {
            std::uint64_t w = 0u;
            std::memcpy(&w, p + i, 8u);
            if (has_non_verbatim_valid_utf8_8(w)) {
                break;
            }
            ensure_space(out, dst, end, 8u);
            std::memcpy(dst, p + i, 8u);
            dst += 8u;
            i += 8u;
        }
        while (i < len) {
            const auto b0 = static_cast<std::uint8_t>(p[i]);
            if (b0 < 0x20u || b0 == 0x22u || b0 == 0x5Cu) {
                break;
            }
            ensure_space(out, dst, end, 1u);
            *dst++ = p[i++];
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

    static SmallObject4* create_small_object_raw() {
        return json::create_small_object_empty();
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
        const std::size_t n = so->count;
        assert(n < kSmallObjCap);
        const std::uint8_t tag = small_obj_key_tag(key);
        ::new (so->slot_ptr(n)) SmallObject4::entry_t(
            std::piecewise_construct,
            std::forward_as_tuple(key.data(), key.size()),
            std::forward_as_tuple(std::move(val)));
        so->tags[n] = tag;
        so->count = static_cast<std::uint8_t>(n + 1u);
    }

    static void small_object_emplace_or_assign(SmallObject4* so, const std::string_view key,
                                               json&& val) {
        const std::size_t n = so->count;
        const std::uint8_t tag = small_obj_key_tag(key);
        for (std::size_t i = 0u; i < n; ++i) {
            if (so->tags[i] == tag && so->entry(i).first == key) {
                so->entry(i).second = std::move(val);
                return;
            }
        }
        assert(n < kSmallObjCap);
        ::new (so->slot_ptr(n)) SmallObject4::entry_t(
            std::piecewise_construct,
            std::forward_as_tuple(key.data(), key.size()),
            std::forward_as_tuple(std::move(val)));
        so->tags[n] = tag;
        so->count = static_cast<std::uint8_t>(n + 1u);
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
};

namespace {

struct SharedYyDoc {
    std::atomic<std::uint32_t> refcount{1u};
    std::atomic<bool> tree_unique_verified{false};
    yyjson_doc* doc = nullptr;

    explicit SharedYyDoc(yyjson_doc* d) noexcept : doc(d) {}
};
static_assert(sizeof(SharedYyDoc) <= 32u, "SharedYyDoc should fit in small pooled slab");

inline void destroy_shared_doc(SharedYyDoc* sdoc) noexcept {
    if (sdoc == nullptr) {
        return;
    }
    yyjson_doc* d = sdoc->doc;
    sdoc->~SharedYyDoc();
    pool_free(sdoc, sizeof(SharedYyDoc));
    yyjson_doc_free(d);
}

inline void shared_doc_release(SharedYyDoc* sdoc) noexcept {
    if (sdoc != nullptr &&
        (sdoc->refcount.load(std::memory_order_acquire) == 1u ||
         sdoc->refcount.fetch_sub(1u, std::memory_order_acq_rel) == 1u)) {
        destroy_shared_doc(sdoc);
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
        raw_len = unsafe_yyjson_get_len(v);
    }
};
static_assert(sizeof(LazyNode) <= 48u, "LazyNode should fit in small pooled slab");

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
        const std::size_t mask = slots.size() - 1u;
        yyjson_obj_iter iter = yyjson_obj_iter_with(obj);
        yyjson_val* key = nullptr;
        while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
            const char* kptr = unsafe_yyjson_get_str(key);
            const std::size_t klen = unsafe_yyjson_get_len(key);
            const std::size_t h = hash_bytes(kptr, klen);
            std::size_t pos = h & mask;
            std::size_t probes = 0u;
            for (;;) {
                auto& e = slots[pos];
                if (e.stamp != stamp) {
                    e = DupProbeEntry{h, kptr, klen, 0u, stamp};
                    break;
                }
                if (e.hash == h && e.len == klen &&
                    (klen == 0u || std::memcmp(e.ptr, kptr, klen) == 0)) {
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
        yyjson_obj_iter it = yyjson_obj_iter_with(val);
        for (std::size_t i = 0u; i < len; ++i) {
            yyjson_val* k = yyjson_obj_iter_next(&it);
            yyjson_val* v = yyjson_obj_iter_get_val(k);
            if (unsafe_yyjson_get_len(k) > 32u ||
                (yyjson_is_ctn(v) && unsafe_yyjson_get_len(v) != 0u)) {
                cheap_leaf = false;
                break;
            }
            keys[i] = k;
            vals[i] = v;
        }
        if (cheap_leaf) {
            if (len <= kSmallObjCap) {
                SmallObject4* so = json_access::create_small_object_raw();
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
        mem = pool_alloc(sizeof(LazyNode));
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

inline json lazy_make_root(yyjson_doc*& doc_ref, yyjson_val* root) {
    void* smem = pool_alloc(sizeof(SharedYyDoc));
    auto* sdoc = ::new (smem) SharedYyDoc(doc_ref);
    void* mem = nullptr;
    try {
        mem = pool_alloc(sizeof(LazyNode));
    } catch (...) {
        sdoc->~SharedYyDoc();
        pool_free(sdoc, sizeof(SharedYyDoc));
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

} // namespace

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
        pool_free(node, sizeof(LazyNode));
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
        pool_free(node, sizeof(LazyNode));
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
        pool_free(node, sizeof(LazyNode));
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
    void* mat = node->materialized.load(std::memory_order_acquire);
    if (mat != nullptr) {
        return static_cast<const json::object_t*>(mat)->contains(key);
    }
    const std::size_t len = node->raw_len;
    if (len > 16u) {
        const std::uint8_t prev = node->lookup_probes.load(std::memory_order_relaxed);
        if (prev >= 3u ||
            node->lookup_probes.fetch_add(1u, std::memory_order_relaxed) >= 2u) {
            const auto* obj =
                static_cast<const json::object_t*>(lazy_materialize_const(hdr, value_t::object));
            return obj->contains(key);
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
            pool_free(node, sizeof(LazyNode));
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

inline bool array_view_equal(const json& lhs, const json& rhs) {
    if (json_access::is_small_array(lhs)) {
        const auto& sa = json_access::small_array_of(lhs);
        if (json_access::is_small_array(rhs)) {
            const auto& sb = json_access::small_array_of(rhs);
            if (sa.count != sb.count) {
                return false;
            }
            for (std::size_t i = 0u; i < sa.count; ++i) {
                if (!(sa.elem(i) == sb.elem(i))) {
                    return false;
                }
            }
            return true;
        }
        const auto* bv = json_access::const_array_of(rhs);
        if (sa.count != bv->size()) {
            return false;
        }
        for (std::size_t i = 0u; i < sa.count; ++i) {
            if (!(sa.elem(i) == (*bv)[i])) {
                return false;
            }
        }
        return true;
    }
    if (json_access::is_small_array(rhs)) {
        return array_view_equal(rhs, lhs);
    }
    return *json_access::const_array_of(lhs) == *json_access::const_array_of(rhs);
}

inline bool array_view_less(const json& lhs, const json& rhs) {
    if (!json_access::is_small_array(lhs) && !json_access::is_small_array(rhs)) {
        return *json_access::const_array_of(lhs) < *json_access::const_array_of(rhs);
    }
    const std::size_t ln = lhs.size();
    const std::size_t rn = rhs.size();
    const std::size_t common = (std::min)(ln, rn);
    const auto* la = json_access::is_small_array(lhs) ? nullptr : json_access::const_array_of(lhs);
    const auto* ra = json_access::is_small_array(rhs) ? nullptr : json_access::const_array_of(rhs);
    for (std::size_t i = 0u; i < common; ++i) {
        const json& le = la != nullptr ? (*la)[i] : json_access::small_array_of(lhs).elem(i);
        const json& re = ra != nullptr ? (*ra)[i] : json_access::small_array_of(rhs).elem(i);
        if (le < re) {
            return true;
        }
        if (re < le) {
            return false;
        }
    }
    return ln < rn;
}

inline bool object_view_equal(const json& lhs, const json& rhs) {
    if (json_access::is_small_object(lhs)) {
        const auto& lo = json_access::small_object_of(lhs);
        if (json_access::is_small_object(rhs)) {
            const auto& ro = json_access::small_object_of(rhs);
            if (lo.count != ro.count) {
                return false;
            }
            for (std::size_t i = 0u; i < lo.count; ++i) {
                const auto& lp = lo.entry(i);
                const auto& rp = ro.entry(i);
                if (!(lp.first == rp.first && lp.second == rp.second)) {
                    return false;
                }
            }
            return true;
        }
        const auto* ro = json_access::const_object_of(rhs);
        if (lo.count != ro->size()) {
            return false;
        }
        for (std::size_t i = 0u; i < lo.count; ++i) {
            const auto& lp = lo.entry(i);
            const auto& rp = (*ro)[i];
            if (!(lp.first == rp.first && lp.second == rp.second)) {
                return false;
            }
        }
        return true;
    }
    if (json_access::is_small_object(rhs)) {
        return object_view_equal(rhs, lhs);
    }
    return *json_access::const_object_of(lhs) == *json_access::const_object_of(rhs);
}

inline bool object_view_less(const json& lhs, const json& rhs) {
    if (!json_access::is_small_object(lhs) && !json_access::is_small_object(rhs)) {
        return *json_access::const_object_of(lhs) < *json_access::const_object_of(rhs);
    }
    const auto* lo =
        json_access::is_small_object(lhs) ? nullptr : json_access::const_object_of(lhs);
    const auto* ro =
        json_access::is_small_object(rhs) ? nullptr : json_access::const_object_of(rhs);
    const std::size_t ln = lo != nullptr ? lo->size() : json_access::small_object_of(lhs).count;
    const std::size_t rn = ro != nullptr ? ro->size() : json_access::small_object_of(rhs).count;
    const std::size_t common = (std::min)(ln, rn);
    for (std::size_t i = 0u; i < common; ++i) {
        const auto& lp = lo != nullptr ? (*lo)[i] : json_access::small_object_of(lhs).entry(i);
        const auto& rp = ro != nullptr ? (*ro)[i] : json_access::small_object_of(rhs).entry(i);
        if (lp.first < rp.first) {
            return true;
        }
        if (rp.first < lp.first) {
            return false;
        }
        if (lp.second < rp.second) {
            return true;
        }
        if (rp.second < lp.second) {
            return false;
        }
    }
    return ln < rn;
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
                    json& val = so->entry(i).second;
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
    pool_free(node, sizeof(LazyNode));
    shared_doc_release(sdoc);
    return mat;
}

namespace {

void dump_compact_strict(DumpBuffer& out, const json& val);
void dump_value(DumpBuffer& out, const json& val, bool pretty_print, bool ensure_ascii,
                error_handler_t error_handler, unsigned int indent_step,
                std::string& indent_string, unsigned int current_indent);
json materialize_yyjson(yyjson_val* root, std::size_t max_depth, bool& depth_exceeded);

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
        const auto& e = so.entry(i);
        utf8_impl::dump_key_colon_fast_strict(out, e.first);
        dump_compact_strict(out, e.second);
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
            const auto& e = so.entry(i);
            out.append(indent_string.data(), new_indent);
            out.push('"');
            utf8_impl::dump_escaped(out, e.first, ensure_ascii, error_handler);
            out.append("\": ", 3);
            dump_value(out, e.second, true, ensure_ascii, error_handler, indent_step,
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
            const auto& e = so.entry(i);
            out.push('"');
            utf8_impl::dump_escaped(out, e.first, ensure_ascii, error_handler);
            out.append("\":", 2);
            dump_value(out, e.second, false, ensure_ascii, error_handler, indent_step,
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
    if (!node->doc->tree_unique_verified.load(std::memory_order_acquire)) {
        const std::uint8_t state = node->dedup_state.load(std::memory_order_acquire);
        if ((state & (2u | 8u)) != 0u) {
            return false;
        }
        if ((state & 4u) == 0u) {
            if (!is_yyjson_tree_unique(node->val)) {
                node->dedup_state.fetch_or(8u, std::memory_order_release);
                return false;
            }
            const std::uint8_t bits = (type == value_t::object) ? (1u | 4u) : 4u;
            node->dedup_state.fetch_or(bits, std::memory_order_release);
            if (node->val == yyjson_doc_get_root(node->doc->doc)) {
                node->doc->tree_unique_verified.store(true, std::memory_order_release);
            }
        }
    }
    const yyjson_write_flag flg =
        YYJSON_WRITE_NEOJSON_FP | YYJSON_WRITE_LOWERCASE_HEX | YYJSON_WRITE_INF_AND_NAN_AS_NULL;
    std::size_t out_len = 0u;
    yyjson_write_err err{};
    char* raw = yyjson_val_write_opts(node->val, flg, &yyjson_ephemeral_alc(), &out_len, &err);
    if (raw == nullptr) {
        if (err.code == YYJSON_WRITE_ERROR_MEMORY_ALLOCATION) {
            throw std::bad_alloc();
        }
        out.clear();
        return false;
    }
    struct LargeBufGuard {
        char* p;
        ~LargeBufGuard() noexcept {
            if (p != nullptr) {
                large_free(nullptr, p);
            }
        }
    } buf_guard{raw};
    out.assign(raw, out_len);
    return true;
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
    if (indent < 0 && !ensure_ascii && error_handler == error_handler_t::strict) {
        std::string direct_out;
        if (detail::try_dump_yyjson_direct(*this, direct_out)) {
            return direct_out;
        }
        detail::DumpBuffer buf;
        detail::dump_compact_strict(buf, *this);
        return buf.to_string();
    }
    detail::DumpBuffer buf;
    if (indent >= 0) {
        std::string indent_string(512, indent_char);
        detail::dump_value(buf, *this, true, ensure_ascii, error_handler,
                           static_cast<unsigned int>(indent), indent_string, 0);
    } else {
        std::string indent_string;
        detail::dump_value(buf, *this, false, ensure_ascii, error_handler, 0, indent_string, 0);
    }
    return buf.to_string();
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

    auto finish_doc = [&](yyjson_doc*& doc) -> json {
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
        return detail::lazy_make_root(doc, root);
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
        return finish_doc(guard.d);
    };

    // Only pre-scan for NUL when comments are enabled (raw NUL inside comments would not fail yyjson).
    if (ignore_comments && s.find('\0') != std::string_view::npos) {
        return parse_with_nul_scan();
    }

    // yyjson_read_opts_max_depth does not mutate the input buffer when YYJSON_READ_INSITU is not set.
    yyjson_read_err err{};
    yyjson_doc* doc = yyjson_read_opts_max_depth(
        const_cast<char*>(s.data()), s.size(), flg, &detail::yyjson_parse_alc(),
        options.max_depth, &err);
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

    return finish_doc(guard.d);
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
    yyjson_doc* doc = yyjson_read_opts_max_depth(
        const_cast<char*>(s.data()), s.size(), flg, &detail::yyjson_parse_alc(),
        options.max_depth, &err);
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
