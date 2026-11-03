#ifndef JSONKIT_JSON_HPP_
#define JSONKIT_JSON_HPP_

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

namespace jsonkit {

// Forward declarations
class json;
using ordered_json = json;

namespace detail {

// Tuning constant: object size above which a lookup index may be built.
// To be set from the benchmark.
inline constexpr std::size_t kObjectIndexThreshold = 64;

// Tuning constant: initial byte capacity reserved by json::dump().
// To be set from the benchmark.
inline constexpr std::size_t kDumpInitialBufferCapacity = 256;

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

} // namespace detail

template <class Key, class T, class IgnoredLess = std::less<Key>,
          class Allocator = std::allocator<std::pair<const Key, T>>>
struct ordered_map : std::vector<std::pair<const Key, T>, Allocator> {
    using key_type = Key;
    using mapped_type = T;
    using Container = std::vector<std::pair<const Key, T>, Allocator>;
    using iterator = typename Container::iterator;
    using const_iterator = typename Container::const_iterator;
    using size_type = typename Container::size_type;
    using value_type = typename Container::value_type;
    using key_compare = std::equal_to<Key>;

    ordered_map() noexcept(noexcept(Container())) : Container() {}
    explicit ordered_map(const Allocator& alloc) noexcept(noexcept(Container(alloc)))
        : Container(alloc) {}

    template <class It, std::enable_if_t<detail::is_InputIt<It>::value, int> = 0>
    ordered_map(It first, It last, const Allocator& alloc = Allocator())
        : Container(first, last, alloc) {}

    ordered_map(std::initializer_list<value_type> init, const Allocator& alloc = Allocator())
        : Container(init, alloc) {}

    template <class KeyType, class... Args,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value, int> = 0>
    std::pair<iterator, bool> emplace(KeyType&& key, Args&&... args) {
        std::string_view sv(key);
        for (auto it = this->begin(); it != this->end(); ++it) {
            if (it->first == sv) {
                return {it, false};
            }
        }
        Container::emplace_back(
            std::piecewise_construct,
            std::forward_as_tuple(std::forward<KeyType>(key)),
            std::forward_as_tuple(std::forward<Args>(args)...));
        return {std::prev(this->end()), true};
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
        for (auto it = this->begin(); it != this->end(); ++it) {
            if (it->first == key) {
                return it->second;
            }
        }
        throw std::out_of_range("key not found");
    }

    template <class KeyType,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value &&
                                   !std::is_same<std::decay_t<KeyType>, key_type>::value,
                               int> = 0>
    T& at(KeyType&& key) {
        std::string_view sv(key);
        for (auto it = this->begin(); it != this->end(); ++it) {
            if (it->first == sv) {
                return it->second;
            }
        }
        throw std::out_of_range("key not found");
    }

    const T& at(const key_type& key) const {
        for (auto it = this->begin(); it != this->end(); ++it) {
            if (it->first == key) {
                return it->second;
            }
        }
        throw std::out_of_range("key not found");
    }

    template <class KeyType,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value &&
                                   !std::is_same<std::decay_t<KeyType>, key_type>::value,
                               int> = 0>
    const T& at(KeyType&& key) const {
        std::string_view sv(key);
        for (auto it = this->begin(); it != this->end(); ++it) {
            if (it->first == sv) {
                return it->second;
            }
        }
        throw std::out_of_range("key not found");
    }

    size_type erase(const key_type& key) {
        for (auto it = this->begin(); it != this->end(); ++it) {
            if (it->first == key) {
                for (auto next = it; ++next != this->end(); ++it) {
                    it->~value_type();
                    new (&*it) value_type(std::move(*next));
                }
                Container::pop_back();
                return 1;
            }
        }
        return 0;
    }

    template <class KeyType,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value &&
                                   !std::is_same<std::decay_t<KeyType>, key_type>::value &&
                                   !std::is_same<std::decay_t<KeyType>, iterator>::value &&
                                   !std::is_same<std::decay_t<KeyType>, const_iterator>::value,
                               int> = 0>
    size_type erase(KeyType&& key) {
        std::string_view sv(key);
        for (auto it = this->begin(); it != this->end(); ++it) {
            if (it->first == sv) {
                for (auto next = it; ++next != this->end(); ++it) {
                    it->~value_type();
                    new (&*it) value_type(std::move(*next));
                }
                Container::pop_back();
                return 1;
            }
        }
        return 0;
    }

