#ifndef NEOJSON_JSON_HPP_
#define NEOJSON_JSON_HPP_

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <forward_list>
#include <functional>
#include <initializer_list>
#include <iosfwd>
#include <istream>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <valarray>
#include <vector>

namespace neojson {

// Forward declarations
class json;
using ordered_json = json;

template <typename T = void, typename SFINAE = void>
struct adl_serializer;

class json_pointer;

namespace detail {

struct json_access;

// Member count above which a parsed object is built by sorting its keys instead of scanning the members
// inserted so far (see materialize_yyjson in json.cpp). Objects up to this size keep the scan, which is faster for
// a handful of members. To be set from the benchmark.
inline constexpr std::size_t kObjectIndexThreshold = 64;

// Levels of arrays and objects that are destroyed by plain recursion before the destruction of a deeper tree moves
// to a stack on the heap. The recursion costs, per level, about 280 to 420 bytes of stack in an unoptimised build
// with or without AddressSanitizer (measured with GCC 13 and Clang 19: 245 to 421 bytes) and 36 to 85 bytes
// optimised. The budget is a quarter of the 512 KiB that a small thread may have, 128 KiB, and the allowance for
// frames that were not measured (MSVC's unoptimised build) is a factor of two: 128 KiB / (2 * 421 bytes) is 155,
// rounded down to 128. Trees that deep are rare, so almost every tree is destroyed by the recursion alone, which
// needs no allocation and no pass over the elements.
inline constexpr std::size_t kMaxRecursiveDestroyDepth = 128;

// The number of arrays and objects being destroyed on this thread, one inside the other. Each thread counts for
// itself: a value is destroyed on the thread that destroys it, with that thread's stack.
inline thread_local std::size_t t_destroy_depth = 0;
// The number of arrays and objects that this thread destroyed past kMaxRecursiveDestroyDepth, that is, through the
// stack on the heap. The tests read it to see which way a tree was destroyed.
inline thread_local std::size_t t_destroy_heap_stack_uses = 0;

// Counts one array or object being destroyed on this thread, for as long as the guard lives.
struct destroy_depth_guard {
    destroy_depth_guard() noexcept { ++t_destroy_depth; }
    ~destroy_depth_guard() { --t_destroy_depth; }
    destroy_depth_guard(const destroy_depth_guard&) = delete;
    destroy_depth_guard& operator=(const destroy_depth_guard&) = delete;
};

// For the tests: how many arrays and objects are being destroyed on this thread, one inside the other (0 outside a
// destructor), and how many arrays and objects this thread has destroyed through the stack on the heap.
inline std::size_t destroy_depth() noexcept { return t_destroy_depth; }
inline std::size_t destroy_heap_stack_uses() noexcept { return t_destroy_heap_stack_uses; }

// Error id of the parse_error thrown when a document is nested deeper than the limit of its parse call. It lies
// outside the ids that nlohmann/json uses for parse errors.
inline constexpr int kNestingLimitErrorId = 120;

// Tuning constant: initial byte capacity reserved by json::dump().
// To be set from the benchmark.
inline constexpr std::size_t kDumpInitialBufferCapacity = 256;

void* pool_alloc(std::size_t n);
void pool_free(void* p, std::size_t n) noexcept;

template <typename T>
class pool_allocator {
public:
    using value_type = T;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::true_type;

    pool_allocator() noexcept = default;

    template <typename U>
    pool_allocator(const pool_allocator<U>&) noexcept {}

    T* allocate(const std::size_t n) {
        static_assert(alignof(T) <= alignof(std::max_align_t),
                      "pool_allocator supports up to max_align_t alignment");
        if (n == 0) {
            return nullptr;
        }
        constexpr std::size_t kMaxCount = (std::numeric_limits<std::size_t>::max)() / sizeof(T);
        if (n > kMaxCount) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(pool_alloc(n * sizeof(T)));
    }

    void deallocate(T* p, const std::size_t n) noexcept {
        if (p != nullptr) {
            pool_free(p, n * sizeof(T));
        }
    }

    template <typename U>
    bool operator==(const pool_allocator<U>&) const noexcept {
        return true;
    }

    template <typename U>
    bool operator!=(const pool_allocator<U>&) const noexcept {
        return false;
    }
};

enum class value_t : std::uint8_t {
    null,
    object,
    array,
    string,
    boolean,
    number_integer,
    number_unsigned,
    number_float,
    binary,
    discarded
};

inline bool operator<(const value_t lhs, const value_t rhs) noexcept {
    static constexpr std::array<std::uint8_t, 9> order = {{
        0 /* null */,
        3 /* object */,
        4 /* array */,
        5 /* string */,
        1 /* boolean */,
        2 /* integer */,
        2 /* unsigned */,
        2 /* float */,
        6 /* binary */
    }};
    const auto l_index = static_cast<std::size_t>(lhs);
    const auto r_index = static_cast<std::size_t>(rhs);
    return l_index < order.size() && r_index < order.size() && order[l_index] < order[r_index];
}

enum class error_handler_t {
    strict,
    replace,
    ignore
};

class exception : public std::exception {
public:
    const char* what() const noexcept override {
        return m_.what();
    }

    const int id;

protected:
    exception(int id_, const char* what_arg)
        : id(id_), m_(what_arg) {}

    static std::string name(const std::string& ename, int id_) {
        return "[json.exception." + ename + "." + std::to_string(id_) + "] ";
    }

private:
    std::runtime_error m_;
};

class parse_error : public exception {
public:
    static parse_error create(int id_, std::size_t byte_, const std::string& detail) {
        const std::string w = exception::name("parse_error", id_) + "parse error" +
                              (byte_ != 0 ? (" at byte " + std::to_string(byte_)) : "") +
                              ": " + detail;
        return parse_error(id_, byte_, w.c_str());
    }

    static parse_error create(int id_, std::size_t byte_, std::size_t line_, std::size_t col_,
                              const std::string& detail) {
        const std::string w = exception::name("parse_error", id_) + "parse error at line " +
                              std::to_string(line_) + ", column " + std::to_string(col_) + ": " +
                              detail;
        return parse_error(id_, byte_, w.c_str());
    }

    const std::size_t byte;

private:
    parse_error(int id_, std::size_t byte_, const char* what_arg)
        : exception(id_, what_arg), byte(byte_) {}
};

class invalid_iterator : public exception {
public:
    static invalid_iterator create(int id_, const std::string& detail) {
        const std::string w = exception::name("invalid_iterator", id_) + detail;
        return invalid_iterator(id_, w.c_str());
    }

private:
    invalid_iterator(int id_, const char* what_arg)
        : exception(id_, what_arg) {}
};

class type_error : public exception {
public:
    static type_error create(int id_, const std::string& detail) {
        const std::string w = exception::name("type_error", id_) + detail;
        return type_error(id_, w.c_str());
    }

private:
    type_error(int id_, const char* what_arg)
        : exception(id_, what_arg) {}
};

class out_of_range : public exception {
public:
    static out_of_range create(int id_, const std::string& detail) {
        const std::string w = exception::name("out_of_range", id_) + detail;
        return out_of_range(id_, w.c_str());
    }

private:
    out_of_range(int id_, const char* what_arg)
        : exception(id_, what_arg) {}
};

class other_error : public exception {
public:
    static other_error create(int id_, const std::string& detail) {
        const std::string w = exception::name("other_error", id_) + detail;
        return other_error(id_, w.c_str());
    }

private:
    other_error(int id_, const char* what_arg)
        : exception(id_, what_arg) {}
};

template <typename T, typename = void>
struct is_iterator_traits : std::false_type {};

template <typename T>
struct is_iterator_traits<T, std::void_t<typename std::iterator_traits<T>::iterator_category>>
    : std::true_type {};

template <typename T>
struct is_InputIt
    : std::integral_constant<
          bool,
          is_iterator_traits<T>::value && !std::is_integral<std::decay_t<T>>::value> {};

// Member count above which ordered_map maintains a flat open-addressed hash index for O(1) lookup.
inline constexpr std::size_t kOrderedMapHashThreshold = 16;

std::uint64_t hash_seed() noexcept;

inline std::uint64_t rotl64(const std::uint64_t x, const unsigned r) noexcept {
    return (x << r) | (x >> (64u - r));
}

inline std::uint64_t fmix64(std::uint64_t k) noexcept {
    k ^= k >> 33u;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33u;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33u;
    return k;
}

inline std::uint64_t hash_bytes(const char* data, const std::size_t len) noexcept {
    static const std::uint64_t seed = hash_seed();
    std::uint64_t h = seed ^ (static_cast<std::uint64_t>(len) * 0x9e3779b97f4a7c15ULL);
    const auto* p = reinterpret_cast<const unsigned char*>(data);
    if (len <= 16) {
        if (len >= 8) {
            std::uint64_t a = 0;
            std::uint64_t b = 0;
            std::memcpy(&a, p, 8);
            std::memcpy(&b, p + len - 8, 8);
            h ^= a * 0xbf58476d1ce4e5b9ULL;
            h = rotl64(h, 27u) ^ (b * 0x94d049bb133111ebULL);
            return fmix64(h);
        }
        if (len >= 4) {
            std::uint32_t a = 0;
            std::uint32_t b = 0;
            std::memcpy(&a, p, 4);
            std::memcpy(&b, p + len - 4, 4);
            h ^= (static_cast<std::uint64_t>(a) << 32u) | b;
            return fmix64(h);
        }
        if (len > 0) {
            const std::uint64_t v = (static_cast<std::uint64_t>(p[0]) << 16u) |
                                    (static_cast<std::uint64_t>(p[len >> 1u]) << 8u) |
                                    static_cast<std::uint64_t>(p[len - 1]);
            h ^= v * 0x9e3779b97f4a7c15ULL;
            return fmix64(h);
        }
        return fmix64(h);
    }
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

} // namespace detail

template <class Key, class T, class IgnoredLess = std::less<Key>,
          class Allocator = std::allocator<std::pair<Key, T>>>
struct ordered_map : std::vector<std::pair<Key, T>, Allocator> {
    using key_type = Key;
    using mapped_type = T;
    using Container = std::vector<std::pair<Key, T>, Allocator>;
    using iterator = typename Container::iterator;
    using const_iterator = typename Container::const_iterator;
    using size_type = typename Container::size_type;
    using difference_type = typename Container::difference_type;
    using reference = typename Container::reference;
    using value_type = typename Container::value_type;
    using key_compare = std::equal_to<Key>;

    ordered_map() noexcept(noexcept(Container())) : Container() {}
    explicit ordered_map(const Allocator& alloc) noexcept(noexcept(Container(alloc)))
        : Container(alloc) {}

    ordered_map(const ordered_map& other)
        : Container(other),
          m_index(other.m_index != nullptr ? copy_index(*other.m_index) : nullptr) {}

    ordered_map(ordered_map&& other) noexcept = default;

    ordered_map& operator=(const ordered_map& other) {
        if (this != &other) {
            ordered_map tmp(other);
            swap(tmp);
        }
        return *this;
    }

    ordered_map& operator=(ordered_map&& other) noexcept = default;

    template <class It, std::enable_if_t<detail::is_InputIt<It>::value, int> = 0>
    ordered_map(It first, It last, const Allocator& alloc = Allocator())
        : Container(first, last, alloc) {
        if (this->size() > detail::kOrderedMapHashThreshold) {
            build_index_with_min_cap(this->size());
        }
    }

    ordered_map(std::initializer_list<value_type> init, const Allocator& alloc = Allocator())
        : Container(init, alloc) {
        if (this->size() > detail::kOrderedMapHashThreshold) {
            build_index_with_min_cap(this->size());
        }
    }

    void swap(ordered_map& other) noexcept {
        Container::swap(other);
        m_index.swap(other.m_index);
    }

    void clear() noexcept {
        Container::clear();
        m_index.reset();
    }

    void reserve_for_build(const std::size_t expected_len) {
        Container::reserve(expected_len);
        if (expected_len > detail::kOrderedMapHashThreshold &&
            expected_len <= kMaxIndexedSize) {
            const std::size_t needed = next_pow2(expected_len * 2);
            if (m_index == nullptr || m_index->cap < needed) {
                build_index_with_cap(needed);
            }
        }
    }

    void push_back(const value_type& value) {
        ensure_index_for_next();
        Container::push_back(value);
        if (m_index != nullptr) {
            const auto& k = this->back().first;
            insert_index_entry(this->size() - 1, detail::hash_bytes(k.data(), k.size()));
        }
    }

    void push_back(value_type&& value) {
        ensure_index_for_next();
        Container::push_back(std::move(value));
        if (m_index != nullptr) {
            const auto& k = this->back().first;
            insert_index_entry(this->size() - 1, detail::hash_bytes(k.data(), k.size()));
        }
    }

    template <class... Args>
    reference emplace_back(Args&&... args) {
        ensure_index_for_next();
        reference inserted = Container::emplace_back(std::forward<Args>(args)...);
        if (m_index != nullptr) {
            insert_index_entry(this->size() - 1,
                               detail::hash_bytes(inserted.first.data(), inserted.first.size()));
        }
        return inserted;
    }

    void pop_back() {
        if (this->empty()) {
            return;
        }
        if (m_index != nullptr) {
            if (this->size() - 1 <= detail::kOrderedMapHashThreshold) {
                m_index.reset();
            } else {
                remove_index_entry(this->size() - 1);
            }
        }
        Container::pop_back();
    }

    template <class KeyType, class... Args,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value, int> = 0>
    std::pair<iterator, bool> emplace(KeyType&& key, Args&&... args) {
        const std::string_view sv(key);
        std::uint64_t h = 0;
        const std::size_t found = find_pos_with_hash(sv, h);
        if (found != kNpos) {
            return {this->begin() + static_cast<difference_type>(found), false};
        }
        const bool had_index = (m_index != nullptr);
        ensure_index_for_next();
        if (m_index != nullptr && !had_index) {
            h = detail::hash_bytes(sv.data(), sv.size());
        }
        Container::emplace_back(
            std::piecewise_construct,
            std::forward_as_tuple(std::forward<KeyType>(key)),
            std::forward_as_tuple(std::forward<Args>(args)...));
        if (m_index != nullptr) {
            insert_index_entry(this->size() - 1, h);
        }
        return {std::prev(this->end()), true};
    }

    T& emplace_or_assign_key(const std::string_view sv, T&& value) {
        std::uint64_t h = 0;
        const std::size_t found = find_pos_with_hash(sv, h);
        if (found != kNpos) {
            (*this)[found].second = std::move(value);
            return (*this)[found].second;
        }
        const bool had_index = (m_index != nullptr);
        ensure_index_for_next();
        if (m_index != nullptr && !had_index) {
            h = detail::hash_bytes(sv.data(), sv.size());
        }
        Container::emplace_back(
            std::piecewise_construct,
            std::forward_as_tuple(sv),
            std::forward_as_tuple(std::move(value)));
        if (m_index != nullptr) {
            insert_index_entry(this->size() - 1, h);
        }
        return this->back().second;
    }

    using Container::operator[];

    T& operator[](const key_type& key) {
        return emplace(key, T{}).first->second;
    }

    template <class KeyType,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value &&
                                   !std::is_same<std::decay_t<KeyType>, key_type>::value &&
                                   !std::is_integral<std::decay_t<KeyType>>::value,
                               int> = 0>
    T& operator[](KeyType&& key) {
        return emplace(std::forward<KeyType>(key), T{}).first->second;
    }

    const T& operator[](const key_type& key) const {
        return at(key);
    }

    template <class KeyType,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value &&
                                   !std::is_same<std::decay_t<KeyType>, key_type>::value &&
                                   !std::is_integral<std::decay_t<KeyType>>::value,
                               int> = 0>
    const T& operator[](KeyType&& key) const {
        return at(std::forward<KeyType>(key));
    }

    T& at(const key_type& key) {
        const std::size_t pos = find_pos(std::string_view(key));
        if (pos != kNpos) {
            return (*this)[pos].second;
        }
        throw std::out_of_range("key not found");
    }

    template <class KeyType,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value &&
                                   !std::is_same<std::decay_t<KeyType>, key_type>::value,
                               int> = 0>
    T& at(KeyType&& key) {
        const std::size_t pos = find_pos(std::string_view(key));
        if (pos != kNpos) {
            return (*this)[pos].second;
        }
        throw std::out_of_range("key not found");
    }

    const T& at(const key_type& key) const {
        const std::size_t pos = find_pos(std::string_view(key));
        if (pos != kNpos) {
            return (*this)[pos].second;
        }
        throw std::out_of_range("key not found");
    }

    template <class KeyType,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value &&
                                   !std::is_same<std::decay_t<KeyType>, key_type>::value,
                               int> = 0>
    const T& at(KeyType&& key) const {
        const std::size_t pos = find_pos(std::string_view(key));
        if (pos != kNpos) {
            return (*this)[pos].second;
        }
        throw std::out_of_range("key not found");
    }

    size_type erase(const key_type& key) {
        const std::size_t pos = find_pos(std::string_view(key));
        if (pos == kNpos) {
            return 0;
        }
        erase_at(pos);
        return 1;
    }

    template <class KeyType,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value &&
                                   !std::is_same<std::decay_t<KeyType>, key_type>::value &&
                                   !std::is_same<std::decay_t<KeyType>, iterator>::value &&
                                   !std::is_same<std::decay_t<KeyType>, const_iterator>::value,
                               int> = 0>
    size_type erase(KeyType&& key) {
        const std::size_t pos = find_pos(std::string_view(key));
        if (pos == kNpos) {
            return 0;
        }
        erase_at(pos);
        return 1;
    }

    iterator erase(iterator pos) {
        const auto offset = static_cast<std::size_t>(std::distance(this->begin(), pos));
        erase_at(offset);
        return this->begin() + static_cast<difference_type>(offset);
    }

    iterator erase(const_iterator pos) {
        const auto offset = static_cast<std::size_t>(std::distance(this->cbegin(), pos));
        erase_at(offset);
        return this->begin() + static_cast<difference_type>(offset);
    }

    iterator erase(const_iterator first, const_iterator last) {
        const auto offset = std::distance(this->cbegin(), first);
        const auto count = std::distance(first, last);
        if (count == 0) {
            return this->begin() + offset;
        }
        if (count == 1) {
            erase_at(static_cast<std::size_t>(offset));
            return this->begin() + offset;
        }
        Container::erase(this->begin() + offset, this->begin() + offset + count);
        if (m_index != nullptr) {
            if (this->size() <= detail::kOrderedMapHashThreshold) {
                m_index.reset();
            } else {
                build_index_with_min_cap(this->size());
            }
        }
        return this->begin() + offset;
    }

    size_type count(const key_type& key) const {
        return find_pos(std::string_view(key)) != kNpos ? 1 : 0;
    }

    template <class KeyType,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value &&
                                   !std::is_same<std::decay_t<KeyType>, key_type>::value,
                               int> = 0>
    size_type count(KeyType&& key) const {
        return find_pos(std::string_view(key)) != kNpos ? 1 : 0;
    }

    iterator find(const key_type& key) {
        const std::size_t pos = find_pos(std::string_view(key));
        return pos != kNpos ? this->begin() + static_cast<difference_type>(pos) : Container::end();
    }

    template <class KeyType,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value &&
                                   !std::is_same<std::decay_t<KeyType>, key_type>::value,
                               int> = 0>
    iterator find(KeyType&& key) {
        const std::size_t pos = find_pos(std::string_view(key));
        return pos != kNpos ? this->begin() + static_cast<difference_type>(pos) : Container::end();
    }

    const_iterator find(const key_type& key) const {
        const std::size_t pos = find_pos(std::string_view(key));
        return pos != kNpos ? this->cbegin() + static_cast<difference_type>(pos) : Container::cend();
    }

    template <class KeyType,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value &&
                                   !std::is_same<std::decay_t<KeyType>, key_type>::value,
                               int> = 0>
    const_iterator find(KeyType&& key) const {
        const std::size_t pos = find_pos(std::string_view(key));
        return pos != kNpos ? this->cbegin() + static_cast<difference_type>(pos) : Container::cend();
    }

    bool contains(const key_type& key) const {
        return find_pos(std::string_view(key)) != kNpos;
    }

    template <class KeyType,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value &&
                                   !std::is_same<std::decay_t<KeyType>, key_type>::value,
                               int> = 0>
    bool contains(KeyType&& key) const {
        return find_pos(std::string_view(key)) != kNpos;
    }

    std::pair<iterator, bool> insert(value_type&& value) {
        return emplace(std::move(value.first), std::move(value.second));
    }

    std::pair<iterator, bool> insert(const value_type& value) {
        return emplace(value.first, value.second);
    }

    iterator insert(const_iterator /*hint*/, const value_type& value) {
        return insert(value).first;
    }

    iterator insert(const_iterator /*hint*/, value_type&& value) {
        return insert(std::move(value)).first;
    }

    template <typename InputIt, std::enable_if_t<detail::is_InputIt<InputIt>::value, int> = 0>
    void insert(InputIt first, InputIt last) {
        for (auto it = first; it != last; ++it) {
            insert(*it);
        }
    }

    void resize(const size_type count) {
        m_index.reset();
        Container::resize(count);
        if (this->size() > detail::kOrderedMapHashThreshold) {
            build_index_with_min_cap(this->size());
        }
    }

    void resize(const size_type count, const value_type& value) {
        m_index.reset();
        Container::resize(count, value);
        if (this->size() > detail::kOrderedMapHashThreshold) {
            build_index_with_min_cap(this->size());
        }
    }

    void assign(const size_type count, const value_type& value) {
        m_index.reset();
        Container::assign(count, value);
        if (this->size() > detail::kOrderedMapHashThreshold) {
            build_index_with_min_cap(this->size());
        }
    }

    template <class InputIt, std::enable_if_t<detail::is_InputIt<InputIt>::value, int> = 0>
    void assign(InputIt first, InputIt last) {
        m_index.reset();
        Container::assign(first, last);
        if (this->size() > detail::kOrderedMapHashThreshold) {
            build_index_with_min_cap(this->size());
        }
    }

    void assign(std::initializer_list<value_type> ilist) {
        m_index.reset();
        Container::assign(ilist);
        if (this->size() > detail::kOrderedMapHashThreshold) {
            build_index_with_min_cap(this->size());
        }
    }

    friend bool operator==(const ordered_map& lhs, const ordered_map& rhs) {
        return static_cast<const Container&>(lhs) == static_cast<const Container&>(rhs);
    }

    friend bool operator!=(const ordered_map& lhs, const ordered_map& rhs) {
        return !(lhs == rhs);
    }

    friend bool operator<(const ordered_map& lhs, const ordered_map& rhs) {
        return static_cast<const Container&>(lhs) < static_cast<const Container&>(rhs);
    }

private:
    struct Slot {
        std::uint32_t idx_plus_one;
        std::uint32_t hash32;
    };

    struct HashIndex {
        std::size_t cap = 0;
        Slot* slots = nullptr;
    };

    struct HashIndexDeleter {
        void operator()(HashIndex* idx) const noexcept {
            if (idx != nullptr) {
                if (idx->slots != nullptr) {
                    detail::pool_free(idx->slots, idx->cap * sizeof(Slot));
                }
                detail::pool_free(idx, sizeof(HashIndex));
            }
        }
    };

    using IndexPtr = std::unique_ptr<HashIndex, HashIndexDeleter>;

    static IndexPtr make_index(const std::size_t c) {
        auto* h = ::new (detail::pool_alloc(sizeof(HashIndex))) HashIndex{0, nullptr};
        IndexPtr ptr(h);
        const std::size_t bytes = c * sizeof(Slot);
        auto* s = static_cast<Slot*>(detail::pool_alloc(bytes));
        for (std::size_t i = 0; i < c; ++i) {
            ::new (s + i) Slot{0u, 0u};
        }
        h->cap = c;
        h->slots = s;
        return ptr;
    }

    static IndexPtr copy_index(const HashIndex& other) {
        auto* h = ::new (detail::pool_alloc(sizeof(HashIndex))) HashIndex{0, nullptr};
        IndexPtr ptr(h);
        const std::size_t c = other.cap;
        const std::size_t bytes = c * sizeof(Slot);
        auto* s = static_cast<Slot*>(detail::pool_alloc(bytes));
        for (std::size_t i = 0; i < c; ++i) {
            ::new (s + i) Slot{other.slots[i].idx_plus_one, other.slots[i].hash32};
        }
        h->cap = c;
        h->slots = s;
        return ptr;
    }

