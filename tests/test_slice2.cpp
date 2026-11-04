#include "differential.hpp"

#include <array>
#include <deque>
#include <forward_list>
#include <list>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <valarray>
#include <vector>

using jsonkit::test::assert_equivalent;
using jsonkit::test::expect_both;
using jsonkit::test::expect_same_exception;

namespace {

using jk = jsonkit::json;
using oj = nlohmann::ordered_json;
using jkp = jsonkit::json_pointer;
using ojp = nlohmann::json_pointer<std::string>;

// ==================== User Types for ADL and Macro Tests ====================

namespace user_adl {

struct Point {
    int x = 0;
    int y = 0;
    friend bool operator==(const Point& a, const Point& b) {
        return a.x == b.x && a.y == b.y;
    }
};

inline void to_json(jk& j, const Point& p) {
    j = jk{{"x", p.x}, {"y", p.y}};
}

inline void from_json(const jk& j, Point& p) {
    j.at("x").get_to(p.x);
    j.at("y").get_to(p.y);
}

inline void to_json(oj& j, const Point& p) {
    j = oj{{"x", p.x}, {"y", p.y}};
}

inline void from_json(const oj& j, Point& p) {
    j.at("x").get_to(p.x);
    j.at("y").get_to(p.y);
}

struct NonDefault {
    int id;
    std::string label;
    NonDefault() = delete;
    NonDefault(int i, std::string l) : id(i), label(std::move(l)) {}
    friend bool operator==(const NonDefault& a, const NonDefault& b) {
        return a.id == b.id && a.label == b.label;
    }
};

} // namespace user_adl

} // namespace

namespace jsonkit {
template <>
struct adl_serializer<user_adl::NonDefault> {
    static user_adl::NonDefault from_json(const json& j) {
        return user_adl::NonDefault(j.at("id").get<int>(), j.at("label").get<std::string>());
    }
    static void to_json(json& j, const user_adl::NonDefault& v) {
        j = json{{"id", v.id}, {"label", v.label}};
    }
};
} // namespace jsonkit

namespace nlohmann {
template <>
struct adl_serializer<user_adl::NonDefault> {
    static user_adl::NonDefault from_json(const ordered_json& j) {
        return user_adl::NonDefault(j.at("id").get<int>(), j.at("label").get<std::string>());
    }
    static void to_json(ordered_json& j, const user_adl::NonDefault& v) {
        j = ordered_json{{"id", v.id}, {"label", v.label}};
    }
};
} // namespace nlohmann

namespace ns_jk {

struct IntrusivePerson {
    std::string name = "anon";
    int age = 0;
    std::vector<int> scores{1, 2};

    friend bool operator==(const IntrusivePerson& a, const IntrusivePerson& b) {
        return a.name == b.name && a.age == b.age && a.scores == b.scores;
    }

    JSONKIT_DEFINE_TYPE_INTRUSIVE(IntrusivePerson, name, age, scores)
};

struct IntrusiveDefaultConfig {
    std::string host = "localhost";
    int port = 8080;
    bool tls = true;

    friend bool operator==(const IntrusiveDefaultConfig& a, const IntrusiveDefaultConfig& b) {
        return a.host == b.host && a.port == b.port && a.tls == b.tls;
    }

    JSONKIT_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(IntrusiveDefaultConfig, host, port, tls)
};

struct NonIntrusiveItem {
    std::string title = "untitled";
    double price = 9.5;
    bool in_stock = false;

    friend bool operator==(const NonIntrusiveItem& a, const NonIntrusiveItem& b) {
        return a.title == b.title && a.price == b.price && a.in_stock == b.in_stock;
    }
};

JSONKIT_DEFINE_TYPE_NON_INTRUSIVE(NonIntrusiveItem, title, price, in_stock)

struct NonIntrusiveDefaultOpts {
    int retries = 3;
    std::string mode = "fast";
    std::vector<std::string> tags{"a", "b"};

    friend bool operator==(const NonIntrusiveDefaultOpts& a, const NonIntrusiveDefaultOpts& b) {
        return a.retries == b.retries && a.mode == b.mode && a.tags == b.tags;
    }
};

JSONKIT_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(NonIntrusiveDefaultOpts, retries, mode, tags)

enum class TaskState {
    Invalid = -1,
    Pending = 0,
    Running = 1,
    Completed = 2,
    Unmapped = 99
};

JSONKIT_JSON_SERIALIZE_ENUM(TaskState, {
    {TaskState::Invalid, nullptr},
    {TaskState::Pending, "pending"},
    {TaskState::Running, "running"},
    {TaskState::Completed, "completed"},
})

enum class PlainColor : int {
    Red = 1,
    Green = 2,
    Blue = 3
};

enum UnsignedFlags : unsigned int {
    FlagNone = 0u,
    FlagRead = 1u,
    FlagWrite = 2u
};

} // namespace ns_jk

namespace ns_oj {

struct IntrusivePerson {
    std::string name = "anon";
    int age = 0;
    std::vector<int> scores{1, 2};

    friend bool operator==(const IntrusivePerson& a, const IntrusivePerson& b) {
        return a.name == b.name && a.age == b.age && a.scores == b.scores;
    }

    friend void to_json(nlohmann::ordered_json& nlohmann_json_j,
                        const IntrusivePerson& nlohmann_json_t) {
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, name, age, scores))
    }
    friend void from_json(const nlohmann::ordered_json& nlohmann_json_j,
                          IntrusivePerson& nlohmann_json_t) {
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_FROM, name, age, scores))
    }
};

struct IntrusiveDefaultConfig {
    std::string host = "localhost";
    int port = 8080;
    bool tls = true;

    friend bool operator==(const IntrusiveDefaultConfig& a, const IntrusiveDefaultConfig& b) {
        return a.host == b.host && a.port == b.port && a.tls == b.tls;
    }

    friend void to_json(nlohmann::ordered_json& nlohmann_json_j,
                        const IntrusiveDefaultConfig& nlohmann_json_t) {
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, host, port, tls))
    }
    friend void from_json(const nlohmann::ordered_json& nlohmann_json_j,
                          IntrusiveDefaultConfig& nlohmann_json_t) {
        const IntrusiveDefaultConfig nlohmann_json_default_obj{};
        NLOHMANN_JSON_EXPAND(
            NLOHMANN_JSON_PASTE(NLOHMANN_JSON_FROM_WITH_DEFAULT, host, port, tls))
    }
};

struct NonIntrusiveItem {
    std::string title = "untitled";
    double price = 9.5;
    bool in_stock = false;

    friend bool operator==(const NonIntrusiveItem& a, const NonIntrusiveItem& b) {
        return a.title == b.title && a.price == b.price && a.in_stock == b.in_stock;
    }
};

inline void to_json(nlohmann::ordered_json& nlohmann_json_j,
                    const NonIntrusiveItem& nlohmann_json_t) {
    NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, title, price, in_stock))
}
inline void from_json(const nlohmann::ordered_json& nlohmann_json_j,
                      NonIntrusiveItem& nlohmann_json_t) {
    NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_FROM, title, price, in_stock))
}

struct NonIntrusiveDefaultOpts {
    int retries = 3;
    std::string mode = "fast";
    std::vector<std::string> tags{"a", "b"};