    iterator erase(iterator pos) {
        return erase(pos, std::next(pos));
    }

    iterator erase(const_iterator pos) {
        return erase(pos, std::next(pos));
    }

    iterator erase(const_iterator first, const_iterator last) {
        if (first == last) {
            return this->begin() + std::distance(this->cbegin(), first);
        }
        const auto elements_affected = std::distance(first, last);
        const auto offset = std::distance(this->cbegin(), first);
        for (auto it = this->begin() + offset; it + elements_affected != this->end(); ++it) {
            it->~value_type();
            new (&*it) value_type(std::move(*(it + elements_affected)));
        }
        for (std::ptrdiff_t i = 0; i < elements_affected; ++i) {
            Container::pop_back();
        }
        return this->begin() + offset;
    }

    size_type count(const key_type& key) const {
        for (auto it = this->begin(); it != this->end(); ++it) {
            if (it->first == key) {
                return 1;
            }
        }
        return 0;
    }

    template <class KeyType,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value &&
                                   !std::is_same<std::decay_t<KeyType>, key_type>::value,
                               int> = 0>
    size_type count(KeyType&& key) const {
        std::string_view sv(key);
        for (auto it = this->begin(); it != this->end(); ++it) {
            if (it->first == sv) {
                return 1;
            }
        }
        return 0;
    }

    iterator find(const key_type& key) {
        for (auto it = this->begin(); it != this->end(); ++it) {
            if (it->first == key) {
                return it;
            }
        }
        return Container::end();
    }

    template <class KeyType,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value &&
                                   !std::is_same<std::decay_t<KeyType>, key_type>::value,
                               int> = 0>
    iterator find(KeyType&& key) {
        std::string_view sv(key);
        for (auto it = this->begin(); it != this->end(); ++it) {
            if (it->first == sv) {
                return it;
            }
        }
        return Container::end();
    }

    const_iterator find(const key_type& key) const {
        for (auto it = this->begin(); it != this->end(); ++it) {
            if (it->first == key) {
                return it;
            }
        }
        return Container::end();
    }

    template <class KeyType,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value &&
                                   !std::is_same<std::decay_t<KeyType>, key_type>::value,
                               int> = 0>
    const_iterator find(KeyType&& key) const {
        std::string_view sv(key);
        for (auto it = this->begin(); it != this->end(); ++it) {
            if (it->first == sv) {
                return it;
            }
        }
        return Container::end();
    }

    bool contains(const key_type& key) const {
        return find(key) != this->end();
    }

    template <class KeyType,
              std::enable_if_t<std::is_convertible<const KeyType&, std::string_view>::value &&
                                   !std::is_same<std::decay_t<KeyType>, key_type>::value,
                               int> = 0>
    bool contains(KeyType&& key) const {
        return find(std::forward<KeyType>(key)) != this->end();
    }

    std::pair<iterator, bool> insert(value_type&& value) {
        return emplace(value.first, std::move(value.second));
    }