    static constexpr std::size_t kNpos = static_cast<std::size_t>(-1);
    static constexpr std::size_t kMaxIndexedSize = 0xFFFFFFFEu;

    static bool key_equals(const key_type& k, const std::string_view sv) noexcept {
        return k.size() == sv.size() &&
               (sv.empty() || std::memcmp(k.data(), sv.data(), sv.size()) == 0);
    }

    static std::size_t next_pow2(const std::size_t v) noexcept {
        constexpr std::size_t kMaxCap = ((std::numeric_limits<std::size_t>::max)() >> 2u) + 1u;
        std::size_t cap = 64;
        while (cap < v && cap < kMaxCap) {
            cap <<= 1u;
        }
        return cap;
    }

    std::size_t find_pos_with_hash(const std::string_view sv, std::uint64_t& out_h) const noexcept {
        if (m_index != nullptr) {
            const std::uint64_t h = detail::hash_bytes(sv.data(), sv.size());
            out_h = h;
            const std::uint32_t h32 = static_cast<std::uint32_t>(h >> 32u);
            const std::size_t mask = m_index->cap - 1;
            std::size_t pos = static_cast<std::size_t>(h) & mask;
            const Slot* slots = m_index->slots;
            const value_type* elems = this->data();
            for (;;) {
                const Slot s = slots[pos];
                if (s.idx_plus_one == 0) {
                    return kNpos;
                }
                if (s.hash32 == h32) {
                    const std::size_t idx = static_cast<std::size_t>(s.idx_plus_one - 1);
                    if (key_equals(elems[idx].first, sv)) {
                        return idx;
                    }
                }
                pos = (pos + 1) & mask;
            }
        }
        const value_type* elems = this->data();
        const std::size_t n = this->size();
        for (std::size_t i = 0; i < n; ++i) {
            if (key_equals(elems[i].first, sv)) {
                return i;
            }
        }
        return kNpos;
    }

    std::size_t find_pos(const std::string_view sv) const noexcept {
        std::uint64_t unused = 0;
        return find_pos_with_hash(sv, unused);
    }

    void build_index_with_cap(const std::size_t target_cap) {
        const std::size_t n = this->size();
        IndexPtr next_idx = make_index(target_cap);
        const std::size_t mask = target_cap - 1;
        Slot* slots = next_idx->slots;
        const value_type* elems = this->data();
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint64_t h =
                detail::hash_bytes(elems[i].first.data(), elems[i].first.size());
            const std::uint32_t h32 = static_cast<std::uint32_t>(h >> 32u);
            std::size_t pos = static_cast<std::size_t>(h) & mask;
            while (slots[pos].idx_plus_one != 0) {
                pos = (pos + 1) & mask;
            }
            slots[pos] = {static_cast<std::uint32_t>(i + 1), h32};
        }
        m_index = std::move(next_idx);
    }

    void build_index_with_min_cap(const std::size_t min_elements) {
        if (min_elements > kMaxIndexedSize) {
            m_index.reset();
            return;
        }
        build_index_with_cap(next_pow2(min_elements * 2));
    }

    void ensure_index_for_next() {
        const std::size_t next_n = this->size() + 1;
        if (m_index != nullptr) {
            if (next_n > kMaxIndexedSize) {
                m_index.reset();
            } else if (next_n * 2 > m_index->cap) {
                build_index_with_min_cap(next_n);
            }
        } else if (next_n > detail::kOrderedMapHashThreshold && next_n <= kMaxIndexedSize) {
            build_index_with_min_cap(next_n);
        }
    }

    void insert_index_entry(const std::size_t idx, const std::uint64_t h) noexcept {
        const std::size_t mask = m_index->cap - 1;
        const std::uint32_t h32 = static_cast<std::uint32_t>(h >> 32u);
        Slot* slots = m_index->slots;
        std::size_t pos = static_cast<std::size_t>(h) & mask;
        while (slots[pos].idx_plus_one != 0) {
            pos = (pos + 1) & mask;
        }
        slots[pos] = {static_cast<std::uint32_t>(idx + 1), h32};
    }

    void remove_index_entry(const std::size_t idx) noexcept {
        const std::size_t mask = m_index->cap - 1;
        Slot* slots = m_index->slots;
        const key_type& target_key = (*this)[idx].first;
        const std::uint64_t h = detail::hash_bytes(target_key.data(), target_key.size());
        std::size_t pos = static_cast<std::size_t>(h) & mask;
        const std::uint32_t want = static_cast<std::uint32_t>(idx + 1);
        while (slots[pos].idx_plus_one != want) {
            pos = (pos + 1) & mask;
        }
        std::size_t cur = pos;
        std::size_t scan = (cur + 1) & mask;
        for (;;) {
            const Slot ns = slots[scan];
            if (ns.idx_plus_one == 0) {
                slots[cur] = Slot{};
                break;
            }
            const key_type& nk = (*this)[static_cast<std::size_t>(ns.idx_plus_one - 1)].first;
            const std::size_t home =
                static_cast<std::size_t>(detail::hash_bytes(nk.data(), nk.size())) & mask;
            if (((cur - home) & mask) < ((scan - home) & mask)) {
                slots[cur] = ns;
                cur = scan;
            }
            scan = (scan + 1) & mask;
        }
    }

    void erase_at(const std::size_t idx) {
        if (m_index != nullptr) {
            if (this->size() - 1 <= detail::kOrderedMapHashThreshold) {
                m_index.reset();
            } else {
                remove_index_entry(idx);
                if (idx + 1 < this->size()) {
                    const std::uint32_t pivot = static_cast<std::uint32_t>(idx + 1);
                    Slot* slots = m_index->slots;
                    for (std::size_t i = 0; i < m_index->cap; ++i) {
                        if (slots[i].idx_plus_one > pivot) {
                            --slots[i].idx_plus_one;
                        }
                    }
                }
            }
        }
        Container::erase(this->begin() + static_cast<difference_type>(idx));
    }

    IndexPtr m_index;
};

namespace detail {

template <typename BasicJsonType>
class json_ref {
public:
    using value_type = BasicJsonType;

    json_ref(value_type&& value)
        : owned_value(std::move(value)) {}

    json_ref(const value_type& value)
        : value_ref(&value) {}

    json_ref(std::initializer_list<json_ref> init)
        : owned_value(init) {}

    template <class... Args,
              std::enable_if_t<
                  std::is_constructible<value_type, Args...>::value &&
                      !(sizeof...(Args) == 1 &&
                        std::disjunction<std::is_same<std::decay_t<Args>, json_ref>...>::value),
                  int> = 0>
    json_ref(Args&&... args)
        : owned_value(std::forward<Args>(args)...) {}

    json_ref(json_ref&&) noexcept = default;
    json_ref(const json_ref&) = delete;
    json_ref& operator=(const json_ref&) = delete;
    json_ref& operator=(json_ref&&) = delete;
    ~json_ref() = default;

    value_type moved_or_copied() const {
        if (value_ref == nullptr) {
            return std::move(owned_value);
        }
        return *value_ref;
    }

    value_type const& operator*() const {
        return value_ref ? *value_ref : owned_value;
    }

    value_type const* operator->() const {
        return &**this;
    }

private:
    mutable value_type owned_value = nullptr;
    value_type const* value_ref = nullptr;
};

template <typename IteratorType>
class iteration_proxy_value {
public:
    using difference_type = std::ptrdiff_t;
    using value_type = iteration_proxy_value;
    using pointer = value_type*;
    using reference = value_type&;
    using iterator_category = std::input_iterator_tag;
    using string_type = std::string;

private:
    IteratorType anchor{};
    std::size_t array_index = 0;
    mutable std::size_t array_index_last = 0;
    mutable string_type array_index_str = "0";
    string_type empty_str{};

public:
    explicit iteration_proxy_value() = default;
    explicit iteration_proxy_value(IteratorType it, std::size_t array_index_ = 0) noexcept
        : anchor(std::move(it)), array_index(array_index_) {}

    const iteration_proxy_value& operator*() const noexcept {
        return *this;
    }

    iteration_proxy_value& operator*() noexcept {
        return *this;
    }

    iteration_proxy_value& operator++() {
        ++anchor;
        ++array_index;
        return *this;
    }

    iteration_proxy_value operator++(int) {
        auto tmp = *this;
        ++(*this);
        return tmp;
    }

    bool operator==(const iteration_proxy_value& o) const {
        return anchor == o.anchor;
    }

    bool operator!=(const iteration_proxy_value& o) const {
        return anchor != o.anchor;
    }

    // Like the iterator's own accessors, these hand the container and the positions to static functions by value
    // instead of passing the address of this object (and of the iterator inside it): a loop over items() then needs
    // no memory copy of the loop's iterator (see the comment at iter_impl::element_at).
    const string_type& key() const {
        return key_at(anchor.m_object, anchor.idx_, array_index, array_index_last, array_index_str, empty_str);
    }

    typename IteratorType::reference value() const {
        return IteratorType::element_at(anchor.m_object, anchor.idx_);
    }

private:
    static const string_type& key_at(typename IteratorType::pointer object,
                                     typename IteratorType::difference_type idx,
                                     std::size_t index, std::size_t& index_last, string_type& index_str,
                                     const string_type& empty) {
        assert(object != nullptr);
        switch (object->type()) {
            case value_t::array: {
                if (index != index_last) {
                    index_str = std::to_string(index);
                    index_last = index;
                }
                return index_str;
            }
            case value_t::object:
                return IteratorType::key_at(object, idx);
            default:
                return empty;
        }
    }
};

template <typename IteratorType>
class iteration_proxy {
private:
    typename IteratorType::reference container;

public:
    explicit iteration_proxy() = delete;
    explicit iteration_proxy(typename IteratorType::reference cont) noexcept
        : container(cont) {}

    iteration_proxy_value<IteratorType> begin() const noexcept {
        return iteration_proxy_value<IteratorType>(container.begin());
    }

    iteration_proxy_value<IteratorType> end() const noexcept {
        return iteration_proxy_value<IteratorType>(container.end());
    }
};

template <typename BasicJsonType>
class iter_impl {
    using other_iter_impl = iter_impl<typename std::conditional<
        std::is_const<BasicJsonType>::value,
        typename std::remove_const<BasicJsonType>::type,
        const BasicJsonType>::type>;

    friend other_iter_impl;
    friend BasicJsonType;
    template <typename It>
    friend class iteration_proxy_value;

public:
    using value_type = typename BasicJsonType::value_type;
    using difference_type = typename BasicJsonType::difference_type;
    using pointer = typename std::conditional<std::is_const<BasicJsonType>::value,
                                              typename BasicJsonType::const_pointer,
                                              typename BasicJsonType::pointer>::type;
    using reference = typename std::conditional<std::is_const<BasicJsonType>::value,
                                                typename BasicJsonType::const_reference,
                                                typename BasicJsonType::reference>::type;
    using iterator_category = std::bidirectional_iterator_tag;

    iter_impl() = default;
    ~iter_impl() = default;
    iter_impl(const iter_impl&) noexcept = default;
    iter_impl& operator=(const iter_impl&) noexcept = default;

    explicit iter_impl(pointer object, difference_type idx = 0) noexcept
        : m_object(object), idx_(idx) {}

    template <typename U = BasicJsonType,
              std::enable_if_t<std::is_const<U>::value, int> = 0>
    iter_impl(const iter_impl<typename std::remove_const<BasicJsonType>::type>& other) noexcept
        : m_object(other.m_object), idx_(other.idx_) {}

    template <typename U = BasicJsonType,
              std::enable_if_t<std::is_const<U>::value, int> = 0>
    iter_impl& operator=(
        const iter_impl<typename std::remove_const<BasicJsonType>::type>& other) noexcept {
        m_object = other.m_object;
        idx_ = other.idx_;
        return *this;
    }

    reference operator*() const {
        return element_at(m_object, idx_);
    }

    pointer operator->() const {
        return std::addressof(element_at(m_object, idx_));
    }

    iter_impl operator++(int) {
        auto result = *this;
        ++(*this);
        return result;
    }

    iter_impl& operator++() {
        if (m_object != nullptr) {
            ++idx_;
        }
        return *this;
    }

    iter_impl operator--(int) {
        auto result = *this;
        --(*this);
        return result;
    }

    iter_impl& operator--() {
        if (m_object != nullptr) {
            --idx_;
        }
        return *this;
    }

    template <typename IterImpl,
              std::enable_if_t<std::is_same<IterImpl, iter_impl>::value ||
                                   std::is_same<IterImpl, other_iter_impl>::value,
                               int> = 0>
    bool operator==(const IterImpl& other) const {
        if (m_object != other.m_object) {
            throw invalid_iterator::create(212, "cannot compare iterators of different containers");
        }
        return idx_ == other.idx_;
    }

    template <typename IterImpl,
              std::enable_if_t<std::is_same<IterImpl, iter_impl>::value ||
                                   std::is_same<IterImpl, other_iter_impl>::value,
                               int> = 0>
    bool operator!=(const IterImpl& other) const {
        return !operator==(other);
    }

    template <typename IterImpl,
              std::enable_if_t<std::is_same<IterImpl, iter_impl>::value ||
                                   std::is_same<IterImpl, other_iter_impl>::value,
                               int> = 0>
    bool operator<(const IterImpl& other) const {
        if (m_object != other.m_object) {
            throw invalid_iterator::create(212, "cannot compare iterators of different containers");
        }
        if (m_object != nullptr && m_object->m_type == value_t::object) {
            throw invalid_iterator::create(213, "cannot compare order of object iterators");
        }
        return idx_ < other.idx_;
    }

    template <typename IterImpl,
              std::enable_if_t<std::is_same<IterImpl, iter_impl>::value ||
                                   std::is_same<IterImpl, other_iter_impl>::value,
                               int> = 0>
    bool operator<=(const IterImpl& other) const {
        return !other.operator<(*this);
    }

    template <typename IterImpl,
              std::enable_if_t<std::is_same<IterImpl, iter_impl>::value ||
                                   std::is_same<IterImpl, other_iter_impl>::value,
                               int> = 0>
    bool operator>(const IterImpl& other) const {
        return !operator<=(other);
    }

    template <typename IterImpl,
              std::enable_if_t<std::is_same<IterImpl, iter_impl>::value ||
                                   std::is_same<IterImpl, other_iter_impl>::value,
                               int> = 0>
    bool operator>=(const IterImpl& other) const {
        return !operator<(other);
    }

    iter_impl& operator+=(difference_type i) {
        if (m_object == nullptr) {
            return *this;
        }
        if (m_object->m_type == value_t::object) {
            throw invalid_iterator::create(209, "cannot use offsets with object iterators");
        }
        idx_ += i;
        return *this;
    }

    iter_impl& operator-=(difference_type i) {
        return operator+=(-i);
    }

    iter_impl operator+(difference_type i) const {
        auto result = *this;
        result += i;
        return result;
    }

    friend iter_impl operator+(difference_type i, const iter_impl& it) {
        auto result = it;
        result += i;
        return result;
    }

    iter_impl operator-(difference_type i) const {
        auto result = *this;
        result -= i;
        return result;
    }

    template <typename IterImpl,
              std::enable_if_t<std::is_same<IterImpl, iter_impl>::value ||
                                   std::is_same<IterImpl, other_iter_impl>::value,
                               int> = 0>
    difference_type operator-(const IterImpl& other) const {
        if (m_object != other.m_object) {
            throw invalid_iterator::create(209, "cannot use offsets with object iterators");
        }
        if (m_object == nullptr || m_object->m_type == value_t::object) {
            throw invalid_iterator::create(209, "cannot use offsets with object iterators");
        }
        return idx_ - other.idx_;
    }

    reference operator[](difference_type n) const {
        if (m_object == nullptr) {
            throw invalid_iterator::create(208, "cannot use operator[] for object iterators");
        }
        if (m_object->m_type == value_t::object) {
            throw invalid_iterator::create(208, "cannot use operator[] for object iterators");
        }
        return *(*this + n);
    }

    const typename BasicJsonType::object_t::key_type& key() const {
        return key_at(m_object, idx_);
    }

    reference value() const {
        return element_at(m_object, idx_);
    }

private:
    // The checked accessors take the container and the index by value, not the iterator. Written as member functions
    // that the optimiser did not inline, they received the address of the loop's iterator, which then lived both in a
    // register and in memory; MSVC 19.52 for arm64 at /O2 stored the index from before ++ into the memory copy, so in
    // `for (auto it = j.begin(); it != j.end(); ++it)` each value() after the first returned the previous element
    // while key() returned the right one. Passed by value, the iterator need not be in memory at all.
    static reference element_at(pointer object, difference_type idx) {
        if (object == nullptr) {
            throw invalid_iterator::create(214, "cannot get value");
        }
        switch (object->m_type) {
            case value_t::object: {
                if (idx < 0 || static_cast<std::size_t>(idx) >= object->m_value.object->size()) {
                    throw invalid_iterator::create(214, "cannot get value");
                }
                return (*object->m_value.object)[static_cast<std::size_t>(idx)].second;
            }
            case value_t::array: {
                if (idx < 0 || static_cast<std::size_t>(idx) >= object->m_value.array->size()) {
                    throw invalid_iterator::create(214, "cannot get value");
                }
                return (*object->m_value.array)[static_cast<std::size_t>(idx)];
            }
            case value_t::null:
                throw invalid_iterator::create(214, "cannot get value");
            default:
                if (idx == 0) {
                    return *object;
                }
                throw invalid_iterator::create(214, "cannot get value");
        }
    }

    static const typename BasicJsonType::object_t::key_type& key_at(pointer object, difference_type idx) {
        if (object == nullptr || object->m_type != value_t::object) {
            throw invalid_iterator::create(207, "cannot use key() for non-object iterators");
        }
        if (idx < 0 || static_cast<std::size_t>(idx) >= object->m_value.object->size()) {
            throw invalid_iterator::create(214, "cannot get value");
        }
        return (*object->m_value.object)[static_cast<std::size_t>(idx)].first;
    }

    pointer m_object = nullptr;
    difference_type idx_ = 0;
};

template <typename Base>
class json_reverse_iterator : public std::reverse_iterator<Base> {
public:
    using difference_type = std::ptrdiff_t;
    using base_iterator = std::reverse_iterator<Base>;
    using reference = typename Base::reference;

    explicit json_reverse_iterator(const typename base_iterator::iterator_type& it) noexcept
        : base_iterator(it) {}

    explicit json_reverse_iterator(const base_iterator& it) noexcept
        : base_iterator(it) {}

    json_reverse_iterator operator++(int) {
        return static_cast<json_reverse_iterator>(base_iterator::operator++(0));
    }

    json_reverse_iterator& operator++() {
        return static_cast<json_reverse_iterator&>(base_iterator::operator++());
    }

    json_reverse_iterator operator--(int) {
        return static_cast<json_reverse_iterator>(base_iterator::operator--(0));
    }

    json_reverse_iterator& operator--() {
        return static_cast<json_reverse_iterator&>(base_iterator::operator--());
    }

    json_reverse_iterator& operator+=(difference_type i) {
        return static_cast<json_reverse_iterator&>(base_iterator::operator+=(i));
    }

    json_reverse_iterator operator+(difference_type i) const {
        return static_cast<json_reverse_iterator>(base_iterator::operator+(i));
    }

    json_reverse_iterator operator-(difference_type i) const {
        return static_cast<json_reverse_iterator>(base_iterator::operator-(i));
    }

    difference_type operator-(const json_reverse_iterator& other) const {
        return base_iterator(*this) - base_iterator(other);
    }

    reference operator[](difference_type n) const {
        return *(this->operator+(n));
    }

    auto key() const -> decltype(std::declval<Base>().key()) {
        auto it = --this->base();
        return it.key();
    }

