// Comparison of numbers stored as signed integer, unsigned integer or floating point. nlohmann::ordered_json is the
// specification: every operator, in both operand orders, gives for neojson what it gives for the oracle, for operands
// around zero, 2^63 and 2^64-1, and against floating-point values.

#include "differential.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using jk = neojson::json;
using oj = nlohmann::ordered_json;

struct Operand {
    std::string name;
    oj oracle;
    jk subject;
};

template <typename T>
Operand make(const std::string& name, T value) {
    return Operand{name, oj(value), jk(value)};
}

std::vector<Operand> operands() {
    constexpr auto i64min = std::numeric_limits<std::int64_t>::min();
    constexpr auto i64max = std::numeric_limits<std::int64_t>::max();
    constexpr auto u64max = std::numeric_limits<std::uint64_t>::max();
    constexpr double two63 = 9223372036854775808.0;
    constexpr double two64 = 18446744073709551616.0;
    std::vector<Operand> v;
    v.push_back(make<std::int64_t>("i:min", i64min));
    v.push_back(make<std::int64_t>("i:min+1", i64min + 1));
    v.push_back(make<std::int64_t>("i:-2", -2));
    v.push_back(make<std::int64_t>("i:-1", -1));
    v.push_back(make<std::int64_t>("i:0", 0));
    v.push_back(make<std::int64_t>("i:1", 1));
    v.push_back(make<std::int64_t>("i:max-1", i64max - 1));
    v.push_back(make<std::int64_t>("i:max", i64max));
    v.push_back(make<std::uint64_t>("u:0", 0));
    v.push_back(make<std::uint64_t>("u:1", 1));
    v.push_back(make<std::uint64_t>("u:i64max-1", static_cast<std::uint64_t>(i64max) - 1));
    v.push_back(make<std::uint64_t>("u:i64max", static_cast<std::uint64_t>(i64max)));
    v.push_back(make<std::uint64_t>("u:2^63", static_cast<std::uint64_t>(i64max) + 1));
    v.push_back(make<std::uint64_t>("u:2^63+1", static_cast<std::uint64_t>(i64max) + 2));
    v.push_back(make<std::uint64_t>("u:max-1", u64max - 1));
    v.push_back(make<std::uint64_t>("u:max", u64max));
    v.push_back(make<double>("f:-inf", -std::numeric_limits<double>::infinity()));
    v.push_back(make<double>("f:-2^64", -two64));
    v.push_back(make<double>("f:-2^63", -two63));
    v.push_back(make<double>("f:-1.5", -1.5));
    v.push_back(make<double>("f:-1", -1.0));
    v.push_back(make<double>("f:-0", -0.0));
    v.push_back(make<double>("f:0", 0.0));
    v.push_back(make<double>("f:0.5", 0.5));
    v.push_back(make<double>("f:1", 1.0));
    v.push_back(make<double>("f:2^53", 9007199254740992.0));
    v.push_back(make<double>("f:2^63", two63));
    v.push_back(make<double>("f:2^64", two64));
    v.push_back(make<double>("f:inf", std::numeric_limits<double>::infinity()));
    v.push_back(make<double>("f:nan", std::numeric_limits<double>::quiet_NaN()));
    return v;
}

TEST(CompareNumbers, EveryOperatorOnEveryPairOfNumberTypes) {
    const std::vector<Operand> all = operands();
    std::size_t checked = 0;
    for (const Operand& a : all) {
        for (const Operand& b : all) {
            SCOPED_TRACE(a.name + " against " + b.name);
            EXPECT_EQ(a.oracle == b.oracle, a.subject == b.subject);
            EXPECT_EQ(a.oracle != b.oracle, a.subject != b.subject);
            EXPECT_EQ(a.oracle < b.oracle, a.subject < b.subject);
            EXPECT_EQ(a.oracle <= b.oracle, a.subject <= b.subject);
            EXPECT_EQ(a.oracle > b.oracle, a.subject > b.subject);
            EXPECT_EQ(a.oracle >= b.oracle, a.subject >= b.subject);
            ++checked;
        }
    }
    EXPECT_EQ(checked, all.size() * all.size());
}

