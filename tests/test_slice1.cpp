#include "differential.hpp"

#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <limits>
#include <list>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using jsonkit::test::assert_equivalent;
using jsonkit::test::expect_same_exception;

namespace {

enum class SampleColor : int { Red = -1, Green = 0, Blue = 7 };

// ==================== 1. Exceptions & 2. Value Layout ====================

TEST(Slice1, LayoutAndAliases) {
    static_assert(sizeof(jsonkit::json) == 16, "jsonkit::json must be 16 bytes");
    static_assert(std::is_same<jsonkit::json, jsonkit::ordered_json>::value,
                  "ordered_json must alias json");
    static_assert(std::is_same<jsonkit::json::boolean_t, bool>::value, "");
    static_assert(std::is_same<jsonkit::json::number_integer_t, std::int64_t>::value, "");
    static_assert(std::is_same<jsonkit::json::number_unsigned_t, std::uint64_t>::value, "");
    static_assert(std::is_same<jsonkit::json::number_float_t, double>::value, "");
    static_assert(std::is_same<jsonkit::json::string_t, std::string>::value, "");

    static_assert(std::is_base_of<std::exception, jsonkit::json::exception>::value, "");
    static_assert(std::is_base_of<jsonkit::json::exception, jsonkit::json::parse_error>::value, "");
    static_assert(
        std::is_base_of<jsonkit::json::exception, jsonkit::json::invalid_iterator>::value, "");
    static_assert(std::is_base_of<jsonkit::json::exception, jsonkit::json::type_error>::value, "");
    static_assert(
        std::is_base_of<jsonkit::json::exception, jsonkit::json::out_of_range>::value, "");
    static_assert(std::is_base_of<jsonkit::json::exception, jsonkit::json::other_error>::value, "");
}

// ==================== 3. Construction & 4. Type Queries / Capacity ====================

TEST(Slice1, ConstructionScalarsAndTypes) {
    {
        nlohmann::ordered_json o;
        jsonkit::json s;
        assert_equivalent(o, s);
        EXPECT_EQ(static_cast<jsonkit::json::value_t>(s), jsonkit::json::value_t::null);
    }
    {
        nlohmann::ordered_json o(nullptr);
        jsonkit::json s(nullptr);
        assert_equivalent(o, s);
    }
    for (bool b : {false, true}) {
        assert_equivalent(nlohmann::ordered_json(b), jsonkit::json(b));
    }

    // Signed integer types
    assert_equivalent(nlohmann::ordered_json(static_cast<signed char>(-128)),
                      jsonkit::json(static_cast<signed char>(-128)));
    assert_equivalent(nlohmann::ordered_json(static_cast<short>(-32768)),
                      jsonkit::json(static_cast<short>(-32768)));
    assert_equivalent(nlohmann::ordered_json(-1), jsonkit::json(-1));
    assert_equivalent(nlohmann::ordered_json(0), jsonkit::json(0));
    assert_equivalent(nlohmann::ordered_json(42), jsonkit::json(42));
    assert_equivalent(nlohmann::ordered_json(std::numeric_limits<long>::min()),
                      jsonkit::json(std::numeric_limits<long>::min()));
    assert_equivalent(nlohmann::ordered_json(std::numeric_limits<long long>::min()),
                      jsonkit::json(std::numeric_limits<long long>::min()));
    assert_equivalent(nlohmann::ordered_json(std::numeric_limits<std::int64_t>::max()),
                      jsonkit::json(std::numeric_limits<std::int64_t>::max()));

    // Unsigned integer types
    assert_equivalent(nlohmann::ordered_json(static_cast<unsigned char>(255u)),
                      jsonkit::json(static_cast<unsigned char>(255u)));
    assert_equivalent(nlohmann::ordered_json(static_cast<unsigned short>(65535u)),
                      jsonkit::json(static_cast<unsigned short>(65535u)));
    assert_equivalent(nlohmann::ordered_json(0u), jsonkit::json(0u));
    assert_equivalent(nlohmann::ordered_json(42u), jsonkit::json(42u));
    assert_equivalent(nlohmann::ordered_json(std::numeric_limits<unsigned long>::max()),
                      jsonkit::json(std::numeric_limits<unsigned long>::max()));
    assert_equivalent(nlohmann::ordered_json(std::numeric_limits<std::uint64_t>::max()),
                      jsonkit::json(std::numeric_limits<std::uint64_t>::max()));

    // Floating point types
    for (double d : {0.0, -0.0, 1.0, -1.0, 3.141592653589793, 1e-4, 1e-5, 1e15, 1e16,
                     std::numeric_limits<double>::min(),
                     std::numeric_limits<double>::max(),
                     std::numeric_limits<double>::denorm_min(),
                     std::numeric_limits<double>::infinity(),
                     -std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::quiet_NaN()}) {
        assert_equivalent(nlohmann::ordered_json(d), jsonkit::json(d));
    }
    assert_equivalent(nlohmann::ordered_json(1.5f), jsonkit::json(1.5f));

    // Enums
    assert_equivalent(nlohmann::ordered_json(SampleColor::Red), jsonkit::json(SampleColor::Red));
    assert_equivalent(nlohmann::ordered_json(SampleColor::Blue), jsonkit::json(SampleColor::Blue));

    // Strings
    assert_equivalent(nlohmann::ordered_json("hello"), jsonkit::json("hello"));
    assert_equivalent(nlohmann::ordered_json(std::string("world")),
                      jsonkit::json(std::string("world")));
    assert_equivalent(nlohmann::ordered_json(std::string_view("sv_test")),
                      jsonkit::json(std::string_view("sv_test")));
    std::string with_nul("a\0b", 3);
    assert_equivalent(nlohmann::ordered_json(with_nul), jsonkit::json(with_nul));

    // value_t constructors
    assert_equivalent(nlohmann::ordered_json(nlohmann::ordered_json::value_t::null),
                      jsonkit::json(jsonkit::json::value_t::null));
    assert_equivalent(nlohmann::ordered_json(nlohmann::ordered_json::value_t::object),
                      jsonkit::json(jsonkit::json::value_t::object));
    assert_equivalent(nlohmann::ordered_json(nlohmann::ordered_json::value_t::array),
                      jsonkit::json(jsonkit::json::value_t::array));
    assert_equivalent(nlohmann::ordered_json(nlohmann::ordered_json::value_t::string),
                      jsonkit::json(jsonkit::json::value_t::string));
    assert_equivalent(nlohmann::ordered_json(nlohmann::ordered_json::value_t::boolean),
                      jsonkit::json(jsonkit::json::value_t::boolean));
    assert_equivalent(nlohmann::ordered_json(nlohmann::ordered_json::value_t::number_integer),
                      jsonkit::json(jsonkit::json::value_t::number_integer));
    assert_equivalent(nlohmann::ordered_json(nlohmann::ordered_json::value_t::number_unsigned),
                      jsonkit::json(jsonkit::json::value_t::number_unsigned));
    assert_equivalent(nlohmann::ordered_json(nlohmann::ordered_json::value_t::number_float),
                      jsonkit::json(jsonkit::json::value_t::number_float));
    assert_equivalent(nlohmann::ordered_json(nlohmann::ordered_json::value_t::discarded),
                      jsonkit::json(jsonkit::json::value_t::discarded));
}

TEST(Slice1, ConstructionContainersAndInitializerLists) {
    std::vector<int> vec = {1, 2, 3, 4};
    assert_equivalent(nlohmann::ordered_json(vec), jsonkit::json(vec));

    std::deque<std::string> deq = {"a", "b"};
    assert_equivalent(nlohmann::ordered_json(deq), jsonkit::json(deq));

    std::list<double> lst = {1.5, 2.5};
    assert_equivalent(nlohmann::ordered_json(lst), jsonkit::json(lst));

    std::set<int> st = {3, 1, 2};
    assert_equivalent(nlohmann::ordered_json(st), jsonkit::json(st));

    std::map<std::string, int> mp = {{"z", 1}, {"a", 2}};
    assert_equivalent(nlohmann::ordered_json(mp), jsonkit::json(mp));

    std::array<int, 3> std_arr = {{10, 20, 30}};
    assert_equivalent(nlohmann::ordered_json(std_arr), jsonkit::json(std_arr));

    int c_arr[3] = {7, 8, 9};
    assert_equivalent(nlohmann::ordered_json(c_arr), jsonkit::json(c_arr));

    std::pair<std::string, int> pr{"key", 99};
    assert_equivalent(nlohmann::ordered_json(pr), jsonkit::json(pr));

    std::tuple<int, std::string, bool> tup{1, "two", true};
    assert_equivalent(nlohmann::ordered_json(tup), jsonkit::json(tup));

    // Initializer list deduction
    assert_equivalent(nlohmann::ordered_json({}), jsonkit::json({}));
    assert_equivalent(nlohmann::ordered_json({1, 2, 3}), jsonkit::json({1, 2, 3}));
    assert_equivalent(nlohmann::ordered_json({{"z", 1}, {"a", 2}, {"m", {true, "x"}}}),
                      jsonkit::json({{"z", 1}, {"a", 2}, {"m", {true, "x"}}}));
    // Duplicate keys in initializer list keep first value (nlohmann::ordered_json behaviour)
    assert_equivalent(nlohmann::ordered_json({{"a", 1}, {"b", 2}, {"a", 3}}),
                      jsonkit::json({{"a", 1}, {"b", 2}, {"a", 3}}));
    // Array because second element has 3 items
    assert_equivalent(nlohmann::ordered_json({{"a", 1}, {"b", 2, 3}}),
                      jsonkit::json({{"a", 1}, {"b", 2, 3}}));
    // Array because first item of pair is not string
    assert_equivalent(nlohmann::ordered_json({{1, 2}, {3, 4}}),
                      jsonkit::json({{1, 2}, {3, 4}}));

    // Static factories object() and array()
    assert_equivalent(nlohmann::ordered_json::object(), jsonkit::json::object());
    assert_equivalent(nlohmann::ordered_json::object({{"k", 1}, {"b", 2}}),
                      jsonkit::json::object({{"k", 1}, {"b", 2}}));
    assert_equivalent(nlohmann::ordered_json::array(), jsonkit::json::array());
    assert_equivalent(nlohmann::ordered_json::array({{"k", 1}, {"b", 2}}),
                      jsonkit::json::array({{"k", 1}, {"b", 2}}));

    // Error 301 when json::object() given non-key-value initializer list
    expect_same_exception(
        [] { return nlohmann::ordered_json::object({1, 2}); },
        [] { return jsonkit::json::object({1, 2}); });

    // Count constructor json(cnt, val)
    assert_equivalent(nlohmann::ordered_json(4, "repeat"), jsonkit::json(4, "repeat"));
    assert_equivalent(nlohmann::ordered_json(0, 123), jsonkit::json(0, 123));

    // Copy and Move semantics
    nlohmann::ordered_json o_orig = {{"k", {1, 2, 3}}};
    jsonkit::json s_orig = {{"k", {1, 2, 3}}};
    nlohmann::ordered_json o_copy(o_orig);
    jsonkit::json s_copy(s_orig);
    assert_equivalent(o_copy, s_copy);
    assert_equivalent(o_orig, s_orig);

    nlohmann::ordered_json o_moved(std::move(o_orig));
    jsonkit::json s_moved(std::move(s_orig));
    assert_equivalent(o_moved, s_moved);
    assert_equivalent(o_orig, s_orig);
    EXPECT_TRUE(s_orig.is_null());

    // Iterator-range constructor
    assert_equivalent(nlohmann::ordered_json(o_moved.begin(), o_moved.end()),
                      jsonkit::json(s_moved.begin(), s_moved.end()));
    assert_equivalent(nlohmann::ordered_json(o_moved["k"].begin() + 1, o_moved["k"].end()),
                      jsonkit::json(s_moved["k"].begin() + 1, s_moved["k"].end()));

    nlohmann::ordered_json o_scalar = 42;
    jsonkit::json s_scalar = 42;
    assert_equivalent(nlohmann::ordered_json(o_scalar.begin(), o_scalar.end()),
                      jsonkit::json(s_scalar.begin(), s_scalar.end()));

    // Iterator range constructor errors (201, 204, 206)
    expect_same_exception(
        [&] { return nlohmann::ordered_json(o_moved.begin(), o_copy.end()); },
        [&] { return jsonkit::json(s_moved.begin(), s_copy.end()); });
    expect_same_exception(
        [&] { return nlohmann::ordered_json(o_scalar.begin(), o_scalar.begin()); },
        [&] { return jsonkit::json(s_scalar.begin(), s_scalar.begin()); });
    nlohmann::ordered_json o_null;
    jsonkit::json s_null;
    expect_same_exception(
        [&] { return nlohmann::ordered_json(o_null.begin(), o_null.end()); },
        [&] { return jsonkit::json(s_null.begin(), s_null.end()); });
}

// ==================== 5. Element Access & Lookup ====================

TEST(Slice1, ElementAccessAndLookup) {
    nlohmann::ordered_json o = {{"first", 10}, {"second", {1, 2, 3}}};
    jsonkit::json s = {{"first", 10}, {"second", {1, 2, 3}}};

    assert_equivalent(o.at("first"), s.at("first"));
    assert_equivalent(o.at(std::string("second")).at(1), s.at(std::string("second")).at(1));
    assert_equivalent(o.at(std::string_view("first")), s.at(std::string_view("first")));

    const auto& co = o;
    const auto& cs = s;
    assert_equivalent(co.at("first"), cs.at("first"));
    assert_equivalent(co["first"], cs["first"]);
    assert_equivalent(co[std::string("second")][2], cs[std::string("second")][2]);
    assert_equivalent(co[std::string_view("first")], cs[std::string_view("first")]);

    // Errors 401, 403, 304, 305
    expect_same_exception([&] { return o.at("missing"); }, [&] { return s.at("missing"); });
    expect_same_exception([&] { return co.at("missing"); }, [&] { return cs.at("missing"); });
    expect_same_exception([&] { return o["second"].at(99); }, [&] { return s["second"].at(99); });
    expect_same_exception([&] { return o.at(0); }, [&] { return s.at(0); });
    expect_same_exception([&] { return o["second"].at("k"); }, [&] { return s["second"].at("k"); });
    expect_same_exception([&] { return o[0]; }, [&] { return s[0]; });
    expect_same_exception([&] { return o["second"]["k"]; }, [&] { return s["second"]["k"]; });

    // null promotion via operator[]
    nlohmann::ordered_json o_arr_prom;
    jsonkit::json s_arr_prom;
    expect_same_exception(
        [&] { return std::as_const(o_arr_prom)[0]; },
        [&] { return std::as_const(s_arr_prom)[0]; });
    expect_same_exception(
        [&] { return std::as_const(o_arr_prom)["k"]; },
        [&] { return std::as_const(s_arr_prom)["k"]; });

    o_arr_prom[3] = "four";
    s_arr_prom[3] = "four";
    assert_equivalent(o_arr_prom, s_arr_prom);

    nlohmann::ordered_json o_obj_prom;
    jsonkit::json s_obj_prom;
    o_obj_prom["hello"] = 123;
    s_obj_prom["hello"] = 123;
    assert_equivalent(o_obj_prom, s_obj_prom);

    // value() with default and errors 306 / 302
    EXPECT_EQ(o.value("first", 999), s.value("first", 999));
    EXPECT_EQ(o.value("nonexistent", 999), s.value("nonexistent", 999));
    EXPECT_EQ(o.value("nonexistent", "def"), s.value("nonexistent", "def"));
    EXPECT_EQ(o.value(std::string_view("first"), 0), s.value(std::string_view("first"), 0));
    expect_same_exception(
        [&] { return o.value("first", std::string("wrong_type")); },
        [&] { return s.value("first", std::string("wrong_type")); });
    expect_same_exception(
        [&] { return o["second"].value("k", 1); },
        [&] { return s["second"].value("k", 1); });

    // front() and back()
    assert_equivalent(o.front(), s.front());
    assert_equivalent(o.back(), s.back());
    assert_equivalent(o["second"].front(), s["second"].front());
    assert_equivalent(o["second"].back(), s["second"].back());
    nlohmann::ordered_json o_null;
    jsonkit::json s_null;
    expect_same_exception([&] { return o_null.front(); }, [&] { return s_null.front(); });
    expect_same_exception([&] { return o_null.back(); }, [&] { return s_null.back(); });

    // find, count, contains
    EXPECT_EQ(o.count("first"), s.count("first"));
    EXPECT_EQ(o.count("absent"), s.count("absent"));
    EXPECT_EQ(o.contains("second"), s.contains("second"));
    EXPECT_EQ(o.contains(std::string_view("second")), s.contains(std::string_view("second")));
    EXPECT_EQ(o.contains("absent"), s.contains("absent"));
    EXPECT_EQ(o["second"].count("first"), s["second"].count("first"));
    EXPECT_EQ(o["second"].contains("first"), s["second"].contains("first"));
    EXPECT_TRUE(s.find("absent") == s.end());
    EXPECT_TRUE(cs.find("absent") == cs.cend());
    assert_equivalent(*o.find("first"), *s.find("first"));
}

// ==================== 6. Iterators ====================

TEST(Slice1, IteratorsAndItems) {
    nlohmann::ordered_json o_obj = {{"c", 1}, {"a", 2}, {"b", 3}};
    jsonkit::json s_obj = {{"c", 1}, {"a", 2}, {"b", 3}};

    // Forward and reverse object iteration
    {
        auto it_o = o_obj.begin();
        auto it_s = s_obj.begin();
        for (; it_o != o_obj.end(); ++it_o, ++it_s) {
            EXPECT_EQ(it_o.key(), it_s.key());
            assert_equivalent(it_o.value(), it_s.value());
        }
        EXPECT_EQ(it_s, s_obj.end());
    }
    {
        auto rit_o = o_obj.rbegin();
        auto rit_s = s_obj.rbegin();
        for (; rit_o != o_obj.rend(); ++rit_o, ++rit_s) {
            EXPECT_EQ(rit_o.key(), rit_s.key());
            assert_equivalent(rit_o.value(), rit_s.value());
        }
        EXPECT_EQ(rit_s, s_obj.rend());
    }

    // Structured bindings with items()
    {
        std::vector<std::pair<std::string, int>> o_items;
        for (auto& [k, v] : o_obj.items()) {
            o_items.emplace_back(k, v.get<int>());
        }
        std::vector<std::pair<std::string, int>> s_items;
        for (auto& [k, v] : s_obj.items()) {
            s_items.emplace_back(k, v.get<int>());
        }
        EXPECT_EQ(o_items, s_items);
    }

    // Array items() uses stringified index as key
    nlohmann::ordered_json o_arr = {"x", "y", "z"};
    jsonkit::json s_arr = {"x", "y", "z"};
    {
        auto it_o = o_arr.items().begin();
        auto it_s = s_arr.items().begin();
        for (; it_o != o_arr.items().end(); ++it_o, ++it_s) {
            EXPECT_EQ(it_o.key(), it_s.key());
            assert_equivalent(it_o.value(), it_s.value());
        }
    }

    // Scalar 1-element iteration & null 0-element iteration
    nlohmann::ordered_json o_num = 77;
    jsonkit::json s_num = 77;
    EXPECT_EQ(std::distance(o_num.begin(), o_num.end()),
              std::distance(s_num.begin(), s_num.end()));
    assert_equivalent(*o_num.begin(), *s_num.begin());

    nlohmann::ordered_json o_null;
    jsonkit::json s_null;
    EXPECT_EQ(o_null.begin(), o_null.end());
    EXPECT_EQ(s_null.begin(), s_null.end());

    // Iterator errors 207, 208, 209, 212, 213, 214
    expect_same_exception([&] { return o_arr.begin().key(); }, [&] { return s_arr.begin().key(); });
    expect_same_exception([&] { return o_obj.begin()[0]; }, [&] { return s_obj.begin()[0]; });
    expect_same_exception([&] { return o_obj.begin() + 1; }, [&] { return s_obj.begin() + 1; });
    expect_same_exception(
        [&] { return o_obj.end() - o_obj.begin(); },
        [&] { return s_obj.end() - s_obj.begin(); });
    expect_same_exception(
        [&] { return o_obj.begin() == o_arr.begin(); },
        [&] { return s_obj.begin() == s_arr.begin(); });
    expect_same_exception(
        [&] { return o_obj.begin() < o_obj.end(); },
        [&] { return s_obj.begin() < s_obj.end(); });
    expect_same_exception([&] { return *o_null.begin(); }, [&] { return *s_null.begin(); });
}

// ==================== 7. Modifiers ====================

TEST(Slice1, ModifiersAndSelfAssignment) {
    // clear() on every type
    for (auto [o, s] : std::vector<std::pair<nlohmann::ordered_json, jsonkit::json>>{
             {nullptr, nullptr},
             {true, true},
             {-42, -42},
             {42u, 42u},
             {3.14, 3.14},
             {"hello", "hello"},
             {{1, 2, 3}, {1, 2, 3}},
             {{{"a", 1}}, {{"a", 1}}}}) {
        o.clear();
        s.clear();
        assert_equivalent(o, s);
    }

    // push_back, operator+=, emplace_back, emplace
    nlohmann::ordered_json o_arr;
    jsonkit::json s_arr;
    o_arr.push_back(1);
    s_arr.push_back(1);
    o_arr += "two";
    s_arr += "two";
    o_arr.emplace_back(3, "x");
    s_arr.emplace_back(3, "x");
    assert_equivalent(o_arr, s_arr);

    nlohmann::ordered_json o_obj;
    jsonkit::json s_obj;
    o_obj.push_back(nlohmann::ordered_json::object_t::value_type{"k1", 10});
    s_obj.push_back(jsonkit::json::object_t::value_type{"k1", 10});
    o_obj += {"k2", 20};
    s_obj += {"k2", 20};
    expect_same_exception(
        [&] { o_obj += {{"k_err", 1}}; },
        [&] { s_obj += {{"k_err", 1}}; });
    auto [o_it, o_ins] = o_obj.emplace("k1", 999);
    auto [s_it, s_ins] = s_obj.emplace("k1", 999);
    EXPECT_EQ(o_ins, s_ins);
    assert_equivalent(*o_it, *s_it);
    auto [o_it2, o_ins2] = o_obj.emplace("k3", 30);
    auto [s_it2, s_ins2] = s_obj.emplace("k3", 30);
    EXPECT_EQ(o_ins2, s_ins2);
    assert_equivalent(o_obj, s_obj);

    // insert all forms
    o_arr.insert(o_arr.begin() + 1, 99);
    s_arr.insert(s_arr.begin() + 1, 99);
    o_arr.insert(o_arr.end(), 2, "dup");
    s_arr.insert(s_arr.end(), 2, "dup");
    o_arr.insert(o_arr.begin(), {7, 8});
    s_arr.insert(s_arr.begin(), {7, 8});
    nlohmann::ordered_json o_extra = {"a", "b"};
    jsonkit::json s_extra = {"a", "b"};
    o_arr.insert(o_arr.begin() + 2, o_extra.begin(), o_extra.end());
    s_arr.insert(s_arr.begin() + 2, s_extra.begin(), s_extra.end());
    assert_equivalent(o_arr, s_arr);

    // insert into object from iterator range
    nlohmann::ordered_json o_obj2 = {{"k2", 200}, {"k4", 400}};
    jsonkit::json s_obj2 = {{"k2", 200}, {"k4", 400}};
    o_obj.insert(o_obj2.begin(), o_obj2.end());
    s_obj.insert(s_obj2.begin(), s_obj2.end());
    assert_equivalent(o_obj, s_obj);

    // update (merge_objects = false and true)
    nlohmann::ordered_json o_u1 = {{"a", {{"x", 1}, {"y", 2}}}, {"b", 10}};
    jsonkit::json s_u1 = {{"a", {{"x", 1}, {"y", 2}}}, {"b", 10}};
    nlohmann::ordered_json o_u2 = {{"a", {{"y", 20}, {"z", 30}}}, {"c", 30}};
    jsonkit::json s_u2 = {{"a", {{"y", 20}, {"z", 30}}}, {"c", 30}};
    o_u1.update(o_u2, true);
    s_u1.update(s_u2, true);
    assert_equivalent(o_u1, s_u1);

    o_u1.update(o_u2, false);
    s_u1.update(s_u2, false);
    assert_equivalent(o_u1, s_u1);

    // erase by key, index, iterator, range
    EXPECT_EQ(o_u1.erase("b"), s_u1.erase("b"));
    EXPECT_EQ(o_u1.erase("nonexistent"), s_u1.erase("nonexistent"));
    assert_equivalent(o_u1, s_u1);

    o_arr.erase(0);
    s_arr.erase(0);
    o_arr.erase(o_arr.begin() + 1);
    s_arr.erase(s_arr.begin() + 1);
    o_arr.erase(o_arr.begin() + 1, o_arr.begin() + 3);
    s_arr.erase(s_arr.begin() + 1, s_arr.begin() + 3);
    assert_equivalent(o_arr, s_arr);

    // Self-assignment and subtree aliasing assignment
    const auto& self_ref = s_u1;
    s_u1 = self_ref;
    assert_equivalent(o_u1, s_u1);

    o_u1 = o_u1["a"];
    s_u1 = s_u1["a"];
    assert_equivalent(o_u1, s_u1);
}

// ==================== 8. Comparison ====================

TEST(Slice1, Comparisons) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<std::pair<nlohmann::ordered_json, jsonkit::json>> vals = {
        {nullptr, nullptr},
        {false, false},
        {true, true},
        {-100, -100},
        {-1, -1},
        {0, 0},
        {0u, 0u},
        {0.0, 0.0},
        {-0.0, -0.0},
        {1, 1},
        {1u, 1u},
        {1.0, 1.0},
        {1.5, 1.5},
        {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::min()},
        {std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::max()},
        {std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max()},
        {nan, nan},
        {"", ""},
        {"a", "a"},
        {"b", "b"},
        {nlohmann::ordered_json::array(), jsonkit::json::array()},
        {{1, 2}, {1, 2}},
        {{1, 3}, {1, 3}},
        {nlohmann::ordered_json::object(), jsonkit::json::object()},
        {{{"a", 1}, {"b", 2}}, {{"a", 1}, {"b", 2}}},
        {{{"b", 2}, {"a", 1}}, {{"b", 2}, {"a", 1}}},
        {nlohmann::ordered_json(nlohmann::ordered_json::value_t::discarded),
         jsonkit::json(jsonkit::json::value_t::discarded)},
    };