    reference value() const {
        auto it = --this->base();
        return it.operator*();
    }
};

enum class parse_event_t : std::uint8_t {
    object_start,
    object_end,
    array_start,
    array_end,
    key,
    value
};

template <typename BasicJsonType>
using parser_callback_t =
    std::function<bool(int depth, parse_event_t event, BasicJsonType& parsed)>;

template <typename StringType>
void replace_substring(StringType& s, const StringType& f, const StringType& t) {
    assert(!f.empty());
    for (auto pos = s.find(f); pos != StringType::npos;
         s.replace(pos, f.size(), t), pos = s.find(f, pos + t.size())) {}
}

template <typename StringType>
StringType escape(StringType s) {
    replace_substring(s, StringType{"~"}, StringType{"~0"});
    replace_substring(s, StringType{"/"}, StringType{"~1"});
    return s;
}

template <typename StringType>
void unescape(StringType& s) {
    replace_substring(s, StringType{"~1"}, StringType{"/"});
    replace_substring(s, StringType{"~0"}, StringType{"~"});
}

template <unsigned N>
struct priority_tag : priority_tag<N - 1> {};

template <>
struct priority_tag<0> {};

template <class T>
struct identity_tag {};

template <typename T>
using uncvref_t = std::remove_cv_t<std::remove_reference_t<T>>;

struct nonesuch {
    nonesuch() = delete;
    ~nonesuch() = delete;
    nonesuch(nonesuch const&) = delete;
    void operator=(nonesuch const&) = delete;
};

template <class Default, class AlwaysVoid, template <class...> class Op, class... Args>
struct detector {
    using value_t = std::false_type;
    using type = Default;
};

template <class Default, template <class...> class Op, class... Args>
struct detector<Default, std::void_t<Op<Args...>>, Op, Args...> {
    using value_t = std::true_type;
    using type = Op<Args...>;
};

template <template <class...> class Op, class... Args>
using is_detected = typename detector<nonesuch, void, Op, Args...>::value_t;

template <template <class...> class Op, class... Args>
using detected_t = typename detector<nonesuch, void, Op, Args...>::type;

template <class Expected, template <class...> class Op, class... Args>
using is_detected_exact = std::is_same<Expected, detected_t<Op, Args...>>;

using std::enable_if_t;
using std::conjunction;
using std::negation;

template <template <class...> class Op, class... Args>
struct is_detected_lazy : is_detected<Op, Args...> {};

template <typename T>
struct static_const {
    static constexpr T value{};
};

template <typename T>
constexpr T static_const<T>::value;

template <typename>
struct is_basic_json : std::false_type {};

template <>
struct is_basic_json<::neojson::json> : std::true_type {};

template <typename>
struct is_json_ref : std::false_type {};

template <typename T>
struct is_json_ref<json_ref<T>> : std::true_type {};

template <typename>
struct is_json_pointer : std::false_type {};

template <>
struct is_json_pointer<::neojson::json_pointer> : std::true_type {};

template <typename>
struct is_std_optional : std::false_type {};

template <typename T>
struct is_std_optional<std::optional<T>> : std::true_type {};

template <typename T>
using mapped_type_t = typename T::mapped_type;

template <typename T>
using key_type_t = typename T::key_type;

template <typename T>
using value_type_t = typename T::value_type;

template <typename T>
using difference_type_t = typename T::difference_type;

template <typename T>
using pointer_t = typename T::pointer;

template <typename T>
using reference_t = typename T::reference;

template <typename T>
using iterator_category_t = typename T::iterator_category;

template <typename T, typename... Args>
using to_json_function = decltype(T::to_json(std::declval<Args>()...));

template <typename T, typename... Args>
using from_json_function = decltype(T::from_json(std::declval<Args>()...));

template <typename T, typename U>
using get_template_function = decltype(std::declval<T>().template get<U>());

template <typename It, typename = void>
struct iterator_types {};

template <typename It>
struct iterator_types<
    It,
    std::void_t<typename It::difference_type, typename It::value_type, typename It::pointer,
                typename It::reference, typename It::iterator_category>> {
    using difference_type = typename It::difference_type;
    using value_type = typename It::value_type;
    using pointer = typename It::pointer;
    using reference = typename It::reference;
    using iterator_category = typename It::iterator_category;
};

template <typename T, typename = void>
struct safe_iterator_traits {};

template <typename T>
struct safe_iterator_traits<T, std::enable_if_t<!std::is_pointer<T>::value>> : iterator_types<T> {};

template <typename T>
struct safe_iterator_traits<T*, std::enable_if_t<std::is_object<T>::value>> {
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = T*;
    using reference = T&;
};

template <typename T, typename = void>
struct is_safe_iterator_traits : std::false_type {};

template <typename T>
struct is_safe_iterator_traits<safe_iterator_traits<T>> {
private:
    using traits = safe_iterator_traits<T>;

public:
    static constexpr auto value =
        is_detected<value_type_t, traits>::value &&
        is_detected<difference_type_t, traits>::value &&
        is_detected<pointer_t, traits>::value &&
        is_detected<iterator_category_t, traits>::value &&
        is_detected<reference_t, traits>::value;
};

namespace begin_detail {
using std::begin;
template <typename T>
using result_of_begin = decltype(begin(std::declval<T>()));
} // namespace begin_detail

namespace end_detail {
using std::end;
template <typename T>
using result_of_end = decltype(end(std::declval<T>()));
} // namespace end_detail

using begin_detail::result_of_begin;
using end_detail::result_of_end;

template <typename T>
struct is_range {
private:
    using t_ref = typename std::add_lvalue_reference<T>::type;
    using iterator = detected_t<result_of_begin, t_ref>;
    using sentinel = detected_t<result_of_end, t_ref>;
    static constexpr auto is_iterator_begin =
        is_safe_iterator_traits<safe_iterator_traits<iterator>>::value;

public:
    static constexpr bool value = !std::is_same<iterator, nonesuch>::value &&
                                  !std::is_same<sentinel, nonesuch>::value &&
                                  is_iterator_begin;
};

template <typename R>
using iterator_t = std::enable_if_t<is_range<R>::value, result_of_begin<decltype(std::declval<R&>())>>;

template <typename T>
using range_value_t = value_type_t<safe_iterator_traits<iterator_t<T>>>;

template <typename T, typename = void>
struct is_complete_type : std::false_type {};

template <typename T>
struct is_complete_type<T, decltype(void(sizeof(T)))> : std::true_type {};

template <typename BasicJsonType, typename T, typename = void>
struct has_from_json : std::false_type {};

template <typename BasicJsonType, typename T>
struct is_getable {
    static constexpr bool value = is_detected<get_template_function, const BasicJsonType&, T>::value;
};

template <typename BasicJsonType, typename T>
struct has_from_json<BasicJsonType, T, std::enable_if_t<!is_basic_json<T>::value>> {
    using serializer = typename BasicJsonType::template json_serializer<T, void>;
    static constexpr bool value =
        is_detected_exact<void, from_json_function, serializer, const BasicJsonType&, T&>::value;
};

template <typename BasicJsonType, typename T, typename = void>
struct has_non_default_from_json : std::false_type {};

template <typename BasicJsonType, typename T>
struct has_non_default_from_json<BasicJsonType, T, std::enable_if_t<!is_basic_json<T>::value>> {
    using serializer = typename BasicJsonType::template json_serializer<T, void>;
    static constexpr bool value =
        is_detected_exact<T, from_json_function, serializer, const BasicJsonType&>::value;
};

template <typename BasicJsonType, typename T, typename = void>
struct has_to_json : std::false_type {};

template <typename BasicJsonType, typename T>
struct has_to_json<BasicJsonType, T, std::enable_if_t<!is_basic_json<T>::value>> {
    using serializer = typename BasicJsonType::template json_serializer<T, void>;
    static constexpr bool value =
        is_detected_exact<void, to_json_function, serializer, BasicJsonType&, T>::value;
};

template <typename T>
struct is_default_constructible : std::is_default_constructible<T> {};

template <typename T1, typename T2>
struct is_default_constructible<std::pair<T1, T2>>
    : std::conjunction<is_default_constructible<T1>, is_default_constructible<T2>> {};

template <typename T1, typename T2>
struct is_default_constructible<const std::pair<T1, T2>>
    : std::conjunction<is_default_constructible<T1>, is_default_constructible<T2>> {};

template <typename... Ts>
struct is_default_constructible<std::tuple<Ts...>>
    : std::conjunction<is_default_constructible<Ts>...> {};

template <typename... Ts>
struct is_default_constructible<const std::tuple<Ts...>>
    : std::conjunction<is_default_constructible<Ts>...> {};

template <typename T, typename... Args>
struct is_constructible : std::is_constructible<T, Args...> {};

template <typename T1, typename T2>
struct is_constructible<std::pair<T1, T2>> : is_default_constructible<std::pair<T1, T2>> {};

template <typename T1, typename T2>
struct is_constructible<const std::pair<T1, T2>> : is_default_constructible<const std::pair<T1, T2>> {};

template <typename... Ts>
struct is_constructible<std::tuple<Ts...>> : is_default_constructible<std::tuple<Ts...>> {};

template <typename... Ts>
struct is_constructible<const std::tuple<Ts...>> : is_default_constructible<const std::tuple<Ts...>> {};

template <typename BasicJsonType, typename CompatibleObjectType, typename = void>
struct is_compatible_object_type_impl : std::false_type {};

template <typename BasicJsonType, typename CompatibleObjectType>
struct is_compatible_object_type_impl<
    BasicJsonType, CompatibleObjectType,
    std::enable_if_t<is_detected<mapped_type_t, CompatibleObjectType>::value &&
                     is_detected<key_type_t, CompatibleObjectType>::value>> {
    using object_t = typename BasicJsonType::object_t;
    static constexpr bool value =
        is_constructible<typename object_t::key_type,
                         typename CompatibleObjectType::key_type>::value &&
        (std::is_same<typename object_t::mapped_type,
                      typename CompatibleObjectType::mapped_type>::value ||
         has_to_json<BasicJsonType, typename CompatibleObjectType::mapped_type>::value);
};

template <typename BasicJsonType, typename CompatibleObjectType>
struct is_compatible_object_type
    : is_compatible_object_type_impl<BasicJsonType, CompatibleObjectType> {};

template <typename BasicJsonType, typename ConstructibleObjectType, typename = void>
struct is_constructible_object_type_impl : std::false_type {};

template <typename BasicJsonType, typename ConstructibleObjectType>
struct is_constructible_object_type_impl<
    BasicJsonType, ConstructibleObjectType,
    std::enable_if_t<is_detected<mapped_type_t, ConstructibleObjectType>::value &&
                     is_detected<key_type_t, ConstructibleObjectType>::value>> {
    using object_t = typename BasicJsonType::object_t;
    static constexpr bool value =
        (is_constructible<typename ConstructibleObjectType::key_type,
                          typename object_t::key_type>::value &&
         (std::is_same<typename object_t::mapped_type,
                       typename ConstructibleObjectType::mapped_type>::value ||
          has_from_json<BasicJsonType, typename ConstructibleObjectType::mapped_type>::value ||
          has_non_default_from_json<BasicJsonType,
                                    typename ConstructibleObjectType::mapped_type>::value));
};

template <typename BasicJsonType, typename ConstructibleObjectType>
struct is_constructible_object_type
    : is_constructible_object_type_impl<BasicJsonType, ConstructibleObjectType> {};

template <typename BasicJsonType, typename CompatibleStringType>
struct is_compatible_string_type {
    static constexpr auto value =
        is_constructible<typename BasicJsonType::string_t, CompatibleStringType>::value;
};

template <typename BasicJsonType, typename ConstructibleStringType>
struct is_constructible_string_type {
    static constexpr auto value =
        is_constructible<ConstructibleStringType, typename BasicJsonType::string_t>::value &&
        is_detected_exact<typename BasicJsonType::string_t::value_type, value_type_t,
                          ConstructibleStringType>::value;
};

template <typename BasicJsonType, typename CompatibleArrayType, typename = void>
struct is_compatible_array_type_impl : std::false_type {};

template <typename BasicJsonType, typename CompatibleArrayType>
struct is_compatible_array_type_impl<
    BasicJsonType, CompatibleArrayType,
    std::enable_if_t<
        is_detected<iterator_t, CompatibleArrayType>::value &&
        is_safe_iterator_traits<safe_iterator_traits<detected_t<iterator_t, CompatibleArrayType>>>::value &&
        !std::is_same<CompatibleArrayType, detected_t<range_value_t, CompatibleArrayType>>::value>> {
    using elem_t = range_value_t<CompatibleArrayType>;
    static constexpr bool value =
        std::is_same<uncvref_t<elem_t>, BasicJsonType>::value ||
        has_to_json<BasicJsonType, elem_t>::value;
};

template <typename BasicJsonType, typename CompatibleArrayType>
struct is_compatible_array_type
    : is_compatible_array_type_impl<BasicJsonType, CompatibleArrayType> {};

template <typename BasicJsonType, typename ConstructibleArrayType, typename = void>
struct is_constructible_array_type_impl : std::false_type {};

template <typename BasicJsonType, typename ConstructibleArrayType>
struct is_constructible_array_type_impl<
    BasicJsonType, ConstructibleArrayType,
    std::enable_if_t<std::is_same<ConstructibleArrayType, typename BasicJsonType::value_type>::value>>
    : std::true_type {};

template <typename BasicJsonType, typename ConstructibleArrayType>
struct is_constructible_array_type_impl<
    BasicJsonType, ConstructibleArrayType,
    std::enable_if_t<
        !std::is_same<ConstructibleArrayType, typename BasicJsonType::value_type>::value &&
        !is_compatible_string_type<BasicJsonType, ConstructibleArrayType>::value &&
        is_default_constructible<ConstructibleArrayType>::value &&
        (std::is_move_assignable<ConstructibleArrayType>::value ||
         std::is_copy_assignable<ConstructibleArrayType>::value) &&
        is_detected<iterator_t, ConstructibleArrayType>::value &&
        is_safe_iterator_traits<safe_iterator_traits<detected_t<iterator_t, ConstructibleArrayType>>>::value &&
        is_detected<range_value_t, ConstructibleArrayType>::value &&
        !std::is_same<ConstructibleArrayType, detected_t<range_value_t, ConstructibleArrayType>>::value &&
        is_complete_type<detected_t<range_value_t, ConstructibleArrayType>>::value>> {
    using value_type = range_value_t<ConstructibleArrayType>;
    static constexpr bool value =
        std::is_same<value_type, typename BasicJsonType::array_t::value_type>::value ||
        has_from_json<BasicJsonType, value_type>::value ||
        has_non_default_from_json<BasicJsonType, value_type>::value;
};

template <typename BasicJsonType, typename ConstructibleArrayType>
struct is_constructible_array_type
    : is_constructible_array_type_impl<BasicJsonType, ConstructibleArrayType> {};

template <typename RealIntegerType, typename CompatibleNumberIntegerType, typename = void>
struct is_compatible_integer_type_impl : std::false_type {};

template <typename RealIntegerType, typename CompatibleNumberIntegerType>
struct is_compatible_integer_type_impl<
    RealIntegerType, CompatibleNumberIntegerType,
    std::enable_if_t<std::is_integral<RealIntegerType>::value &&
                     std::is_integral<CompatibleNumberIntegerType>::value &&
                     !std::is_same<bool, CompatibleNumberIntegerType>::value>> {
    using RealLimits = std::numeric_limits<RealIntegerType>;
    using CompatibleLimits = std::numeric_limits<CompatibleNumberIntegerType>;
    static constexpr auto value =
        is_constructible<RealIntegerType, CompatibleNumberIntegerType>::value &&
        CompatibleLimits::is_integer &&
        RealLimits::is_signed == CompatibleLimits::is_signed;
};

template <typename RealIntegerType, typename CompatibleNumberIntegerType>
struct is_compatible_integer_type
    : is_compatible_integer_type_impl<RealIntegerType, CompatibleNumberIntegerType> {};

template <typename BasicJsonType, typename CompatibleType, typename = void>
struct is_compatible_type_impl : std::false_type {};

template <typename BasicJsonType, typename CompatibleType>
struct is_compatible_type_impl<
    BasicJsonType, CompatibleType,
    std::enable_if_t<is_complete_type<CompatibleType>::value>> {
    static constexpr bool value = has_to_json<BasicJsonType, CompatibleType>::value;
};

template <typename BasicJsonType, typename CompatibleType>
struct is_compatible_type : is_compatible_type_impl<BasicJsonType, CompatibleType> {};

template <typename T1, typename T2>
struct is_constructible_tuple : std::false_type {};

template <typename T1, typename... Args>
struct is_constructible_tuple<T1, std::tuple<Args...>>
    : std::conjunction<is_constructible<T1, Args>...> {};

template <value_t>
struct external_constructor;

template <>
struct external_constructor<value_t::boolean> {
    template <typename BasicJsonType>
    static void construct(BasicJsonType& j, typename BasicJsonType::boolean_t b) noexcept {
        j.m_value.destroy(j.m_type);
        j.m_type = value_t::boolean;
        j.m_value.boolean = b;
    }
};

template <>
struct external_constructor<value_t::string> {
    template <typename BasicJsonType>
    static void construct(BasicJsonType& j, const typename BasicJsonType::string_t& s) {
        j.m_value.destroy(j.m_type);
        j.m_value.string = BasicJsonType::create_string(s);
        j.m_type = value_t::string;
    }

    template <typename BasicJsonType>
    static void construct(BasicJsonType& j, typename BasicJsonType::string_t&& s) {
        j.m_value.destroy(j.m_type);
        j.m_value.string = BasicJsonType::create_string(std::move(s));
        j.m_type = value_t::string;
    }

    template <typename BasicJsonType, typename CompatibleStringType,
              enable_if_t<!std::is_same<CompatibleStringType,
                                        typename BasicJsonType::string_t>::value,
                          int> = 0>
    static void construct(BasicJsonType& j, const CompatibleStringType& str) {
        j.m_value.destroy(j.m_type);
        j.m_value.string = BasicJsonType::create_string(str);
        j.m_type = value_t::string;
    }
};

template <>
struct external_constructor<value_t::number_float> {
    template <typename BasicJsonType>
    static void construct(BasicJsonType& j, typename BasicJsonType::number_float_t val) noexcept {
        j.m_value.destroy(j.m_type);
        j.m_type = value_t::number_float;
        j.m_value.number_float = val;
    }
};

template <>
struct external_constructor<value_t::number_unsigned> {
    template <typename BasicJsonType>
    static void construct(BasicJsonType& j,
                          typename BasicJsonType::number_unsigned_t val) noexcept {
        j.m_value.destroy(j.m_type);
        j.m_type = value_t::number_unsigned;
        j.m_value.number_unsigned = val;
    }
};

template <>
struct external_constructor<value_t::number_integer> {
    template <typename BasicJsonType>
    static void construct(BasicJsonType& j,
                          typename BasicJsonType::number_integer_t val) noexcept {
        j.m_value.destroy(j.m_type);
        j.m_type = value_t::number_integer;
        j.m_value.number_integer = val;
    }
};

template <>
struct external_constructor<value_t::array> {
    template <typename BasicJsonType>
    static void construct(BasicJsonType& j, const typename BasicJsonType::array_t& arr) {
        j.m_value.destroy(j.m_type);
        j.m_value.array = BasicJsonType::create_array(arr);
        j.m_type = value_t::array;
    }

    template <typename BasicJsonType>
    static void construct(BasicJsonType& j, typename BasicJsonType::array_t&& arr) {
        j.m_value.destroy(j.m_type);
        j.m_value.array = BasicJsonType::create_array(std::move(arr));
        j.m_type = value_t::array;
    }

    template <typename BasicJsonType, typename CompatibleArrayType,
              enable_if_t<!std::is_same<CompatibleArrayType,
                                        typename BasicJsonType::array_t>::value,
                          int> = 0>
    static void construct(BasicJsonType& j, const CompatibleArrayType& arr) {
        using std::begin;
        using std::end;
        j.m_value.destroy(j.m_type);
        j.m_value.array = BasicJsonType::create_array(begin(arr), end(arr));
        j.m_type = value_t::array;
    }

    template <typename BasicJsonType>
    static void construct(BasicJsonType& j, const std::vector<bool>& b) {
        j.m_value.destroy(j.m_type);
        auto* arr = BasicJsonType::create_array();
        j.m_value.array = arr;
        j.m_type = value_t::array;
        arr->reserve(b.size());
        for (const bool x : b) {
            arr->emplace_back(x);
        }
    }

    template <typename BasicJsonType, typename T,
              enable_if_t<std::is_convertible<T, BasicJsonType>::value, int> = 0>
    static void construct(BasicJsonType& j, const std::valarray<T>& arr) {
        j.m_value.destroy(j.m_type);
        j.m_value.array = BasicJsonType::create_array(std::begin(arr), std::end(arr));
        j.m_type = value_t::array;
    }
};

template <>
struct external_constructor<value_t::object> {
    template <typename BasicJsonType>
    static void construct(BasicJsonType& j, const typename BasicJsonType::object_t& obj) {
        j.m_value.destroy(j.m_type);
        j.m_value.object = BasicJsonType::create_object(obj);
        j.m_type = value_t::object;
    }

    template <typename BasicJsonType>
    static void construct(BasicJsonType& j, typename BasicJsonType::object_t&& obj) {
        j.m_value.destroy(j.m_type);
        j.m_value.object = BasicJsonType::create_object(std::move(obj));
        j.m_type = value_t::object;
    }