    friend bool operator==(const NonIntrusiveDefaultOpts& a, const NonIntrusiveDefaultOpts& b) {
        return a.retries == b.retries && a.mode == b.mode && a.tags == b.tags;
    }
};

inline void to_json(nlohmann::ordered_json& nlohmann_json_j,
                    const NonIntrusiveDefaultOpts& nlohmann_json_t) {
    NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, retries, mode, tags))
}
inline void from_json(const nlohmann::ordered_json& nlohmann_json_j,
                      NonIntrusiveDefaultOpts& nlohmann_json_t) {
    const NonIntrusiveDefaultOpts nlohmann_json_default_obj{};
    NLOHMANN_JSON_EXPAND(
        NLOHMANN_JSON_PASTE(NLOHMANN_JSON_FROM_WITH_DEFAULT, retries, mode, tags))
}

enum class TaskState {
    Invalid = -1,
    Pending = 0,
    Running = 1,
    Completed = 2,
    Unmapped = 99
};

NLOHMANN_JSON_SERIALIZE_ENUM(TaskState, {
    {TaskState::Invalid, nullptr},
    {TaskState::Pending, "pending"},
    {TaskState::Running, "running"},
    {TaskState::Completed, "completed"},
})

enum class PlainColor : int {
    Red = 1,
    Green = 2,
    Blue = 3
};

enum UnsignedFlags : unsigned int {
    FlagNone = 0u,
    FlagRead = 1u,
    FlagWrite = 2u
};

} // namespace ns_oj

// Also test NLOHMANN_DEFINE_TYPE_* directly on nlohmann::json to compare dumped JSON
namespace ns_nlohmann_std {

struct MacroDirect {
    std::string a = "hello";
    int b = 42;
    std::vector<int> c{10, 20};
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(MacroDirect, a, b, c)
};

struct MacroNonIntrusiveDirect {
    std::string k = "v";
    int n = 7;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MacroNonIntrusiveDirect, k, n)

} // namespace ns_nlohmann_std

namespace ns_jsonkit_std {

struct MacroDirect {
    std::string a = "hello";
    int b = 42;
    std::vector<int> c{10, 20};
    JSONKIT_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(MacroDirect, a, b, c)
};

struct MacroNonIntrusiveDirect {
    std::string k = "v";
    int n = 7;
};
JSONKIT_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MacroNonIntrusiveDirect, k, n)

} // namespace ns_jsonkit_std

namespace {

// ==================== 1. JSON Pointer Tests ====================

TEST(Slice2Pointer, ParsingAndEscapes) {
    const std::vector<std::string> pointer_inputs = {
        "",
        "/",
        "//",
        "///",
        "/foo",
        "/foo/0",
        "/a~1b",
        "/c%d",
        "/e^f",
        "/g|h",
        "/i\\j",
        "/k\"l",
        "/ ",
        "/m~0n",
        "/~0",
        "/~1",
        "/~01",
        "/~10",
        "/~0~1",
        "/~1~0",
        "/~0~0/~1~1",
        "/0",
        "/00",
        "/01",
        "/-",
        "/-1",
        "/1234567890",
        "/9999999999999999999999999999999",
        "no_leading_slash",
        "a/b",
        "/~",
        "/~2",
        "/~a",
        "/foo~",
        "/foo~2bar",
        "/~0~",
    };

    for (const auto& s : pointer_inputs) {
        SCOPED_TRACE("pointer: " + s);
        expect_both(
            [&]() {
                ojp p(s);
                return p.to_string() + "|" + std::string(p) + "|" +
                       (p.empty() ? "E" : "N");
            },
            [&]() {
                jkp p(s);
                return p.to_string() + "|" + std::string(p) + "|" +
                       (p.empty() ? "E" : "N");
            });
    }
}

TEST(Slice2Pointer, OperationsAndComparisons) {
    using namespace jsonkit::literals;

    jkp jp = "/a/b~1c/0"_json_pointer;
    ojp op("/a/b~1c/0");
    EXPECT_EQ(jp.to_string(), op.to_string());

    // Stream output
    {
        std::ostringstream ss_j, ss_o;
        ss_j << jp;
        ss_o << op;
        EXPECT_EQ(ss_j.str(), ss_o.str());
    }

    // push_back, pop_back, back, parent_pointer, empty
    for (int step = 0; step < 5; ++step) {
        expect_both(
            [&]() {
                return op.back();
            },
            [&]() {
                return jp.back();
            });
        expect_both(
            [&]() {
                return op.parent_pointer().to_string();
            },
            [&]() {
                return jp.parent_pointer().to_string();
            });
        expect_same_exception(
            [&]() {
                op.pop_back();
            },
            [&]() {
                jp.pop_back();
            });
        EXPECT_EQ(jp.to_string(), op.to_string());
        EXPECT_EQ(jp.empty(), op.empty());
    }

    // operator/ and operator/=
    jkp jp1("/root");
    ojp op1("/root");
    jkp jp2("/sub/leaf");
    ojp op2("/sub/leaf");

    EXPECT_EQ((jp1 / jp2).to_string(), (op1 / op2).to_string());
    EXPECT_EQ((jp1 / std::string("a~b/c")).to_string(),
              (op1 / std::string("a~b/c")).to_string());
    EXPECT_EQ((jp1 / std::size_t(42)).to_string(), (op1 / std::size_t(42)).to_string());

    jp1 /= jp2;
    op1 /= op2;
    EXPECT_EQ(jp1.to_string(), op1.to_string());

    jp1 /= std::string("tok~1~0");
    op1 /= std::string("tok~1~0");
    EXPECT_EQ(jp1.to_string(), op1.to_string());

    jp1 /= std::size_t(7);
    op1 /= std::size_t(7);
    EXPECT_EQ(jp1.to_string(), op1.to_string());

    std::string lval_tok = "lval";
    jp1.push_back(lval_tok);
    op1.push_back(lval_tok);
    jp1.push_back(std::string("rval"));
    op1.push_back(std::string("rval"));
    EXPECT_EQ(jp1.to_string(), op1.to_string());

    // Comparisons
    std::vector<std::string> cmp_strs = {"", "/", "/a", "/a/0", "/a/1", "/b", "/~0", "/~1"};
    for (const auto& s1 : cmp_strs) {
        for (const auto& s2 : cmp_strs) {
            jkp j_a(s1), j_b(s2);
            ojp o_a(s1), o_b(s2);
            EXPECT_EQ(j_a == j_b, o_a == o_b);
            EXPECT_EQ(j_a != j_b, o_a != o_b);
            EXPECT_EQ(j_a < j_b, o_a < o_b);
        }
    }
}

TEST(Slice2Pointer, AccessAtBracketValueContains) {
    const std::string doc_str = R"({
        "foo": ["bar", "baz"],
        "": 0,
        "a/b": 1,
        "c%d": 2,
        "e^f": 3,
        "g|h": 4,
        "i\\j": 5,
        "k\"l": 6,
        " ": 7,
        "m~n": 8,
        "~01": 9,
        "~10": 10,
        "nested": {"arr": [10, 20, {"deep": "val"}], "null_v": null, "num_v": 42}
    })";

    const jk j_const = jk::parse(doc_str);
    const oj o_const = oj::parse(doc_str);

    const std::vector<std::string> ptr_strs = {
        "",
        "/foo",
        "/foo/0",
        "/foo/1",
        "/foo/2",
        "/foo/-",
        "/foo/00",
        "/foo/01",
        "/foo/-1",
        "/foo/abc",
        "/foo/a",
        "/foo/1a",
        "/foo/999999999999999999999999999999",
        "/",
        "/a~1b",
        "/c%d",
        "/e^f",
        "/g|h",
        "/i\\j",
        "/k\"l",
        "/ ",
        "/m~0n",
        "/~001",
        "/~010",
        "/missing",
        "/missing/child",
        "/nested/arr/2/deep",
        "/nested/arr/2/deep/nope",
        "/nested/null_v/child",
        "/nested/num_v/0",
        "/nested/0",
    };

    for (const auto& ps : ptr_strs) {
        SCOPED_TRACE("ptr: " + ps);
        const jkp jp(ps);
        const ojp op(ps);

        // const at
        expect_both(
            [&]() { return o_const.at(op); },
            [&]() { return j_const.at(jp); });

        // mutable at
        expect_both(
            [&]() {
                oj o_mut = o_const;
                return o_mut.at(op);
            },
            [&]() {
                jk j_mut = j_const;
                return j_mut.at(jp);
            });

        // const operator[] (only on existing paths or paths that trigger checked errors in get_unchecked,
        // since const operator[] on missing object keys or out-of-bounds array indices is UB)
        bool has_path = false;
        try {
            has_path = o_const.contains(op);
        } catch (const oj::exception&) {
            has_path = false;
        }
        if (has_path || ps == "/foo/-" || ps == "/foo/00" || ps == "/foo/01" ||
            ps == "/foo/-1" || ps == "/foo/abc" || ps == "/foo/a" || ps == "/foo/1a" ||
            ps == "/foo/999999999999999999999999999999" || ps == "/nested/arr/2/deep/nope" ||
            ps == "/nested/null_v/child" || ps == "/nested/num_v/0") {
            expect_both(
                [&]() { return o_const[op]; },
                [&]() { return j_const[jp]; });
        }

        // mutable operator[] reading/creating path
        expect_both(
            [&]() {
                oj o_mut = o_const;
                auto& ref = o_mut[op];
                if (ref.is_null()) {
                    ref = "created";
                }
                return o_mut;
            },
            [&]() {
                jk j_mut = j_const;
                auto& ref = j_mut[jp];
                if (ref.is_null()) {
                    ref = "created";
                }
                return j_mut;
            });

        // contains
        expect_both(
            [&]() { return o_const.contains(op); },
            [&]() { return j_const.contains(jp); });

        // value with int default
        expect_both(
            [&]() { return o_const.value(op, -999); },
            [&]() { return j_const.value(jp, -999); });

        // value with string default and const char* default
        expect_both(
            [&]() { return o_const.value(op, std::string("def")); },
            [&]() { return j_const.value(jp, std::string("def")); });

        expect_both(
            [&]() { return o_const.value(op, "cstr_def"); },
            [&]() { return j_const.value(jp, "cstr_def"); });
    }

    // Also test value(ptr, default) on non-object receivers (array, null, integer, string)
    const std::vector<std::string> non_obj_docs = {"[1, 2, 3]", "null", "42", "\"str\"", "true"};
    for (const auto& nd : non_obj_docs) {
        const jk jn = jk::parse(nd);
        const oj on = oj::parse(nd);
        for (const auto& ps : {"", "/0", "/a"}) {
            const jkp jp(ps);
            const ojp op(ps);
            expect_both(
                [&]() { return on.value(op, 123); },
                [&]() { return jn.value(jp, 123); });
            expect_both(
                [&]() { return on.value(op, "abc"); },
                [&]() { return jn.value(jp, "abc"); });
        }
    }
}