    for (std::size_t i = 0; i < vals.size(); ++i) {
        for (std::size_t j = 0; j < vals.size(); ++j) {
            const auto& [o1, s1] = vals[i];
            const auto& [o2, s2] = vals[j];
            EXPECT_EQ(o1 == o2, s1 == s2) << "i=" << i << " j=" << j;
            EXPECT_EQ(o1 != o2, s1 != s2) << "i=" << i << " j=" << j;
            EXPECT_EQ(o1 < o2, s1 < s2) << "i=" << i << " j=" << j;
            EXPECT_EQ(o1 <= o2, s1 <= s2) << "i=" << i << " j=" << j;
            EXPECT_EQ(o1 > o2, s1 > s2) << "i=" << i << " j=" << j;
            EXPECT_EQ(o1 >= o2, s1 >= s2) << "i=" << i << " j=" << j;
        }
    }

    // Scalar comparison overloads
    jsonkit::json s_num = 42;
    EXPECT_TRUE(s_num == 42);
    EXPECT_TRUE(42 == s_num);
    EXPECT_TRUE(s_num == 42.0);
    EXPECT_TRUE(s_num < 43);
    EXPECT_TRUE(41 < s_num);
    jsonkit::json s_str = "abc";
    EXPECT_TRUE(s_str == "abc");
    EXPECT_TRUE("abc" == s_str);
}

