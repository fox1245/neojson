// Loops over the members of an object and the elements of an array through the iterators of json: in every iteration,
// key(), value(), *it and it-> refer to the same member. The loops are written as user code writes them, with end()
// evaluated in each test of the loop condition, and the library functions built on such loops get inputs whose members
// all hold different values, so that reading a neighbouring member shows.

#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "neojson/json.hpp"

namespace {

using neojson::json;

std::string members_by_key_and_value(const json& j) {
    std::string s;
    for (auto it = j.begin(); it != j.end(); ++it) {
        s += it.key();
        s += '=';
        s += it.value().dump();
        s += ';';
    }
    return s;
}

std::string members_by_dereference(const json& j) {
    std::string s;
    for (auto it = j.begin(); it != j.end(); ++it) {
        s += it.key();
        s += '=';
        s += (*it).dump();
        s += ',';
        s += it->dump();
        s += ';';
    }
    return s;
}

std::string members_through_mutable_iterator(json& j) {
    std::string s;
    for (auto it = j.begin(); it != j.end(); ++it) {
        s += it.key();
        s += '=';
        s += it.value().dump();
        s += ';';
    }
    return s;
}

std::string members_through_const_iterator(const json& j) {
    std::string s;
    for (json::const_iterator it = j.cbegin(); it != j.cend(); ++it) {
        s += it.key();
        s += '=';
        s += it.value().dump();
        s += ';';
    }
    return s;
}

std::string elements_with_index(const json& j) {
    std::string s;
    std::size_t i = 0;
    for (auto it = j.begin(); it != j.end(); ++it, ++i) {
        s += std::to_string(i);
        s += '=';
        s += it.value().dump();
        s += ';';
    }
    return s;
}

std::string members_by_items(const json& j) {
    std::string s;
    for (const auto& member : j.items()) {
        s += member.key();
        s += '=';
        s += member.value().dump();
        s += ';';
    }
    return s;
}

} // namespace

TEST(Iteration, ObjectLoopReadsKeyAndValueOfTheSameMember) {
    const json j = json::parse(R"({"a":1,"b":"two","c":[3],"d":{"e":4},"f":null,"g":true})");
    const std::string want = R"(a=1;b="two";c=[3];d={"e":4};f=null;g=true;)";
    EXPECT_EQ(members_by_key_and_value(j), want);

    json m = j;
    EXPECT_EQ(members_through_mutable_iterator(m), want);
    EXPECT_EQ(members_through_const_iterator(j), want);
    EXPECT_EQ(members_by_items(j), want);
}

TEST(Iteration, DereferenceAndArrowReadTheMemberOfTheKey) {
    const json j = json::parse(R"({"a":1,"b":2,"c":3})");
    EXPECT_EQ(members_by_dereference(j), "a=1,1;b=2,2;c=3,3;");
}

TEST(Iteration, ArrayLoopReadsTheElementOfTheIndex) {
    const json j = json::parse(R"([10,"x",null,[1],{"k":2}])");
    EXPECT_EQ(elements_with_index(j), R"(0=10;1="x";2=null;3=[1];4={"k":2};)");
}

TEST(Iteration, MergePatchAppliesEachMemberOfThePatchToItsOwnKey) {
    json target = json::parse(R"({"title":"Goodbye!","author":{"givenName":"John","familyName":"Doe"},)"
                              R"("tags":["example","sample"],"content":"This will be unchanged"})");
    target.merge_patch(json::parse(
        R"({"title":"Hello!","phoneNumber":"+01-123-456-7890","author":{"familyName":null},"tags":["example"]})"));
    EXPECT_EQ(target.dump(), R"({"title":"Hello!","author":{"givenName":"John"},"tags":["example"],)"
                             R"("content":"This will be unchanged","phoneNumber":"+01-123-456-7890"})");

    json empty = json::object();
    empty.merge_patch(json::parse(R"({"a":1,"b":null,"c":3,"d":4})"));
    EXPECT_EQ(empty.dump(), R"({"a":1,"c":3,"d":4})");
}

TEST(Iteration, UnflattenPutsEachValueAtItsOwnPointer) {
    const json flat =
        json::parse(R"({"/a/b/0":1,"/a/b/1":2,"/a/b/2/c":true,"/a/b/2/d":null,"/e":"five","/f":6})");
    EXPECT_EQ(flat.unflatten().dump(), R"({"a":{"b":[1,2,{"c":true,"d":null}]},"e":"five","f":6})");

    const json doc = json::parse(R"({"x":[1,2,3],"y":{"z":"w","v":false}})");
    EXPECT_EQ(doc.flatten().unflatten(), doc);
}

TEST(Iteration, UpdateAndDiffReadEachMemberOfTheSource) {
    json target = json::parse(R"({"a":0})");
    target.update(json::parse(R"({"b":1,"c":2,"d":3})"));
    EXPECT_EQ(target.dump(), R"({"a":0,"b":1,"c":2,"d":3})");

    const json source = json::parse(R"({"a":1,"b":2,"c":3})");
    const json goal = json::parse(R"({"a":1,"b":20,"c":30,"d":40})");
    EXPECT_EQ(source.patch(json::diff(source, goal)), goal);
}