TEST(Slice2Pointer, FlattenAndUnflatten) {
    const std::vector<std::string> docs = {
        R"({"a": {"b": [1, 2, {"c": true, "d": null}]}, "e": [], "f": {}})",
        R"({})",
        R"([])",
        R"({"~": {"/": "tilde_slash"}})",
    };

    for (const auto& d : docs) {
        const jk j = jk::parse(d);
        const oj o = oj::parse(d);

        expect_both(
            [&]() { return o.flatten(); },
            [&]() { return j.flatten(); });

        expect_both(
            [&]() { return o.flatten().unflatten(); },
            [&]() { return j.flatten().unflatten(); });
    }

    // Error cases for unflatten: 314 (non-object) and 315 (non-primitive value)
    for (const auto& bad : {"[1,2]", "42", R"({"/a": [1, 2]})", R"({"/a": {"b": 1}})"}) {
        const jk j = jk::parse(bad);
        const oj o = oj::parse(bad);
        expect_both(
            [&]() { return o.unflatten(); },
            [&]() { return j.unflatten(); });
    }
}

// ==================== 2. Conversions and ADL Tests ====================

template <typename T>
void check_stl_roundtrip(const T& val, const std::vector<std::string>& bad_jsons) {
    jk j(val);
    oj o(val);
    assert_equivalent(o, j);

    EXPECT_EQ(j.get<T>(), o.get<T>());
    EXPECT_EQ(j.get<T>(), val);

    T j_to{}, o_to{};
    j.get_to(j_to);
    o.get_to(o_to);
    EXPECT_EQ(j_to, o_to);
    EXPECT_EQ(j_to, val);

    // Implicit conversion
    T j_imp = j;
    T o_imp = o;
    EXPECT_EQ(j_imp, o_imp);

    // Via value(key, default)
    jk j_obj = {{"k", val}};
    oj o_obj = {{"k", val}};
    assert_equivalent(o_obj, j_obj);
    EXPECT_EQ(j_obj.value("k", T{}), o_obj.value("k", T{}));
    EXPECT_EQ(j_obj.value("missing", val), o_obj.value("missing", val));

    for (const auto& bad : bad_jsons) {
        const jk jb = jk::parse(bad);
        const oj ob = oj::parse(bad);
        expect_both(
            [&]() { return ob.get<T>(); },
            [&]() { return jb.get<T>(); });
    }
}