    template <typename BasicJsonType, typename CompatibleObjectType,
              enable_if_t<!std::is_same<CompatibleObjectType,
                                        typename BasicJsonType::object_t>::value,
                          int> = 0>
    static void construct(BasicJsonType& j, const CompatibleObjectType& obj) {
        using std::begin;
        using std::end;
        j.m_value.destroy(j.m_type);
        auto* o = BasicJsonType::create_object();
        j.m_value.object = o;
        j.m_type = value_t::object;
        for (auto it = begin(obj); it != end(obj); ++it) {
            o->emplace(it->first, it->second);
        }
    }
};

// ==================== detail::from_json ====================

template <typename BasicJsonType>
inline void from_json(const BasicJsonType& j, typename std::nullptr_t& n) {
    if (!j.is_null()) {
        throw type_error::create(302, std::string("type must be null, but is ") + j.type_name());
    }
    n = nullptr;
}

template <typename BasicJsonType, typename ArithmeticType,
          enable_if_t<std::is_arithmetic<ArithmeticType>::value &&
                          !std::is_same<ArithmeticType, typename BasicJsonType::boolean_t>::value,
                      int> = 0>
inline void get_arithmetic_value(const BasicJsonType& j, ArithmeticType& val) {
    switch (static_cast<value_t>(j)) {
        case value_t::number_unsigned:
            val = static_cast<ArithmeticType>(
                *j.template get_ptr<const typename BasicJsonType::number_unsigned_t*>());
            break;
        case value_t::number_integer:
            val = static_cast<ArithmeticType>(
                *j.template get_ptr<const typename BasicJsonType::number_integer_t*>());
            break;
        case value_t::number_float:
            val = static_cast<ArithmeticType>(
                *j.template get_ptr<const typename BasicJsonType::number_float_t*>());
            break;
        default:
            throw type_error::create(
                302, std::string("type must be number, but is ") + j.type_name());
    }
}

template <typename BasicJsonType>
inline void from_json(const BasicJsonType& j, typename BasicJsonType::boolean_t& b) {
    if (!j.is_boolean()) {
        throw type_error::create(
            302, std::string("type must be boolean, but is ") + j.type_name());
    }
    b = *j.template get_ptr<const typename BasicJsonType::boolean_t*>();
}

template <typename BasicJsonType>
inline void from_json(const BasicJsonType& j, typename BasicJsonType::string_t& s) {
    if (!j.is_string()) {
        throw type_error::create(
            302, std::string("type must be string, but is ") + j.type_name());
    }
    s = *j.template get_ptr<const typename BasicJsonType::string_t*>();
}

template <typename BasicJsonType, typename StringType,
          enable_if_t<
              std::is_assignable<StringType&, const typename BasicJsonType::string_t>::value &&
                  !std::is_same<typename BasicJsonType::string_t, StringType>::value &&
                  !is_json_ref<StringType>::value && !is_std_optional<StringType>::value,
              int> = 0>
inline void from_json(const BasicJsonType& j, StringType& s) {
    if (!j.is_string()) {
        throw type_error::create(
            302, std::string("type must be string, but is ") + j.type_name());
    }
    s = *j.template get_ptr<const typename BasicJsonType::string_t*>();
}

template <typename BasicJsonType>
inline void from_json(const BasicJsonType& j, typename BasicJsonType::number_float_t& val) {
    get_arithmetic_value(j, val);
}

template <typename BasicJsonType>
inline void from_json(const BasicJsonType& j, typename BasicJsonType::number_unsigned_t& val) {
    get_arithmetic_value(j, val);
}

template <typename BasicJsonType>
inline void from_json(const BasicJsonType& j, typename BasicJsonType::number_integer_t& val) {
    get_arithmetic_value(j, val);
}

template <typename BasicJsonType, typename ArithmeticType,
          enable_if_t<
              std::is_arithmetic<ArithmeticType>::value &&
                  !std::is_same<ArithmeticType, typename BasicJsonType::number_unsigned_t>::value &&
                  !std::is_same<ArithmeticType, typename BasicJsonType::number_integer_t>::value &&
                  !std::is_same<ArithmeticType, typename BasicJsonType::number_float_t>::value &&
                  !std::is_same<ArithmeticType, typename BasicJsonType::boolean_t>::value,
              int> = 0>
inline void from_json(const BasicJsonType& j, ArithmeticType& val) {
    switch (static_cast<value_t>(j)) {
        case value_t::number_unsigned: {
            val = static_cast<ArithmeticType>(
                *j.template get_ptr<const typename BasicJsonType::number_unsigned_t*>());
            break;
        }
        case value_t::number_integer: {
            val = static_cast<ArithmeticType>(
                *j.template get_ptr<const typename BasicJsonType::number_integer_t*>());
            break;
        }
        case value_t::number_float: {
            val = static_cast<ArithmeticType>(
                *j.template get_ptr<const typename BasicJsonType::number_float_t*>());
            break;
        }
        case value_t::boolean: {
            val = static_cast<ArithmeticType>(
                *j.template get_ptr<const typename BasicJsonType::boolean_t*>());
            break;
        }
        case value_t::null:
        case value_t::object:
        case value_t::array:
        case value_t::string:
        case value_t::binary:
        case value_t::discarded:
        default:
            throw type_error::create(
                302, std::string("type must be number, but is ") + j.type_name());
    }
}

template <typename BasicJsonType, typename EnumType,
          enable_if_t<std::is_enum<EnumType>::value, int> = 0>
inline void from_json(const BasicJsonType& j, EnumType& e) {
    typename std::underlying_type<EnumType>::type val;
    get_arithmetic_value(j, val);
    e = static_cast<EnumType>(val);
}

template <typename BasicJsonType, typename T, typename Allocator,
          enable_if_t<is_getable<BasicJsonType, T>::value, int> = 0>
inline void from_json_forward_list(const BasicJsonType& j, std::forward_list<T, Allocator>& l) {
    if (!j.is_array()) {
        throw type_error::create(302, std::string("type must be array, but is ") + j.type_name());
    }
    l.clear();
    for (auto it = j.rbegin(), end = j.rend(); it != end; ++it) {
        l.push_front(it->template get<T>());
    }
}

template <typename BasicJsonType, typename T,
          enable_if_t<is_getable<BasicJsonType, T>::value, int> = 0>
inline void from_json_valarray(const BasicJsonType& j, std::valarray<T>& l) {
    if (!j.is_array()) {
        throw type_error::create(302, std::string("type must be array, but is ") + j.type_name());
    }
    l.resize(j.size());
    std::size_t i = 0;
    for (const auto& elem : j) {
        l[i++] = elem.template get<T>();
    }
}

template <typename BasicJsonType, typename T, std::size_t N>
auto from_json(const BasicJsonType& j, T (&arr)[N])
    -> decltype(j.template get<T>(), void()) {
    for (std::size_t i = 0; i < N; ++i) {
        arr[i] = j.at(i).template get<T>();
    }
}

template <typename BasicJsonType>
inline void from_json_array_impl(const BasicJsonType& j, typename BasicJsonType::array_t& arr,
                                 priority_tag<3> /*unused*/) {
    arr = *j.template get_ptr<const typename BasicJsonType::array_t*>();
}

template <typename BasicJsonType, typename T, std::size_t N>
auto from_json_array_impl(const BasicJsonType& j, std::array<T, N>& arr,
                          priority_tag<2> /*unused*/)
    -> decltype(j.template get<T>(), void()) {
    for (std::size_t i = 0; i < N; ++i) {
        arr[i] = j.at(i).template get<T>();
    }
}

template <typename BasicJsonType, typename ConstructibleArrayType,
          enable_if_t<std::is_assignable<ConstructibleArrayType&, ConstructibleArrayType>::value,
                      int> = 0>
auto from_json_array_impl(const BasicJsonType& j, ConstructibleArrayType& arr,
                          priority_tag<1> /*unused*/)
    -> decltype(arr.reserve(std::declval<typename ConstructibleArrayType::size_type>()),
                j.template get<typename ConstructibleArrayType::value_type>(), void()) {
    using std::end;
    ConstructibleArrayType ret;
    ret.reserve(j.size());
    std::transform(j.begin(), j.end(), std::inserter(ret, end(ret)),
                   [](const BasicJsonType& i) {
                       return i.template get<typename ConstructibleArrayType::value_type>();
                   });
    arr = std::move(ret);
}

template <typename BasicJsonType, typename ConstructibleArrayType,
          enable_if_t<std::is_assignable<ConstructibleArrayType&, ConstructibleArrayType>::value,
                      int> = 0>
inline void from_json_array_impl(const BasicJsonType& j, ConstructibleArrayType& arr,
                                 priority_tag<0> /*unused*/) {
    using std::end;
    ConstructibleArrayType ret;
    std::transform(j.begin(), j.end(), std::inserter(ret, end(ret)),
                   [](const BasicJsonType& i) {
                       return i.template get<typename ConstructibleArrayType::value_type>();
                   });
    arr = std::move(ret);
}

template <typename BasicJsonType, typename ConstructibleArrayType,
          enable_if_t<
              is_constructible_array_type<BasicJsonType, ConstructibleArrayType>::value &&
                  !is_constructible_object_type<BasicJsonType, ConstructibleArrayType>::value &&
                  !is_constructible_string_type<BasicJsonType, ConstructibleArrayType>::value &&
                  !is_basic_json<ConstructibleArrayType>::value,
              int> = 0>
inline void from_json(const BasicJsonType& j, ConstructibleArrayType& arr) {
    if (!j.is_array()) {
        throw type_error::create(302, std::string("type must be array, but is ") + j.type_name());
    }
    from_json_array_impl(j, arr, priority_tag<3>{});
}

template <typename BasicJsonType, typename T, typename Allocator,
          enable_if_t<is_getable<BasicJsonType, T>::value, int> = 0>
inline void from_json(const BasicJsonType& j, std::forward_list<T, Allocator>& l) {
    from_json_forward_list(j, l);
}

template <typename BasicJsonType, typename T,
          enable_if_t<is_getable<BasicJsonType, T>::value, int> = 0>
inline void from_json(const BasicJsonType& j, std::valarray<T>& l) {
    from_json_valarray(j, l);
}

template <typename BasicJsonType, typename ConstructibleObjectType,
          enable_if_t<is_constructible_object_type<BasicJsonType, ConstructibleObjectType>::value,
                      int> = 0>
inline void from_json(const BasicJsonType& j, ConstructibleObjectType& obj) {
    if (!j.is_object()) {
        throw type_error::create(302, std::string("type must be object, but is ") + j.type_name());
    }
    ConstructibleObjectType ret;
    const auto* inner_object = j.template get_ptr<const typename BasicJsonType::object_t*>();
    using value_type = typename ConstructibleObjectType::value_type;
    for (const auto& p : *inner_object) {
        ret.emplace(
            typename ConstructibleObjectType::key_type(p.first),
            p.second.template get<typename value_type::second_type>());
    }
    obj = std::move(ret);
}

template <typename BasicJsonType, typename Key, typename Value, typename Compare,
          typename Allocator,
          typename = enable_if_t<
              !std::is_constructible<typename BasicJsonType::string_t, Key>::value &&
              is_getable<BasicJsonType, Key>::value && is_getable<BasicJsonType, Value>::value>>
inline void from_json(const BasicJsonType& j, std::map<Key, Value, Compare, Allocator>& m) {
    if (!j.is_array()) {
        throw type_error::create(302, std::string("type must be array, but is ") + j.type_name());
    }
    m.clear();
    for (const auto& p : j) {
        if (!p.is_array()) {
            throw type_error::create(302,
                                     std::string("type must be array, but is ") + p.type_name());
        }
        m.emplace(p.at(0).template get<Key>(), p.at(1).template get<Value>());
    }
}

template <typename BasicJsonType, typename Key, typename Value, typename Hash, typename KeyEqual,
          typename Allocator,
          typename = enable_if_t<
              !std::is_constructible<typename BasicJsonType::string_t, Key>::value &&
              is_getable<BasicJsonType, Key>::value && is_getable<BasicJsonType, Value>::value>>
inline void from_json(const BasicJsonType& j,
                      std::unordered_map<Key, Value, Hash, KeyEqual, Allocator>& m) {
    if (!j.is_array()) {
        throw type_error::create(302, std::string("type must be array, but is ") + j.type_name());
    }
    m.clear();
    for (const auto& p : j) {
        if (!p.is_array()) {
            throw type_error::create(302,
                                     std::string("type must be array, but is ") + p.type_name());
        }
        m.emplace(p.at(0).template get<Key>(), p.at(1).template get<Value>());
    }
}

template <typename BasicJsonType, typename A1, typename A2,
          enable_if_t<is_getable<BasicJsonType, A1>::value &&
                          is_getable<BasicJsonType, A2>::value,
                      int> = 0>
inline void from_json(const BasicJsonType& j, std::pair<A1, A2>& p) {
    if (!j.is_array()) {
        throw type_error::create(302, std::string("type must be array, but is ") + j.type_name());
    }
    p = {j.at(0).template get<A1>(), j.at(1).template get<A2>()};
}

template <typename BasicJsonType, typename... Args, std::size_t... Idx>
inline std::tuple<Args...> from_json_tuple_impl_base(const BasicJsonType& j,
                                                     std::index_sequence<Idx...> /*unused*/) {
    return std::make_tuple(j.at(Idx).template get<Args>()...);
}

template <typename BasicJsonType>
inline std::tuple<> from_json_tuple_impl_base(const BasicJsonType& /*unused*/,
                                              std::index_sequence<> /*unused*/) {
    return {};
}

template <typename BasicJsonType, class A1, class A2>
inline std::pair<A1, A2> from_json_tuple_impl(const BasicJsonType& j,
                                              identity_tag<std::pair<A1, A2>> /*unused*/,
                                              priority_tag<0> /*unused*/) {
    return {j.at(0).template get<A1>(), j.at(1).template get<A2>()};
}

template <typename BasicJsonType, typename A1, typename A2>
inline void from_json_tuple_impl(const BasicJsonType& j, std::pair<A1, A2>& p,
                                 priority_tag<1> /*unused*/) {
    p = from_json_tuple_impl(j, identity_tag<std::pair<A1, A2>>{}, priority_tag<0>{});
}

template <typename BasicJsonType, typename... Args>
inline std::tuple<Args...> from_json_tuple_impl(const BasicJsonType& j,
                                                identity_tag<std::tuple<Args...>> /*unused*/,
                                                priority_tag<2> /*unused*/) {
    return from_json_tuple_impl_base<BasicJsonType, Args...>(j, std::index_sequence_for<Args...>{});
}

template <typename BasicJsonType, typename... Args>
inline void from_json_tuple_impl(const BasicJsonType& j, std::tuple<Args...>& t,
                                 priority_tag<3> /*unused*/) {
    t = from_json_tuple_impl_base<BasicJsonType, Args...>(j, std::index_sequence_for<Args...>{});
}

template <typename BasicJsonType, typename... Args>
inline void from_json(const BasicJsonType& j, std::tuple<Args...>& t) {
    if (!j.is_array()) {
        throw type_error::create(302, std::string("type must be array, but is ") + j.type_name());
    }
    from_json_tuple_impl(j, t, priority_tag<3>{});
}

template <typename BasicJsonType, typename Ret,
          enable_if_t<is_constructible_tuple<BasicJsonType, Ret>::value, int> = 0>
inline Ret from_json(const BasicJsonType& j, identity_tag<Ret> tag) {
    if (!j.is_array()) {
        throw type_error::create(302, std::string("type must be array, but is ") + j.type_name());
    }
    return from_json_tuple_impl(j, tag, priority_tag<2>{});
}

template <typename BasicJsonType, typename T,
          enable_if_t<is_getable<BasicJsonType, T>::value, int> = 0>
inline void from_json(const BasicJsonType& j, std::optional<T>& opt) {
    if (j.is_null()) {
        opt = std::nullopt;
    } else {
        opt.emplace(j.template get<T>());
    }
}

// ==================== detail::to_json ====================

template <typename BasicJsonType, typename T,
          enable_if_t<std::is_same<T, typename BasicJsonType::boolean_t>::value, int> = 0>
inline void to_json(BasicJsonType& j, T b) noexcept {
    external_constructor<value_t::boolean>::construct(j, b);
}

template <typename BasicJsonType, typename BoolRef,
          enable_if_t<
              ((std::is_same<std::vector<bool>::reference, BoolRef>::value &&
                !std::is_same<std::vector<bool>::reference,
                              typename BasicJsonType::boolean_t&>::value) ||
               (std::is_same<std::vector<bool>::const_reference, BoolRef>::value &&
                !std::is_same<uncvref_t<std::vector<bool>::const_reference>,
                              typename BasicJsonType::boolean_t>::value)) &&
                  std::is_convertible<const BoolRef&, typename BasicJsonType::boolean_t>::value,
              int> = 0>
inline void to_json(BasicJsonType& j, const BoolRef& b) noexcept {
    external_constructor<value_t::boolean>::construct(
        j, static_cast<typename BasicJsonType::boolean_t>(b));
}

template <typename BasicJsonType, typename CompatibleString,
          enable_if_t<std::is_constructible<typename BasicJsonType::string_t,
                                            CompatibleString>::value,
                      int> = 0>
inline void to_json(BasicJsonType& j, const CompatibleString& s) {
    external_constructor<value_t::string>::construct(j, s);
}

template <typename BasicJsonType>
inline void to_json(BasicJsonType& j, typename BasicJsonType::string_t&& s) {
    external_constructor<value_t::string>::construct(j, std::move(s));
}

template <typename BasicJsonType, typename FloatType,
          enable_if_t<std::is_floating_point<FloatType>::value, int> = 0>
inline void to_json(BasicJsonType& j, FloatType val) noexcept {
    external_constructor<value_t::number_float>::construct(
        j, static_cast<typename BasicJsonType::number_float_t>(val));
}

template <typename BasicJsonType, typename CompatibleNumberUnsignedType,
          enable_if_t<is_compatible_integer_type<typename BasicJsonType::number_unsigned_t,
                                                 CompatibleNumberUnsignedType>::value,
                      int> = 0>
inline void to_json(BasicJsonType& j, CompatibleNumberUnsignedType val) noexcept {
    external_constructor<value_t::number_unsigned>::construct(
        j, static_cast<typename BasicJsonType::number_unsigned_t>(val));
}

template <typename BasicJsonType, typename CompatibleNumberIntegerType,
          enable_if_t<is_compatible_integer_type<typename BasicJsonType::number_integer_t,
                                                 CompatibleNumberIntegerType>::value,
                      int> = 0>
inline void to_json(BasicJsonType& j, CompatibleNumberIntegerType val) noexcept {
    external_constructor<value_t::number_integer>::construct(
        j, static_cast<typename BasicJsonType::number_integer_t>(val));
}

template <typename BasicJsonType, typename EnumType,
          enable_if_t<std::is_enum<EnumType>::value, int> = 0>
inline void to_json(BasicJsonType& j, EnumType e) noexcept {
    using underlying_type = typename std::underlying_type<EnumType>::type;
    static constexpr value_t vt =
        std::is_unsigned<underlying_type>::value ? value_t::number_integer : value_t::number_integer;
    external_constructor<vt>::construct(
        j, static_cast<typename BasicJsonType::number_integer_t>(static_cast<underlying_type>(e)));
}

template <typename BasicJsonType>
inline void to_json(BasicJsonType& j, const std::vector<bool>& e) {
    external_constructor<value_t::array>::construct(j, e);
}

template <typename BasicJsonType, typename CompatibleArrayType,
          enable_if_t<
              is_compatible_array_type<BasicJsonType, CompatibleArrayType>::value &&
                  !is_compatible_object_type<BasicJsonType, CompatibleArrayType>::value &&
                  !is_compatible_string_type<BasicJsonType, CompatibleArrayType>::value &&
                  !is_basic_json<CompatibleArrayType>::value,
              int> = 0>
inline void to_json(BasicJsonType& j, const CompatibleArrayType& arr) {
    external_constructor<value_t::array>::construct(j, arr);
}

template <typename BasicJsonType, typename T,
          enable_if_t<std::is_convertible<T, BasicJsonType>::value, int> = 0>
inline void to_json(BasicJsonType& j, const std::valarray<T>& arr) {
    external_constructor<value_t::array>::construct(j, std::move(arr));
}

template <typename BasicJsonType>
inline void to_json(BasicJsonType& j, typename BasicJsonType::array_t&& arr) {
    external_constructor<value_t::array>::construct(j, std::move(arr));
}

template <typename BasicJsonType, typename CompatibleObjectType,
          enable_if_t<is_compatible_object_type<BasicJsonType, CompatibleObjectType>::value &&
                          !is_basic_json<CompatibleObjectType>::value,
                      int> = 0>
inline void to_json(BasicJsonType& j, const CompatibleObjectType& obj) {
    external_constructor<value_t::object>::construct(j, obj);
}

template <typename BasicJsonType>
inline void to_json(BasicJsonType& j, typename BasicJsonType::object_t&& obj) {
    external_constructor<value_t::object>::construct(j, std::move(obj));
}

template <typename BasicJsonType, typename T, std::size_t N,
          enable_if_t<!std::is_convertible<const T (&)[N],
                                           typename BasicJsonType::string_t>::value,
                      int> = 0>
inline void to_json(BasicJsonType& j, const T (&arr)[N]) {
    external_constructor<value_t::array>::construct(j, arr);
}

template <typename BasicJsonType, typename T1, typename T2,
          enable_if_t<std::is_constructible<BasicJsonType, T1>::value &&
                          std::is_constructible<BasicJsonType, T2>::value,
                      int> = 0>
inline void to_json(BasicJsonType& j, const std::pair<T1, T2>& p) {
    j = {p.first, p.second};
}

template <typename BasicJsonType, typename T,
          enable_if_t<std::is_same<T, iteration_proxy_value<typename BasicJsonType::iterator>>::value,
                      int> = 0>
inline void to_json(BasicJsonType& j, const T& b) {
    j = {{b.key(), b.value()}};
}

template <typename BasicJsonType, typename Tuple>
inline void to_json_tuple_impl(BasicJsonType& j, const Tuple& /*unused*/,
                               std::index_sequence<> /*unused*/) {
    j = BasicJsonType();
}

template <typename BasicJsonType, typename Tuple, std::size_t... Idx>
inline void to_json_tuple_impl(BasicJsonType& j, const Tuple& t,
                               std::index_sequence<Idx...> /*unused*/) {
    j = {std::get<Idx>(t)...};
}

template <typename BasicJsonType, typename T,
          enable_if_t<is_constructible_tuple<BasicJsonType, T>::value, int> = 0>
inline void to_json(BasicJsonType& j, const T& t) {
    to_json_tuple_impl(j, t, std::make_index_sequence<std::tuple_size<T>::value>{});
}

template <typename BasicJsonType, typename T,
          enable_if_t<std::is_constructible<BasicJsonType, const T&>::value, int> = 0>
inline void to_json(BasicJsonType& j, const std::optional<T>& opt) {
    if (opt.has_value()) {
        j = *opt;
    } else {
        j = nullptr;
    }
}

struct to_json_fn {
    template <typename BasicJsonType, typename T>
    auto operator()(BasicJsonType& j, T&& val) const
        noexcept(noexcept(to_json(j, std::forward<T>(val))))
            -> decltype(to_json(j, std::forward<T>(val)), void()) {
        return to_json(j, std::forward<T>(val));
    }
};

struct from_json_fn {
    template <typename BasicJsonType, typename T>
    auto operator()(const BasicJsonType& j, T&& val) const
        noexcept(noexcept(from_json(j, std::forward<T>(val))))
            -> decltype(from_json(j, std::forward<T>(val))) {
        return from_json(j, std::forward<T>(val));
    }
};

} // namespace detail

#ifndef NEOJSON_HAS_CPP_17_INLINE_VARIABLES
namespace {
#endif
inline constexpr const auto& to_json = detail::static_const<detail::to_json_fn>::value;
inline constexpr const auto& from_json = detail::static_const<detail::from_json_fn>::value;
#ifndef NEOJSON_HAS_CPP_17_INLINE_VARIABLES
} // namespace
#endif

template <typename ValueType, typename>
struct adl_serializer {
    template <typename BasicJsonType, typename TargetType = ValueType>
    static auto from_json(const BasicJsonType& j, TargetType& val) noexcept(
        noexcept(::neojson::from_json(j, val)))
        -> decltype(::neojson::from_json(j, val), void()) {
        ::neojson::from_json(j, val);
    }

    template <typename BasicJsonType, typename TargetType = ValueType>
    static auto from_json(const BasicJsonType& j) noexcept(
        noexcept(::neojson::from_json(j, detail::identity_tag<TargetType>{})))
        -> decltype(::neojson::from_json(j, detail::identity_tag<TargetType>{})) {
        return ::neojson::from_json(j, detail::identity_tag<TargetType>{});
    }

    template <typename BasicJsonType, typename TargetType = ValueType>
    static auto to_json(BasicJsonType& j, TargetType&& val) noexcept(
        noexcept(::neojson::to_json(j, std::forward<TargetType>(val))))
        -> decltype(::neojson::to_json(j, std::forward<TargetType>(val)), void()) {
        ::neojson::to_json(j, std::forward<TargetType>(val));
    }
};

class json_pointer {
    friend class json;

public:
    using string_t = std::string;

    explicit json_pointer(const string_t& s = "") : reference_tokens(split(s)) {}

    string_t to_string() const {
        return std::accumulate(reference_tokens.begin(), reference_tokens.end(), string_t{},
                               [](const string_t& a, const string_t& b) {
                                   return a + "/" + detail::escape(b);
                               });
    }

    operator string_t() const {
        return to_string();
    }

    friend std::ostream& operator<<(std::ostream& o, const json_pointer& ptr) {
        return o << ptr.to_string();
    }

    json_pointer& operator/=(const json_pointer& ptr) {
        reference_tokens.insert(reference_tokens.end(), ptr.reference_tokens.begin(),
                                ptr.reference_tokens.end());
        return *this;
    }

    json_pointer& operator/=(string_t token) {
        push_back(std::move(token));
        return *this;
    }

    json_pointer& operator/=(std::size_t array_idx) {
        return *this /= std::to_string(array_idx);
    }

    friend json_pointer operator/(const json_pointer& lhs, const json_pointer& rhs) {
        return json_pointer(lhs) /= rhs;
    }

    friend json_pointer operator/(const json_pointer& lhs, string_t token) {
        return json_pointer(lhs) /= std::move(token);
    }

    friend json_pointer operator/(const json_pointer& lhs, std::size_t array_idx) {
        return json_pointer(lhs) /= array_idx;
    }

    json_pointer parent_pointer() const {
        if (empty()) {
            return *this;
        }
        json_pointer res = *this;
        res.pop_back();
        return res;
    }

    void pop_back() {
        if (empty()) {
            throw detail::out_of_range::create(405, "JSON pointer has no parent");
        }
        reference_tokens.pop_back();
    }

    const string_t& back() const {
        if (empty()) {
            throw detail::out_of_range::create(405, "JSON pointer has no parent");
        }
        return reference_tokens.back();
    }

    void push_back(const string_t& token) {
        reference_tokens.push_back(token);
    }

    void push_back(string_t&& token) {
        reference_tokens.push_back(std::move(token));
    }

    bool empty() const noexcept {
        return reference_tokens.empty();
    }

    friend bool operator==(const json_pointer& lhs, const json_pointer& rhs) noexcept {
        return lhs.reference_tokens == rhs.reference_tokens;
    }

    friend bool operator!=(const json_pointer& lhs, const json_pointer& rhs) noexcept {
        return !(lhs == rhs);
    }

    friend bool operator<(const json_pointer& lhs, const json_pointer& rhs) noexcept {
        return lhs.reference_tokens < rhs.reference_tokens;
    }

private:
    static std::size_t array_index(const string_t& s) {
        if (s.size() > 1 && s[0] == '0') {
            throw detail::parse_error::create(
                106, 0, "array index '" + s + "' must not begin with '0'");
        }
        if (s.size() > 1 && !(s[0] >= '1' && s[0] <= '9')) {
            throw detail::parse_error::create(
                109, 0, "array index '" + s + "' is not a number");
        }

        const char* p = s.c_str();
        char* p_end = nullptr;
        errno = 0;
        const unsigned long long res = std::strtoull(p, &p_end, 10);
        if (p == p_end || errno == ERANGE || p_end != p + s.size()) {
            throw detail::out_of_range::create(
                404, "unresolved reference token '" + s + "'");
        }
        if (res > static_cast<unsigned long long>((std::numeric_limits<std::size_t>::max)())) {
            throw detail::out_of_range::create(
                410, "array index " + s + " exceeds size_type");
        }
        return static_cast<std::size_t>(res);
    }

    json_pointer top() const {
        if (empty()) {
            throw detail::out_of_range::create(405, "JSON pointer has no parent");
        }
        json_pointer result = *this;
        result.reference_tokens = {reference_tokens[0]};
        return result;
    }

    template <typename BasicJsonType>
    BasicJsonType& get_and_create(BasicJsonType& j) const {
        auto* result = &j;
        for (const auto& reference_token : reference_tokens) {
            switch (result->type()) {
                case detail::value_t::null: {
                    if (reference_token == "0") {
                        result = &result->operator[](0);
                    } else {
                        result = &result->operator[](reference_token);
                    }
                    break;
                }
                case detail::value_t::object: {
                    result = &result->operator[](reference_token);
                    break;
                }
                case detail::value_t::array: {
                    result = &result->operator[](array_index(reference_token));
                    break;
                }
                default:
                    throw detail::type_error::create(313, "invalid value to unflatten");
            }
        }
        return *result;
    }

    template <typename BasicJsonType>
    BasicJsonType& get_unchecked(BasicJsonType* ptr) const {
        for (const auto& reference_token : reference_tokens) {
            if (ptr->is_null()) {
                const bool nums = std::all_of(
                    reference_token.begin(), reference_token.end(),
                    [](const unsigned char x) { return std::isdigit(x); });
                *ptr = (nums || reference_token == "-") ? detail::value_t::array
                                                        : detail::value_t::object;
            }

            switch (ptr->type()) {
                case detail::value_t::object: {
                    ptr = &ptr->operator[](reference_token);
                    break;
                }
                case detail::value_t::array: {
                    if (reference_token == "-") {
                        ptr->push_back(nullptr);
                        ptr = &ptr->back();
                    } else {
                        const auto idx = array_index(reference_token);
                        ptr = &ptr->operator[](idx);
                    }
                    break;
                }
                default:
                    throw detail::out_of_range::create(
                        404, "unresolved reference token '" + reference_token + "'");
            }
        }
        return *ptr;
    }

    template <typename BasicJsonType>
    BasicJsonType& get_checked(BasicJsonType* ptr) const {
        for (const auto& reference_token : reference_tokens) {
            switch (ptr->type()) {
                case detail::value_t::object: {
                    ptr = &ptr->at(reference_token);
                    break;
                }
                case detail::value_t::array: {
                    if (reference_token == "-") {
                        throw detail::out_of_range::create(
                            402, "array index '-' (" + std::to_string(ptr->size()) +
                                     ") is out of range");
                    }
                    ptr = &ptr->at(array_index(reference_token));
                    break;
                }
                default:
                    throw detail::out_of_range::create(
                        404, "unresolved reference token '" + reference_token + "'");
            }
        }
        return *ptr;
    }

    template <typename BasicJsonType>
    const BasicJsonType& get_unchecked(const BasicJsonType* ptr) const {
        for (const auto& reference_token : reference_tokens) {
            switch (ptr->type()) {
                case detail::value_t::object: {
                    ptr = &ptr->at(reference_token);
                    break;
                }
                case detail::value_t::array: {
                    if (reference_token == "-") {
                        throw detail::out_of_range::create(
                            402, "array index '-' (" + std::to_string(ptr->size()) +
                                     ") is out of range");
                    }
                    ptr = &ptr->at(array_index(reference_token));
                    break;
                }
                default:
                    throw detail::out_of_range::create(
                        404, "unresolved reference token '" + reference_token + "'");
            }
        }
        return *ptr;
    }