// ==================== 9. Parse Corpus & 10. Dump ====================

TEST(Slice1, ParseValidCorpus) {
    const std::vector<std::string> valid_corpus = {
        "null",
        "true",
        "false",
        "0",
        "-0",
        "0.0",
        "-0.0",
        "1",
        "1.0",
        "-1",
        "-1.0",
        "42",
        "-42",
        "9223372036854775807",
        "-9223372036854775808",
        "9223372036854775808",
        "18446744073709551615",
        // Beyond 64-bit integer limits -> double
        "18446744073709551616",
        "-9223372036854775809",
        "1e-308",
        "1e308",
        "1.7976931348623157e+308",
        "2.2250738585072014e-308",
        "5e-324",
        "\"\"",
        "\"hello world\"",
        "\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"",
        "\"\\u0000\\u001f\\u007f\\u0080\\u07ff\\u0800\\uffff\"",
        "\"\\ud83d\\ude00\\ud834\\udd1e\"",
        "\"한글과 日本語와 🚀\"",
        "[]",
        "[null,true,false,0,-0,0.0,-0.0,\"str\",[],{}]",
        "{}",
        "{\"z\":1,\"a\":2,\"m\":3}",
        // Duplicate keys: first position kept, last value wins
        "{\"a\":1,\"b\":2,\"a\":3,\"b\":4,\"c\":5}",
        // UTF-8 BOM prefix
        "\xEF\xBB\xBF{\"bom\":true}",
    };

    for (const auto& doc : valid_corpus) {
        SCOPED_TRACE(doc);
        EXPECT_EQ(nlohmann::ordered_json::accept(doc), jsonkit::json::accept(doc));
        auto o = nlohmann::ordered_json::parse(doc);
        auto s = jsonkit::json::parse(doc);
        assert_equivalent(o, s);

        // Also test std::istream parse & accept
        std::istringstream iss_o(doc);
        std::istringstream iss_s(doc);
        assert_equivalent(nlohmann::ordered_json::parse(iss_o), jsonkit::json::parse(iss_s));

        // Iterator-range parse
        assert_equivalent(nlohmann::ordered_json::parse(doc.begin(), doc.end()),
                          jsonkit::json::parse(doc.begin(), doc.end()));
    }

    // Deep nesting (256 and 257 levels)
    for (std::size_t depth : {1u, 64u, 256u, 257u}) {
        std::string deep(depth, '[');
        deep += "42";
        deep.append(depth, ']');
        EXPECT_EQ(nlohmann::ordered_json::accept(deep), jsonkit::json::accept(deep));
        assert_equivalent(nlohmann::ordered_json::parse(deep), jsonkit::json::parse(deep));
    }

    // Comments with ignore_comments = true and false
    const std::string with_comments = R"({
        // single line comment
        "a": 1, /* multi-line
        comment */ "b": [2, /* inline */ 3]
    })";
    EXPECT_FALSE(nlohmann::ordered_json::accept(with_comments, false));
    EXPECT_FALSE(jsonkit::json::accept(with_comments, false));
    EXPECT_TRUE(nlohmann::ordered_json::accept(with_comments, true));
    EXPECT_TRUE(jsonkit::json::accept(with_comments, true));
    expect_same_exception(
        [&] { return nlohmann::ordered_json::parse(with_comments, nullptr, true, false); },
        [&] { return jsonkit::json::parse(with_comments, nullptr, true, false); });
    assert_equivalent(nlohmann::ordered_json::parse(with_comments, nullptr, true, true),
                      jsonkit::json::parse(with_comments, nullptr, true, true));
    assert_equivalent(nlohmann::ordered_json::parse(with_comments, nullptr, false, false),
                      jsonkit::json::parse(with_comments, nullptr, false, false));
}