TEST(Slice2Conversions, SequenceAndSetContainers) {
    check_stl_roundtrip(std::vector<int>{1, -2, 3, 0, 42},
                        {"null", "true", "42", "\"s\"", "{\"a\":1}", "[\"not_int\"]"});
    check_stl_roundtrip(std::vector<std::string>{"a", "", "hello"},
                        {"123", "[1, 2]"});
    check_stl_roundtrip(std::vector<bool>{true, false, true, true, false},
                        {"null", "[1, 0]", "{\"a\":true}"});
    check_stl_roundtrip(std::deque<int>{10, 20, 30},
                        {"null", "\"str\"", "[\"bad\"]"});
    check_stl_roundtrip(std::list<std::string>{"x", "y", "z"},
                        {"42", "[false]"});
    check_stl_roundtrip(std::forward_list<int>{4, 5, 6},
                        {"{}", "[\"bad\"]"});
    check_stl_roundtrip(std::array<int, 4>{{1, 2, 3, 4}},
                        {"null", "[1, 2]", "[1, 2, 3, \"x\"]"});
    check_stl_roundtrip(std::set<std::string>{"alpha", "beta", "gamma"},
                        {"null", "[123]"});
    check_stl_roundtrip(std::unordered_set<int>{10, 20, 30},
                        {"{}", "[\"s\"]"});

    // std::valarray
    {
        std::valarray<int> va{3, 1, 4, 1, 5};
        jk j(va);
        oj o(va);
        assert_equivalent(o, j);
        auto j_va = j.get<std::valarray<int>>();
        auto o_va = o.get<std::valarray<int>>();
        ASSERT_EQ(j_va.size(), o_va.size());
        for (std::size_t i = 0; i < j_va.size(); ++i) {
            EXPECT_EQ(j_va[i], o_va[i]);
        }
        for (const auto& bad : {"null", "{\"a\":1}", "[\"x\"]"}) {
            const jk jb = jk::parse(bad);
            const oj ob = oj::parse(bad);
            expect_both(
                [&]() { return ob.get<std::valarray<int>>().size(); },
                [&]() { return jb.get<std::valarray<int>>().size(); });
        }
    }

    // C array
    {
        int carr[3] = {7, 8, 9};
        jk j(carr);
        oj o(carr);
        assert_equivalent(o, j);

        int j_out[3] = {0, 0, 0};
        int o_out[3] = {0, 0, 0};
        j.get_to(j_out);
        o.get_to(o_out);
        for (int i = 0; i < 3; ++i) {
            EXPECT_EQ(j_out[i], o_out[i]);
            EXPECT_EQ(j_out[i], carr[i]);
        }
    }
}

TEST(Slice2Conversions, AssociativePairTupleOptional) {
    check_stl_roundtrip(std::map<std::string, int>{{"b", 2}, {"a", 1}},
                        {"null", "[1,2]", "{\"a\":\"not_int\"}"});
    check_stl_roundtrip(std::unordered_map<std::string, int>{{"k1", 10}, {"k2", 20}},
                        {"null", "[1,2]", "{\"k1\":[]}"});

    // Non-string keys (serialized as array of [key, value] pairs)
    check_stl_roundtrip(std::map<int, std::string>{{1, "one"}, {2, "two"}},
                        {"null", "{\"1\":\"one\"}", "[1, 2]", "[[1]]", "[[\"bad\", \"one\"]]"});
    check_stl_roundtrip(std::unordered_map<int, std::string>{{5, "five"}, {6, "six"}},
                        {"null", "{\"5\":\"five\"}", "[123]", "[[5]]"});

    // Pair and Tuple
    check_stl_roundtrip(std::pair<int, std::string>{42, "answer"},
                        {"null", "{}", "[42]", "[\"bad\", \"answer\"]"});
    check_stl_roundtrip(std::tuple<int, std::string, bool, double>{1, "two", true, 3.5},
                        {"null", "[1, \"two\"]", "[1, \"two\", \"not_bool\", 3.5]"});
    {
        // Empty tuple: oracle's to_json sets j = {} (null), while from_json requires array
        std::tuple<> empty_t{};
        jk j_et(empty_t);
        oj o_et(empty_t);
        assert_equivalent(o_et, j_et);
        expect_both(
            [&]() { return o_et.get<std::tuple<>>(); },
            [&]() { return j_et.get<std::tuple<>>(); });

        jk j_arr = jk::array();
        oj o_arr = oj::array();
        EXPECT_EQ(j_arr.get<std::tuple<>>(), o_arr.get<std::tuple<>>());
    }

    // std::optional<T>
    {
        std::optional<int> opt_none = std::nullopt;
        std::optional<int> opt_some = 42;

        jk j_none(opt_none);
        EXPECT_TRUE(j_none.is_null());
        EXPECT_EQ(j_none.get<std::optional<int>>(), std::nullopt);

        jk j_some(opt_some);
        EXPECT_EQ(j_some, jk(42));
        EXPECT_EQ(j_some.get<std::optional<int>>(), std::optional<int>(42));

        std::optional<int> target = 99;
        j_none.get_to(target);
        EXPECT_FALSE(target.has_value());
        j_some.get_to(target);
        EXPECT_EQ(target, std::optional<int>(42));

        jk j_bad = "not_an_int";
        EXPECT_THROW((void)j_bad.get<std::optional<int>>(), jk::type_error);
    }
}

TEST(Slice2Conversions, AdlAndCustomSerializer) {
    user_adl::Point pt{12, -34};
    jk jp(pt);
    oj op(pt);
    assert_equivalent(op, jp);
    EXPECT_EQ(jp.get<user_adl::Point>(), op.get<user_adl::Point>());
    EXPECT_EQ(jp.get<user_adl::Point>(), pt);

    user_adl::Point pt_to_j{}, pt_to_o{};
    jp.get_to(pt_to_j);
    op.get_to(pt_to_o);
    EXPECT_EQ(pt_to_j, pt_to_o);

    jk j_wrap = {{"pt", pt}};
    oj o_wrap = {{"pt", pt}};
    EXPECT_EQ(j_wrap.value("pt", user_adl::Point{0, 0}),
              o_wrap.value("pt", user_adl::Point{0, 0}));
    EXPECT_EQ(j_wrap.value("missing", user_adl::Point{9, 9}),
              o_wrap.value("missing", user_adl::Point{9, 9}));

    // Non-default-constructible type via adl_serializer
    user_adl::NonDefault nd(77, "item");
    jk j_nd(nd);
    oj o_nd(nd);
    assert_equivalent(o_nd, j_nd);
    EXPECT_EQ(j_nd.get<user_adl::NonDefault>(), o_nd.get<user_adl::NonDefault>());
    EXPECT_EQ(j_nd.get<user_adl::NonDefault>(), nd);

    std::vector<user_adl::NonDefault> vec_nd{user_adl::NonDefault(1, "a"),
                                             user_adl::NonDefault(2, "b")};
    jk j_vnd(vec_nd);
    oj o_vnd(vec_nd);
    assert_equivalent(o_vnd, j_vnd);
    EXPECT_EQ(j_vnd.get<std::vector<user_adl::NonDefault>>(),
              o_vnd.get<std::vector<user_adl::NonDefault>>());
}