    std::pair<iterator, bool> insert(const value_type& value) {
        for (auto it = this->begin(); it != this->end(); ++it) {
            if (it->first == value.first) {
                return {it, false};
            }
        }
        Container::push_back(value);
        return {std::prev(this->end()), true};
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

    const string_type& key() const {
        assert(anchor.m_object != nullptr);
        switch (anchor.m_object->type()) {
            case value_t::array: {
                if (array_index != array_index_last) {
                    array_index_str = std::to_string(array_index);
                    array_index_last = array_index;
                }
                return array_index_str;
            }
            case value_t::object:
                return anchor.key();
            default:
                return empty_str;
        }
    }

    typename IteratorType::reference value() const {
        return anchor.value();
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
        if (m_object == nullptr) {
            throw invalid_iterator::create(214, "cannot get value");
        }
        switch (m_object->m_type) {
            case value_t::object: {
                if (idx_ < 0 ||
                    static_cast<std::size_t>(idx_) >= m_object->m_value.object->size()) {
                    throw invalid_iterator::create(214, "cannot get value");
                }
                return (*m_object->m_value.object)[static_cast<std::size_t>(idx_)].second;
            }
            case value_t::array: {
                if (idx_ < 0 ||
                    static_cast<std::size_t>(idx_) >= m_object->m_value.array->size()) {
                    throw invalid_iterator::create(214, "cannot get value");
                }
                return (*m_object->m_value.array)[static_cast<std::size_t>(idx_)];
            }
            case value_t::null:
                throw invalid_iterator::create(214, "cannot get value");
            default:
                if (idx_ == 0) {
                    return *m_object;
                }
                throw invalid_iterator::create(214, "cannot get value");
        }
    }

    pointer operator->() const {
        if (m_object == nullptr) {
            throw invalid_iterator::create(214, "cannot get value");
        }
        switch (m_object->m_type) {
            case value_t::object: {
                if (idx_ < 0 ||
                    static_cast<std::size_t>(idx_) >= m_object->m_value.object->size()) {
                    throw invalid_iterator::create(214, "cannot get value");
                }
                return &((*m_object->m_value.object)[static_cast<std::size_t>(idx_)].second);
            }
            case value_t::array: {
                if (idx_ < 0 ||
                    static_cast<std::size_t>(idx_) >= m_object->m_value.array->size()) {
                    throw invalid_iterator::create(214, "cannot get value");
                }
                return &((*m_object->m_value.array)[static_cast<std::size_t>(idx_)]);
            }
            case value_t::null:
                throw invalid_iterator::create(214, "cannot get value");
            default:
                if (idx_ == 0) {
                    return m_object;
                }
                throw invalid_iterator::create(214, "cannot get value");
        }
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
        if (m_object == nullptr || m_object->m_type != value_t::object) {
            throw invalid_iterator::create(207, "cannot use key() for non-object iterators");
        }
        if (idx_ < 0 || static_cast<std::size_t>(idx_) >= m_object->m_value.object->size()) {
            throw invalid_iterator::create(214, "cannot get value");
        }
        return (*m_object->m_value.object)[static_cast<std::size_t>(idx_)].first;
    }

    reference value() const {
        return operator*();
    }

private:
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

// Traits for container and conversion detection
template <typename T>
struct is_std_array : std::false_type {};

template <typename T, std::size_t N>
struct is_std_array<std::array<T, N>> : std::true_type {};

template <typename T>
struct is_std_pair : std::false_type {};

template <typename T1, typename T2>
struct is_std_pair<std::pair<T1, T2>> : std::true_type {};

template <typename T>
struct is_std_tuple : std::false_type {};

template <typename... Args>
struct is_std_tuple<std::tuple<Args...>> : std::true_type {};

template <typename T, typename = void>
struct is_map_like_impl : std::false_type {};

template <typename T>
struct is_map_like_impl<
    T,
    std::void_t<typename T::key_type,
                typename T::mapped_type,
                decltype(std::declval<const T&>().begin()),
                decltype(std::declval<const T&>().end())>>
    : std::integral_constant<bool, std::is_constructible<std::string, typename T::key_type>::value> {};

template <typename T>
struct is_map_like : is_map_like_impl<T> {};

template <>
struct is_map_like<json> : std::false_type {};

template <typename B>
struct is_map_like<json_ref<B>> : std::false_type {};

template <typename T, typename = void>
struct is_sequence_like_impl : std::false_type {};

template <typename T>
struct is_sequence_like_impl<
    T,
    std::void_t<typename T::value_type,
                decltype(std::begin(std::declval<const T&>())),
                decltype(std::end(std::declval<const T&>()))>>
    : std::integral_constant<
          bool,
          !is_map_like<T>::value &&
              !std::is_convertible<const T&, std::string_view>::value &&
              !std::is_same<std::decay_t<T>, json>::value> {};

template <typename T>
struct is_sequence_like : is_sequence_like_impl<T> {};

template <>
struct is_sequence_like<json> : std::false_type {};

template <typename B>
struct is_sequence_like<json_ref<B>> : std::false_type {};

template <>
struct is_sequence_like<std::string> : std::false_type {};

template <>
struct is_sequence_like<std::string_view> : std::false_type {};

} // namespace detail

class json {
private:
    template <typename BasicJsonType>
    friend class detail::iter_impl;

public:
    using value_t = detail::value_t;
    using error_handler_t = detail::error_handler_t;

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
    using object_t = ordered_map<std::string, json, object_comparator_t,
                                 std::allocator<std::pair<const std::string, json>>>;
    using array_t = std::vector<json, std::allocator<json>>;
    using string_t = std::string;
    using boolean_t = bool;
    using number_integer_t = std::int64_t;
    using number_unsigned_t = std::uint64_t;
    using number_float_t = double;
    using initializer_list_t = std::initializer_list<detail::json_ref<json>>;
    using parser_callback_t = std::nullptr_t;

private:
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
                    object = new object_t();
                    break;
                case value_t::array:
                    array = new array_t();
                    break;
                case value_t::string:
                    string = new string_t("");
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