TEST(Slice1, ParseInvalidCorpus) {
    const std::vector<std::string> invalid_corpus = {
        "",
        "   ",
        "\xEF\xBB\xBF",
        "\xEF\xBB",
        "nul",
        "tru",
        "fals",
        "True",
        "NaN",
        "Infinity",
        "-Infinity",
        "01",
        "-01",
        "1.",
        ".5",
        "1e",
        "1e+",
        "1e-",
        "1e309",
        "-1e309",
        "\"unterminated",
        "\"\\x20\"",
        "\"\\u123\"",
        "\"\\u123g\"",
        "\"\\ud800\"",
        "\"\\udc00\"",
        "\"\\ud800\\u0041\"",
        "\"\\ud800\\ud800\"",
        "\"\n\"",
        "\"\t\"",
        "\"\x80\"",
        "\"\xC0\xAF\"",
        "\"\xE0\x80\xAF\"",
        "\"\xED\xA0\x80\"",
        "\"\xF4\x90\x80\x80\"",
        "[1,]",
        "[,1]",
        "[1 2]",
        "{\"a\":1,}",
        "{\"a\" 1}",
        "{1:2}",
        "{\"a\":1} trailing",
        "/* unclosed comment",
    };

    for (const auto& doc : invalid_corpus) {
        SCOPED_TRACE(doc);
        EXPECT_FALSE(nlohmann::ordered_json::accept(doc));
        EXPECT_FALSE(jsonkit::json::accept(doc));
        expect_same_exception(
            [&] { return nlohmann::ordered_json::parse(doc); },
            [&] { return jsonkit::json::parse(doc); });
        assert_equivalent(nlohmann::ordered_json::parse(doc, nullptr, false),
                          jsonkit::json::parse(doc, nullptr, false));
    }

    // Embedded NUL outside and inside strings
    std::string trailing_nul("123\0", 4);
    EXPECT_EQ(nlohmann::ordered_json::accept(trailing_nul), jsonkit::json::accept(trailing_nul));
    assert_equivalent(nlohmann::ordered_json::parse(trailing_nul),
                      jsonkit::json::parse(trailing_nul));
    std::string only_nul("\0", 1);
    EXPECT_FALSE(nlohmann::ordered_json::accept(only_nul));
    EXPECT_FALSE(jsonkit::json::accept(only_nul));
    expect_same_exception(
        [&] { return nlohmann::ordered_json::parse(only_nul); },
        [&] { return jsonkit::json::parse(only_nul); });
    std::string raw_nul_in_str("\"a\0b\"", 5);
    expect_same_exception(
        [&] { return nlohmann::ordered_json::parse(raw_nul_in_str); },
        [&] { return jsonkit::json::parse(raw_nul_in_str); });
}