TEST(Slice2Conversions, MacrosAndEnums) {
    // Compare NLOHMANN_DEFINE_TYPE_* on nlohmann::json directly with JSONKIT_DEFINE_TYPE_*
    {
        ns_jsonkit_std::MacroDirect jm{"custom", 100, {7, 8, 9}};
        ns_nlohmann_std::MacroDirect nm{"custom", 100, {7, 8, 9}};
        jk jj(jm);
        nlohmann::json nj(nm);
        EXPECT_EQ(jk::parse(nj.dump()), jj);

        ns_jsonkit_std::MacroNonIntrusiveDirect jni{"key", 55};
        ns_nlohmann_std::MacroNonIntrusiveDirect nni{"key", 55};
        EXPECT_EQ(jk(jni).dump(), nlohmann::json(nni).dump());
    }

    // Intrusive (without default)
    {
        ns_jk::IntrusivePerson jp{"Alice", 30, {10, 20, 30}};
        ns_oj::IntrusivePerson op{"Alice", 30, {10, 20, 30}};
        jk j(jp);
        oj o(op);
        assert_equivalent(o, j);
        EXPECT_EQ(j.get<ns_jk::IntrusivePerson>().name, o.get<ns_oj::IntrusivePerson>().name);
        EXPECT_EQ(j.get<ns_jk::IntrusivePerson>().age, o.get<ns_oj::IntrusivePerson>().age);
        EXPECT_EQ(j.get<ns_jk::IntrusivePerson>().scores, o.get<ns_oj::IntrusivePerson>().scores);

        // Missing field throws out_of_range.403; wrong type throws type_error.302
        for (const auto& bad : {R"({"name": "Alice"})",
                                R"({"name": "Alice", "age": "not_int", "scores": []})",
                                R"(null)",
                                R"([1, 2, 3])"}) {
            const jk jb = jk::parse(bad);
            const oj ob = oj::parse(bad);
            expect_both(
                [&]() { return ob.get<ns_oj::IntrusivePerson>().age; },
                [&]() { return jb.get<ns_jk::IntrusivePerson>().age; });
        }
    }

    // Intrusive with default
    {
        ns_jk::IntrusiveDefaultConfig jc{"example.com", 443, false};
        ns_oj::IntrusiveDefaultConfig oc{"example.com", 443, false};
        jk j(jc);
        oj o(oc);
        assert_equivalent(o, j);

        for (const auto& input : {R"({})",
                                  R"({"host": "custom.org"})",
                                  R"({"port": 9000, "tls": false})",
                                  R"({"host": 123})",
                                  R"(null)",
                                  R"([1, 2])"}) {
            const jk ji = jk::parse(input);
            const oj oi = oj::parse(input);
            expect_both(
                [&]() {
                    auto v = oi.get<ns_oj::IntrusiveDefaultConfig>();
                    return v.host + ":" + std::to_string(v.port) + ":" + (v.tls ? "1" : "0");
                },
                [&]() {
                    auto v = ji.get<ns_jk::IntrusiveDefaultConfig>();
                    return v.host + ":" + std::to_string(v.port) + ":" + (v.tls ? "1" : "0");
                });
        }
    }

    // Non-intrusive (without default)
    {
        ns_jk::NonIntrusiveItem ji{"Book", 19.99, true};
        ns_oj::NonIntrusiveItem oi{"Book", 19.99, true};
        jk j(ji);
        oj o(oi);
        assert_equivalent(o, j);

        for (const auto& input : {R"({"title": "Pen", "price": 1.5, "in_stock": false})",
                                  R"({"title": "Pen"})",
                                  R"({"title": "Pen", "price": "free", "in_stock": true})",
                                  R"(null)"}) {
            const jk jb = jk::parse(input);
            const oj ob = oj::parse(input);
            expect_both(
                [&]() {
                    auto v = ob.get<ns_oj::NonIntrusiveItem>();
                    return v.title + ":" + (v.in_stock ? "1" : "0");
                },
                [&]() {
                    auto v = jb.get<ns_jk::NonIntrusiveItem>();
                    return v.title + ":" + (v.in_stock ? "1" : "0");
                });
        }
    }

    // Non-intrusive with default
    {
        ns_jk::NonIntrusiveDefaultOpts jo{5, "slow", {"x"}};
        ns_oj::NonIntrusiveDefaultOpts oo{5, "slow", {"x"}};
        jk j(jo);
        oj o(oo);
        assert_equivalent(o, j);

        for (const auto& input : {R"({})",
                                  R"({"retries": 10})",
                                  R"({"mode": "turbo", "tags": ["p", "q"]})",
                                  R"({"retries": "bad"})",
                                  R"(null)"}) {
            const jk jb = jk::parse(input);
            const oj ob = oj::parse(input);
            expect_both(
                [&]() {
                    auto v = ob.get<ns_oj::NonIntrusiveDefaultOpts>();
                    return std::to_string(v.retries) + ":" + v.mode + ":" +
                           std::to_string(v.tags.size());
                },
                [&]() {
                    auto v = jb.get<ns_jk::NonIntrusiveDefaultOpts>();
                    return std::to_string(v.retries) + ":" + v.mode + ":" +
                           std::to_string(v.tags.size());
                });
        }
    }

    // Enum serialization (JSONKIT_JSON_SERIALIZE_ENUM and default integer enums)
    {
        const std::pair<ns_jk::TaskState, ns_oj::TaskState> states[] = {
            {ns_jk::TaskState::Invalid, ns_oj::TaskState::Invalid},
            {ns_jk::TaskState::Pending, ns_oj::TaskState::Pending},
            {ns_jk::TaskState::Running, ns_oj::TaskState::Running},
            {ns_jk::TaskState::Completed, ns_oj::TaskState::Completed},
            {ns_jk::TaskState::Unmapped, ns_oj::TaskState::Unmapped},
        };
        for (const auto& [js, os] : states) {
            jk j(js);
            oj o(os);
            assert_equivalent(o, j);
            EXPECT_EQ(static_cast<int>(j.get<ns_jk::TaskState>()),
                      static_cast<int>(o.get<ns_oj::TaskState>()));
        }

        for (const auto& raw : {R"("pending")", R"("running")", R"("completed")",
                                R"(null)", R"("unknown_state")", R"(123)"}) {
            const jk j = jk::parse(raw);
            const oj o = oj::parse(raw);
            EXPECT_EQ(static_cast<int>(j.get<ns_jk::TaskState>()),
                      static_cast<int>(o.get<ns_oj::TaskState>()));
        }

        // Plain enums without macro
        jk jc(ns_jk::PlainColor::Green);
        oj oc(ns_oj::PlainColor::Green);
        assert_equivalent(oc, jc);
        EXPECT_EQ(static_cast<int>(jc.get<ns_jk::PlainColor>()),
                  static_cast<int>(oc.get<ns_oj::PlainColor>()));

        jk ju(ns_jk::UnsignedFlags::FlagWrite);
        oj ou(ns_oj::UnsignedFlags::FlagWrite);
        assert_equivalent(ou, ju);
        EXPECT_EQ(static_cast<unsigned>(ju.get<ns_jk::UnsignedFlags>()),
                  static_cast<unsigned>(ou.get<ns_oj::UnsignedFlags>()));
    }
}

// ==================== 3. Patch, Merge Patch, and Diff Tests ====================