    template <typename BasicJsonType>
    const BasicJsonType& get_checked(const BasicJsonType* ptr) const {
        for (const auto& reference_token : reference_tokens) {
            switch (ptr->type()) {
                case detail::value_t::object: {
                    ptr = &ptr->at(reference_token);
                    break;
                }
                case detail::value_t::array: {
                    if (reference_token == "-") {
                        throw detail::out_of_range::create(
                            402, "array index '-' (" + std::to_string(ptr->size()) +
                                     ") is out of range");
                    }
                    ptr = &ptr->at(array_index(reference_token));
                    break;
                }
                default:
                    throw detail::out_of_range::create(
                        404, "unresolved reference token '" + reference_token + "'");
            }
        }
        return *ptr;
    }

    template <typename BasicJsonType>
    bool contains(const BasicJsonType* ptr) const {
        for (const auto& reference_token : reference_tokens) {
            switch (ptr->type()) {
                case detail::value_t::object: {
                    if (!ptr->contains(reference_token)) {
                        return false;
                    }
                    ptr = &ptr->operator[](reference_token);
                    break;
                }
                case detail::value_t::array: {
                    if (reference_token == "-") {
                        return false;
                    }
                    if (reference_token.size() == 1 &&
                        !(reference_token >= "0" && reference_token <= "9")) {
                        return false;
                    }
                    if (reference_token.size() > 1) {
                        if (!(reference_token[0] >= '1' && reference_token[0] <= '9')) {
                            return false;
                        }
                        for (std::size_t i = 1; i < reference_token.size(); i++) {
                            if (!(reference_token[i] >= '0' && reference_token[i] <= '9')) {
                                return false;
                            }
                        }
                    }

                    const auto idx = array_index(reference_token);
                    if (idx >= ptr->size()) {
                        return false;
                    }

                    ptr = &ptr->operator[](idx);
                    break;
                }
                default:
                    return false;
            }
        }
        return true;
    }

    static std::vector<string_t> split(const string_t& reference_string) {
        std::vector<string_t> result;
        if (reference_string.empty()) {
            return result;
        }
        if (reference_string[0] != '/') {
            throw detail::parse_error::create(
                107, 1,
                "JSON pointer must be empty or begin with '/' - was: '" + reference_string + "'");
        }
        for (std::size_t slash = 0; slash != string_t::npos;) {
            const std::size_t start = slash + 1;
            slash = reference_string.find('/', start);
            auto reference_token = reference_string.substr(start, slash - start);
            for (std::size_t pos = reference_token.find('~'); pos != string_t::npos;
                 pos = reference_token.find('~', pos + 1)) {
                if (pos == reference_token.size() - 1 ||
                    (reference_token[pos + 1] != '0' && reference_token[pos + 1] != '1')) {
                    throw detail::parse_error::create(
                        108, 0, "escape character '~' must be followed with '0' or '1'");
                }
            }
            detail::unescape(reference_token);
            result.push_back(std::move(reference_token));
        }
        return result;
    }

    template <typename BasicJsonType>
    static void flatten(const string_t& reference_string, const BasicJsonType& value,
                        BasicJsonType& result) {
        switch (value.type()) {
            case detail::value_t::array: {
                if (value.empty()) {
                    result[reference_string] = nullptr;
                } else {
                    for (std::size_t i = 0; i < value.size(); ++i) {
                        flatten(reference_string + "/" + std::to_string(i), value[i], result);
                    }
                }
                break;
            }
            case detail::value_t::object: {
                if (value.empty()) {
                    result[reference_string] = nullptr;
                } else {
                    for (auto it = value.begin(); it != value.end(); ++it) {
                        flatten(reference_string + "/" + detail::escape(it.key()), it.value(),
                                result);
                    }
                }
                break;
            }
            default:
                result[reference_string] = value;
                break;
        }
    }

    template <typename BasicJsonType>
    static BasicJsonType unflatten(const BasicJsonType& value) {
        if (!value.is_object()) {
            throw detail::type_error::create(314, "only objects can be unflattened");
        }
        BasicJsonType result;
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!it.value().is_primitive()) {
                throw detail::type_error::create(315, "values in object must be primitive");
            }
            json_pointer(it.key()).get_and_create(result) = it.value();
        }
        return result;
    }

    std::vector<string_t> reference_tokens;
};

/// Limits applied to one call of json::parse or json::accept.
struct parse_options {
    /// Default value of max_depth. Parsing keeps its own stack of open arrays and objects, so a deep document costs
    /// it no call stack; the recursive walks of a finished tree (copy, dump, destruction, comparison) do. The most
    /// stack one level takes in an unoptimised build, measured on a 512 KiB thread, in bytes per level:
    ///   copy 701 (Clang 19), 586 (GCC 13), 1398 (GCC 13 with AddressSanitizer);
    ///   destruction 343, 309, 408; dump 327, 147, 147.
    /// A quarter of that stack (128 KiB) would allow only 93 to 220 levels, fewer than the 1025 levels that the
    /// tests of this library parse. The default is 1100: copying such a tree takes 0.8 MB of stack in an unoptimised
    /// Clang build and 1.5 MB with AddressSanitizer, and a small fraction of that when optimised. A thread with a
    /// smaller stack than that should pass a smaller max_depth.
    static constexpr std::size_t default_max_depth = 1100;

    /// A document whose arrays and objects are nested more than max_depth levels deep is a parse error (the
    /// outermost array or object is level 1; a document that is a single value has no levels).
    std::size_t max_depth = default_max_depth;
};

class json {
private:
    template <typename BasicJsonType>
    friend class detail::iter_impl;
    template <detail::value_t>
    friend struct detail::external_constructor;
    friend struct detail::json_access;
    friend class ::neojson::json_pointer;

public:
    using value_t = detail::value_t;
    using error_handler_t = detail::error_handler_t;
    using json_pointer = ::neojson::json_pointer;
    template <typename T = void, typename SFINAE = void>
    using json_serializer = adl_serializer<T, SFINAE>;

    using exception = detail::exception;
    using parse_error = detail::parse_error;
    using invalid_iterator = detail::invalid_iterator;
    using type_error = detail::type_error;
    using out_of_range = detail::out_of_range;
    using other_error = detail::other_error;

    using value_type = json;
    using reference = value_type&;
    using const_reference = const value_type&;
    using difference_type = std::ptrdiff_t;
    using size_type = std::size_t;
    using allocator_type = std::allocator<json>;
    using pointer = value_type*;
    using const_pointer = const value_type*;

    using iterator = detail::iter_impl<json>;
    using const_iterator = detail::iter_impl<const json>;
    using reverse_iterator = detail::json_reverse_iterator<iterator>;
    using const_reverse_iterator = detail::json_reverse_iterator<const_iterator>;

    using object_comparator_t = std::less<std::string>;
    using object_t =
        ordered_map<std::string, json, object_comparator_t,
                    detail::pool_allocator<std::pair<std::string, json>>>;
    using array_t = std::vector<json, detail::pool_allocator<json>>;
    using string_t = std::string;
    using boolean_t = bool;
    using number_integer_t = std::int64_t;
    using number_unsigned_t = std::uint64_t;
    using number_float_t = double;
    using initializer_list_t = std::initializer_list<detail::json_ref<json>>;
    using parse_event_t = detail::parse_event_t;
    using parser_callback_t = detail::parser_callback_t<json>;
    using parse_options = ::neojson::parse_options;

private:
    template <typename... Args>
    static string_t* create_string(Args&&... args) {
        void* mem = detail::pool_alloc(sizeof(string_t));
        try {
            return ::new (mem) string_t(std::forward<Args>(args)...);
        } catch (...) {
            detail::pool_free(mem, sizeof(string_t));
            throw;
        }
    }

    static void destroy_string(string_t* s) noexcept {
        if (s != nullptr) {
            s->~string_t();
            detail::pool_free(s, sizeof(string_t));
        }
    }

    template <typename... Args>
    static array_t* create_array(Args&&... args) {
        void* mem = detail::pool_alloc(sizeof(array_t));
        try {
            return ::new (mem) array_t(std::forward<Args>(args)...);
        } catch (...) {
            detail::pool_free(mem, sizeof(array_t));
            throw;
        }
    }

    static void destroy_array(array_t* a) noexcept {
        if (a != nullptr) {
            a->~array_t();
            detail::pool_free(a, sizeof(array_t));
        }
    }

    template <typename... Args>
    static object_t* create_object(Args&&... args) {
        void* mem = detail::pool_alloc(sizeof(object_t));
        try {
            return ::new (mem) object_t(std::forward<Args>(args)...);
        } catch (...) {
            detail::pool_free(mem, sizeof(object_t));
            throw;
        }
    }

    static void destroy_object(object_t* o) noexcept {
        if (o != nullptr) {
            o->~object_t();
            detail::pool_free(o, sizeof(object_t));
        }
    }

    union json_value {
        object_t* object;
        array_t* array;
        string_t* string;
        boolean_t boolean;
        number_integer_t number_integer;
        number_unsigned_t number_unsigned;
        number_float_t number_float;

        json_value() noexcept : object(nullptr) {}
        json_value(boolean_t v) noexcept : boolean(v) {}
        json_value(number_integer_t v) noexcept : number_integer(v) {}
        json_value(number_unsigned_t v) noexcept : number_unsigned(v) {}
        json_value(number_float_t v) noexcept : number_float(v) {}
        json_value(value_t t) {
            switch (t) {
                case value_t::object:
                    object = create_object();
                    break;
                case value_t::array:
                    array = create_array();
                    break;
                case value_t::string:
                    string = create_string();
                    break;
                case value_t::boolean:
                    boolean = false;
                    break;
                case value_t::number_integer:
                    number_integer = 0;
                    break;
                case value_t::number_unsigned:
                    number_unsigned = 0;
                    break;
                case value_t::number_float:
                    number_float = 0.0;
                    break;
                case value_t::null:
                case value_t::binary:
                case value_t::discarded:
                default:
                    object = nullptr;
                    break;
            }
        }

        static bool is_container(const json& j) noexcept {
            return j.m_type == value_t::array || j.m_type == value_t::object;
        }

        static json& element_of(json& j) noexcept {
            return j;
        }

        static json& element_of(std::pair<std::string, json>& member) noexcept {
            return member.second;
        }

        // Moves the arrays and objects among the elements of `c` onto `stack`, leaving null in their place.
        template <typename Container>
        static void take_containers(Container& c, std::vector<json>& stack) {
            for (auto& element : c) {
                json& child = element_of(element);
                if (is_container(child)) {
                    stack.push_back(std::move(child));
                }
            }
        }

        // Destroys the arrays and objects below the array or object `c` without a call per level: they are moved to
        // a stack that lives on the heap, and each is taken apart once it holds no container any more. What `c`
        // holds afterwards is plain values (and nulls), whose destruction does not recurse. If the heap stack cannot
        // grow, the containers still on it and in `c` are destroyed by recursion, as they would be without this
        // function.
        template <typename Container>
        static void release_containers(Container& c) noexcept {
            bool nested = false;
            for (auto& element : c) {
                if (is_container(element_of(element))) {
                    nested = true;
                    break;
                }
            }
            if (!nested) {
                return;
            }
            try {
                std::vector<json> stack;
                take_containers(c, stack);
                while (!stack.empty()) {
                    json current(std::move(stack.back()));
                    stack.pop_back();
                    if (current.m_type == value_t::array) {
                        take_containers(*current.m_value.array, stack);
                    } else {
                        take_containers(*current.m_value.object, stack);
                    }
                }
            } catch (...) {
            }
        }

        // Destroys the array or object `c` points to. While fewer than kMaxRecursiveDestroyDepth of them are being
        // destroyed on this thread, one inside the other, it deletes it, which destroys its elements by recursion.
        // Below that depth nothing is allocated and the elements are not looked at twice. At that depth the
        // containers below `c` are taken apart on the heap stack first, so that the recursion stops there.
        static void free_container(object_t* c) noexcept {
            destroy_object(c);
        }

        static void free_container(array_t* c) noexcept {
            destroy_array(c);
        }

        template <typename Container>
        static void destroy_container(Container*& c) noexcept {
            if (c == nullptr) {
                return;
            }
            if (detail::t_destroy_depth < detail::kMaxRecursiveDestroyDepth) {
                const detail::destroy_depth_guard guard;
                free_container(c);
            } else {
                ++detail::t_destroy_heap_stack_uses;
                release_containers(*c);
                free_container(c);
            }
            c = nullptr;
        }

        void destroy(value_t t) noexcept {
            switch (t) {
                case value_t::object:
                    destroy_container(object);
                    break;
                case value_t::array:
                    destroy_container(array);
                    break;
                case value_t::string:
                    destroy_string(string);
                    string = nullptr;
                    break;
                default:
                    break;
            }
        }
    };

public:
    // ==================== Construction & Assignment ====================

    json() noexcept = default;

    json(std::nullptr_t) noexcept : m_type(value_t::null), m_value() {}

    json(const value_t v) : m_type(v), m_value(v) {}

    template <typename CompatibleType,
              typename U = detail::uncvref_t<CompatibleType>,
              std::enable_if_t<
                  !detail::is_basic_json<U>::value &&
                      !detail::is_json_ref<U>::value &&
                      !std::is_same<U, value_t>::value &&
                      detail::is_compatible_type<json, U>::value,
                  int> = 0>
    json(CompatibleType&& val) noexcept(noexcept(
        adl_serializer<U>::to_json(std::declval<json&>(), std::forward<CompatibleType>(val)))) {
        adl_serializer<U>::to_json(*this, std::forward<CompatibleType>(val));
    }

    json(const detail::json_ref<json>& ref) : json(ref.moved_or_copied()) {}

    json(initializer_list_t init, bool type_deduction = true,
         value_t manual_type = value_t::array) {
        bool is_an_object = std::all_of(
            init.begin(), init.end(), [](const detail::json_ref<json>& element_ref) {
                return element_ref->is_array() && element_ref->size() == 2 &&
                       (*element_ref)[0].is_string();
            });

        if (!type_deduction) {
            if (manual_type == value_t::array) {
                is_an_object = false;
            } else if (manual_type == value_t::object && !is_an_object) {
                throw type_error::create(301, "cannot create object from initializer list");
            }
        }

        if (is_an_object) {
            m_value.object = create_object();
            m_type = value_t::object;
            m_value.object->reserve(init.size());
            for (auto& element_ref : init) {
                auto element = element_ref.moved_or_copied();
                m_value.object->emplace(
                    std::move(*((*element.m_value.array)[0].m_value.string)),
                    std::move((*element.m_value.array)[1]));
            }
        } else {
            m_value.array = create_array();
            m_type = value_t::array;
            m_value.array->reserve(init.size());
            for (auto const& element_ref : init) {
                m_value.array->emplace_back(element_ref.moved_or_copied());
            }
        }
    }

    json(size_type cnt, const json& val) {
        m_value.array = create_array(cnt, val);
        m_type = value_t::array;
    }

    template <class InputIT,
              std::enable_if_t<std::is_same<InputIT, iterator>::value ||
                                   std::is_same<InputIT, const_iterator>::value,
                               int> = 0>
    json(InputIT first, InputIT last) {
        if (first.m_object != last.m_object) {
            throw invalid_iterator::create(201, "iterators are not compatible");
        }
        if (first.m_object == nullptr) {
            throw invalid_iterator::create(206, "cannot construct with iterators from null");
        }

        m_type = first.m_object->m_type;
        switch (m_type) {
            case value_t::boolean:
            case value_t::number_integer:
            case value_t::number_unsigned:
            case value_t::number_float:
            case value_t::string: {
                if (first.idx_ != 0 || last.idx_ != 1) {
                    m_type = value_t::null;
                    throw invalid_iterator::create(204, "iterators out of range");
                }
                if (m_type == value_t::string) {
                    m_value.string = create_string(*(first.m_object->m_value.string));
                } else {
                    m_value = first.m_object->m_value;
                }
                break;
            }
            case value_t::array: {
                const auto sz = static_cast<difference_type>(first.m_object->m_value.array->size());
                if (first.idx_ < 0 || last.idx_ < first.idx_ || last.idx_ > sz) {
                    m_type = value_t::null;
                    throw invalid_iterator::create(204, "iterators out of range");
                }
                m_value.array = create_array(
                    first.m_object->m_value.array->begin() + first.idx_,
                    first.m_object->m_value.array->begin() + last.idx_);
                break;
            }
            case value_t::object: {
                const auto sz =
                    static_cast<difference_type>(first.m_object->m_value.object->size());
                if (first.idx_ < 0 || last.idx_ < first.idx_ || last.idx_ > sz) {
                    m_type = value_t::null;
                    throw invalid_iterator::create(204, "iterators out of range");
                }
                m_value.object = create_object(
                    first.m_object->m_value.object->begin() + first.idx_,
                    first.m_object->m_value.object->begin() + last.idx_);
                break;
            }
            case value_t::null:
            case value_t::binary:
            case value_t::discarded:
            default: {
                const auto tname = std::string(first.m_object->type_name());
                m_type = value_t::null;
                throw invalid_iterator::create(206, "cannot construct with iterators from " + tname);
            }
        }
    }

    static json object(initializer_list_t init = {}) {
        return json(init, false, value_t::object);
    }

    static json array(initializer_list_t init = {}) {
        return json(init, false, value_t::array);
    }

    json(const json& other) : m_type(other.m_type) {
        switch (m_type) {
            case value_t::object:
                m_value.object = create_object(*(other.m_value.object));
                break;
            case value_t::array:
                m_value.array = create_array(*(other.m_value.array));
                break;
            case value_t::string:
                m_value.string = create_string(*(other.m_value.string));
                break;
            case value_t::boolean:
                m_value.boolean = other.m_value.boolean;
                break;
            case value_t::number_integer:
                m_value.number_integer = other.m_value.number_integer;
                break;
            case value_t::number_unsigned:
                m_value.number_unsigned = other.m_value.number_unsigned;
                break;
            case value_t::number_float:
                m_value.number_float = other.m_value.number_float;
                break;
            case value_t::null:
            case value_t::binary:
            case value_t::discarded:
            default:
                m_value.object = nullptr;
                break;
        }
    }

    json(json&& other) noexcept
        : m_type(std::exchange(other.m_type, value_t::null)),
          m_value(std::exchange(other.m_value, json_value{})) {}

    ~json() noexcept {
        m_value.destroy(m_type);
    }

    json& operator=(const json& other) {
        if (this != &other) {
            json tmp(other);
            swap(tmp);
        }
        return *this;
    }

    json& operator=(json&& other) noexcept {
        if (this != &other) {
            json tmp(std::move(other));
            swap(tmp);
        }
        return *this;
    }

    json& operator=(initializer_list_t init) {
        json tmp(init);
        swap(tmp);
        return *this;
    }

    // ==================== Type Queries ====================

    constexpr value_t type() const noexcept { return m_type; }
    constexpr operator value_t() const noexcept { return m_type; }

    constexpr bool is_null() const noexcept { return m_type == value_t::null; }
    constexpr bool is_boolean() const noexcept { return m_type == value_t::boolean; }
    constexpr bool is_number() const noexcept {
        return is_number_integer() || is_number_unsigned() || is_number_float();
    }
    constexpr bool is_number_integer() const noexcept {
        return m_type == value_t::number_integer || m_type == value_t::number_unsigned;
    }
    constexpr bool is_number_unsigned() const noexcept {
        return m_type == value_t::number_unsigned;
    }
    constexpr bool is_number_float() const noexcept { return m_type == value_t::number_float; }
    constexpr bool is_object() const noexcept { return m_type == value_t::object; }
    constexpr bool is_array() const noexcept { return m_type == value_t::array; }
    constexpr bool is_string() const noexcept { return m_type == value_t::string; }
    constexpr bool is_binary() const noexcept { return m_type == value_t::binary; }
    constexpr bool is_discarded() const noexcept { return m_type == value_t::discarded; }
    constexpr bool is_primitive() const noexcept {
        return is_null() || is_string() || is_boolean() || is_number() || is_binary();
    }
    constexpr bool is_structured() const noexcept { return is_array() || is_object(); }

    const char* type_name() const noexcept {
        switch (m_type) {
            case value_t::null:
                return "null";
            case value_t::object:
                return "object";
            case value_t::array:
                return "array";
            case value_t::string:
                return "string";
            case value_t::boolean:
                return "boolean";
            case value_t::binary:
                return "binary";
            case value_t::discarded:
                return "discarded";
            case value_t::number_integer:
            case value_t::number_unsigned:
            case value_t::number_float:
            default:
                return "number";
        }
    }

    // ==================== Capacity ====================

    bool empty() const noexcept {
        switch (m_type) {
            case value_t::null:
                return true;
            case value_t::array:
                return m_value.array->empty();
            case value_t::object:
                return m_value.object->empty();
            default:
                return false;
        }
    }

    size_type size() const noexcept {
        switch (m_type) {
            case value_t::null:
                return 0;
            case value_t::array:
                return m_value.array->size();
            case value_t::object:
                return m_value.object->size();
            default:
                return 1;
        }
    }

    size_type max_size() const noexcept {
        switch (m_type) {
            case value_t::array:
                return m_value.array->max_size();
            case value_t::object:
                return m_value.object->max_size();
            default:
                return size();
        }
    }

    // ==================== Element Access & Lookup ====================

    reference at(size_type idx) {
        if (is_array()) {
            if (idx >= m_value.array->size()) {
                throw out_of_range::create(
                    401, "array index " + std::to_string(idx) + " is out of range");
            }
            return (*m_value.array)[idx];
        }
        throw type_error::create(304, std::string("cannot use at() with ") + type_name());
    }

    const_reference at(size_type idx) const {
        if (is_array()) {
            if (idx >= m_value.array->size()) {
                throw out_of_range::create(
                    401, "array index " + std::to_string(idx) + " is out of range");
            }
            return (*m_value.array)[idx];
        }
        throw type_error::create(304, std::string("cannot use at() with ") + type_name());
    }

    reference at(const typename object_t::key_type& key) {
        if (is_object()) {
            auto it = m_value.object->find(key);
            if (it == m_value.object->end()) {
                throw out_of_range::create(403, "key '" + key + "' not found");
            }
            return it->second;
        }
        throw type_error::create(304, std::string("cannot use at() with ") + type_name());
    }

    const_reference at(const typename object_t::key_type& key) const {
        if (is_object()) {
            auto it = m_value.object->find(key);
            if (it == m_value.object->end()) {
                throw out_of_range::create(403, "key '" + key + "' not found");
            }
            return it->second;
        }
        throw type_error::create(304, std::string("cannot use at() with ") + type_name());
    }

    template <typename CharT,
              std::enable_if_t<std::is_same<std::remove_cv_t<CharT>, char>::value, int> = 0>
    reference at(CharT* key) {
        return at(std::string_view(key));
    }

    template <typename CharT,
              std::enable_if_t<std::is_same<std::remove_cv_t<CharT>, char>::value, int> = 0>
    const_reference at(CharT* key) const {
        return at(std::string_view(key));
    }

    template <typename KeyType,
              std::enable_if_t<
                  std::is_convertible<const KeyType&, std::string_view>::value &&
                      !detail::is_json_pointer<detail::uncvref_t<KeyType>>::value &&
                      !std::is_same<std::decay_t<KeyType>, typename object_t::key_type>::value &&
                      !std::is_pointer<std::decay_t<KeyType>>::value &&
                      !std::is_integral<std::decay_t<KeyType>>::value,
                  int> = 0>
    reference at(const KeyType& key) {
        if (is_object()) {
            std::string_view sv(key);
            auto it = m_value.object->find(sv);
            if (it == m_value.object->end()) {
                throw out_of_range::create(403, "key '" + std::string(sv) + "' not found");
            }
            return it->second;
        }
        throw type_error::create(304, std::string("cannot use at() with ") + type_name());
    }

    template <typename KeyType,
              std::enable_if_t<
                  std::is_convertible<const KeyType&, std::string_view>::value &&
                      !detail::is_json_pointer<detail::uncvref_t<KeyType>>::value &&
                      !std::is_same<std::decay_t<KeyType>, typename object_t::key_type>::value &&
                      !std::is_pointer<std::decay_t<KeyType>>::value &&
                      !std::is_integral<std::decay_t<KeyType>>::value,
                  int> = 0>
    const_reference at(const KeyType& key) const {
        if (is_object()) {
            std::string_view sv(key);
            auto it = m_value.object->find(sv);
            if (it == m_value.object->end()) {
                throw out_of_range::create(403, "key '" + std::string(sv) + "' not found");
            }
            return it->second;
        }
        throw type_error::create(304, std::string("cannot use at() with ") + type_name());
    }