        void destroy(value_t t) noexcept {
            switch (t) {
                case value_t::object:
                    delete object;
                    object = nullptr;
                    break;
                case value_t::array:
                    delete array;
                    array = nullptr;
                    break;
                case value_t::string:
                    delete string;
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

    json(boolean_t val) noexcept : m_type(value_t::boolean), m_value(val) {}

    template <typename IntegerType,
              std::enable_if_t<std::is_integral<IntegerType>::value &&
                                   !std::is_same<IntegerType, bool>::value &&
                                   std::is_signed<IntegerType>::value,
                               int> = 0>
    json(IntegerType val) noexcept
        : m_type(value_t::number_integer),
          m_value(static_cast<number_integer_t>(val)) {}

    template <typename UnsignedType,
              std::enable_if_t<std::is_integral<UnsignedType>::value &&
                                   !std::is_same<UnsignedType, bool>::value &&
                                   std::is_unsigned<UnsignedType>::value,
                               int> = 0>
    json(UnsignedType val) noexcept
        : m_type(value_t::number_unsigned),
          m_value(static_cast<number_unsigned_t>(val)) {}

    template <typename FloatType,
              std::enable_if_t<std::is_floating_point<FloatType>::value, int> = 0>
    json(FloatType val) noexcept
        : m_type(value_t::number_float),
          m_value(static_cast<number_float_t>(val)) {}

    template <typename EnumType,
              std::enable_if_t<std::is_enum<EnumType>::value &&
                                   !std::is_same<EnumType, value_t>::value,
                               int> = 0>
    json(EnumType val) noexcept
        : json(static_cast<typename std::underlying_type<EnumType>::type>(val)) {}

    json(const char* s) : m_type(value_t::string) {
        m_value.string = new string_t(s ? s : "");
    }

    json(const string_t& s) : m_type(value_t::string) {
        m_value.string = new string_t(s);
    }

    json(string_t&& s) : m_type(value_t::string) {
        m_value.string = new string_t(std::move(s));
    }

    json(std::string_view sv) : m_type(value_t::string) {
        m_value.string = new string_t(sv);
    }

    json(const object_t& obj) : m_type(value_t::object) {
        m_value.object = new object_t(obj);
    }

    json(object_t&& obj) : m_type(value_t::object) {
        m_value.object = new object_t(std::move(obj));
    }

    json(const array_t& arr) : m_type(value_t::array) {
        m_value.array = new array_t(arr);
    }

    json(array_t&& arr) : m_type(value_t::array) {
        m_value.array = new array_t(std::move(arr));
    }

    template <typename MapType,
              std::enable_if_t<detail::is_map_like<std::decay_t<MapType>>::value &&
                                   !std::is_same<std::decay_t<MapType>, object_t>::value &&
                                   !std::is_same<std::decay_t<MapType>, json>::value,
                               int> = 0>
    json(const MapType& m) : m_type(value_t::object) {
        m_value.object = new object_t();
        m_value.object->reserve(m.size());
        for (const auto& kv : m) {
            m_value.object->emplace(string_t(kv.first), json(kv.second));
        }
    }

    template <typename SeqType,
              std::enable_if_t<detail::is_sequence_like<std::decay_t<SeqType>>::value &&
                                   !std::is_same<std::decay_t<SeqType>, array_t>::value &&
                                   !std::is_same<std::decay_t<SeqType>, json>::value &&
                                   !std::is_same<std::decay_t<SeqType>, detail::json_ref<json>>::value,
                               int> = 0>
    json(const SeqType& seq) : m_type(value_t::array) {
        m_value.array = new array_t();
        for (const auto& elem : seq) {
            m_value.array->emplace_back(elem);
        }
    }

    template <typename T, std::size_t N,
              std::enable_if_t<!std::is_same<std::remove_cv_t<T>, char>::value, int> = 0>
    json(const T (&arr)[N]) : m_type(value_t::array) {
        m_value.array = new array_t();
        m_value.array->reserve(N);
        for (std::size_t i = 0; i < N; ++i) {
            m_value.array->emplace_back(arr[i]);
        }
    }

    template <typename T1, typename T2>
    json(const std::pair<T1, T2>& p) : m_type(value_t::array) {
        m_value.array = new array_t();
        m_value.array->reserve(2);
        m_value.array->emplace_back(p.first);
        m_value.array->emplace_back(p.second);
    }

    template <typename... Args>
    json(const std::tuple<Args...>& t) : m_type(value_t::array) {
        m_value.array = new array_t();
        m_value.array->reserve(sizeof...(Args));
        construct_tuple(t, std::index_sequence_for<Args...>{});
    }

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
            m_type = value_t::object;
            m_value.object = new object_t();
            m_value.object->reserve(init.size());
            for (auto& element_ref : init) {
                auto element = element_ref.moved_or_copied();
                m_value.object->emplace(
                    std::move(*((*element.m_value.array)[0].m_value.string)),
                    std::move((*element.m_value.array)[1]));
            }
        } else {
            m_type = value_t::array;
            m_value.array = new array_t();
            m_value.array->reserve(init.size());
            for (auto const& element_ref : init) {
                m_value.array->emplace_back(element_ref.moved_or_copied());
            }
        }
    }