TEST(Slice2PatchAndDiff, Rfc6902ExamplesAndErrors) {
    struct PatchCase {
        const char* doc;
        const char* patch;
    };

    const std::vector<PatchCase> cases = {
        // RFC 6902 Appendix A.1 - A.16
        {R"({"foo": "bar"})",
         R"([{"op": "add", "path": "/baz", "value": "qux"}])"},
        {R"({"foo": ["bar", "baz"]})",
         R"([{"op": "add", "path": "/foo/1", "value": "qux"}])"},
        {R"({"baz": "qux", "foo": "bar"})",
         R"([{"op": "remove", "path": "/baz"}])"},
        {R"({"foo": ["bar", "qux", "baz"]})",
         R"([{"op": "remove", "path": "/foo/1"}])"},
        {R"({"baz": "qux", "foo": "bar"})",
         R"([{"op": "replace", "path": "/baz", "value": "boo"}])"},
        {R"({"foo": {"bar": "baz", "waldo": "fred"}, "qux": {"corge": "grault"}})",
         R"([{"op": "move", "from": "/foo/waldo", "path": "/qux/thud"}])"},
        {R"({"foo": ["all", "grass", "cows", "eat"]})",
         R"([{"op": "move", "from": "/foo/1", "path": "/foo/3"}])"},
        {R"({"baz": "qux", "foo": ["a", 2, "c"]})",
         R"([{"op": "test", "path": "/baz", "value": "qux"},
             {"op": "test", "path": "/foo/1", "value": 2}])"},
        {R"({"baz": "qux"})",
         R"([{"op": "test", "path": "/baz", "value": "bar"}])"},
        {R"({"foo": "bar"})",
         R"([{"op": "add", "path": "/child", "value": {"grandchild": {}}}])"},
        {R"({"foo": "bar"})",
         R"([{"op": "add", "path": "/baz", "value": "qux", "xyz": 123}])"},
        {R"({"foo": "bar"})",
         R"([{"op": "add", "path": "/baz/bat", "value": "qux"}])"},
        {R"({"/": 9, "~1": 10})",
         R"([{"op": "test", "path": "/~01", "value": 10}])"},
        {R"({"/": 9, "~1": 10})",
         R"([{"op": "test", "path": "/~01", "value": "10"}])"},
        {R"({"foo": ["bar"]})",
         R"([{"op": "add", "path": "/foo/-", "value": ["abc", "def"]}])"},
        // Additional operations: copy, root add/replace, error cases
        {R"({"foo": {"bar": "baz"}})",
         R"([{"op": "copy", "from": "/foo/bar", "path": "/foo/qux"}])"},
        {R"({"a": 1})",
         R"([{"op": "add", "path": "", "value": {"replaced": true}}])"},
        {R"({"a": 1})",
         R"([{"op": "replace", "path": "", "value": [1, 2, 3]}])"},
        // Error cases
        {R"({"a": 1})", R"({"op": "add", "path": "/b", "value": 2})"}, // non-array patch (104)
        {R"({"a": 1})", R"([123])"},                                   // non-object elem (104)
        {R"({"a": 1})", R"([{"path": "/a"}])"},                        // missing op (105)
        {R"({"a": 1})", R"([{"op": 123, "path": "/a"}])"},             // non-string op (105)
        {R"({"a": 1})", R"([{"op": "add", "value": 1}])"},             // missing path (105)
        {R"({"a": 1})", R"([{"op": "add", "path": 1, "value": 1}])"},  // non-string path (105)
        {R"({"a": 1})", R"([{"op": "add", "path": "/b"}])"},           // missing value (105)
        {R"({"a": 1})", R"([{"op": "replace", "path": "/a"}])"},       // missing value (105)
        {R"({"a": 1})", R"([{"op": "test", "path": "/a"}])"},          // missing value (105)
        {R"({"a": 1})", R"([{"op": "move", "path": "/b"}])"},          // missing from (105)
        {R"({"a": 1})", R"([{"op": "move", "path": "/b", "from": 1}])"}, // non-string from (105)
        {R"({"a": 1})", R"([{"op": "copy", "path": "/b"}])"},          // missing from (105)
        {R"({"a": 1})", R"([{"op": "unknown", "path": "/a"}])"},       // invalid op (105)
        {R"({"a": 1})", R"([{"op": "remove", "path": ""}])"},          // remove root (405)
        {R"({"a": 1})", R"([{"op": "remove", "path": "/missing"}])"},  // remove missing key (403)
        {R"({"a": [1, 2]})", R"([{"op": "add", "path": "/a/5", "value": 9}])"}, // index > size (401)
        {R"({"a": [1, 2]})", R"([{"op": "remove", "path": "/a/2"}])"}, // remove index >= size (401)
        {R"({"a": [1, 2]})", R"([{"op": "remove", "path": "/a/-"}])"}, // remove '-' (402)
        {R"({"a": [1, 2]})", R"([{"op": "replace", "path": "/a/-", "value": 3}])"}, // replace '-' (402)
        {R"({"a": [1, 2]})", R"([{"op": "add", "path": "/a/01", "value": 3}])"}, // leading zero (106)
        {R"({"a": [1, 2]})", R"([{"op": "add", "path": "/a/xy", "value": 3}])"}, // non-number (109)
        {R"({"a": [1, 2]})", R"([{"op": "add", "path": "no_slash", "value": 3}])"}, // bad ptr (107)
        {R"({"a": [1, 2]})", R"([{"op": "add", "path": "/~2", "value": 3}])"}, // bad escape (108)
        {R"({"a": 1})", R"([{"op": "test", "path": "/missing", "value": 1}])"}, // test missing (501)
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(std::string("doc=") + c.doc + " patch=" + c.patch);
        const jk j_doc = jk::parse(c.doc);
        const oj o_doc = oj::parse(c.doc);
        const jk j_patch = jk::parse(c.patch);
        const oj o_patch = oj::parse(c.patch);

        expect_both(
            [&]() { return o_doc.patch(o_patch); },
            [&]() { return j_doc.patch(j_patch); });

        expect_both(
            [&]() {
                oj o_copy = o_doc;
                o_copy.patch_inplace(o_patch);
                return o_copy;
            },
            [&]() {
                jk j_copy = j_doc;
                j_copy.patch_inplace(j_patch);
                return j_copy;
            });
    }
}

TEST(Slice2PatchAndDiff, Rfc7396MergePatch) {
    // All 15 test cases from RFC 7396 Appendix A + RFC example
    const std::pair<const char*, const char*> rfc7396_cases[] = {
        {R"({"a":"b"})", R"({"a":"c"})"},
        {R"({"a":"b"})", R"({"b":"c"})"},
        {R"({"a":"b"})", R"({"a":null})"},
        {R"({"a":"b","b":"c"})", R"({"a":null})"},
        {R"({"a":["b"]})", R"({"a":"c"})"},
        {R"({"a":"c"})", R"({"a":["b"]})"},
        {R"({"a":{"b":"c"}})", R"({"a":{"b":"d","c":null}})"},
        {R"({"a":[{"b":"c"}]})", R"({"a":[1]})"},
        {R"(["a","b"])", R"(["c","d"])"},
        {R"({"a":"b"})", R"(["c"])"},
        {R"({"a":"foo"})", R"(null)"},
        {R"({"a":"foo"})", R"("bar")"},
        {R"({"e":null})", R"({"a":1})"},
        {R"([1,2])", R"({"a":"b","c":null})"},
        {R"({})", R"({"a":{"bb":{"ccc":null}}})"},
        {R"({
            "title": "Goodbye!",
            "author": {"givenName": "John", "familyName": "Doe"},
            "tags": ["example", "sample"],
            "content": "This will be unchanged"
        })",
         R"({
            "title": "Hello!",
            "phoneNumber": "+01-123-456-7890",
            "author": {"familyName": null},
            "tags": ["example"]
        })"},
    };

    for (const auto& [orig_str, patch_str] : rfc7396_cases) {
        jk j = jk::parse(orig_str);
        oj o = oj::parse(orig_str);
        const jk jp = jk::parse(patch_str);
        const oj op = oj::parse(patch_str);

        j.merge_patch(jp);
        o.merge_patch(op);
        assert_equivalent(o, j);
    }
}