TEST(Slice1, DumpInvalidUtf8AndStreamFormatting) {
    const std::vector<std::string> bad_utf8_strings = {
        std::string("\x80"),
        std::string("abc\xFFxyz"),
        std::string("\xC0\x80"),
        std::string("\xE0\x80\x80"),
        std::string("\xED\xA0\x80"),
        std::string("\xF0\x80\x80\x80"),
        std::string("\xF4\x90\x80\x80"),
        std::string("trunc2\xC3"),
        std::string("trunc3\xE2\x82"),
        std::string("trunc4\xF0\x9F\x98"),
        std::string("\xE2\x82" "ascii"),
    };

    for (const auto& s : bad_utf8_strings) {
        nlohmann::ordered_json o = s;
        jsonkit::json j = s;
        assert_equivalent(o, j);
    }

    // Stream operator<< with width and fill
    nlohmann::ordered_json o = {{"a", {1, 2}}, {"b", "text"}};
    jsonkit::json j = {{"a", {1, 2}}, {"b", "text"}};
    std::ostringstream oss_o;
    std::ostringstream oss_j;
    oss_o << std::setw(2) << std::setfill('.') << o;
    oss_j << std::setw(2) << std::setfill('.') << j;
    EXPECT_EQ(oss_o.str(), oss_j.str());
}

// ==================== 11. Conversions & Literals ====================

TEST(Slice1, ConversionsAndLiterals) {
    using jsonkit::literals::operator""_json;
    auto j = R"({"b":true,"i":-42,"u":42,"f":3.5,"s":"hello","a":[1,2,3],"o":{"k":9}})"_json;
    auto o = nlohmann::ordered_json::parse(
        R"({"b":true,"i":-42,"u":42,"f":3.5,"s":"hello","a":[1,2,3],"o":{"k":9}})");
    assert_equivalent(o, j);

    EXPECT_EQ(j["b"].get<bool>(), true);
    EXPECT_EQ(j["i"].get<int>(), -42);
    EXPECT_EQ(j["i"].get<double>(), -42.0);
    EXPECT_EQ(j["f"].get<int>(), 3);
    EXPECT_EQ(j["s"].get<std::string>(), "hello");
    EXPECT_EQ(j["s"].get<std::string_view>(), "hello");

    std::vector<int> vec;
    j["a"].get_to(vec);
    EXPECT_EQ(vec, (std::vector<int>{1, 2, 3}));

    std::map<std::string, int> mp = j["o"];
    EXPECT_EQ(mp["k"], 9);

    std::unordered_map<std::string, int> ump = j["o"].get<std::unordered_map<std::string, int>>();
    EXPECT_EQ(ump["k"], 9);

    // get_ptr and get_ref
    EXPECT_NE(j["s"].get_ptr<jsonkit::json::string_t*>(), nullptr);
    EXPECT_EQ(j["s"].get_ptr<jsonkit::json::boolean_t*>(), nullptr);
    EXPECT_EQ(j["s"].get_ref<const jsonkit::json::string_t&>(), "hello");
    expect_same_exception(
        [&] { return o["s"].get_ref<const nlohmann::ordered_json::boolean_t&>(); },
        [&] { return j["s"].get_ref<const jsonkit::json::boolean_t&>(); });

    // type_error.302 on invalid conversions
    expect_same_exception([&] { return o["s"].get<int>(); }, [&] { return j["s"].get<int>(); });
    expect_same_exception([&] { return o["i"].get<bool>(); }, [&] { return j["i"].get<bool>(); });
    expect_same_exception(
        [&] { return o["i"].get<std::string>(); },
        [&] { return j["i"].get<std::string>(); });
    expect_same_exception(
        [&] { return o["i"].get<std::vector<int>>(); },
        [&] { return j["i"].get<std::vector<int>>(); });
    expect_same_exception(
        [&] { return o["a"].get<std::map<std::string, int>>(); },
        [&] { return j["a"].get<std::map<std::string, int>>(); });
}

// ==================== Reference Stability ====================

TEST(Slice1, ReferenceStability) {
    jsonkit::json a = {{"k", 1}, {"nested", {{"inner", "val"}}}};
    jsonkit::json& r = a["k"];
    jsonkit::json b = a;
    r = 99;
    EXPECT_EQ(a["k"], 99);
    EXPECT_EQ(b["k"], 1);

    // Reference into child subtree survives copy of parent and mutations of other keys
    jsonkit::json& inner_ref = a["nested"]["inner"];
    a["k"] = 12345;
    EXPECT_EQ(inner_ref, "val");
    inner_ref = "updated";
    EXPECT_EQ(a["nested"]["inner"], "updated");
    EXPECT_EQ(b["nested"]["inner"], "val");
}

// ==================== Generated Operation Sequences (spec 5.1) ====================