TEST(CompareNumbers, TheLargestUnsignedAndMinusOne) {
    // nlohmann/json 3.11.3 casts one integer type to the other before it compares, so 2^64-1 and -1 are equal and
    // neither is less than the other. neojson gives what the oracle gives.
    const oj o_max(std::numeric_limits<std::uint64_t>::max());
    const oj o_minus_one(std::int64_t{-1});
    const jk s_max(std::numeric_limits<std::uint64_t>::max());
    const jk s_minus_one(std::int64_t{-1});
    EXPECT_EQ(o_max == o_minus_one, s_max == s_minus_one);
    EXPECT_EQ(o_minus_one == o_max, s_minus_one == s_max);
    EXPECT_EQ(o_max < o_minus_one, s_max < s_minus_one);
    EXPECT_EQ(o_minus_one < o_max, s_minus_one < s_max);
}

TEST(CompareNumbers, AgainstPlainCppNumbers) {
    const std::vector<Operand> all = operands();
    const std::int64_t signed_values[] = {std::numeric_limits<std::int64_t>::min(), -1, 0, 1,
                                          std::numeric_limits<std::int64_t>::max()};
    const std::uint64_t unsigned_values[] = {0, 1, std::uint64_t{1} << 63,
                                             std::numeric_limits<std::uint64_t>::max()};
    const double float_values[] = {-1.0, 0.0, 1.0, 9223372036854775808.0};
    for (const Operand& a : all) {
        SCOPED_TRACE(a.name);
        for (std::int64_t c : signed_values) {
            EXPECT_EQ(a.oracle == c, a.subject == c);
            EXPECT_EQ(c == a.oracle, c == a.subject);
            EXPECT_EQ(a.oracle < c, a.subject < c);
            EXPECT_EQ(c < a.oracle, c < a.subject);
            EXPECT_EQ(a.oracle >= c, a.subject >= c);
            EXPECT_EQ(c > a.oracle, c > a.subject);
        }
        for (std::uint64_t c : unsigned_values) {
            EXPECT_EQ(a.oracle == c, a.subject == c);
            EXPECT_EQ(c == a.oracle, c == a.subject);
            EXPECT_EQ(a.oracle < c, a.subject < c);
            EXPECT_EQ(c < a.oracle, c < a.subject);
            EXPECT_EQ(a.oracle >= c, a.subject >= c);
            EXPECT_EQ(c > a.oracle, c > a.subject);
        }
        for (double c : float_values) {
            EXPECT_EQ(a.oracle == c, a.subject == c);
            EXPECT_EQ(c == a.oracle, c == a.subject);
            EXPECT_EQ(a.oracle < c, a.subject < c);
            EXPECT_EQ(c < a.oracle, c < a.subject);
            EXPECT_EQ(a.oracle >= c, a.subject >= c);
            EXPECT_EQ(c > a.oracle, c > a.subject);
        }
    }
}

TEST(CompareNumbers, NumbersInsideArraysAndObjects) {
    // The containers compare their elements with the same rule.
    const std::vector<Operand> all = operands();
    for (const Operand& a : all) {
        for (const Operand& b : all) {
            SCOPED_TRACE(a.name + " against " + b.name);
            const oj oa = oj::array({a.oracle, 1});
            const oj ob = oj::array({b.oracle, 1});
            const jk sa = jk::array({a.subject, 1});
            const jk sb = jk::array({b.subject, 1});
            EXPECT_EQ(oa == ob, sa == sb);
            EXPECT_EQ(oa < ob, sa < sb);
            EXPECT_EQ(oa >= ob, sa >= sb);
            oj oo = oj::object();
            oo["k"] = a.oracle;
            oj op = oj::object();
            op["k"] = b.oracle;
            jk so = jk::object();
            so["k"] = a.subject;
            jk sp = jk::object();
            sp["k"] = b.subject;
            EXPECT_EQ(oo == op, so == sp);
            EXPECT_EQ(oo < op, so < sp);
        }
    }
}

} // namespace