namespace {

std::pair<jk, oj> make_random_doc(std::mt19937_64& rng, int depth = 0) {
    std::uniform_int_distribution<int> type_dist(0, depth >= 3 ? 4 : 6);
    int kind = type_dist(rng);
    switch (kind) {
        case 0:
            return {jk(nullptr), oj(nullptr)};
        case 1: {
            bool b = (rng() & 1) != 0;
            return {jk(b), oj(b)};
        }
        case 2: {
            int64_t v = static_cast<int64_t>(rng() % 200) - 100;
            return {jk(v), oj(v)};
        }
        case 3: {
            double d = static_cast<double>(static_cast<int>(rng() % 1000) - 500) / 4.0;
            return {jk(d), oj(d)};
        }
        case 4: {
            static const char* const strs[] = {
                "", "a", "b", "foo", "a/b", "m~n", "~01", "hello", "0", "1", "-"};
            const char* s = strs[rng() % (sizeof(strs) / sizeof(strs[0]))];
            return {jk(s), oj(s)};
        }
        case 5: {
            jk ja = jk::array();
            oj oa = oj::array();
            std::size_t len = rng() % 5;
            for (std::size_t i = 0; i < len; ++i) {
                auto [je, oe] = make_random_doc(rng, depth + 1);
                ja.push_back( std::move(je));
                oa.push_back(std::move(oe));
            }
            return {std::move(ja), std::move(oa)};
        }
        default: {
            jk jo = jk::object();
            oj oo = oj::object();
            static const char* const keys[] = {
                "a", "b", "c", "d", "a/b", "m~n", "", "0", "1", "nested"};
            std::size_t len = rng() % 5;
            for (std::size_t i = 0; i < len; ++i) {
                const char* k = keys[rng() % (sizeof(keys) / sizeof(keys[0]))];
                auto [jv, ov] = make_random_doc(rng, depth + 1);
                jo[k] = std::move(jv);
                oo[k] = std::move(ov);
            }
            return {std::move(jo), std::move(oo)};
        }
    }
}

void mutate_random_doc(std::mt19937_64& rng, jk& j, oj& o, int depth = 0) {
    if (j.is_object() && !j.empty() && (rng() % 3 != 0)) {
        // Modify, remove, or add a key
        int action = static_cast<int>(rng() % 3);
        if (action == 0) {
            auto idx = rng() % j.size();
            auto jit = j.begin();
            auto oit = o.begin();
            for (std::size_t k = 0; k < idx; ++k) {
                ++jit;
                ++oit;
            }
            mutate_random_doc(rng, *jit, *oit, depth + 1);
        } else if (action == 1) {
            auto idx = rng() % j.size();
            auto jit = j.begin();
            auto oit = o.begin();
            for (std::size_t k = 0; k < idx; ++k) {
                ++jit;
                ++oit;
            }
            j.erase(jit);
            o.erase(oit);
        } else {
            auto [jv, ov] = make_random_doc(rng, depth + 1);
            std::string k = "new_" + std::to_string(rng() % 6);
            j[k] = std::move(jv);
            o[k] = std::move(ov);
        }
    } else if (j.is_array() && !j.empty() && (rng() % 3 != 0)) {
        int action = static_cast<int>(rng() % 3);
        if (action == 0) {
            std::size_t idx = rng() % j.size();
            mutate_random_doc(rng, j[idx], o[idx], depth + 1);
        } else if (action == 1) {
            std::size_t idx = rng() % j.size();
            j.erase(idx);
            o.erase(idx);
        } else {
            auto [jv, ov] = make_random_doc(rng, depth + 1);
            j.push_back(std::move(jv));
            o.push_back(std::move(ov));
        }
    } else {
        auto [jv, ov] = make_random_doc(rng, depth + 1);
        j = std::move(jv);
        o = std::move(ov);
    }
}

} // namespace

TEST(Slice2PatchAndDiff, GeneratedPairsDiffAndPatchRoundTrip) {
    std::mt19937_64 rng(0x51C2'D1FF'BEEFULL);

    for (int i = 0; i < 250; ++i) {
        auto [ja, oa] = make_random_doc(rng);
        jk jb = ja;
        oj ob = oa;

        int edits = static_cast<int>(rng() % 4);
        for (int e = 0; e < edits; ++e) {
            mutate_random_doc(rng, jb, ob);
        }

        assert_equivalent(oa, ja);
        assert_equivalent(ob, jb);

        jk j_diff = jk::diff(ja, jb);
        oj o_diff = oj::diff(oa, ob);
        assert_equivalent(o_diff, j_diff);

        jk j_patched = ja.patch(j_diff);
        oj o_patched = oa.patch(o_diff);
        assert_equivalent(o_patched, j_patched);
        EXPECT_EQ(nlohmann::json::parse(j_patched.dump()), nlohmann::json::parse(jb.dump()));
        EXPECT_EQ(nlohmann::json::parse(o_patched.dump()), nlohmann::json::parse(ob.dump()));
    }
}

// ==================== 4. Parser Callback Tests ====================

