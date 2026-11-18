// Equality and ordering of objects whose members are the same but stored in a different order, and of objects that
// differ in size, keys or values. nlohmann::ordered_json (not nlohmann::json) is the specification: it keeps the
// members in the order of insertion and compares them in that order, so the same members in another order are not
// equal.

#include "differential.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace {

using jk = neojson::json;
using oj = nlohmann::ordered_json;

void expect_same_relations(const std::string& a, const std::string& b) {
    SCOPED_TRACE(a.substr(0, 60) + " against " + b.substr(0, 60));
    const oj oa = oj::parse(a);
    const oj ob = oj::parse(b);
    const jk sa = jk::parse(a);
    const jk sb = jk::parse(b);
    EXPECT_EQ(oa == ob, sa == sb);
    EXPECT_EQ(oa != ob, sa != sb);
    EXPECT_EQ(oa < ob, sa < sb);
    EXPECT_EQ(oa <= ob, sa <= sb);
    EXPECT_EQ(oa > ob, sa > sb);
    EXPECT_EQ(oa >= ob, sa >= sb);
}

const std::vector<std::string>& documents() {
    static const std::vector<std::string> docs = {
        R"({})",
        R"({"a":1})",
        R"({"a":2})",
        R"({"b":1})",
        R"({"a":1,"b":2})",
        R"({"b":2,"a":1})",
        R"({"a":1,"b":3})",
        R"({"a":1,"b":2,"c":3})",
        R"({"c":3,"b":2,"a":1})",
        R"({"b":2,"c":3,"a":1})",
        R"({"a":1,"c":3})",
        R"({"x":{"a":1,"b":2},"y":[1,2]})",
        R"({"y":[1,2],"x":{"a":1,"b":2}})",
        R"({"x":{"b":2,"a":1},"y":[1,2]})",
        R"({"x":{"a":1,"b":2},"y":[2,1]})",
        R"([{"a":1,"b":2}])",
        R"([{"b":2,"a":1}])",
        R"(null)",
        R"(1)",
        R"("a")",
    };
    return docs;
}

TEST(ObjectEquality, EveryRelationOnEveryPairOfDocuments) {
    for (const std::string& a : documents()) {
        for (const std::string& b : documents()) {
            expect_same_relations(a, b);
        }
    }
}

TEST(ObjectEquality, SameMembersInAnotherOrderAreReportedLikeTheOracle) {
    // Spelled out, so that a reader sees what the specification says.
    const oj oa = oj::parse(R"({"a":1,"b":2})");
    const oj ob = oj::parse(R"({"b":2,"a":1})");
    const jk sa = jk::parse(R"({"a":1,"b":2})");
    const jk sb = jk::parse(R"({"b":2,"a":1})");
    EXPECT_EQ(oa == ob, sa == sb);
    EXPECT_EQ(oa != ob, sa != sb);
}

TEST(ObjectEquality, ObjectsBuiltByInsertionAndByParsing) {
    oj oa = oj::object();
    oa["z"] = 1;
    oa["y"] = 2;
    jk sa = jk::object();
    sa["z"] = 1;
    sa["y"] = 2;
    const oj ob = oj::parse(R"({"y":2,"z":1})");
    const jk sb = jk::parse(R"({"y":2,"z":1})");
    EXPECT_EQ(oa == ob, sa == sb);
    EXPECT_EQ(ob == oa, sb == sa);
    EXPECT_EQ(oa == oj::parse(R"({"z":1,"y":2})"), sa == jk::parse(R"({"z":1,"y":2})"));
}

TEST(ObjectEquality, LargeObjectsInAnotherOrder) {
    // Above the member count at which parsing sorts the keys.
    const std::size_t n = 600;
    std::string forward = "{";
    std::string backward = "{";
    for (std::size_t i = 0; i < n; ++i) {
        if (i != 0) {
            forward += ',';
            backward += ',';
        }
        forward += "\"k" + std::to_string(i) + "\":" + std::to_string(i);
        backward += "\"k" + std::to_string(n - 1 - i) + "\":" + std::to_string(n - 1 - i);
    }
    forward += '}';
    backward += '}';
    expect_same_relations(forward, backward);
    expect_same_relations(forward, forward);
}

} // namespace