    json(size_type cnt, const json& val) : m_type(value_t::array) {
        m_value.array = new array_t(cnt, val);
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
                    m_value.string = new string_t(*(first.m_object->m_value.string));
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
                m_value.array = new array_t(
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
                m_value.object = new object_t(
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
                m_value.object = new object_t(*(other.m_value.object));
                break;
            case value_t::array:
                m_value.array = new array_t(*(other.m_value.array));
                break;
            case value_t::string:
                m_value.string = new string_t(*(other.m_value.string));
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

    reference operator[](size_type idx) {
        if (is_null()) {
            m_type = value_t::array;
            m_value.array = new array_t();
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
            m_type = value_t::object;
            m_value.object = new object_t();
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
            m_type = value_t::object;
            m_value.object = new object_t();
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
                      !std::is_same<std::decay_t<KeyType>, typename object_t::key_type>::value &&
                      !std::is_pointer<std::decay_t<KeyType>>::value &&
                      !std::is_integral<std::decay_t<KeyType>>::value,
                  int> = 0>
    reference operator[](const KeyType& key) {
        if (is_null()) {
            m_type = value_t::object;
            m_value.object = new object_t();
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

    template <class ValueType,
              std::enable_if_t<!std::is_same<std::decay_t<ValueType>, const char*>::value &&
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

    string_t value(const typename object_t::key_type& key, const char* default_value) const {
        return value(key, string_t(default_value));
    }

    template <class KeyType, class ValueType,
              std::enable_if_t<
                  std::is_convertible<const KeyType&, std::string_view>::value &&
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
                      !std::is_same<std::decay_t<KeyType>, typename object_t::key_type>::value,
                  int> = 0>
    string_t value(KeyType&& key, const char* default_value) const {
        return value(std::forward<KeyType>(key), string_t(default_value));
    }

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
                      !std::is_same<std::decay_t<KeyType>, typename object_t::key_type>::value,
                  int> = 0>
    bool contains(KeyType&& key) const {
        return is_object() && m_value.object->contains(std::string_view(key));
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
            m_type = value_t::array;
            m_value.array = new array_t();
        }
        json tmp(std::move(val));
        m_value.array->push_back(std::move(tmp));
    }

    void push_back(const json& val) {
        if (!is_null() && !is_array()) {
            throw type_error::create(308, std::string("cannot use push_back() with ") + type_name());
        }
        if (is_null()) {
            m_type = value_t::array;
            m_value.array = new array_t();
        }
        json tmp(val);
        m_value.array->push_back(std::move(tmp));
    }

    void push_back(const typename object_t::value_type& val) {
        if (!is_null() && !is_object()) {
            throw type_error::create(308, std::string("cannot use push_back() with ") + type_name());
        }
        if (is_null()) {
            m_type = value_t::object;
            m_value.object = new object_t();
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
            m_type = value_t::array;
            m_value.array = new array_t();
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
            m_type = value_t::object;
            m_value.object = new object_t();
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
            m_type = value_t::object;
            m_value.object = new object_t();
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
                    delete m_value.string;
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
                    delete m_value.string;
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

    static json parse(std::string_view s, parser_callback_t cb = nullptr,
                      bool allow_exceptions = true, bool ignore_comments = false);

    static json parse(std::string_view s, bool allow_exceptions, bool ignore_comments = false) {
        return parse(s, nullptr, allow_exceptions, ignore_comments);
    }

    static json parse(std::istream& i, parser_callback_t cb = nullptr,
                      bool allow_exceptions = true, bool ignore_comments = false);

    static json parse(std::istream&& i, parser_callback_t cb = nullptr,
                      bool allow_exceptions = true, bool ignore_comments = false) {
        return parse(i, cb, allow_exceptions, ignore_comments);
    }

    static json parse(std::istream& i, bool allow_exceptions, bool ignore_comments = false) {
        return parse(i, nullptr, allow_exceptions, ignore_comments);
    }

    static json parse(std::istream&& i, bool allow_exceptions, bool ignore_comments = false) {
        return parse(i, nullptr, allow_exceptions, ignore_comments);
    }

    template <typename IteratorType,
              std::enable_if_t<detail::is_InputIt<IteratorType>::value, int> = 0>
    static json parse(IteratorType first, IteratorType last, parser_callback_t cb = nullptr,
                      bool allow_exceptions = true, bool ignore_comments = false) {
        std::string buf(first, last);
        return parse(std::string_view(buf), cb, allow_exceptions, ignore_comments);
    }

    template <typename IteratorType,
              std::enable_if_t<detail::is_InputIt<IteratorType>::value, int> = 0>
    static json parse(IteratorType first, IteratorType last, bool allow_exceptions,
                      bool ignore_comments = false) {
        std::string buf(first, last);
        return parse(std::string_view(buf), nullptr, allow_exceptions, ignore_comments);
    }

    static bool accept(std::string_view s, bool ignore_comments = false);

    static bool accept(std::istream& i, bool ignore_comments = false);

    static bool accept(std::istream&& i, bool ignore_comments = false) {
        return accept(i, ignore_comments);
    }

    template <typename IteratorType,
              std::enable_if_t<detail::is_InputIt<IteratorType>::value, int> = 0>
    static bool accept(IteratorType first, IteratorType last, bool ignore_comments = false) {
        std::string buf(first, last);
        return accept(std::string_view(buf), ignore_comments);
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
              std::enable_if_t<std::is_pointer<PointerType>::value, int> = 0>
    auto get_ptr() noexcept -> decltype(this->get_impl_ptr(static_cast<PointerType>(nullptr))) {
        return get_impl_ptr(static_cast<PointerType>(nullptr));
    }

    template <typename PointerType,
              std::enable_if_t<std::is_pointer<PointerType>::value &&
                                   std::is_const<std::remove_pointer_t<PointerType>>::value,
                               int> = 0>
    constexpr auto get_ptr() const noexcept
        -> decltype(this->get_impl_ptr(static_cast<PointerType>(nullptr))) {
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

    template <typename ValueType,
              std::enable_if_t<std::is_pointer<ValueType>::value, int> = 0>
    auto get() noexcept -> decltype(this->template get_ptr<ValueType>()) {
        return get_ptr<ValueType>();
    }

    template <typename ValueType,
              std::enable_if_t<std::is_pointer<ValueType>::value &&
                                   std::is_const<std::remove_pointer_t<ValueType>>::value,
                               int> = 0>
    constexpr auto get() const noexcept -> decltype(this->template get_ptr<ValueType>()) {
        return get_ptr<ValueType>();
    }

    template <typename ValueType,
              std::enable_if_t<!std::is_pointer<ValueType>::value &&
                                   !std::is_reference<ValueType>::value,
                               int> = 0>
    ValueType get() const {
        return get_impl(static_cast<std::remove_cv_t<ValueType>*>(nullptr));
    }

    template <typename ValueType,
              std::enable_if_t<!std::is_const<ValueType>::value &&
                                   !std::is_same<std::decay_t<ValueType>, json>::value,
                               int> = 0>
    ValueType& get_to(ValueType& v) const {
        v = get<ValueType>();
        return v;
    }

    template <typename ValueType,
              std::enable_if_t<
                  !std::is_pointer<ValueType>::value &&
                      !std::is_same<ValueType, std::nullptr_t>::value &&
                      !std::is_same<ValueType, std::string_view>::value &&
                      !std::is_same<ValueType, value_t>::value &&
                      !std::is_same<std::decay_t<ValueType>, json>::value &&
                      !std::is_same<std::decay_t<ValueType>, detail::json_ref<json>>::value &&
                      !std::is_same<ValueType,
                                    std::initializer_list<typename string_t::value_type>>::value &&
                      (std::is_arithmetic<ValueType>::value || std::is_enum<ValueType>::value ||
                       std::is_same<ValueType, string_t>::value ||
                       std::is_same<ValueType, object_t>::value ||
                       std::is_same<ValueType, array_t>::value ||
                       detail::is_map_like<ValueType>::value ||
                       detail::is_sequence_like<ValueType>::value ||
                       detail::is_std_pair<ValueType>::value ||
                       detail::is_std_tuple<ValueType>::value),
                  int> = 0>
    operator ValueType() const {
        return get<ValueType>();
    }

private:
    template <typename Tuple, std::size_t... Is>
    void construct_tuple(const Tuple& t, std::index_sequence<Is...>) {
        (m_value.array->emplace_back(std::get<Is>(t)), ...);
    }

    json get_impl(json* /*unused*/) const { return *this; }

    object_t get_impl(object_t* /*unused*/) const {
        if (is_object()) {
            return *m_value.object;
        }
        throw type_error::create(302, std::string("type must be object, but is ") + type_name());
    }

    array_t get_impl(array_t* /*unused*/) const {
        if (is_array()) {
            return *m_value.array;
        }
        throw type_error::create(302, std::string("type must be array, but is ") + type_name());
    }

    string_t get_impl(string_t* /*unused*/) const {
        if (is_string()) {
            return *m_value.string;
        }
        throw type_error::create(302, std::string("type must be string, but is ") + type_name());
    }

    std::string_view get_impl(std::string_view* /*unused*/) const {
        if (is_string()) {
            return std::string_view(*m_value.string);
        }
        throw type_error::create(302, std::string("type must be string, but is ") + type_name());
    }

    boolean_t get_impl(boolean_t* /*unused*/) const {
        if (is_boolean()) {
            return m_value.boolean;
        }
        throw type_error::create(302, std::string("type must be boolean, but is ") + type_name());
    }

    template <typename ArithmeticType,
              std::enable_if_t<std::is_arithmetic<ArithmeticType>::value &&
                                   !std::is_same<ArithmeticType, boolean_t>::value,
                               int> = 0>
    ArithmeticType get_impl(ArithmeticType* /*unused*/) const {
        switch (m_type) {
            case value_t::number_unsigned:
                return static_cast<ArithmeticType>(m_value.number_unsigned);
            case value_t::number_integer:
                return static_cast<ArithmeticType>(m_value.number_integer);
            case value_t::number_float:
                return static_cast<ArithmeticType>(m_value.number_float);
            default:
                throw type_error::create(
                    302, std::string("type must be number, but is ") + type_name());
        }
    }

    template <typename EnumType,
              std::enable_if_t<std::is_enum<EnumType>::value, int> = 0>
    EnumType get_impl(EnumType* /*unused*/) const {
        using underlying_t = typename std::underlying_type<EnumType>::type;
        return static_cast<EnumType>(get<underlying_t>());
    }

    template <typename MapType,
              std::enable_if_t<detail::is_map_like<MapType>::value &&
                                   !std::is_same<MapType, object_t>::value,
                               int> = 0>
    MapType get_impl(MapType* /*unused*/) const {
        if (!is_object()) {
            throw type_error::create(
                302, std::string("type must be object, but is ") + type_name());
        }
        MapType res;
        for (const auto& kv : *m_value.object) {
            res.emplace(typename MapType::key_type(kv.first),
                        kv.second.template get<typename MapType::mapped_type>());
        }
        return res;
    }

    template <typename T, std::size_t N>
    std::array<T, N> get_impl(std::array<T, N>* /*unused*/) const {
        if (!is_array()) {
            throw type_error::create(302, std::string("type must be array, but is ") + type_name());
        }
        std::array<T, N> res{};
        for (std::size_t i = 0; i < N; ++i) {
            res[i] = at(i).template get<T>();
        }
        return res;
    }

    template <typename T1, typename T2>
    std::pair<T1, T2> get_impl(std::pair<T1, T2>* /*unused*/) const {
        if (!is_array()) {
            throw type_error::create(302, std::string("type must be array, but is ") + type_name());
        }
        return std::make_pair(at(0).template get<T1>(), at(1).template get<T2>());
    }

    template <typename... Args>
    std::tuple<Args...> get_impl(std::tuple<Args...>* /*unused*/) const {
        if (!is_array()) {
            throw type_error::create(302, std::string("type must be array, but is ") + type_name());
        }
        return get_tuple_impl<std::tuple<Args...>>(std::index_sequence_for<Args...>{});
    }

    template <typename Tuple, std::size_t... Is>
    Tuple get_tuple_impl(std::index_sequence<Is...>) const {
        return std::make_tuple(at(Is).template get<std::tuple_element_t<Is, Tuple>>()...);
    }

    template <typename SeqType,
              std::enable_if_t<detail::is_sequence_like<SeqType>::value &&
                                   !std::is_same<SeqType, array_t>::value &&
                                   !detail::is_std_array<SeqType>::value,
                               int> = 0>
    SeqType get_impl(SeqType* /*unused*/) const {
        if (!is_array()) {
            throw type_error::create(302, std::string("type must be array, but is ") + type_name());
        }
        SeqType res;
        for (const auto& elem : *m_value.array) {
            insert_into_sequence(res, elem.template get<typename SeqType::value_type>(), 0);
        }
        return res;
    }

    template <typename C, typename V>
    static auto insert_into_sequence(C& c, V&& v, int) -> decltype(c.push_back(std::forward<V>(v)), void()) {
        c.push_back(std::forward<V>(v));
    }

    template <typename C, typename V>
    static auto insert_into_sequence(C& c, V&& v, long) -> decltype(c.insert(std::forward<V>(v)), void()) {
        c.insert(std::forward<V>(v));
    }

    value_t m_type = value_t::null;
    json_value m_value{};
};

static_assert(sizeof(json) == 16, "jsonkit::json must be 16 bytes");

inline namespace literals {
inline namespace json_literals {

inline json operator""_json(const char* s, std::size_t n) {
    return json::parse(std::string_view(s, n));
}

} // namespace json_literals
} // namespace literals

} // namespace jsonkit

namespace std {

template <typename IteratorType>
struct tuple_size<::jsonkit::detail::iteration_proxy_value<IteratorType>>
    : std::integral_constant<std::size_t, 2> {};

template <typename IteratorType>
struct tuple_element<0, ::jsonkit::detail::iteration_proxy_value<IteratorType>> {
    using type = const std::string&;
};

template <typename IteratorType>
struct tuple_element<1, ::jsonkit::detail::iteration_proxy_value<IteratorType>> {
    using type = typename IteratorType::reference;
};

template <>
inline void swap<::jsonkit::json>(::jsonkit::json& j1, ::jsonkit::json& j2) noexcept {
    j1.swap(j2);
}

} // namespace std

namespace jsonkit {
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
} // namespace jsonkit

#endif // JSONKIT_JSON_HPP_