TEST(Slice2Callback, EventSequenceAndDiscardDecisions) {
    const std::vector<std::string> corpus = {
        "null",
        "true",
        "false",
        "0",
        "-42",
        "3.25",
        "\"hello\"",
        "[]",
        "[1, 2, 3]",
        "{}",
        R"({"a": 1, "b": [true, false, null, {"c": "d"}], "e": {"f": []}})",
        R"([{"k1": [1, [2, 3]], "k2": {"nested": "val"}}, [], {}])",
    };

    struct EventRecord {
        int depth;
        int event;
        std::string dumped;
        bool operator==(const EventRecord& o) const {
            return depth == o.depth && event == o.event && dumped == o.dumped;
        }
    };

    for (const auto& doc : corpus) {
        SCOPED_TRACE("doc: " + doc);
        std::vector<EventRecord> j_events;
        std::vector<EventRecord> o_events;

        jk jr = jk::parse(doc, [&](int depth, jk::parse_event_t ev, jk& parsed) {
            j_events.push_back({depth, static_cast<int>(ev), parsed.dump()});
            return true;
        });

        oj or_ = oj::parse(doc, [&](int depth, oj::parse_event_t ev, oj& parsed) {
            o_events.push_back({depth, static_cast<int>(ev), parsed.dump()});
            return true;
        });

        EXPECT_EQ(j_events, o_events);
        assert_equivalent(or_, jr);

        // Test discarding at each single callback invocation index k
        const std::size_t total_events = o_events.size();
        for (std::size_t discard_idx = 0; discard_idx < total_events; ++discard_idx) {
            std::vector<EventRecord> j_sub;
            std::vector<EventRecord> o_sub;
            std::size_t j_call = 0;
            std::size_t o_call = 0;

            jk j_disc = jk::parse(doc, [&](int depth, jk::parse_event_t ev, jk& parsed) {
                j_sub.push_back({depth, static_cast<int>(ev), parsed.dump()});
                return (j_call++ != discard_idx);
            });

            oj o_disc = oj::parse(doc, [&](int depth, oj::parse_event_t ev, oj& parsed) {
                o_sub.push_back({depth, static_cast<int>(ev), parsed.dump()});
                return (o_call++ != discard_idx);
            });

            EXPECT_EQ(j_sub, o_sub) << "discard_idx=" << discard_idx;
            assert_equivalent(o_disc, j_disc);
        }

        // Also test discarding by depth (depth == 0, 1, 2)
        for (int target_depth = 0; target_depth <= 3; ++target_depth) {
            jk j_d = jk::parse(doc, [target_depth](int depth, jk::parse_event_t, jk&) {
                return depth != target_depth;
            });
            oj o_d = oj::parse(doc, [target_depth](int depth, oj::parse_event_t, oj&) {
                return depth != target_depth;
            });
            assert_equivalent(o_d, j_d);
        }
    }

    // Stream and iterator overloads with callback
    {
        const std::string src = R"({"keep": 1, "drop": 2, "arr": [10, 20]})";
        std::istringstream iss_j(src);
        std::istringstream iss_o(src);
        jk jr = jk::parse(iss_j, [&](int, jk::parse_event_t ev, jk& p) {
            return !(ev == jk::parse_event_t::key && p == jk("drop"));
        });
        oj or_ = oj::parse(iss_o, [&](int, oj::parse_event_t ev, oj& p) {
            return !(ev == oj::parse_event_t::key && p == oj("drop"));
        });
        assert_equivalent(or_, jr);

        jk jr_it = jk::parse(src.begin(), src.end(), [&](int, jk::parse_event_t ev, jk& p) {
            return !(ev == jk::parse_event_t::value && p == jk(10));
        });
        oj or_it = oj::parse(src.begin(), src.end(), [&](int, oj::parse_event_t ev, oj& p) {
            return !(ev == oj::parse_event_t::value && p == oj(10));
        });
        assert_equivalent(or_it, jr_it);
    }
}

// ==================== 5. Extended Random Operation Sequence & Disagreement Pin ====================

TEST(Slice2DifferentialSequence, PointerPatchAndMergeOps) {
    std::mt19937_64 rng(0x51C2'CAFE'1234ULL);

    for (int trial = 0; trial < 60; ++trial) {
        auto [j, o] = make_random_doc(rng);
        assert_equivalent(o, j);

        static const char* const sample_ptrs[] = {
            "", "/", "/a", "/b", "/0", "/1", "/-", "/01", "/a/b", "/a~1b", "/m~0n", "/nested/0"
        };

        for (int step = 0; step < 25; ++step) {
            int op = static_cast<int>(rng() % 7);
            const std::string ps = sample_ptrs[rng() % (sizeof(sample_ptrs) / sizeof(sample_ptrs[0]))];
            const jkp jp(ps);
            const ojp op_ptr(ps);

            switch (op) {
                case 0: { // const at(json_pointer)
                    const jk& jc = j;
                    const oj& oc = o;
                    expect_both([&]() { return oc.at(op_ptr); }, [&]() { return jc.at(jp); });
                    break;
                }
                case 1: { // mutable operator[](json_pointer)
                    auto [jv, ov] = make_random_doc(rng, 2);
                    expect_same_exception(
                        [&]() { o[op_ptr] = ov; },
                        [&]() { j[jp] = jv; });
                    assert_equivalent(o, j);
                    break;
                }
                case 2: { // contains(json_pointer)
                    expect_both(
                        [&]() { return o.contains(op_ptr); },
                        [&]() { return j.contains(jp); });
                    break;
                }
                case 3: { // value(json_pointer, default)
                    expect_both(
                        [&]() { return o.value(op_ptr, 42); },
                        [&]() { return j.value(jp, 42); });
                    break;
                }
                case 4: { // diff + patch
                    auto [jt, ot] = make_random_doc(rng, 1);
                    jk jd = jk::diff(j, jt);
                    oj od = oj::diff(o, ot);
                    assert_equivalent(od, jd);
                    j.patch_inplace(jd);
                    o.patch_inplace(od);
                    assert_equivalent(o, j);
                    break;
                }
                case 5: { // merge_patch
                    auto [jm, om] = make_random_doc(rng, 1);
                    j.merge_patch(jm);
                    o.merge_patch(om);
                    assert_equivalent(o, j);
                    break;
                }
                case 6: { // flatten + unflatten
                    expect_both(
                        [&]() { return o.flatten().unflatten(); },
                        [&]() { return j.flatten().unflatten(); });
                    break;
                }
            }
        }
    }
}

TEST(Slice2ParseDisagreementPin, GeneratedCorporaNeverDisagree) {
    std::mt19937_64 rng(0x51C2'9999'AAAAULL);
    std::vector<std::string> corpus = {
        "", " ", "null", "true", "false", "0", "-0", "01", "-01", "1.", ".1", "1e", "1e+",
        "\"\"", "\"\\uD800\"", "\"\\uD800\\uD800\"", "\"\\uDC00\"", "\"\\x\"",
        "{\"a\":1,}", "[1,]", "[1 2]", "{\"a\" 1}", "null trailing", "// comment\n1",
        "/* comment */ true", "/* unclosed", "/invalid",
        std::string("{\"a\":\"b\"}\0trailing", 18),
        std::string("{\"a\":\"\0\"}", 9),
    };

    // Add 300 random valid and mutated (often invalid) JSON strings
    for (int i = 0; i < 150; ++i) {
        auto [j, o] = make_random_doc(rng);
        std::string dumped = j.dump();
        corpus.push_back(dumped);
        if (!dumped.empty()) {
            std::string mutated = dumped;
            std::size_t pos = rng() % mutated.size();
            mutated[pos] = static_cast<char>(rng() % 128);
            corpus.push_back(mutated);
            corpus.push_back(dumped.substr(0, pos));
        }
    }

    for (bool ignore_comments : {false, true}) {
        for (const auto& s : corpus) {
            bool j_threw = false;
            int j_id = 0;
            std::string j_msg;
            jk j_val;

            try {
                j_val = jk::parse(s, nullptr, true, ignore_comments);
                EXPECT_FALSE(j_val.is_discarded());
            } catch (const jk::exception& e) {
                j_threw = true;
                j_id = e.id;
                j_msg = e.what();
                // Pin that internal disagreement error 599 is never reached
                EXPECT_NE(j_id, 599) << "Scanner/yyjson disagreement on input: " << s;
            }

            bool o_threw = false;
            int o_id = 0;
            std::string o_msg;
            oj o_val;

            try {
                o_val = oj::parse(s.begin(), s.end(), nullptr, true, ignore_comments);
            } catch (const oj::exception& e) {
                o_threw = true;
                o_id = e.id;
                o_msg = e.what();
            }

            EXPECT_EQ(j_threw, o_threw);
            if (j_threw && o_threw) {
                EXPECT_EQ(j_id, o_id);
                EXPECT_EQ(j_msg, o_msg);
            } else if (!j_threw && !o_threw) {
                assert_equivalent(o_val, j_val);
            }
        }
    }
}

} // namespace