    reference at(const json_pointer& ptr) {
        return ptr.get_checked(this);
    }

    const_reference at(const json_pointer& ptr) const {
        return ptr.get_checked(this);
    }

    reference operator[](size_type idx) {
        if (is_null()) {
            m_value.array = create_array();
            m_type = value_t::array;
        }
        if (is_array()) {
            if (idx >= m_value.array->size()) {
                m_value.array->resize(idx + 1);
            }
            return (*m_value.array)[idx];
        }
        throw type_error::create(
            305, std::string("cannot use operator[] with a numeric argument with ") + type_name());
    }

    const_reference operator[](size_type idx) const {
        if (is_array()) {
            return (*m_value.array)[idx];
        }
        throw type_error::create(
            305, std::string("cannot use operator[] with a numeric argument with ") + type_name());
    }

    reference operator[](const typename object_t::key_type& key) {
        if (is_null()) {
            m_value.object = create_object();
            m_type = value_t::object;
        }
        if (is_object()) {
            return (*m_value.object)[key];
        }
        throw type_error::create(
            305, std::string("cannot use operator[] with a string argument with ") + type_name());
    }

    const_reference operator[](const typename object_t::key_type& key) const {
        if (is_object()) {
            auto it = m_value.object->find(key);
            assert(it != m_value.object->end());
            return it->second;
        }
        throw type_error::create(
            305, std::string("cannot use operator[] with a string argument with ") + type_name());
    }

    template <typename CharT,
              std::enable_if_t<std::is_same<std::remove_cv_t<CharT>, char>::value, int> = 0>
    reference operator[](CharT* key) {
        if (is_null()) {
            m_value.object = create_object();
            m_type = value_t::object;
        }
        if (is_object()) {
            return (*m_value.object)[std::string_view(key)];
        }
        throw type_error::create(
            305, std::string("cannot use operator[] with a string argument with ") + type_name());
    }

    template <typename CharT,
              std::enable_if_t<std::is_same<std::remove_cv_t<CharT>, char>::value, int> = 0>
    const_reference operator[](CharT* key) const {
        if (is_object()) {
            auto it = m_value.object->find(std::string_view(key));
            assert(it != m_value.object->end());
            return it->second;
        }
        throw type_error::create(
            305, std::string("cannot use operator[] with a string argument with ") + type_name());
    }

    template <typename KeyType,
              std::enable_if_t<
                  std::is_convertible<const KeyType&, std::string_view>::value &&
                      !detail::is_json_pointer<detail::uncvref_t<KeyType>>::value &&
                      !std::is_same<std::decay_t<KeyType>, typename object_t::key_type>::value &&
                      !std::is_pointer<std::decay_t<KeyType>>::value &&
                      !std::is_integral<std::decay_t<KeyType>>::value,
                  int> = 0>
    reference operator[](const KeyType& key) {
        if (is_null()) {
            m_value.object = create_object();
            m_type = value_t::object;
        }
        if (is_object()) {
            return (*m_value.object)[std::string_view(key)];
        }
        throw type_error::create(
            305, std::string("cannot use operator[] with a string argument with ") + type_name());
    }

    template <typename KeyType,
              std::enable_if_t<
                  std::is_convertible<const KeyType&, std::string_view>::value &&
                      !detail::is_json_pointer<detail::uncvref_t<KeyType>>::value &&
                      !std::is_same<std::decay_t<KeyType>, typename object_t::key_type>::value &&
                      !std::is_pointer<std::decay_t<KeyType>>::value &&
                      !std::is_integral<std::decay_t<KeyType>>::value,
                  int> = 0>
    const_reference operator[](const KeyType& key) const {
        if (is_object()) {
            auto it = m_value.object->find(std::string_view(key));
            assert(it != m_value.object->end());
            return it->second;
        }
        throw type_error::create(
            305, std::string("cannot use operator[] with a string argument with ") + type_name());
    }

    reference operator[](const json_pointer& ptr) {
        return ptr.get_unchecked(this);
    }

    const_reference operator[](const json_pointer& ptr) const {
        return ptr.get_unchecked(this);
    }

    template <class ValueType,
              std::enable_if_t<
                  detail::is_getable<json, ValueType>::value &&
                      !std::is_same<value_t, detail::uncvref_t<ValueType>>::value &&
                      !std::is_same<std::decay_t<ValueType>, const char*>::value &&
                      !std::is_same<std::decay_t<ValueType>, char*>::value,
                  int> = 0>
    ValueType value(const typename object_t::key_type& key, const ValueType& default_value) const {
        if (is_object()) {
            const auto it = find(key);
            if (it != end()) {
                return it->template get<ValueType>();
            }
            return default_value;
        }
        throw type_error::create(306, std::string("cannot use value() with ") + type_name());
    }

    string_t value(const typename object_t::key_type& key, const char* default_value) const;

    template <class KeyType, class ValueType,
              std::enable_if_t<
                  detail::is_getable<json, ValueType>::value &&
                      !std::is_same<value_t, detail::uncvref_t<ValueType>>::value &&
                      std::is_convertible<const KeyType&, std::string_view>::value &&
                      !detail::is_json_pointer<detail::uncvref_t<KeyType>>::value &&
                      !std::is_same<std::decay_t<KeyType>, typename object_t::key_type>::value &&
                      !std::is_same<std::decay_t<ValueType>, const char*>::value &&
                      !std::is_same<std::decay_t<ValueType>, char*>::value,
                  int> = 0>
    ValueType value(KeyType&& key, const ValueType& default_value) const {
        if (is_object()) {
            const auto it = find(std::forward<KeyType>(key));
            if (it != end()) {
                return it->template get<ValueType>();
            }
            return default_value;
        }
        throw type_error::create(306, std::string("cannot use value() with ") + type_name());
    }

    template <class KeyType,
              std::enable_if_t<
                  std::is_convertible<const KeyType&, std::string_view>::value &&
                      !detail::is_json_pointer<detail::uncvref_t<KeyType>>::value &&
                      !std::is_same<std::decay_t<KeyType>, typename object_t::key_type>::value,
                  int> = 0>
    string_t value(KeyType&& key, const char* default_value) const {
        return value(std::forward<KeyType>(key), string_t(default_value));
    }

    template <class ValueType,
              std::enable_if_t<
                  detail::is_getable<json, ValueType>::value &&
                      !std::is_same<value_t, detail::uncvref_t<ValueType>>::value &&
                      !std::is_same<std::decay_t<ValueType>, const char*>::value &&
                      !std::is_same<std::decay_t<ValueType>, char*>::value,
                  int> = 0>
    ValueType value(const json_pointer& ptr, const ValueType& default_value) const {
        if (is_object()) {
            try {
                return ptr.get_checked(this).template get<ValueType>();
            } catch (out_of_range&) {
                return default_value;
            }
        }
        throw type_error::create(306, std::string("cannot use value() with ") + type_name());
    }

    string_t value(const json_pointer& ptr, const char* default_value) const;

    reference front() { return *begin(); }
    const_reference front() const { return *cbegin(); }

    reference back() {
        auto tmp = end();
        --tmp;
        return *tmp;
    }

    const_reference back() const {
        auto tmp = cend();
        --tmp;
        return *tmp;
    }

    iterator find(const typename object_t::key_type& key) {
        if (is_object()) {
            auto it = m_value.object->find(key);
            return iterator(
                this, static_cast<difference_type>(std::distance(m_value.object->begin(), it)));
        }
        return end();
    }

    const_iterator find(const typename object_t::key_type& key) const {
        if (is_object()) {
            auto it = m_value.object->find(key);
            return const_iterator(
                this, static_cast<difference_type>(std::distance(m_value.object->begin(), it)));
        }
        return cend();
    }

    template <class KeyType,
              std::enable_if_t<
                  std::is_convertible<const KeyType&, std::string_view>::value &&
                      !detail::is_json_pointer<detail::uncvref_t<KeyType>>::value &&
                      !std::is_same<std::decay_t<KeyType>, typename object_t::key_type>::value,
                  int> = 0>
    iterator find(KeyType&& key) {
        if (is_object()) {
            auto it = m_value.object->find(std::string_view(key));
            return iterator(
                this, static_cast<difference_type>(std::distance(m_value.object->begin(), it)));
        }
        return end();
    }

    template <class KeyType,
              std::enable_if_t<
                  std::is_convertible<const KeyType&, std::string_view>::value &&
                      !detail::is_json_pointer<detail::uncvref_t<KeyType>>::value &&
                      !std::is_same<std::decay_t<KeyType>, typename object_t::key_type>::value,
                  int> = 0>
    const_iterator find(KeyType&& key) const {
        if (is_object()) {
            auto it = m_value.object->find(std::string_view(key));
            return const_iterator(
                this, static_cast<difference_type>(std::distance(m_value.object->begin(), it)));
        }
        return cend();
    }

    size_type count(const typename object_t::key_type& key) const {
        return is_object() ? m_value.object->count(key) : 0;
    }

    template <class KeyType,
              std::enable_if_t<
                  std::is_convertible<const KeyType&, std::string_view>::value &&
                      !detail::is_json_pointer<detail::uncvref_t<KeyType>>::value &&
                      !std::is_same<std::decay_t<KeyType>, typename object_t::key_type>::value,
                  int> = 0>
    size_type count(KeyType&& key) const {
        return is_object() ? m_value.object->count(std::string_view(key)) : 0;
    }

    bool contains(const typename object_t::key_type& key) const {
        return is_object() && m_value.object->contains(key);
    }

    template <class KeyType,
              std::enable_if_t<
                  std::is_convertible<const KeyType&, std::string_view>::value &&
                      !detail::is_json_pointer<detail::uncvref_t<KeyType>>::value &&
                      !std::is_same<std::decay_t<KeyType>, typename object_t::key_type>::value,
                  int> = 0>
    bool contains(KeyType&& key) const {
        return is_object() && m_value.object->contains(std::string_view(key));
    }

    bool contains(const json_pointer& ptr) const {
        return ptr.contains(this);
    }

    // ==================== Iterators ====================

    iterator begin() noexcept {
        return iterator(this, 0);
    }

    const_iterator begin() const noexcept { return cbegin(); }

    const_iterator cbegin() const noexcept {
        return const_iterator(this, 0);
    }

    iterator end() noexcept {
        switch (m_type) {
            case value_t::null:
                return iterator(this, 0);
            case value_t::object:
                return iterator(this, static_cast<difference_type>(m_value.object->size()));
            case value_t::array:
                return iterator(this, static_cast<difference_type>(m_value.array->size()));
            default:
                return iterator(this, 1);
        }
    }

    const_iterator end() const noexcept { return cend(); }

    const_iterator cend() const noexcept {
        switch (m_type) {
            case value_t::null:
                return const_iterator(this, 0);
            case value_t::object:
                return const_iterator(this, static_cast<difference_type>(m_value.object->size()));
            case value_t::array:
                return const_iterator(this, static_cast<difference_type>(m_value.array->size()));
            default:
                return const_iterator(this, 1);
        }
    }

    reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    const_reverse_iterator rbegin() const noexcept { return crbegin(); }
    const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(cend()); }

    reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator rend() const noexcept { return crend(); }
    const_reverse_iterator crend() const noexcept { return const_reverse_iterator(cbegin()); }

    detail::iteration_proxy<iterator> items() noexcept {
        return detail::iteration_proxy<iterator>(*this);
    }

    detail::iteration_proxy<const_iterator> items() const noexcept {
        return detail::iteration_proxy<const_iterator>(*this);
    }

    // ==================== Modifiers ====================

    void clear() noexcept {
        switch (m_type) {
            case value_t::number_integer:
                m_value.number_integer = 0;
                break;
            case value_t::number_unsigned:
                m_value.number_unsigned = 0;
                break;
            case value_t::number_float:
                m_value.number_float = 0.0;
                break;
            case value_t::boolean:
                m_value.boolean = false;
                break;
            case value_t::string:
                m_value.string->clear();
                break;
            case value_t::array:
                m_value.array->clear();
                break;
            case value_t::object:
                m_value.object->clear();
                break;
            default:
                break;
        }
    }

    void push_back(json&& val) {
        if (!is_null() && !is_array()) {
            throw type_error::create(308, std::string("cannot use push_back() with ") + type_name());
        }
        if (is_null()) {
            m_value.array = create_array();
            m_type = value_t::array;
        }
        json tmp(std::move(val));
        m_value.array->push_back(std::move(tmp));
    }

    void push_back(const json& val) {
        if (!is_null() && !is_array()) {
            throw type_error::create(308, std::string("cannot use push_back() with ") + type_name());
        }
        if (is_null()) {
            m_value.array = create_array();
            m_type = value_t::array;
        }
        json tmp(val);
        m_value.array->push_back(std::move(tmp));
    }

    void push_back(const typename object_t::value_type& val) {
        if (!is_null() && !is_object()) {
            throw type_error::create(308, std::string("cannot use push_back() with ") + type_name());
        }
        if (is_null()) {
            m_value.object = create_object();
            m_type = value_t::object;
        }
        typename object_t::value_type tmp(val);
        m_value.object->insert(std::move(tmp));
    }

    void push_back(initializer_list_t init) {
        if (is_object() && init.size() == 2 && (*init.begin())->is_string()) {
            json key = init.begin()->moved_or_copied();
            push_back(typename object_t::value_type(
                std::move(*(key.m_value.string)), (init.begin() + 1)->moved_or_copied()));
        } else {
            push_back(json(init));
        }
    }

    reference operator+=(json&& val) {
        push_back(std::move(val));
        return *this;
    }

    reference operator+=(const json& val) {
        push_back(val);
        return *this;
    }

    reference operator+=(const typename object_t::value_type& val) {
        push_back(val);
        return *this;
    }

    reference operator+=(initializer_list_t init) {
        push_back(init);
        return *this;
    }

    template <class... Args>
    reference emplace_back(Args&&... args) {
        if (!is_null() && !is_array()) {
            throw type_error::create(
                311, std::string("cannot use emplace_back() with ") + type_name());
        }
        if (is_null()) {
            m_value.array = create_array();
            m_type = value_t::array;
        }
        json tmp(std::forward<Args>(args)...);
        m_value.array->emplace_back(std::move(tmp));
        return m_value.array->back();
    }

    template <class... Args>
    std::pair<iterator, bool> emplace(Args&&... args) {
        if (!is_null() && !is_object()) {
            throw type_error::create(311, std::string("cannot use emplace() with ") + type_name());
        }
        if (is_null()) {
            m_value.object = create_object();
            m_type = value_t::object;
        }
        auto res = m_value.object->emplace(std::forward<Args>(args)...);
        const auto idx =
            static_cast<difference_type>(std::distance(m_value.object->begin(), res.first));
        return {iterator(this, idx), res.second};
    }

    iterator insert(const_iterator pos, const json& val) {
        if (is_array()) {
            if (pos.m_object != this) {
                throw invalid_iterator::create(202, "iterator does not fit current value");
            }
            json tmp(val);
            m_value.array->insert(m_value.array->begin() + pos.idx_, std::move(tmp));
            return iterator(this, pos.idx_);
        }
        throw type_error::create(309, std::string("cannot use insert() with ") + type_name());
    }

    iterator insert(const_iterator pos, json&& val) {
        return insert(pos, val);
    }

    iterator insert(const_iterator pos, size_type cnt, const json& val) {
        if (is_array()) {
            if (pos.m_object != this) {
                throw invalid_iterator::create(202, "iterator does not fit current value");
            }
            json tmp(val);
            m_value.array->insert(m_value.array->begin() + pos.idx_, cnt, tmp);
            return iterator(this, pos.idx_);
        }
        throw type_error::create(309, std::string("cannot use insert() with ") + type_name());
    }

    iterator insert(const_iterator pos, const_iterator first, const_iterator last) {
        if (!is_array()) {
            throw type_error::create(309, std::string("cannot use insert() with ") + type_name());
        }
        if (pos.m_object != this) {
            throw invalid_iterator::create(202, "iterator does not fit current value");
        }
        if (first.m_object != last.m_object) {
            throw invalid_iterator::create(210, "iterators do not fit");
        }
        if (first.m_object == this) {
            throw invalid_iterator::create(211, "passed iterators may not belong to container");
        }
        if (first.m_object == nullptr || !first.m_object->is_array() || first.idx_ < 0 ||
            last.idx_ < first.idx_ ||
            static_cast<std::size_t>(last.idx_) > first.m_object->m_value.array->size()) {
            throw invalid_iterator::create(204, "iterators out of range");
        }
        m_value.array->insert(m_value.array->begin() + pos.idx_,
                              first.m_object->m_value.array->begin() + first.idx_,
                              first.m_object->m_value.array->begin() + last.idx_);
        return iterator(this, pos.idx_);
    }

    iterator insert(const_iterator pos, initializer_list_t ilist) {
        if (!is_array()) {
            throw type_error::create(309, std::string("cannot use insert() with ") + type_name());
        }
        if (pos.m_object != this) {
            throw invalid_iterator::create(202, "iterator does not fit current value");
        }
        auto offset = pos.idx_;
        for (const auto& elem : ilist) {
            m_value.array->insert(m_value.array->begin() + offset, elem.moved_or_copied());
            ++offset;
        }
        return iterator(this, pos.idx_);
    }

    void insert(const_iterator first, const_iterator last) {
        if (m_type != value_t::object) {
            throw type_error::create(309, std::string("cannot use insert() with ") + type_name());
        }
        if (first.m_object != last.m_object) {
            throw invalid_iterator::create(210, "iterators do not fit");
        }
        if (first.m_object == nullptr || first.m_object->m_type != value_t::object) {
            throw invalid_iterator::create(202, "iterators first and last must point to objects");
        }
        if (first.idx_ < 0 || last.idx_ < first.idx_ ||
            static_cast<std::size_t>(last.idx_) > first.m_object->m_value.object->size()) {
            throw invalid_iterator::create(204, "iterators out of range");
        }
        for (auto it = first; it != last; ++it) {
            m_value.object->insert(std::make_pair(it.key(), *it));
        }
    }

    void update(const_reference j, bool merge_objects = false) {
        update(j.begin(), j.end(), merge_objects);
    }

    void update(const_iterator first, const_iterator last, bool merge_objects = false) {
        if (is_null()) {
            m_value.object = create_object();
            m_type = value_t::object;
        }
        if (!is_object()) {
            throw type_error::create(312, std::string("cannot use update() with ") + type_name());
        }
        if (first.m_object != last.m_object) {
            throw invalid_iterator::create(210, "iterators do not fit");
        }
        if (first.m_object == nullptr || !first.m_object->is_object()) {
            throw type_error::create(
                312, std::string("cannot use update() with ") +
                         (first.m_object ? first.m_object->type_name() : "null"));
        }
        if (first.idx_ < 0 || last.idx_ < first.idx_ ||
            static_cast<std::size_t>(last.idx_) > first.m_object->m_value.object->size()) {
            throw invalid_iterator::create(204, "iterators out of range");
        }
        if (first.m_object == this) {
            return;
        }
        for (auto it = first; it != last; ++it) {
            if (merge_objects && it.value().is_object()) {
                auto existing = m_value.object->find(it.key());
                if (existing != m_value.object->end()) {
                    existing->second.update(it.value(), true);
                    continue;
                }
            }
            (*m_value.object)[it.key()] = it.value();
        }
    }

    void erase(const size_type idx) {
        if (is_array()) {
            if (idx >= size()) {
                throw out_of_range::create(
                    401, "array index " + std::to_string(idx) + " is out of range");
            }
            m_value.array->erase(m_value.array->begin() + static_cast<difference_type>(idx));
        } else {
            throw type_error::create(307, std::string("cannot use erase() with ") + type_name());
        }
    }

    size_type erase(const typename object_t::key_type& key) {
        if (is_object()) {
            return m_value.object->erase(key);
        }
        throw type_error::create(307, std::string("cannot use erase() with ") + type_name());
    }

    template <typename CharT,
              std::enable_if_t<std::is_same<std::remove_cv_t<CharT>, char>::value, int> = 0>
    size_type erase(CharT* key) {
        if (is_object()) {
            return m_value.object->erase(std::string_view(key));
        }
        throw type_error::create(307, std::string("cannot use erase() with ") + type_name());
    }

    template <class KeyType,
              std::enable_if_t<
                  std::is_convertible<const KeyType&, std::string_view>::value &&
                      !detail::is_json_pointer<detail::uncvref_t<KeyType>>::value &&
                      !std::is_same<std::decay_t<KeyType>, typename object_t::key_type>::value &&
                      !std::is_pointer<std::decay_t<KeyType>>::value &&
                      !std::is_integral<std::decay_t<KeyType>>::value,
                  int> = 0>
    size_type erase(const KeyType& key) {
        if (is_object()) {
            return m_value.object->erase(std::string_view(key));
        }
        throw type_error::create(307, std::string("cannot use erase() with ") + type_name());
    }

    template <class IteratorType,
              std::enable_if_t<std::is_same<IteratorType, iterator>::value ||
                                   std::is_same<IteratorType, const_iterator>::value,
                               int> = 0>
    IteratorType erase(IteratorType pos) {
        if (pos.m_object != this) {
            throw invalid_iterator::create(202, "iterator does not fit current value");
        }
        switch (m_type) {
            case value_t::boolean:
            case value_t::number_integer:
            case value_t::number_unsigned:
            case value_t::number_float:
            case value_t::string: {
                if (pos.idx_ != 0) {
                    throw invalid_iterator::create(205, "iterator out of range");
                }
                if (is_string()) {
                    destroy_string(m_value.string);
                    m_value.string = nullptr;
                }
                m_type = value_t::null;
                return IteratorType(this, 0);
            }
            case value_t::object: {
                if (pos.idx_ < 0 ||
                    static_cast<std::size_t>(pos.idx_) >= m_value.object->size()) {
                    throw invalid_iterator::create(205, "iterator out of range");
                }
                m_value.object->erase(m_value.object->begin() + pos.idx_);
                return IteratorType(this, pos.idx_);
            }
            case value_t::array: {
                if (pos.idx_ < 0 ||
                    static_cast<std::size_t>(pos.idx_) >= m_value.array->size()) {
                    throw invalid_iterator::create(205, "iterator out of range");
                }
                m_value.array->erase(m_value.array->begin() + pos.idx_);
                return IteratorType(this, pos.idx_);
            }
            default:
                throw type_error::create(
                    307, std::string("cannot use erase() with ") + type_name());
        }
    }

    template <class IteratorType,
              std::enable_if_t<std::is_same<IteratorType, iterator>::value ||
                                   std::is_same<IteratorType, const_iterator>::value,
                               int> = 0>
    IteratorType erase(IteratorType first, IteratorType last) {
        if (first.m_object != this || last.m_object != this) {
            throw invalid_iterator::create(203, "iterators do not fit current value");
        }
        switch (m_type) {
            case value_t::boolean:
            case value_t::number_integer:
            case value_t::number_unsigned:
            case value_t::number_float:
            case value_t::string: {
                if (first.idx_ != 0 || last.idx_ != 1) {
                    throw invalid_iterator::create(204, "iterators out of range");
                }
                if (is_string()) {
                    destroy_string(m_value.string);
                    m_value.string = nullptr;
                }
                m_type = value_t::null;
                return IteratorType(this, 0);
            }
            case value_t::object: {
                const auto sz = static_cast<difference_type>(m_value.object->size());
                if (first.idx_ < 0 || last.idx_ < first.idx_ || last.idx_ > sz) {
                    throw invalid_iterator::create(204, "iterators out of range");
                }
                m_value.object->erase(m_value.object->begin() + first.idx_,
                                      m_value.object->begin() + last.idx_);
                return IteratorType(this, first.idx_);
            }
            case value_t::array: {
                const auto sz = static_cast<difference_type>(m_value.array->size());
                if (first.idx_ < 0 || last.idx_ < first.idx_ || last.idx_ > sz) {
                    throw invalid_iterator::create(204, "iterators out of range");
                }
                m_value.array->erase(m_value.array->begin() + first.idx_,
                                     m_value.array->begin() + last.idx_);
                return IteratorType(this, first.idx_);
            }
            default:
                throw type_error::create(
                    307, std::string("cannot use erase() with ") + type_name());
        }
    }

    void swap(reference other) noexcept {
        std::swap(m_type, other.m_type);
        std::swap(m_value, other.m_value);
    }

    void swap(array_t& other) {
        if (is_array()) {
            std::swap(*(m_value.array), other);
        } else {
            throw type_error::create(
                310, std::string("cannot use swap(array_t&) with ") + type_name());
        }
    }

    void swap(object_t& other) {
        if (is_object()) {
            std::swap(*(m_value.object), other);
        } else {
            throw type_error::create(
                310, std::string("cannot use swap(object_t&) with ") + type_name());
        }
    }

    void swap(string_t& other) {
        if (is_string()) {
            std::swap(*(m_value.string), other);
        } else {
            throw type_error::create(
                310, std::string("cannot use swap(string_t&) with ") + type_name());
        }
    }

    friend void swap(json& lhs, json& rhs) noexcept {
        lhs.swap(rhs);
    }

    // ==================== Comparison ====================

    static bool compares_unordered(const_reference lhs, const_reference rhs,
                                   bool inverse = false) noexcept {
        if ((lhs.is_number_float() && std::isnan(lhs.m_value.number_float) && rhs.is_number()) ||
            (rhs.is_number_float() && std::isnan(rhs.m_value.number_float) && lhs.is_number())) {
            return true;
        }
        if (inverse) {
            return lhs.is_discarded() || rhs.is_discarded();
        }
        return lhs.is_discarded() || rhs.is_discarded();
    }

    friend bool operator==(const json& lhs, const json& rhs) noexcept {
        const auto lhs_type = lhs.type();
        const auto rhs_type = rhs.type();
        if (lhs_type == rhs_type) {
            switch (lhs_type) {
                case value_t::array:
                    return *lhs.m_value.array == *rhs.m_value.array;
                case value_t::object:
                    return *lhs.m_value.object == *rhs.m_value.object;
                case value_t::null:
                    return true;
                case value_t::string:
                    return *lhs.m_value.string == *rhs.m_value.string;
                case value_t::boolean:
                    return lhs.m_value.boolean == rhs.m_value.boolean;
                case value_t::number_integer:
                    return lhs.m_value.number_integer == rhs.m_value.number_integer;
                case value_t::number_unsigned:
                    return lhs.m_value.number_unsigned == rhs.m_value.number_unsigned;
                case value_t::number_float:
                    return lhs.m_value.number_float == rhs.m_value.number_float;
                case value_t::binary:
                case value_t::discarded:
                default:
                    return false;
            }
        }
        if (lhs_type == value_t::number_integer && rhs_type == value_t::number_float) {
            return static_cast<number_float_t>(lhs.m_value.number_integer) ==
                   rhs.m_value.number_float;
        }
        if (lhs_type == value_t::number_float && rhs_type == value_t::number_integer) {
            return lhs.m_value.number_float ==
                   static_cast<number_float_t>(rhs.m_value.number_integer);
        }
        if (lhs_type == value_t::number_unsigned && rhs_type == value_t::number_float) {
            return static_cast<number_float_t>(lhs.m_value.number_unsigned) ==
                   rhs.m_value.number_float;
        }
        if (lhs_type == value_t::number_float && rhs_type == value_t::number_unsigned) {
            return lhs.m_value.number_float ==
                   static_cast<number_float_t>(rhs.m_value.number_unsigned);
        }
        if (lhs_type == value_t::number_unsigned && rhs_type == value_t::number_integer) {
            return static_cast<number_integer_t>(lhs.m_value.number_unsigned) ==
                   rhs.m_value.number_integer;
        }
        if (lhs_type == value_t::number_integer && rhs_type == value_t::number_unsigned) {
            return lhs.m_value.number_integer ==
                   static_cast<number_integer_t>(rhs.m_value.number_unsigned);
        }
        if (compares_unordered(lhs, rhs)) {
            return false;
        }
        return false;
    }

    template <typename ScalarType,
              std::enable_if_t<
                  !std::is_same<std::decay_t<ScalarType>, json>::value &&
                      !std::is_same<std::decay_t<ScalarType>, value_t>::value &&
                      !std::is_same<std::decay_t<ScalarType>, detail::json_ref<json>>::value &&
                      (std::is_scalar<std::decay_t<ScalarType>>::value ||
                       std::is_convertible<const ScalarType&, std::string_view>::value),
                  int> = 0>
    friend bool operator==(const json& lhs, const ScalarType& rhs) noexcept {
        return lhs == json(rhs);
    }

    template <typename ScalarType,
              std::enable_if_t<
                  !std::is_same<std::decay_t<ScalarType>, json>::value &&
                      !std::is_same<std::decay_t<ScalarType>, value_t>::value &&
                      !std::is_same<std::decay_t<ScalarType>, detail::json_ref<json>>::value &&
                      (std::is_scalar<std::decay_t<ScalarType>>::value ||
                       std::is_convertible<const ScalarType&, std::string_view>::value),
                  int> = 0>
    friend bool operator==(const ScalarType& lhs, const json& rhs) noexcept {
        return json(lhs) == rhs;
    }

    friend bool operator!=(const json& lhs, const json& rhs) noexcept {
        if (compares_unordered(lhs, rhs, true)) {
            return false;
        }
        return !(lhs == rhs);
    }

    template <typename ScalarType,
              std::enable_if_t<
                  !std::is_same<std::decay_t<ScalarType>, json>::value &&
                      !std::is_same<std::decay_t<ScalarType>, value_t>::value &&
                      !std::is_same<std::decay_t<ScalarType>, detail::json_ref<json>>::value &&
                      (std::is_scalar<std::decay_t<ScalarType>>::value ||
                       std::is_convertible<const ScalarType&, std::string_view>::value),
                  int> = 0>
    friend bool operator!=(const json& lhs, const ScalarType& rhs) noexcept {
        return lhs != json(rhs);
    }

    template <typename ScalarType,
              std::enable_if_t<
                  !std::is_same<std::decay_t<ScalarType>, json>::value &&
                      !std::is_same<std::decay_t<ScalarType>, value_t>::value &&
                      !std::is_same<std::decay_t<ScalarType>, detail::json_ref<json>>::value &&
                      (std::is_scalar<std::decay_t<ScalarType>>::value ||
                       std::is_convertible<const ScalarType&, std::string_view>::value),
                  int> = 0>
    friend bool operator!=(const ScalarType& lhs, const json& rhs) noexcept {
        return json(lhs) != rhs;
    }

    friend bool operator<(const json& lhs, const json& rhs) noexcept {
        const auto lhs_type = lhs.type();
        const auto rhs_type = rhs.type();
        if (lhs_type == rhs_type) {
            switch (lhs_type) {
                case value_t::array:
                    return *lhs.m_value.array < *rhs.m_value.array;
                case value_t::object:
                    return *lhs.m_value.object < *rhs.m_value.object;
                case value_t::null:
                    return false;
                case value_t::string:
                    return *lhs.m_value.string < *rhs.m_value.string;
                case value_t::boolean:
                    return lhs.m_value.boolean < rhs.m_value.boolean;
                case value_t::number_integer:
                    return lhs.m_value.number_integer < rhs.m_value.number_integer;
                case value_t::number_unsigned:
                    return lhs.m_value.number_unsigned < rhs.m_value.number_unsigned;
                case value_t::number_float:
                    return lhs.m_value.number_float < rhs.m_value.number_float;
                case value_t::binary:
                case value_t::discarded:
                default:
                    return false;
            }
        }
        if (lhs_type == value_t::number_integer && rhs_type == value_t::number_float) {
            return static_cast<number_float_t>(lhs.m_value.number_integer) <
                   rhs.m_value.number_float;
        }
        if (lhs_type == value_t::number_float && rhs_type == value_t::number_integer) {
            return lhs.m_value.number_float <
                   static_cast<number_float_t>(rhs.m_value.number_integer);
        }
        if (lhs_type == value_t::number_unsigned && rhs_type == value_t::number_float) {
            return static_cast<number_float_t>(lhs.m_value.number_unsigned) <
                   rhs.m_value.number_float;
        }
        if (lhs_type == value_t::number_float && rhs_type == value_t::number_unsigned) {
            return lhs.m_value.number_float <
                   static_cast<number_float_t>(rhs.m_value.number_unsigned);
        }
        if (lhs_type == value_t::number_unsigned && rhs_type == value_t::number_integer) {
            return static_cast<number_integer_t>(lhs.m_value.number_unsigned) <
                   rhs.m_value.number_integer;
        }
        if (lhs_type == value_t::number_integer && rhs_type == value_t::number_unsigned) {
            return lhs.m_value.number_integer <
                   static_cast<number_integer_t>(rhs.m_value.number_unsigned);
        }
        if (compares_unordered(lhs, rhs)) {
            return false;
        }
        return operator<(lhs_type, rhs_type);
    }

    template <typename ScalarType,
              std::enable_if_t<
                  !std::is_same<std::decay_t<ScalarType>, json>::value &&
                      !std::is_same<std::decay_t<ScalarType>, value_t>::value &&
                      !std::is_same<std::decay_t<ScalarType>, detail::json_ref<json>>::value &&
                      (std::is_scalar<std::decay_t<ScalarType>>::value ||
                       std::is_convertible<const ScalarType&, std::string_view>::value),
                  int> = 0>
    friend bool operator<(const json& lhs, const ScalarType& rhs) noexcept {
        return lhs < json(rhs);
    }

    template <typename ScalarType,
              std::enable_if_t<
                  !std::is_same<std::decay_t<ScalarType>, json>::value &&
                      !std::is_same<std::decay_t<ScalarType>, value_t>::value &&
                      !std::is_same<std::decay_t<ScalarType>, detail::json_ref<json>>::value &&
                      (std::is_scalar<std::decay_t<ScalarType>>::value ||
                       std::is_convertible<const ScalarType&, std::string_view>::value),
                  int> = 0>
    friend bool operator<(const ScalarType& lhs, const json& rhs) noexcept {
        return json(lhs) < rhs;
    }

    friend bool operator<=(const json& lhs, const json& rhs) noexcept {
        if (compares_unordered(lhs, rhs, true)) {
            return false;
        }
        return !(rhs < lhs);
    }

    template <typename ScalarType,
              std::enable_if_t<
                  !std::is_same<std::decay_t<ScalarType>, json>::value &&
                      !std::is_same<std::decay_t<ScalarType>, value_t>::value &&
                      !std::is_same<std::decay_t<ScalarType>, detail::json_ref<json>>::value &&
                      (std::is_scalar<std::decay_t<ScalarType>>::value ||
                       std::is_convertible<const ScalarType&, std::string_view>::value),
                  int> = 0>
    friend bool operator<=(const json& lhs, const ScalarType& rhs) noexcept {
        return lhs <= json(rhs);
    }

    template <typename ScalarType,
              std::enable_if_t<
                  !std::is_same<std::decay_t<ScalarType>, json>::value &&
                      !std::is_same<std::decay_t<ScalarType>, value_t>::value &&
                      !std::is_same<std::decay_t<ScalarType>, detail::json_ref<json>>::value &&
                      (std::is_scalar<std::decay_t<ScalarType>>::value ||
                       std::is_convertible<const ScalarType&, std::string_view>::value),
                  int> = 0>
    friend bool operator<=(const ScalarType& lhs, const json& rhs) noexcept {
        return json(lhs) <= rhs;
    }

    friend bool operator>(const json& lhs, const json& rhs) noexcept {
        if (compares_unordered(lhs, rhs)) {
            return false;
        }
        return !(lhs <= rhs);
    }

    template <typename ScalarType,
              std::enable_if_t<
                  !std::is_same<std::decay_t<ScalarType>, json>::value &&
                      !std::is_same<std::decay_t<ScalarType>, value_t>::value &&
                      !std::is_same<std::decay_t<ScalarType>, detail::json_ref<json>>::value &&
                      (std::is_scalar<std::decay_t<ScalarType>>::value ||
                       std::is_convertible<const ScalarType&, std::string_view>::value),
                  int> = 0>
    friend bool operator>(const json& lhs, const ScalarType& rhs) noexcept {
        return lhs > json(rhs);
    }

    template <typename ScalarType,
              std::enable_if_t<
                  !std::is_same<std::decay_t<ScalarType>, json>::value &&
                      !std::is_same<std::decay_t<ScalarType>, value_t>::value &&
                      !std::is_same<std::decay_t<ScalarType>, detail::json_ref<json>>::value &&
                      (std::is_scalar<std::decay_t<ScalarType>>::value ||
                       std::is_convertible<const ScalarType&, std::string_view>::value),
                  int> = 0>
    friend bool operator>(const ScalarType& lhs, const json& rhs) noexcept {
        return json(lhs) > rhs;
    }

    friend bool operator>=(const json& lhs, const json& rhs) noexcept {
        if (compares_unordered(lhs, rhs, true)) {
            return false;
        }
        return !(lhs < rhs);
    }

    template <typename ScalarType,
              std::enable_if_t<
                  !std::is_same<std::decay_t<ScalarType>, json>::value &&
                      !std::is_same<std::decay_t<ScalarType>, value_t>::value &&
                      !std::is_same<std::decay_t<ScalarType>, detail::json_ref<json>>::value &&
                      (std::is_scalar<std::decay_t<ScalarType>>::value ||
                       std::is_convertible<const ScalarType&, std::string_view>::value),
                  int> = 0>
    friend bool operator>=(const json& lhs, const ScalarType& rhs) noexcept {
        return lhs >= json(rhs);
    }

    template <typename ScalarType,
              std::enable_if_t<
                  !std::is_same<std::decay_t<ScalarType>, json>::value &&
                      !std::is_same<std::decay_t<ScalarType>, value_t>::value &&
                      !std::is_same<std::decay_t<ScalarType>, detail::json_ref<json>>::value &&
                      (std::is_scalar<std::decay_t<ScalarType>>::value ||
                       std::is_convertible<const ScalarType&, std::string_view>::value),
                  int> = 0>
    friend bool operator>=(const ScalarType& lhs, const json& rhs) noexcept {
        return json(lhs) >= rhs;
    }

    // ==================== Dump & Stream Output ====================

    string_t dump(int indent = -1, char indent_char = ' ', bool ensure_ascii = false,
                  error_handler_t error_handler = error_handler_t::strict) const;

    friend std::ostream& operator<<(std::ostream& o, const json& j);

    // ==================== Parse & Accept ====================

    // Every parse and accept call takes a trailing parse_options. Its default allows nesting up to
    // parse_options::default_max_depth levels; a document nested deeper is a parse_error (id 120) when exceptions
    // are allowed, a discarded value when not, and rejected by accept. The stream operator uses the default.
    static json parse(std::string_view s, parser_callback_t cb = nullptr,
                      bool allow_exceptions = true, bool ignore_comments = false,
                      const parse_options& options = {});

    static json parse(std::string_view s, bool allow_exceptions, bool ignore_comments = false,
                      const parse_options& options = {}) {
        return parse(s, nullptr, allow_exceptions, ignore_comments, options);
    }

    static json parse(std::istream& i, parser_callback_t cb = nullptr,
                      bool allow_exceptions = true, bool ignore_comments = false,
                      const parse_options& options = {});

    static json parse(std::istream&& i, parser_callback_t cb = nullptr,
                      bool allow_exceptions = true, bool ignore_comments = false,
                      const parse_options& options = {}) {
        return parse(i, cb, allow_exceptions, ignore_comments, options);
    }

    static json parse(std::istream& i, bool allow_exceptions, bool ignore_comments = false,
                      const parse_options& options = {}) {
        return parse(i, nullptr, allow_exceptions, ignore_comments, options);
    }

    static json parse(std::istream&& i, bool allow_exceptions, bool ignore_comments = false,
                      const parse_options& options = {}) {
        return parse(i, nullptr, allow_exceptions, ignore_comments, options);
    }

    template <typename IteratorType,
              std::enable_if_t<detail::is_InputIt<IteratorType>::value, int> = 0>
    static json parse(IteratorType first, IteratorType last, parser_callback_t cb = nullptr,
                      bool allow_exceptions = true, bool ignore_comments = false,
                      const parse_options& options = {}) {
        std::string buf(first, last);
        return parse(std::string_view(buf), cb, allow_exceptions, ignore_comments, options);
    }

    template <typename IteratorType,
              std::enable_if_t<detail::is_InputIt<IteratorType>::value, int> = 0>
    static json parse(IteratorType first, IteratorType last, bool allow_exceptions,
                      bool ignore_comments = false, const parse_options& options = {}) {
        std::string buf(first, last);
        return parse(std::string_view(buf), nullptr, allow_exceptions, ignore_comments, options);
    }

    static bool accept(std::string_view s, bool ignore_comments = false,
                       const parse_options& options = {});

    static bool accept(std::istream& i, bool ignore_comments = false,
                       const parse_options& options = {});

    static bool accept(std::istream&& i, bool ignore_comments = false,
                       const parse_options& options = {}) {
        return accept(i, ignore_comments, options);
    }

    template <typename IteratorType,
              std::enable_if_t<detail::is_InputIt<IteratorType>::value, int> = 0>
    static bool accept(IteratorType first, IteratorType last, bool ignore_comments = false,
                       const parse_options& options = {}) {
        std::string buf(first, last);
        return accept(std::string_view(buf), ignore_comments, options);
    }

    friend std::istream& operator>>(std::istream& i, json& j) {
        j = parse(i);
        return i;
    }

    // ==================== Conversion ====================

private:
    object_t* get_impl_ptr(object_t* /*unused*/) noexcept {
        return is_object() ? m_value.object : nullptr;
    }
    constexpr const object_t* get_impl_ptr(const object_t* /*unused*/) const noexcept {
        return is_object() ? m_value.object : nullptr;
    }
    array_t* get_impl_ptr(array_t* /*unused*/) noexcept {
        return is_array() ? m_value.array : nullptr;
    }
    constexpr const array_t* get_impl_ptr(const array_t* /*unused*/) const noexcept {
        return is_array() ? m_value.array : nullptr;
    }
    string_t* get_impl_ptr(string_t* /*unused*/) noexcept {
        return is_string() ? m_value.string : nullptr;
    }
    constexpr const string_t* get_impl_ptr(const string_t* /*unused*/) const noexcept {
        return is_string() ? m_value.string : nullptr;
    }
    boolean_t* get_impl_ptr(boolean_t* /*unused*/) noexcept {
        return is_boolean() ? &m_value.boolean : nullptr;
    }
    constexpr const boolean_t* get_impl_ptr(const boolean_t* /*unused*/) const noexcept {
        return is_boolean() ? &m_value.boolean : nullptr;
    }
    number_integer_t* get_impl_ptr(number_integer_t* /*unused*/) noexcept {
        return is_number_integer() ? &m_value.number_integer : nullptr;
    }
    constexpr const number_integer_t* get_impl_ptr(
        const number_integer_t* /*unused*/) const noexcept {
        return is_number_integer() ? &m_value.number_integer : nullptr;
    }
    number_unsigned_t* get_impl_ptr(number_unsigned_t* /*unused*/) noexcept {
        return is_number_unsigned() ? &m_value.number_unsigned : nullptr;
    }
    constexpr const number_unsigned_t* get_impl_ptr(
        const number_unsigned_t* /*unused*/) const noexcept {
        return is_number_unsigned() ? &m_value.number_unsigned : nullptr;
    }
    number_float_t* get_impl_ptr(number_float_t* /*unused*/) noexcept {
        return is_number_float() ? &m_value.number_float : nullptr;
    }
    constexpr const number_float_t* get_impl_ptr(const number_float_t* /*unused*/) const noexcept {
        return is_number_float() ? &m_value.number_float : nullptr;
    }

public:
    template <typename PointerType,
              typename std::enable_if<std::is_pointer<PointerType>::value, int>::type = 0>
    auto get_ptr() noexcept
        -> decltype(std::declval<json&>().get_impl_ptr(std::declval<PointerType>())) {
        return get_impl_ptr(static_cast<PointerType>(nullptr));
    }

    template <typename PointerType,
              typename std::enable_if<
                  std::is_pointer<PointerType>::value &&
                      std::is_const<typename std::remove_pointer<PointerType>::type>::value,
                  int>::type = 0>
    constexpr auto get_ptr() const noexcept
        -> decltype(std::declval<const json&>().get_impl_ptr(std::declval<PointerType>())) {
        return get_impl_ptr(static_cast<PointerType>(nullptr));
    }

    template <typename ReferenceType,
              std::enable_if_t<std::is_reference<ReferenceType>::value, int> = 0>
    ReferenceType get_ref() {
        auto* ptr = get_ptr<std::add_pointer_t<ReferenceType>>();
        if (ptr != nullptr) {
            return *ptr;
        }
        throw type_error::create(
            303, std::string("incompatible ReferenceType for get_ref, actual type is ") +
                     type_name());
    }

    template <typename ReferenceType,
              std::enable_if_t<std::is_reference<ReferenceType>::value &&
                                   std::is_const<std::remove_reference_t<ReferenceType>>::value,
                               int> = 0>
    ReferenceType get_ref() const {
        auto* ptr = get_ptr<std::add_pointer_t<ReferenceType>>();
        if (ptr != nullptr) {
            return *ptr;
        }
        throw type_error::create(
            303, std::string("incompatible ReferenceType for get_ref, actual type is ") +
                     type_name());
    }

private:
    template <typename ValueType,
              detail::enable_if_t<
                  detail::is_default_constructible<ValueType>::value &&
                      detail::has_from_json<json, ValueType>::value,
                  int> = 0>
    ValueType get_impl(detail::priority_tag<0> /*unused*/) const noexcept(noexcept(
        json_serializer<ValueType>::from_json(std::declval<const json&>(),
                                              std::declval<ValueType&>()))) {
        auto ret = ValueType();
        json_serializer<ValueType>::from_json(*this, ret);
        return ret;
    }

    template <typename ValueType,
              detail::enable_if_t<
                  detail::has_non_default_from_json<json, ValueType>::value,
                  int> = 0>
    ValueType get_impl(detail::priority_tag<1> /*unused*/) const noexcept(noexcept(
        json_serializer<ValueType>::from_json(std::declval<const json&>()))) {
        return json_serializer<ValueType>::from_json(*this);
    }

    template <typename BasicJsonType,
              detail::enable_if_t<detail::is_basic_json<BasicJsonType>::value, int> = 0>
    BasicJsonType get_impl(detail::priority_tag<2> /*unused*/) const {
        return *this;
    }

    template <typename BasicJsonType,
              detail::enable_if_t<std::is_same<BasicJsonType, json>::value, int> = 0>
    json get_impl(detail::priority_tag<3> /*unused*/) const {
        return *this;
    }

    template <typename PointerType,
              detail::enable_if_t<std::is_pointer<PointerType>::value, int> = 0>
    constexpr auto get_impl(detail::priority_tag<4> /*unused*/) const noexcept
        -> decltype(std::declval<const json&>().template get_ptr<PointerType>()) {
        return get_ptr<PointerType>();
    }

public:
    template <typename ValueTypeCV, typename ValueType = detail::uncvref_t<ValueTypeCV>>
    constexpr auto get() const noexcept(noexcept(
        std::declval<const json&>().template get_impl<ValueType>(detail::priority_tag<4>{})))
        -> decltype(std::declval<const json&>().template get_impl<ValueType>(
            detail::priority_tag<4>{})) {
        static_assert(!std::is_reference<ValueTypeCV>::value,
                      "get() cannot be used with reference types, you might want to use get_ref()");
        return get_impl<ValueType>(detail::priority_tag<4>{});
    }

    template <typename PointerType,
              typename std::enable_if<std::is_pointer<PointerType>::value, int>::type = 0>
    auto get() noexcept -> decltype(std::declval<json&>().template get_ptr<PointerType>()) {
        return get_ptr<PointerType>();
    }

    template <typename ValueType,
              std::enable_if_t<!std::is_const<ValueType>::value &&
                                   !detail::is_basic_json<ValueType>::value &&
                                   detail::has_from_json<json, ValueType>::value,
                               int> = 0>
    ValueType& get_to(ValueType& v) const noexcept(noexcept(
        json_serializer<ValueType>::from_json(std::declval<const json&>(), v))) {
        json_serializer<ValueType>::from_json(*this, v);
        return v;
    }

    template <typename ValueType,
              std::enable_if_t<detail::is_basic_json<ValueType>::value, int> = 0>
    ValueType& get_to(ValueType& v) const {
        v = *this;
        return v;
    }

    template <typename T, std::size_t N, typename Array = T (&)[N],
              std::enable_if_t<!std::is_const<T>::value &&
                                   detail::has_from_json<json, Array>::value,
                               int> = 0>
    Array get_to(T (&v)[N]) const noexcept(noexcept(
        json_serializer<Array>::from_json(std::declval<const json&>(), v))) {
        json_serializer<Array>::from_json(*this, v);
        return v;
    }

    template <typename ValueType,
              std::enable_if_t<
                  detail::conjunction<
                      detail::negation<std::is_pointer<ValueType>>,
                      detail::negation<std::is_same<ValueType, std::nullptr_t>>,
                      detail::negation<std::is_same<ValueType, std::string_view>>,
                      detail::negation<std::is_same<ValueType, value_t>>,
                      detail::negation<detail::is_basic_json<std::decay_t<ValueType>>>,
                      detail::negation<std::is_same<std::decay_t<ValueType>, detail::json_ref<json>>>,
                      detail::negation<std::is_same<
                          ValueType, std::initializer_list<typename string_t::value_type>>>,
                      // A std::optional is not a target of the implicit conversion (get<std::optional<T>>() still
                      // works): its own constructors from json would otherwise compete with this operator, and
                      // `std::optional<json> x; x = j;` would be ambiguous.
                      detail::negation<detail::is_std_optional<std::decay_t<ValueType>>>,
                      detail::is_detected_lazy<detail::get_template_function, const json&,
                                               ValueType>>::value,
                  int> = 0>
    operator ValueType() const {
        return get<ValueType>();
    }

    // ==================== Flatten / Unflatten / Patch / Diff ====================

    json flatten() const;
    json unflatten() const;
    void patch_inplace(const json& json_patch);
    json patch(const json& json_patch) const;
    static json diff(const json& source, const json& target, const std::string& path = "");
    void merge_patch(const json& apply_patch);

