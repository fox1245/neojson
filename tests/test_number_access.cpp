// How a number is reported and read back: is_number_integer(), is_number_unsigned(), get_ptr<> to the three number
// types and get<> to integer and floating-point types, on signed, unsigned and floating-point values.
// nlohmann::ordered_json is the specification. In nlohmann/json 3.11.3 is_number_integer() is true for an unsigned
// value, get_ptr<std::int64_t*>() of an unsigned value is a pointer to its stored bits, and get_ptr<std::uint64_t*>()
// of a signed value is null.

#include "differential.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
    std::vector<Operand> v;
    v.push_back(make<std::int64_t>("i:min", i64min));
    v.push_back(make<std::int64_t>("i:-1", -1));
    v.push_back(make<std::int64_t>("i:0", 0));
    v.push_back(make<std::int64_t>("i:1", 1));
    v.push_back(make<std::int64_t>("i:300", 300));
    v.push_back(make<std::int64_t>("i:max", i64max));
    v.push_back(make<std::uint64_t>("u:0", 0));
    v.push_back(make<std::uint64_t>("u:1", 1));
    v.push_back(make<std::uint64_t>("u:300", 300));
    v.push_back(make<std::uint64_t>("u:i64max", static_cast<std::uint64_t>(i64max)));
    v.push_back(make<std::uint64_t>("u:2^63", static_cast<std::uint64_t>(i64max) + 1));
    v.push_back(make<std::uint64_t>("u:max", u64max));
    v.push_back(make<double>("f:-1.5", -1.5));
    v.push_back(make<double>("f:0", 0.0));
    v.push_back(make<double>("f:2.5", 2.5));
    v.push_back(make<double>("f:1e300", 1e300));
    v.push_back(make<double>("f:2^63", 9223372036854775808.0));
    v.push_back(make<double>("f:nan", std::numeric_limits<double>::quiet_NaN()));
    // Values that are not numbers: the accessors must refuse them in the same way.
    v.push_back(Operand{"null", oj(nullptr), jk(nullptr)});
    v.push_back(make<bool>("true", true));
    v.push_back(make<std::string>("string", "12"));
    v.push_back(Operand{"array", oj::array({1}), jk::array({1})});
    v.push_back(Operand{"object", oj::object(), jk::object()});
    return v;
}

template <typename P, typename Oracle, typename Subject>
void expect_same_pointer(Oracle& oracle, Subject& subject) {
    auto* po = oracle.template get_ptr<P>();
    auto* ps = subject.template get_ptr<P>();
    ASSERT_EQ(po == nullptr, ps == nullptr);
    if (po != nullptr) {
        // A pointer to a number reads the stored value: compare bit patterns, which also tells NaN apart.
        using V = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<decltype(po)>>>;
        V vo = *po;
        V vs = *ps;
        EXPECT_EQ(0, std::memcmp(&vo, &vs, sizeof(V)));
    }
}

// Whether converting the floating-point value `d` to the integer or floating-point type T is defined: the C++
// standard leaves a conversion of a value outside the range of T undefined, and so does the oracle, which converts
// with a plain cast. (Out-of-range conversions between integer types are defined and are tested.)
template <typename T>
bool conversion_is_defined(double d) {
    if constexpr (std::is_floating_point<T>::value) {
        return !std::isfinite(d) || std::fabs(d) <= static_cast<double>(std::numeric_limits<T>::max());
    }
    return std::isfinite(d) && d > static_cast<double>(std::numeric_limits<T>::min()) - 1.0 &&
           d < static_cast<double>(std::numeric_limits<T>::max()) + 1.0;
}

template <typename T>
void expect_same_get(const oj& oracle, const jk& subject) {
    if (oracle.is_number_float() && !conversion_is_defined<T>(oracle.get<double>())) {
        return;
    }
    neojson::test::expect_same_exception([&] { return oracle.get<T>(); }, [&] { return subject.get<T>(); });
    T vo{};
    T vs{};
    bool ok_o = true;
    bool ok_s = true;
    try { vo = oracle.get<T>(); } catch (...) { ok_o = false; }
    try { vs = subject.get<T>(); } catch (...) { ok_s = false; }
    ASSERT_EQ(ok_o, ok_s);
    if (ok_o) {
        EXPECT_EQ(0, std::memcmp(&vo, &vs, sizeof(T)));
    }
}

TEST(NumberAccess, TypeQueries) {
    for (const Operand& a : operands()) {
        SCOPED_TRACE(a.name);
        EXPECT_EQ(a.oracle.is_number(), a.subject.is_number());
        EXPECT_EQ(a.oracle.is_number_integer(), a.subject.is_number_integer());
        EXPECT_EQ(a.oracle.is_number_unsigned(), a.subject.is_number_unsigned());
        EXPECT_EQ(a.oracle.is_number_float(), a.subject.is_number_float());
    }
}

TEST(NumberAccess, PointersToTheThreeNumberTypes) {
    for (const Operand& a : operands()) {
        SCOPED_TRACE(a.name);
        {
            oj oc = a.oracle;
            jk sc = a.subject;
            expect_same_pointer<std::int64_t*>(oc, sc);
            expect_same_pointer<std::uint64_t*>(oc, sc);
            expect_same_pointer<double*>(oc, sc);
        }
        {
            const oj& oc = a.oracle;
            const jk& sc = a.subject;
            expect_same_pointer<const std::int64_t*>(oc, sc);
            expect_same_pointer<const std::uint64_t*>(oc, sc);
            expect_same_pointer<const double*>(oc, sc);
        }
    }
}

TEST(NumberAccess, GetToIntegerAndFloatingPointTypes) {
    for (const Operand& a : operands()) {
        SCOPED_TRACE(a.name);
        expect_same_get<std::int64_t>(a.oracle, a.subject);
        expect_same_get<std::uint64_t>(a.oracle, a.subject);
        expect_same_get<std::int32_t>(a.oracle, a.subject);
        expect_same_get<std::uint32_t>(a.oracle, a.subject);
        expect_same_get<std::int8_t>(a.oracle, a.subject);
        expect_same_get<std::uint8_t>(a.oracle, a.subject);
        expect_same_get<double>(a.oracle, a.subject);
        expect_same_get<float>(a.oracle, a.subject);
    }
}

TEST(NumberAccess, GetRefOfNumberTypes) {
    for (const Operand& a : operands()) {
        SCOPED_TRACE(a.name);
        neojson::test::expect_same_exception([&] { return a.oracle.get_ref<const std::int64_t&>(); },
                                             [&] { return a.subject.get_ref<const std::int64_t&>(); });
        neojson::test::expect_same_exception([&] { return a.oracle.get_ref<const std::uint64_t&>(); },
                                             [&] { return a.subject.get_ref<const std::uint64_t&>(); });
        neojson::test::expect_same_exception([&] { return a.oracle.get_ref<const double&>(); },
                                             [&] { return a.subject.get_ref<const double&>(); });
    }
}

} // namespace
