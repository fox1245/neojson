#include <gtest/gtest.h>
#include <neojson/json.hpp>

#include <cstddef>
#include <string>
#include <utility>

namespace {

using neojson::json;

TEST(Ilp32LayoutTest, Arena11To15ByteKeysAndValuesRoundtripAndDestroyCleanly) {
    // 11..15 byte strings exceed 32-bit libc++ SSO inline capacity (10 bytes)
    // while staying inside libstdc++ / 64-bit libc++ inline buffers.
    const std::string raw = R"({
        "key_11_byte": "val_11_byte",
        "key_12_bytes": "val_12_bytes",
        "key_13_bytess": "val_13_bytess",
        "key_14_bytesss": "val_14_bytesss",
        "key_15_bytes___": "val_15_bytes___",
        "arr": [
            "item_11_byt",
            "item_12_byte",
            "item_13_bytes",
            "item_14_bytess",
            "item_15_bytesss"
        ],
        "nested": {
            "sub_key_12__1": "sub_val_12__1",
            "sub_key_15____2": "sub_val_15____2"
        }
    })";

    const json parsed = json::parse(raw);
    ASSERT_TRUE(parsed.is_object());
    EXPECT_EQ(parsed.at("key_11_byte").get<std::string>(), "val_11_byte");
    EXPECT_EQ(parsed.at("key_15_bytes___").get<std::string>(), "val_15_bytes___");
    EXPECT_EQ(parsed.at("arr").size(), 5u);
    EXPECT_EQ(parsed.at("arr").at(0).get<std::string>(), "item_11_byt");
    EXPECT_EQ(parsed.at("arr").at(4).get<std::string>(), "item_15_bytesss");
    EXPECT_EQ(parsed.at("nested").at("sub_key_15____2").get<std::string>(), "sub_val_15____2");

    json copy = parsed;
    copy["added_13_bytes"] = "added_val_14__";
    copy["nested"]["sub_key_12__1"] = "mutated_13_by";
    EXPECT_EQ(parsed.at("nested").at("sub_key_12__1").get<std::string>(), "sub_val_12__1");

    const json reparsed = json::parse(copy.dump());
    EXPECT_EQ(reparsed, copy);
}

TEST(Ilp32LayoutTest, SmallContainersAndLargeHashShapeArenaPaths) {
    // SmallArray2 (0..2 elements) and SmallObject4 (0..4 keys)
    for (std::size_t n = 0; n <= 2u; ++n) {
        std::string s = "[";
        for (std::size_t i = 0; i < n; ++i) {
            if (i > 0) {
                s += ',';
            }
            s += "\"elem_12_by_" + std::to_string(i) + "\"";
        }
        s += "]";
        json a = json::parse(s);
        json b = a;
        b.push_back("extra_12_byt");
        EXPECT_EQ(a.size(), n);
        EXPECT_EQ(b.size(), n + 1u);
    }

    for (std::size_t n = 0; n <= 4u; ++n) {
        std::string s = "{";
        for (std::size_t i = 0; i < n; ++i) {
            if (i > 0) {
                s += ',';
            }
            s += "\"small_key_1" + std::to_string(i) + "\":\"small_val_1" + std::to_string(i) + "\"";
        }
        s += "}";
        json o = json::parse(s);
        json c = o;
        c["extra_13_keys"] = 42;
        EXPECT_EQ(o.size(), n);
        EXPECT_EQ(c.size(), n + 1u);
    }

    // Large object (> 16 keys) exercising ArenaHashIndex, KeyInternSlot, DupProbeEntry, ShapeEntry
    std::string large_obj = "{";
    for (int i = 0; i < 48; ++i) {
        if (i > 0) {
            large_obj += ',';
        }
        std::string k = "intern_key_" + std::to_string(10 + i);  // 13 bytes each
        std::string v = "payload_val_" + std::to_string(10 + i);
        large_obj += "\"" + k + "\":\"" + v + "\"";
    }
    large_obj += "}";

    const json parsed_first = json::parse(large_obj);
    const json parsed_second = json::parse(large_obj);
    EXPECT_EQ(parsed_first.size(), 48u);
    EXPECT_EQ(parsed_first, parsed_second);
    EXPECT_EQ(parsed_first.at("intern_key_35").get<std::string>(), "payload_val_35");
}

TEST(Ilp32LayoutTest, LargeBuffersExceedFourKiB) {
    json root = json::array();
    for (int i = 0; i < 256; ++i) {
        json entry = json::object();
        entry["idx_11_bytes"] = i;
        entry["label_13_byt"] = "record_value_" + std::to_string(1000 + i);
        root.push_back(std::move(entry));
    }
    const std::string dumped = root.dump();
    ASSERT_GT(dumped.size(), 8192u);
    const json reparsed = json::parse(dumped);
    EXPECT_EQ(reparsed, root);
    EXPECT_EQ(reparsed.at(255).at("idx_11_bytes").get<int>(), 255);
}

}  // namespace