private:
    value_t m_type = value_t::null;
    json_value m_value{};
};

static_assert(sizeof(json) == 16, "neojson::json must be 16 bytes");
static_assert(std::is_nothrow_move_constructible<std::pair<std::string, json>>::value,
              "ordered_map value_type must be nothrow move-constructible");

// Out-of-line json member functions that depend on adl_serializer

inline json::string_t json::value(const typename object_t::key_type& key,
                                  const char* default_value) const {
    return value(key, string_t(default_value));
}

inline json::string_t json::value(const json_pointer& ptr, const char* default_value) const {
    return value(ptr, string_t(default_value));
}

inline json json::flatten() const {
    json result(value_t::object);
    json_pointer::flatten("", *this, result);
    return result;
}

inline json json::unflatten() const {
    return json_pointer::unflatten(*this);
}

inline void json::patch_inplace(const json& json_patch) {
    json& result = *this;

    enum class patch_operations { add, remove, replace, move, copy, test, invalid };

    const auto get_op = [](const std::string& op) {
        if (op == "add") {
            return patch_operations::add;
        }
        if (op == "remove") {
            return patch_operations::remove;
        }
        if (op == "replace") {
            return patch_operations::replace;
        }
        if (op == "move") {
            return patch_operations::move;
        }
        if (op == "copy") {
            return patch_operations::copy;
        }
        if (op == "test") {
            return patch_operations::test;
        }
        return patch_operations::invalid;
    };

    const auto operation_add = [&result](json_pointer& ptr, json val) {
        if (ptr.empty()) {
            result = std::move(val);
            return;
        }

        auto last_path = ptr.back();
        ptr.pop_back();

        json& parent = result.at(ptr);

        switch (parent.m_type) {
            case value_t::null:
            case value_t::object: {
                parent[last_path] = std::move(val);
                break;
            }

            case value_t::array: {
                if (last_path == "-") {
                    parent.push_back(std::move(val));
                } else {
                    const auto idx = json_pointer::array_index(last_path);
                    if (idx > parent.size()) {
                        throw out_of_range::create(
                            401, "array index " + std::to_string(idx) + " is out of range");
                    }
                    parent.insert(parent.begin() + static_cast<difference_type>(idx),
                                  std::move(val));
                }
                break;
            }

            default:
                assert(false);
        }
    };

    const auto operation_remove = [&result](json_pointer& ptr) {
        if (ptr.empty()) {
            throw out_of_range::create(405, "JSON pointer has no parent");
        }

        auto last_path = ptr.back();
        ptr.pop_back();

        json& parent = result.at(ptr);

        if (parent.is_object()) {
            auto it = parent.find(last_path);
            if (it != parent.end()) {
                parent.erase(it);
            } else {
                throw out_of_range::create(403, "key '" + last_path + "' not found");
            }
        } else if (parent.is_array()) {
            parent.erase(json_pointer::array_index(last_path));
        }
    };

    if (!json_patch.is_array()) {
        throw parse_error::create(104, 0, "JSON patch must be an array of objects");
    }

    for (const auto& val : json_patch) {
        if (!val.is_object()) {
            throw parse_error::create(104, 0, "JSON patch must be an array of objects");
        }

        const auto get_member = [&val](const std::string& op, const std::string& member,
                                       bool string_type) -> const json& {
            auto it = val.m_value.object->find(member);
            const std::string error_msg =
                (op == "op") ? "operation" : ("operation '" + op + "'");
            if (it == val.m_value.object->end()) {
                throw parse_error::create(
                    105, 0, error_msg + " must have member '" + member + "'");
            }
            if (string_type && !it->second.is_string()) {
                throw parse_error::create(
                    105, 0, error_msg + " must have string member '" + member + "'");
            }
            return it->second;
        };

        const std::string op = *get_member("op", "op", true).m_value.string;
        const std::string path = *get_member(op, "path", true).m_value.string;
        json_pointer ptr(path);

        switch (get_op(op)) {
            case patch_operations::add: {
                operation_add(ptr, get_member("add", "value", false));
                break;
            }

            case patch_operations::remove: {
                operation_remove(ptr);
                break;
            }

            case patch_operations::replace: {
                result.at(ptr) = get_member("replace", "value", false);
                break;
            }

            case patch_operations::move: {
                const std::string from_path = *get_member("move", "from", true).m_value.string;
                json_pointer from_ptr(from_path);

                json v = result.at(from_ptr);

                operation_remove(from_ptr);
                operation_add(ptr, std::move(v));
                break;
            }

            case patch_operations::copy: {
                const std::string from_path = *get_member("copy", "from", true).m_value.string;
                const json_pointer from_ptr(from_path);

                json v = result.at(from_ptr);
                operation_add(ptr, std::move(v));
                break;
            }

            case patch_operations::test: {
                bool success = false;
                try {
                    success = (result.at(ptr) == get_member("test", "value", false));
                } catch (out_of_range&) {
                }

                if (!success) {
                    throw other_error::create(501, "unsuccessful: " + val.dump());
                }

                break;
            }

            case patch_operations::invalid: {
                throw parse_error::create(105, 0, "operation value '" + op + "' is invalid");
            }
        }
    }
}

inline json json::patch(const json& json_patch) const {
    json result = *this;
    result.patch_inplace(json_patch);
    return result;
}

inline json json::diff(const json& source, const json& target, const std::string& path) {
    json result(value_t::array);

    if (source == target) {
        return result;
    }

    if (source.type() != target.type()) {
        result.push_back({{"op", "replace"}, {"path", path}, {"value", target}});
        return result;
    }

    switch (source.type()) {
        case value_t::array: {
            std::size_t i = 0;
            while (i < source.size() && i < target.size()) {
                json temp_diff = diff(source[i], target[i], path + "/" + std::to_string(i));
                result.insert(result.end(), temp_diff.begin(), temp_diff.end());
                ++i;
            }

            // Remove remaining elements in reverse order to avoid invalidating earlier indices
            const auto end_index = static_cast<difference_type>(result.size());
            while (i < source.size()) {
                result.insert(
                    result.begin() + end_index,
                    object({{"op", "remove"},
                            {"path", path + "/" + std::to_string(i)}}));
                ++i;
            }

            // Add other remaining elements
            while (i < target.size()) {
                result.push_back({{"op", "add"},
                                  {"path", path + "/-"},
                                  {"value", target[i]}});
                ++i;
            }

            break;
        }

        case value_t::object: {
            for (auto it = source.cbegin(); it != source.cend(); ++it) {
                const auto path_key = path + "/" + detail::escape(it.key());

                if (target.find(it.key()) != target.end()) {
                    json temp_diff = diff(it.value(), target[it.key()], path_key);
                    result.insert(result.end(), temp_diff.begin(), temp_diff.end());
                } else {
                    result.push_back(object({{"op", "remove"}, {"path", path_key}}));
                }
            }

            for (auto it = target.cbegin(); it != target.cend(); ++it) {
                if (source.find(it.key()) == source.end()) {
                    const auto path_key = path + "/" + detail::escape(it.key());
                    result.push_back(
                        {{"op", "add"}, {"path", path_key}, {"value", it.value()}});
                }
            }

            break;
        }

        default: {
            result.push_back({{"op", "replace"}, {"path", path}, {"value", target}});
            break;
        }
    }

    return result;
}

inline void json::merge_patch(const json& apply_patch) {
    if (apply_patch.is_object()) {
        if (!is_object()) {
            *this = object();
        }
        for (auto it = apply_patch.begin(); it != apply_patch.end(); ++it) {
            if (it.value().is_null()) {
                erase(it.key());
            } else {
                operator[](it.key()).merge_patch(it.value());
            }
        }
    } else {
        *this = apply_patch;
    }
}

inline namespace literals {
inline namespace json_literals {

inline json operator""_json(const char* s, std::size_t n) {
    return json::parse(std::string_view(s, n));
}

inline json::json_pointer operator""_json_pointer(const char* s, std::size_t n) {
    return json::json_pointer(std::string(s, n));
}

} // namespace json_literals
} // namespace literals

} // namespace neojson

// ==================== User-Type Macros ====================

#define NEOJSON_JSON_EXPAND(x) x
#define NEOJSON_JSON_GET_MACRO(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, _33, _34, _35, _36, _37, _38, _39, _40, _41, _42, _43, _44, _45, _46, _47, _48, _49, _50, _51, _52, _53, _54, _55, _56, _57, _58, _59, _60, _61, _62, _63, _64, NAME, ...) NAME
#define NEOJSON_JSON_PASTE(...) NEOJSON_JSON_EXPAND(NEOJSON_JSON_GET_MACRO(__VA_ARGS__, \
    NEOJSON_JSON_PASTE64, NEOJSON_JSON_PASTE63, NEOJSON_JSON_PASTE62, NEOJSON_JSON_PASTE61, \
    NEOJSON_JSON_PASTE60, NEOJSON_JSON_PASTE59, NEOJSON_JSON_PASTE58, NEOJSON_JSON_PASTE57, \
    NEOJSON_JSON_PASTE56, NEOJSON_JSON_PASTE55, NEOJSON_JSON_PASTE54, NEOJSON_JSON_PASTE53, \
    NEOJSON_JSON_PASTE52, NEOJSON_JSON_PASTE51, NEOJSON_JSON_PASTE50, NEOJSON_JSON_PASTE49, \
    NEOJSON_JSON_PASTE48, NEOJSON_JSON_PASTE47, NEOJSON_JSON_PASTE46, NEOJSON_JSON_PASTE45, \
    NEOJSON_JSON_PASTE44, NEOJSON_JSON_PASTE43, NEOJSON_JSON_PASTE42, NEOJSON_JSON_PASTE41, \
    NEOJSON_JSON_PASTE40, NEOJSON_JSON_PASTE39, NEOJSON_JSON_PASTE38, NEOJSON_JSON_PASTE37, \
    NEOJSON_JSON_PASTE36, NEOJSON_JSON_PASTE35, NEOJSON_JSON_PASTE34, NEOJSON_JSON_PASTE33, \
    NEOJSON_JSON_PASTE32, NEOJSON_JSON_PASTE31, NEOJSON_JSON_PASTE30, NEOJSON_JSON_PASTE29, \
    NEOJSON_JSON_PASTE28, NEOJSON_JSON_PASTE27, NEOJSON_JSON_PASTE26, NEOJSON_JSON_PASTE25, \
    NEOJSON_JSON_PASTE24, NEOJSON_JSON_PASTE23, NEOJSON_JSON_PASTE22, NEOJSON_JSON_PASTE21, \
    NEOJSON_JSON_PASTE20, NEOJSON_JSON_PASTE19, NEOJSON_JSON_PASTE18, NEOJSON_JSON_PASTE17, \
    NEOJSON_JSON_PASTE16, NEOJSON_JSON_PASTE15, NEOJSON_JSON_PASTE14, NEOJSON_JSON_PASTE13, \
    NEOJSON_JSON_PASTE12, NEOJSON_JSON_PASTE11, NEOJSON_JSON_PASTE10, NEOJSON_JSON_PASTE9, \
    NEOJSON_JSON_PASTE8, NEOJSON_JSON_PASTE7, NEOJSON_JSON_PASTE6, NEOJSON_JSON_PASTE5, \
    NEOJSON_JSON_PASTE4, NEOJSON_JSON_PASTE3, NEOJSON_JSON_PASTE2, NEOJSON_JSON_PASTE1)(__VA_ARGS__))
#define NEOJSON_JSON_PASTE2(func, v1) func(v1)
#define NEOJSON_JSON_PASTE3(func, v1, v2) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE2(func, v2)
#define NEOJSON_JSON_PASTE4(func, v1, v2, v3) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE3(func, v2, v3)
#define NEOJSON_JSON_PASTE5(func, v1, v2, v3, v4) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE4(func, v2, v3, v4)
#define NEOJSON_JSON_PASTE6(func, v1, v2, v3, v4, v5) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE5(func, v2, v3, v4, v5)
#define NEOJSON_JSON_PASTE7(func, v1, v2, v3, v4, v5, v6) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE6(func, v2, v3, v4, v5, v6)
#define NEOJSON_JSON_PASTE8(func, v1, v2, v3, v4, v5, v6, v7) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE7(func, v2, v3, v4, v5, v6, v7)
#define NEOJSON_JSON_PASTE9(func, v1, v2, v3, v4, v5, v6, v7, v8) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE8(func, v2, v3, v4, v5, v6, v7, v8)
#define NEOJSON_JSON_PASTE10(func, v1, v2, v3, v4, v5, v6, v7, v8, v9) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE9(func, v2, v3, v4, v5, v6, v7, v8, v9)
#define NEOJSON_JSON_PASTE11(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE10(func, v2, v3, v4, v5, v6, v7, v8, v9, v10)
#define NEOJSON_JSON_PASTE12(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE11(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11)
#define NEOJSON_JSON_PASTE13(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE12(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12)
#define NEOJSON_JSON_PASTE14(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE13(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13)
#define NEOJSON_JSON_PASTE15(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE14(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14)
#define NEOJSON_JSON_PASTE16(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE15(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15)
#define NEOJSON_JSON_PASTE17(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE16(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16)
#define NEOJSON_JSON_PASTE18(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE17(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17)
#define NEOJSON_JSON_PASTE19(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE18(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18)
#define NEOJSON_JSON_PASTE20(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE19(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19)
#define NEOJSON_JSON_PASTE21(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE20(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20)
#define NEOJSON_JSON_PASTE22(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE21(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21)
#define NEOJSON_JSON_PASTE23(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE22(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22)
#define NEOJSON_JSON_PASTE24(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE23(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23)
#define NEOJSON_JSON_PASTE25(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE24(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24)
#define NEOJSON_JSON_PASTE26(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE25(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25)
#define NEOJSON_JSON_PASTE27(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE26(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26)
#define NEOJSON_JSON_PASTE28(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE27(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27)
#define NEOJSON_JSON_PASTE29(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE28(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28)
#define NEOJSON_JSON_PASTE30(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE29(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29)
#define NEOJSON_JSON_PASTE31(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE30(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30)
#define NEOJSON_JSON_PASTE32(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE31(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31)
#define NEOJSON_JSON_PASTE33(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE32(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32)
#define NEOJSON_JSON_PASTE34(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE33(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33)
#define NEOJSON_JSON_PASTE35(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE34(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34)
#define NEOJSON_JSON_PASTE36(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE35(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35)
#define NEOJSON_JSON_PASTE37(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE36(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36)
#define NEOJSON_JSON_PASTE38(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE37(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37)
#define NEOJSON_JSON_PASTE39(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE38(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38)
#define NEOJSON_JSON_PASTE40(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE39(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39)
#define NEOJSON_JSON_PASTE41(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE40(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40)
#define NEOJSON_JSON_PASTE42(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE41(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41)
#define NEOJSON_JSON_PASTE43(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE42(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42)
#define NEOJSON_JSON_PASTE44(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE43(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43)
#define NEOJSON_JSON_PASTE45(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE44(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44)
#define NEOJSON_JSON_PASTE46(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE45(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45)
#define NEOJSON_JSON_PASTE47(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE46(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46)
#define NEOJSON_JSON_PASTE48(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE47(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47)
#define NEOJSON_JSON_PASTE49(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE48(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48)
#define NEOJSON_JSON_PASTE50(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE49(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49)
#define NEOJSON_JSON_PASTE51(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE50(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50)
#define NEOJSON_JSON_PASTE52(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE51(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51)
#define NEOJSON_JSON_PASTE53(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE52(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52)
#define NEOJSON_JSON_PASTE54(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE53(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53)
#define NEOJSON_JSON_PASTE55(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE54(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54)
#define NEOJSON_JSON_PASTE56(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE55(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55)
#define NEOJSON_JSON_PASTE57(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE56(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56)
#define NEOJSON_JSON_PASTE58(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56, v57) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE57(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56, v57)
#define NEOJSON_JSON_PASTE59(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56, v57, v58) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE58(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56, v57, v58)
#define NEOJSON_JSON_PASTE60(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56, v57, v58, v59) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE59(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56, v57, v58, v59)
#define NEOJSON_JSON_PASTE61(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56, v57, v58, v59, v60) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE60(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56, v57, v58, v59, v60)
#define NEOJSON_JSON_PASTE62(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56, v57, v58, v59, v60, v61) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE61(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56, v57, v58, v59, v60, v61)
#define NEOJSON_JSON_PASTE63(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56, v57, v58, v59, v60, v61, v62) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE62(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56, v57, v58, v59, v60, v61, v62)
#define NEOJSON_JSON_PASTE64(func, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56, v57, v58, v59, v60, v61, v62, v63) NEOJSON_JSON_PASTE2(func, v1) NEOJSON_JSON_PASTE63(func, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56, v57, v58, v59, v60, v61, v62, v63)

#define NEOJSON_JSON_TO(v1) neojson_json_j[#v1] = neojson_json_t.v1;
#define NEOJSON_JSON_FROM(v1) neojson_json_j.at(#v1).get_to(neojson_json_t.v1);
#define NEOJSON_JSON_FROM_WITH_DEFAULT(v1) \
    neojson_json_t.v1 = neojson_json_j.value(#v1, neojson_json_default_obj.v1);

#define NEOJSON_DEFINE_TYPE_INTRUSIVE(Type, ...) \
    friend void to_json(::neojson::json& neojson_json_j, const Type& neojson_json_t) { \
        NEOJSON_JSON_EXPAND(NEOJSON_JSON_PASTE(NEOJSON_JSON_TO, __VA_ARGS__)) \
    } \
    friend void from_json(const ::neojson::json& neojson_json_j, Type& neojson_json_t) { \
        NEOJSON_JSON_EXPAND(NEOJSON_JSON_PASTE(NEOJSON_JSON_FROM, __VA_ARGS__)) \
    }

#define NEOJSON_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Type, ...) \
    friend void to_json(::neojson::json& neojson_json_j, const Type& neojson_json_t) { \
        NEOJSON_JSON_EXPAND(NEOJSON_JSON_PASTE(NEOJSON_JSON_TO, __VA_ARGS__)) \
    } \
    friend void from_json(const ::neojson::json& neojson_json_j, Type& neojson_json_t) { \
        const Type neojson_json_default_obj{}; \
        NEOJSON_JSON_EXPAND(NEOJSON_JSON_PASTE(NEOJSON_JSON_FROM_WITH_DEFAULT, __VA_ARGS__)) \
    }

#define NEOJSON_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(Type, ...) \
    friend void to_json(::neojson::json& neojson_json_j, const Type& neojson_json_t) { \
        NEOJSON_JSON_EXPAND(NEOJSON_JSON_PASTE(NEOJSON_JSON_TO, __VA_ARGS__)) \
    }

#define NEOJSON_DEFINE_TYPE_NON_INTRUSIVE(Type, ...) \
    inline void to_json(::neojson::json& neojson_json_j, const Type& neojson_json_t) { \
        NEOJSON_JSON_EXPAND(NEOJSON_JSON_PASTE(NEOJSON_JSON_TO, __VA_ARGS__)) \
    } \
    inline void from_json(const ::neojson::json& neojson_json_j, Type& neojson_json_t) { \
        NEOJSON_JSON_EXPAND(NEOJSON_JSON_PASTE(NEOJSON_JSON_FROM, __VA_ARGS__)) \
    }

#define NEOJSON_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Type, ...) \
    inline void to_json(::neojson::json& neojson_json_j, const Type& neojson_json_t) { \
        NEOJSON_JSON_EXPAND(NEOJSON_JSON_PASTE(NEOJSON_JSON_TO, __VA_ARGS__)) \
    } \
    inline void from_json(const ::neojson::json& neojson_json_j, Type& neojson_json_t) { \
        const Type neojson_json_default_obj{}; \
        NEOJSON_JSON_EXPAND(NEOJSON_JSON_PASTE(NEOJSON_JSON_FROM_WITH_DEFAULT, __VA_ARGS__)) \
    }

#define NEOJSON_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(Type, ...) \
    inline void to_json(::neojson::json& neojson_json_j, const Type& neojson_json_t) { \
        NEOJSON_JSON_EXPAND(NEOJSON_JSON_PASTE(NEOJSON_JSON_TO, __VA_ARGS__)) \
    }

#define NEOJSON_JSON_SERIALIZE_ENUM(ENUM_TYPE, ...) \
    template <typename BasicJsonType> \
    inline void to_json(BasicJsonType& j, const ENUM_TYPE& e) { \
        static_assert(std::is_enum<ENUM_TYPE>::value, #ENUM_TYPE " must be an enum!"); \
        static const std::pair<ENUM_TYPE, BasicJsonType> m[] = __VA_ARGS__; \
        auto it = std::find_if(std::begin(m), std::end(m), \
                               [e](const std::pair<ENUM_TYPE, BasicJsonType>& ej_pair) -> bool { \
                                   return ej_pair.first == e; \
                               }); \
        j = ((it != std::end(m)) ? it : std::begin(m))->second; \
    } \
    template <typename BasicJsonType> \
    inline void from_json(const BasicJsonType& j, ENUM_TYPE& e) { \
        static_assert(std::is_enum<ENUM_TYPE>::value, #ENUM_TYPE " must be an enum!"); \
        static const std::pair<ENUM_TYPE, BasicJsonType> m[] = __VA_ARGS__; \
        auto it = std::find_if(std::begin(m), std::end(m), \
                               [&j](const std::pair<ENUM_TYPE, BasicJsonType>& ej_pair) -> bool { \
                                   return ej_pair.second == j; \
                               }); \
        e = ((it != std::end(m)) ? it : std::begin(m))->first; \
    }

#define NEOJSON_SERIALIZE_ENUM(ENUM_TYPE, ...) \
    NEOJSON_JSON_SERIALIZE_ENUM(ENUM_TYPE, __VA_ARGS__)

namespace std {

template <typename IteratorType>
struct tuple_size<::neojson::detail::iteration_proxy_value<IteratorType>>
    : std::integral_constant<std::size_t, 2> {};

template <typename IteratorType>
struct tuple_element<0, ::neojson::detail::iteration_proxy_value<IteratorType>> {
    using type = const std::string&;
};

template <typename IteratorType>
struct tuple_element<1, ::neojson::detail::iteration_proxy_value<IteratorType>> {
    using type = typename IteratorType::reference;
};

template <>
inline void swap<::neojson::json>(::neojson::json& j1, ::neojson::json& j2) noexcept {
    j1.swap(j2);
}

} // namespace std

namespace neojson {
namespace detail {

template <std::size_t N, typename IteratorType, std::enable_if_t<N == 0, int> = 0>
auto get(const iteration_proxy_value<IteratorType>& i) -> decltype(i.key()) {
    return i.key();
}

template <std::size_t N, typename IteratorType, std::enable_if_t<N == 1, int> = 0>
auto get(const iteration_proxy_value<IteratorType>& i) -> decltype(i.value()) {
    return i.value();
}

} // namespace detail
} // namespace neojson

#endif // NEOJSON_JSON_HPP_
