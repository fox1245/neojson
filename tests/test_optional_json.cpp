// A std::optional that holds a json value, and a json value converted to an optional. The same statements are
// compiled for nlohmann::ordered_json (the specification) and for neojson::json, and both must compile and give
// the same values. Every statement here is one that is ambiguous when the implicit conversion of json to a value
// type also applies to std::optional.

#include "differential.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

// Runs the statements for one library and returns what they produced, as text.
template <typename J>
std::string exercise() {
    std::string out;
    auto note = [&out](const std::optional<J>& o) {
        out += o.has_value() ? o->dump() : std::string("-");
        out += ';';
    };

    J lvalue = J::parse(R"({"a":1,"b":"two","c":[3]})");
    const J clvalue = lvalue;

    std::optional<J> x;
    x = lvalue;
    note(x);
    x = std::nullopt;
    note(x);
    x = clvalue;
    note(x);
    x = J::parse("[1]");
    note(x);
    x = std::move(lvalue);
    note(x);
    x = 5;
    note(x);
    x = "str";
    note(x);
    x = nullptr;
    note(x);
    x = std::string("s");
    note(x);
    x = true;
    note(x);
    x = 1.5;
    note(x);
    x = J::object();
    note(x);
    x = J::array({1, 2});
    note(x);
    x = std::optional<J>(clvalue);
    note(x);
    std::optional<J> from_optional;
    from_optional = x;
    note(from_optional);
    x.emplace(clvalue);
    note(x);

    std::optional<J> a = clvalue;
    note(a);
    std::optional<J> b(clvalue);
    note(b);
    std::optional<J> c{clvalue};
    note(c);
    std::optional<J> d = J::parse("1");
    note(d);
    std::optional<J> e(std::in_place, clvalue);
    note(e);
    std::optional<J> f = a;
    note(f);
    std::optional<J> g(5);
    note(g);
    note(std::make_optional(clvalue));
    note(std::make_optional<J>(clvalue));

    out += (a == clvalue) ? "eq;" : "ne;";
    out += (a == std::optional<J>(clvalue)) ? "eq;" : "ne;";
    out += (a == std::nullopt) ? "eq;" : "ne;";
    out += a.value_or(J(0)).dump() + ";";

    // A json value converted to an optional of a plain type.
    std::optional<int> number;
    number = clvalue["a"];
    out += std::to_string(number.value()) + ";";
    std::optional<int> number2 = clvalue["a"];
    out += std::to_string(number2.value()) + ";";
    std::optional<std::string> text(clvalue["b"]);
    out += text.value() + ";";
    return out;
}

TEST(OptionalJson, AssignmentsAndConversionsGiveWhatTheOracleGives) {
    EXPECT_EQ(exercise<nlohmann::ordered_json>(), exercise<neojson::json>());
}

TEST(OptionalJson, TheOptionalHoldsTheValueThatWasAssigned) {
    std::optional<neojson::json> x;
    const neojson::json j = neojson::json::parse(R"({"k":[1,2,3]})");
    x = j;
    ASSERT_TRUE(x.has_value());
    EXPECT_EQ(*x, j);
    x = std::nullopt;
    EXPECT_FALSE(x.has_value());
}

} // namespace