TEST(Slice1, RandomOperationSequences) {
    for (std::uint32_t seed = 1; seed <= 30; ++seed) {
        SCOPED_TRACE("seed=" + std::to_string(seed));
        std::mt19937 rng(seed);

        nlohmann::ordered_json o_root = nlohmann::ordered_json::object();
        jsonkit::json s_root = jsonkit::json::object();
        nlohmann::ordered_json o_aux = nlohmann::ordered_json::array();
        jsonkit::json s_aux = jsonkit::json::array();

        static const std::array<const char*, 8> kKeys = {
            {"a", "b", "c", "d", "e", "f", "g", "h"}};

        auto make_leaf = [&](std::uint32_t code)
            -> std::pair<nlohmann::ordered_json, jsonkit::json> {
            switch (code % 8) {
                case 0:
                    return {nullptr, nullptr};
                case 1:
                    return {false, false};
                case 2:
                    return {true, true};
                case 3: {
                    int v = static_cast<int>(rng() % 2000) - 1000;
                    return {v, v};
                }
                case 4: {
                    std::uint64_t u = rng();
                    return {u, u};
                }
                case 5: {
                    double d = (static_cast<double>(rng() % 10000) - 5000.0) / 37.0;
                    return {d, d};
                }
                case 6: {
                    std::string str = std::string("s_") + std::to_string(rng() % 50);
                    return {str, str};
                }
                default: {
                    int xv = static_cast<int>(rng() % 10);
                    return {{{"x", xv}}, {{"x", xv}}};
                }
            }
        };

        for (int step = 0; step < 120; ++step) {
            const std::uint32_t op = rng() % 12;
            const char* key = kKeys[rng() % kKeys.size()];

            switch (op) {
                case 0: { // operator[](key) = leaf on object
                    if (!o_root.is_object() && !o_root.is_null()) {
                        o_root = nlohmann::ordered_json::object();
                        s_root = jsonkit::json::object();
                    }
                    auto [ov, sv] = make_leaf(rng());
                    expect_same_exception([&] { o_root[key] = std::move(ov); },
                                          [&] { s_root[key] = std::move(sv); });
                    break;
                }
                case 1: { // erase(key) on object
                    expect_same_exception([&] { o_root.erase(key); },
                                          [&] { s_root.erase(key); });
                    break;
                }
                case 2: { // emplace on object
                    auto [ov, sv] = make_leaf(rng());
                    expect_same_exception([&] { o_root.emplace(key, std::move(ov)); },
                                          [&] { s_root.emplace(key, std::move(sv)); });
                    break;
                }
                case 3: { // push_back on aux array
                    if (!o_aux.is_array() && !o_aux.is_null()) {
                        o_aux = nlohmann::ordered_json::array();
                        s_aux = jsonkit::json::array();
                    }
                    auto [ov, sv] = make_leaf(rng());
                    expect_same_exception([&] { o_aux.push_back(std::move(ov)); },
                                          [&] { s_aux.push_back(std::move(sv)); });
                    break;
                }
                case 4: { // insert into aux array
                    if (o_aux.is_array()) {
                        const std::size_t idx =
                            o_aux.empty() ? 0 : (rng() % (o_aux.size() + 1));
                        auto [ov, sv] = make_leaf(rng());
                        expect_same_exception(
                            [&] { o_aux.insert(o_aux.begin() + static_cast<std::ptrdiff_t>(idx), ov); },
                            [&] { s_aux.insert(s_aux.begin() + static_cast<std::ptrdiff_t>(idx), sv); });
                    }
                    break;
                }
                case 5: { // erase from aux array
                    if (o_aux.is_array() && !o_aux.empty()) {
                        const std::size_t idx = rng() % o_aux.size();
                        expect_same_exception([&] { o_aux.erase(idx); },
                                              [&] { s_aux.erase(idx); });
                    }
                    break;
                }
                case 6: { // operator[](idx) on aux array (up to small resize)
                    const std::size_t idx = rng() % 6;
                    auto [ov, sv] = make_leaf(rng());
                    expect_same_exception([&] { o_aux[idx] = std::move(ov); },
                                          [&] { s_aux[idx] = std::move(sv); });
                    break;
                }
                case 7: { // update root object with another small object
                    const char* k2 = kKeys[rng() % kKeys.size()];
                    auto [ov, sv] = make_leaf(rng());
                    nlohmann::ordered_json o_patch = {{key, ov}, {k2, 123}};
                    jsonkit::json s_patch = {{key, sv}, {k2, 123}};
                    const bool merge = (rng() & 1u) != 0;
                    expect_same_exception([&] { o_root.update(o_patch, merge); },
                                          [&] { s_root.update(s_patch, merge); });
                    break;
                }
                case 8: { // nest aux into root[key]
                    expect_same_exception([&] { o_root[key] = o_aux; },
                                          [&] { s_root[key] = s_aux; });
                    break;
                }
                case 9: { // roundtrip via dump() and parse()
                    std::string o_dumped;
                    std::string s_dumped;
                    expect_same_exception([&] { o_dumped = o_root.dump(); },
                                          [&] { s_dumped = s_root.dump(); });
                    EXPECT_EQ(o_dumped, s_dumped);
                    if (!s_dumped.empty()) {
                        expect_same_exception(
                            [&] { o_root = nlohmann::ordered_json::parse(o_dumped); },
                            [&] { s_root = jsonkit::json::parse(s_dumped); });
                    }
                    break;
                }
                case 10: { // copy & move cycle
                    nlohmann::ordered_json o_tmp = o_root;
                    jsonkit::json s_tmp = s_root;
                    o_root = std::move(o_tmp);
                    s_root = std::move(s_tmp);
                    assert_equivalent(o_tmp, s_tmp);
                    break;
                }
                case 11: { // comparison check between root and aux
                    EXPECT_EQ(o_root == o_aux, s_root == s_aux);
                    EXPECT_EQ(o_root != o_aux, s_root != s_aux);
                    EXPECT_EQ(o_root < o_aux, s_root < s_aux);
                    EXPECT_EQ(o_root <= o_aux, s_root <= s_aux);
                    EXPECT_EQ(o_root > o_aux, s_root > s_aux);
                    EXPECT_EQ(o_root >= o_aux, s_root >= s_aux);
                    break;
                }
            }

            assert_equivalent(o_root, s_root);
            assert_equivalent(o_aux, s_aux);
        }
    }
}

// ==================== Edge-Case Contracts ====================

TEST(Slice1, EdgeCaseContracts) {
    // 1. Dereferencing end()/rend() and calling key() at end()/rend() throw invalid_iterator.214.
    {
        nlohmann::ordered_json o_null;
        jsonkit::json s_null;
        expect_same_exception([&] { (void)*o_null.end(); }, [&] { (void)*s_null.end(); });

        nlohmann::ordered_json o_num = 42;
        jsonkit::json s_num = 42;
        expect_same_exception([&] { (void)*o_num.end(); }, [&] { (void)*s_num.end(); });

        nlohmann::ordered_json o_disc(nlohmann::ordered_json::value_t::discarded);
        jsonkit::json s_disc(jsonkit::json::value_t::discarded);
        expect_same_exception([&] { (void)*o_disc.end(); }, [&] { (void)*s_disc.end(); });

        jsonkit::json s_obj = {{"a", 1}, {"b", 2}};
        const jsonkit::json& cs_obj = s_obj;
        auto expect_214 = [](auto&& fn) {
            bool threw = false;
            try {
                fn();
            } catch (const jsonkit::json::invalid_iterator& e) {
                threw = true;
                EXPECT_EQ(e.id, 214);
                EXPECT_EQ(std::string_view(e.what()).substr(0, 36),
                          "[json.exception.invalid_iterator.214");
            }
            EXPECT_TRUE(threw);
        };
        expect_214([&] { (void)s_obj.end().key(); });
        expect_214([&] { (void)cs_obj.end().key(); });
        expect_214([&] { (void)s_obj.rend().key(); });
        expect_214([&] { (void)cs_obj.rend().key(); });
        expect_214([&] { (void)*s_obj.end(); });
        expect_214([&] { (void)*cs_obj.end(); });
        expect_214([&] { (void)*s_obj.rend(); });

        jsonkit::json s_arr = {10, 20};
        expect_214([&] { (void)*s_arr.end(); });
        expect_214([&] { (void)*s_arr.cend(); });
    }

    // 2. Default-constructed iterators: equality and relational operators on iterator and
    //    const_iterator.
    {
        jsonkit::json::iterator it1{};
        jsonkit::json::iterator it2{};
        jsonkit::json::const_iterator cit1{};
        jsonkit::json::const_iterator cit2{};

        EXPECT_TRUE(it1 == it2);
        EXPECT_FALSE(it1 != it2);
        EXPECT_FALSE(it1 < it2);
        EXPECT_TRUE(it1 <= it2);
        EXPECT_FALSE(it1 > it2);
        EXPECT_TRUE(it1 >= it2);

        EXPECT_TRUE(cit1 == cit2);
        EXPECT_FALSE(cit1 != cit2);
        EXPECT_FALSE(cit1 < cit2);
        EXPECT_TRUE(cit1 <= cit2);
        EXPECT_FALSE(cit1 > cit2);
        EXPECT_TRUE(cit1 >= cit2);

        EXPECT_TRUE(it1 == cit1);
        EXPECT_FALSE(it1 != cit1);
        EXPECT_FALSE(it1 < cit1);
        EXPECT_FALSE(cit1 < it1);
    }

    // 3. Range insert: non-array target (309), foreign pos (202), same array (211),
    //    iterators of two different values (210), inverted range (204), and valid range insert.
    {
        nlohmann::ordered_json o_a = {1, 2, 3};
        jsonkit::json s_a = {1, 2, 3};
        nlohmann::ordered_json o_b = {4, 5};
        jsonkit::json s_b = {4, 5};
        nlohmann::ordered_json o_c = {6, 7};
        jsonkit::json s_c = {6, 7};
        nlohmann::ordered_json o_obj = {{"k", 1}};
        jsonkit::json s_obj = {{"k", 1}};

        // Non-array target -> type_error.309
        expect_same_exception([&] { o_obj.insert(o_a.begin(), o_b.begin(), o_b.end()); },
                              [&] { s_obj.insert(s_a.begin(), s_b.begin(), s_b.end()); });

        // Foreign pos -> invalid_iterator.202
        expect_same_exception([&] { o_a.insert(o_b.begin(), o_b.begin(), o_b.end()); },
                              [&] { s_a.insert(s_b.begin(), s_b.begin(), s_b.end()); });

        // Same array -> invalid_iterator.211
        expect_same_exception([&] { o_a.insert(o_a.begin(), o_a.begin(), o_a.end()); },
                              [&] { s_a.insert(s_a.begin(), s_a.begin(), s_a.end()); });

        // Iterators of two different values -> invalid_iterator.210
        expect_same_exception([&] { o_a.insert(o_a.begin(), o_b.begin(), o_c.end()); },
                              [&] { s_a.insert(s_a.begin(), s_b.begin(), s_c.end()); });

        // Inverted range -> invalid_iterator.204
        nlohmann::ordered_json o_prim = 99;
        jsonkit::json s_prim = 99;
        expect_same_exception(
            [&] { (void)nlohmann::ordered_json(o_prim.end(), o_prim.begin()); },
            [&] { (void)jsonkit::json(s_prim.end(), s_prim.begin()); });
        expect_same_exception([&] { (void)o_prim.erase(o_prim.end(), o_prim.begin()); },
                              [&] { (void)s_prim.erase(s_prim.end(), s_prim.begin()); });
        // Inverted range on array insert -> invalid_iterator.204.
        // Asserted on jsonkit alone because nlohmann::basic_json::insert(pos, first, last)
        // forwards [first.m_it.array_iterator, last.m_it.array_iterator) unchecked to
        // std::vector::insert, which is undefined behaviour when last < first.
        {
            bool threw_204 = false;
            try {
                s_a.insert(s_a.begin(), s_b.end(), s_b.begin());
            } catch (const jsonkit::json::invalid_iterator& e) {
                threw_204 = true;
                EXPECT_EQ(e.id, 204);
            }
            EXPECT_TRUE(threw_204);
        }

        // Valid range from another array that succeeds
        expect_same_exception([&] { o_a.insert(o_a.begin() + 1, o_b.begin(), o_b.end()); },
                              [&] { s_a.insert(s_a.begin() + 1, s_b.begin(), s_b.end()); });
        assert_equivalent(o_a, s_a);
    }

    // 4. update(first, last) validation order and null target promotion.
    {
        nlohmann::ordered_json o_target = {{"k", 1}};
        jsonkit::json s_target = {{"k", 1}};
        nlohmann::ordered_json o_o1 = {{"a", 2}};
        jsonkit::json s_o1 = {{"a", 2}};
        nlohmann::ordered_json o_o2 = {{"b", 3}};
        jsonkit::json s_o2 = {{"b", 3}};
        nlohmann::ordered_json o_arr = {1, 2};
        jsonkit::json s_arr = {1, 2};
        nlohmann::ordered_json o_arr2 = {3, 4};
        jsonkit::json s_arr2 = {3, 4};

        // Two different objects -> invalid_iterator.210
        expect_same_exception([&] { o_target.update(o_o1.begin(), o_o2.end()); },
                              [&] { s_target.update(s_o1.begin(), s_o2.end()); });
        assert_equivalent(o_target, s_target);

        // Non-object range from a single array -> type_error.312
        expect_same_exception([&] { o_target.update(o_arr.begin(), o_arr.end()); },
                              [&] { s_target.update(s_arr.begin(), s_arr.end()); });
        assert_equivalent(o_target, s_target);

        // Two different arrays on object target: iterator match (210) precedes source-type (312)
        expect_same_exception([&] { o_target.update(o_arr.begin(), o_arr2.end()); },
                              [&] { s_target.update(s_arr.begin(), s_arr2.end()); });
        assert_equivalent(o_target, s_target);

        // Array target with two different objects: target-type (312) precedes iterator match (210)
        nlohmann::ordered_json o_arr_target = {10};
        jsonkit::json s_arr_target = {10};
        expect_same_exception([&] { o_arr_target.update(o_o1.begin(), o_o2.end()); },
                              [&] { s_arr_target.update(s_o1.begin(), s_o2.end()); });
        assert_equivalent(o_arr_target, s_arr_target);

        // Null target with non-object range: promotes null to {} before throwing type_error.312
        nlohmann::ordered_json o_null_target;
        jsonkit::json s_null_target;
        expect_same_exception([&] { o_null_target.update(o_arr.begin(), o_arr.end()); },
                              [&] { s_null_target.update(s_arr.begin(), s_arr.end()); });
        assert_equivalent(o_null_target, s_null_target);

        // Null target with two different arrays: promotes null to {} before throwing 210
        nlohmann::ordered_json o_null_target2;
        jsonkit::json s_null_target2;
        expect_same_exception([&] { o_null_target2.update(o_arr.begin(), o_arr2.end()); },
                              [&] { s_null_target2.update(s_arr.begin(), s_arr2.end()); });
        assert_equivalent(o_null_target2, s_null_target2);
    }

    // 5. All six relational operators for every pair drawn from: discarded, a NaN number,
    //    null, 0, 0.0, -0.0, "a", [], {} in both key orders, including discarded against itself.
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        std::vector<std::pair<nlohmann::ordered_json, jsonkit::json>> vals = {
            {nlohmann::ordered_json(nlohmann::ordered_json::value_t::discarded),
             jsonkit::json(jsonkit::json::value_t::discarded)},
            {nlohmann::ordered_json(nan), jsonkit::json(nan)},
            {nlohmann::ordered_json(nullptr), jsonkit::json(nullptr)},
            {nlohmann::ordered_json(0), jsonkit::json(0)},
            {nlohmann::ordered_json(0u), jsonkit::json(0u)},
            {nlohmann::ordered_json(0.0), jsonkit::json(0.0)},
            {nlohmann::ordered_json(-0.0), jsonkit::json(-0.0)},
            {nlohmann::ordered_json("a"), jsonkit::json("a")},
            {nlohmann::ordered_json::array(), jsonkit::json::array()},
            {nlohmann::ordered_json::object(), jsonkit::json::object()},
            {nlohmann::ordered_json({{"a", 1}, {"b", 2}}),
             jsonkit::json({{"a", 1}, {"b", 2}})},
            {nlohmann::ordered_json({{"b", 2}, {"a", 1}}),
             jsonkit::json({{"b", 2}, {"a", 1}})},
        };

        for (std::size_t i = 0; i < vals.size(); ++i) {
            for (std::size_t j = 0; j < vals.size(); ++j) {
                SCOPED_TRACE("pair (" + std::to_string(i) + ", " + std::to_string(j) + ")");
                const auto& o1 = vals[i].first;
                const auto& s1 = vals[i].second;
                const auto& o2 = vals[j].first;
                const auto& s2 = vals[j].second;
                EXPECT_EQ(o1 == o2, s1 == s2);
                EXPECT_EQ(o1 != o2, s1 != s2);
                EXPECT_EQ(o1 < o2, s1 < s2);
                EXPECT_EQ(o1 <= o2, s1 <= s2);
                EXPECT_EQ(o1 > o2, s1 > s2);
                EXPECT_EQ(o1 >= o2, s1 >= s2);
            }
        }
    }

    // 6. json(value_t::discarded): size(), empty(), begin() == end(), and iteration count.
    {
        nlohmann::ordered_json o_disc(nlohmann::ordered_json::value_t::discarded);
        jsonkit::json s_disc(jsonkit::json::value_t::discarded);

        EXPECT_EQ(o_disc.size(), s_disc.size());
        EXPECT_EQ(o_disc.empty(), s_disc.empty());
        EXPECT_EQ(o_disc.begin() == o_disc.end(), s_disc.begin() == s_disc.end());
        EXPECT_EQ(o_disc.cbegin() == o_disc.cend(), s_disc.cbegin() == s_disc.cend());

        std::size_t o_count = 0;
        for (auto it = o_disc.begin(); it != o_disc.end(); ++it) {
            EXPECT_TRUE(it->is_discarded());
            EXPECT_TRUE((*it).is_discarded());
            ++o_count;
        }
        std::size_t s_count = 0;
        for (auto it = s_disc.begin(); it != s_disc.end(); ++it) {
            EXPECT_TRUE(it->is_discarded());
            EXPECT_TRUE((*it).is_discarded());
            ++s_count;
        }
        EXPECT_EQ(o_count, s_count);
        EXPECT_EQ(s_count, 1u);
    }

    // 7. Parse errors: full message text, id, line/column, and byte for explicit and
    //    generated invalid documents (all prefixes and single-byte replacements of the
    //    valid parse corpus).
    {
        auto escape_input = [](std::string_view sv) -> std::string {
            std::string out;
            out.reserve(sv.size() + 8);
            for (unsigned char c : sv) {
                if (c == '\\') {
                    out += "\\\\";
                } else if (c == '\"') {
                    out += "\\\"";
                } else if (c == '\n') {
                    out += "\\n";
                } else if (c == '\r') {
                    out += "\\r";
                } else if (c == '\t') {
                    out += "\\t";
                } else if (c >= 0x20 && c < 0x7F) {
                    out.push_back(static_cast<char>(c));
                } else {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\x%02X", static_cast<unsigned int>(c));
                    out += buf;
                }
            }
            return out;
        };

        auto check_parse_candidate = [&](std::string_view candidate, bool ignore_comments) {
            SCOPED_TRACE("input=\"" + escape_input(candidate) +
                         "\" ignore_comments=" + (ignore_comments ? "true" : "false"));

            const bool o_acc = nlohmann::ordered_json::accept(candidate, ignore_comments);
            const bool s_acc = jsonkit::json::accept(candidate, ignore_comments);
            EXPECT_EQ(o_acc, s_acc);

            // allow_exceptions = false
            const auto o_noexc =
                nlohmann::ordered_json::parse(candidate, nullptr, false, ignore_comments);
            const auto s_noexc =
                jsonkit::json::parse(candidate, nullptr, false, ignore_comments);
            assert_equivalent(o_noexc, s_noexc);

            // allow_exceptions = true
            bool o_threw = false;
            int o_id = 0;
            std::size_t o_byte = std::numeric_limits<std::size_t>::max();
            std::string o_cat;
            std::string o_what;
            nlohmann::ordered_json o_val;
            try {
                o_val = nlohmann::ordered_json::parse(candidate, nullptr, true, ignore_comments);
            } catch (const nlohmann::ordered_json::parse_error& e) {
                o_threw = true;
                o_id = e.id;
                o_byte = e.byte;
                o_cat = "parse_error";
                o_what = e.what();
            } catch (const nlohmann::ordered_json::out_of_range& e) {
                o_threw = true;
                o_id = e.id;
                o_byte = 0;
                o_cat = "out_of_range";
                o_what = e.what();
            } catch (const nlohmann::ordered_json::exception& e) {
                o_threw = true;
                o_id = e.id;
                o_cat = "exception";
                o_what = e.what();
            }

            bool s_threw = false;
            int s_id = 0;
            std::size_t s_byte = std::numeric_limits<std::size_t>::max() - 1;
            std::string s_cat;
            std::string s_what;
            jsonkit::json s_val;
            try {
                s_val = jsonkit::json::parse(candidate, nullptr, true, ignore_comments);
            } catch (const jsonkit::json::parse_error& e) {
                s_threw = true;
                s_id = e.id;
                s_byte = e.byte;
                s_cat = "parse_error";
                s_what = e.what();
            } catch (const jsonkit::json::out_of_range& e) {
                s_threw = true;
                s_id = e.id;
                s_byte = 0;
                s_cat = "out_of_range";
                s_what = e.what();
            } catch (const jsonkit::json::exception& e) {
                s_threw = true;
                s_id = e.id;
                s_cat = "exception";
                s_what = e.what();
            }

            ASSERT_EQ(o_threw, s_threw)
                << "Oracle what: " << o_what << "\nSubject what: " << s_what;
            if (o_threw) {
                EXPECT_EQ(o_cat, s_cat)
                    << "Oracle what: " << o_what << "\nSubject what: " << s_what;
                EXPECT_EQ(o_id, s_id)
                    << "Oracle what: " << o_what << "\nSubject what: " << s_what;
                if (o_cat == "parse_error") {
                    EXPECT_GT(o_byte, 0u);
                }
                EXPECT_EQ(o_byte, s_byte)
                    << "Oracle what: " << o_what << "\nSubject what: " << s_what;
                EXPECT_EQ(o_what, s_what);
            } else {
                assert_equivalent(o_val, s_val);
            }
        };

        const std::vector<std::string> bad_inputs = {
            "[1, 2",
            "\"a\n\"",
            "[\n  tru]",
            "[\n  tru\n]",
            "[\n  fal]",
            "[\n  nul]",
            "[\n  bad]",
            "[1,]",
            "[1, ]",
            "{\"a\":1,}",
            "{\n  \"a\": 1,\n}",
            // 64-bit signed/unsigned integer boundaries and float overflows (out_of_range.406)
            "9223372036854775806",
            "9223372036854775807",
            "9223372036854775808",
            "9223372036854775809",
            "-9223372036854775807",
            "-9223372036854775808",
            "-9223372036854775809",
            "-9223372036854775810",
            "18446744073709551614",
            "18446744073709551615",
            "18446744073709551616",
            "18446744073709551617",
            "-18446744073709551615",
            "-18446744073709551616",
            "-18446744073709551617",
            "1.7976931348623157e+308",
            "-1.7976931348623157e+308",
            "1.7976931348623159e+308",
            "-1.7976931348623159e+308",
            "1e309",
            "-1e309",
            "[1, 1e309]",
            "{\"a\": -1e309}",
            std::string(310, '9'),
            "-" + std::string(310, '9'),
            // NUL byte cases (outside strings, inside strings, and inside // and /* */ comments)
            std::string("true\0x", 6),
            std::string("123\0", 4),
            std::string("0/*\0*/", 6),
            std::string("0//\0]", 5),
            std::string("0//\0", 4),
            std::string("[1,//\0 2]", 9),
            std::string("// comment\0 123", 15),
            std::string("\"a\0b\"", 5),
            std::string("\0", 1),
        };

        for (const auto& input : bad_inputs) {
            check_parse_candidate(input, false);
            check_parse_candidate(input, true);
        }

        // Deeply nested arrays and objects (valid and unclosed)
        for (std::size_t depth : {64u, 256u, 512u, 1025u}) {
            std::string nested_arr(depth, '[');
            nested_arr += "0";
            nested_arr.append(depth, ']');
            check_parse_candidate(nested_arr, false);
            check_parse_candidate(nested_arr, true);

            std::string unclosed_arr(depth, '[');
            unclosed_arr += "0";
            unclosed_arr.append(depth - 1, ']');
            check_parse_candidate(unclosed_arr, false);
            check_parse_candidate(unclosed_arr, true);

            std::string nested_obj;
            nested_obj.reserve(depth * 7 + 2);
            for (std::size_t d = 0; d < depth; ++d) {
                nested_obj += "{\"a\":";
            }
            nested_obj += "0";
            nested_obj.append(depth, '}');
            check_parse_candidate(nested_obj, false);
            check_parse_candidate(nested_obj, true);

            std::string unclosed_obj = nested_obj.substr(0, nested_obj.size() - 1);
            check_parse_candidate(unclosed_obj, false);
            check_parse_candidate(unclosed_obj, true);
        }

        const std::vector<std::string> valid_corpus = {
            "null",
            "true",
            "false",
            "0",
            "-0",
            "0.0",
            "-0.0",
            "1",
            "1.0",
            "-1",
            "-1.0",
            "42",
            "-42",
            "9223372036854775807",
            "-9223372036854775808",
            "9223372036854775808",
            "18446744073709551615",
            "18446744073709551616",
            "-9223372036854775809",
            "1e-308",
            "1e308",
            "1.7976931348623157e+308",
            "2.2250738585072014e-308",
            "5e-324",
            "\"\"",
            "\"hello world\"",
            "\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"",
            "\"\\u0000\\u001f\\u007f\\u0080\\u07ff\\u0800\\uffff\"",
            "\"\\ud83d\\ude00\\ud834\\udd1e\"",
            "\"한글과 日本語와 🚀\"",
            "[]",
            "[null,true,false,0,-0,0.0,-0.0,\"str\",[],{}]",
            "{}",
            "{\"z\":1,\"a\":2,\"m\":3}",
            "{\"a\":1,\"b\":2,\"a\":3,\"b\":4,\"c\":5}",
            "\xEF\xBB\xBF{\"bom\":true}",
            "{\n  // single line comment\n  \"a\": 1, /* multi-line\n  comment */ \"b\": [2, /* inline */ 3]\n}",
        };

        const char replacement_bytes[] = {']', '}', ',', ':', '"', '\n', '\x01', '\xff', '\0'};

        for (const auto& doc : valid_corpus) {
            // Every prefix of doc
            for (std::size_t len = 0; len <= doc.size(); ++len) {
                std::string_view prefix(doc.data(), len);
                check_parse_candidate(prefix, false);
                check_parse_candidate(prefix, true);
            }

            // Every single-byte replacement of each byte in doc
            for (std::size_t pos = 0; pos < doc.size(); ++pos) {
                for (char rep : replacement_bytes) {
                    std::string mutated = doc;
                    mutated[pos] = rep;
                    check_parse_candidate(mutated, false);
                    check_parse_candidate(mutated, true);
                }
            }

            // Every single-byte insertion of each replacement byte at every position in doc
            for (std::size_t pos = 0; pos <= doc.size(); ++pos) {
                for (char rep : replacement_bytes) {
                    std::string mutated = doc;
                    mutated.insert(pos, 1, rep);
                    check_parse_candidate(mutated, false);
                    check_parse_candidate(mutated, true);
                }
            }
        }
    }
}

} // namespace
