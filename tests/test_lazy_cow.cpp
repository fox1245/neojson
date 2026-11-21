#include <gtest/gtest.h>

#include "differential.hpp"
#include "small_stack.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using jk = neojson::json;
using oj = nlohmann::ordered_json;

static_assert(sizeof(jk) == 16, "neojson::json handle must remain 16 bytes");

std::string build_dup_object_text(const std::size_t n) {
    std::string s = "{";
    for (std::size_t i = 0; i < n; ++i) {
        if (i > 0) {
            s.push_back(',');
        }
        s += "\"k" + std::to_string(i) + "\":" + std::to_string(i);
    }
    // Overwrite first and middle keys so unique count < raw member count.
    if (n >= 2) {
        s += ",\"k0\":9001";
        s += ",\"k" + std::to_string(n / 2) + "\":[1,2,{\"dup\":true}]";
    }
    s.push_back('}');
    return s;
}

TEST(LazyCow, LayoutAndSmallScalarArrays) {
    static constexpr std::array<const char*, 15> kInputs = {{
        "{}",
        "[]",
        "null",
        "true",
        "-42",
        "18446744073709551615",
        "-0.0",
        "\"hello\\nworld\"",
        "[1]",
        "[1, 2, 3, 4]",
        "[1, \"two\", true, null]",
        R"({"pts":[[1.5,-2.25],[3.5,4.75]],"big_arr":[1,2,3,4,5],"nested":[[1],2,3,4],"ok":false})",
        R"({"wrap":[{"areaId":123,"blockIds":[]},{"a":1,"b":2,"c":3,"d":true}],"ok":1})",
        R"({"wrap":{"a":1,"b":{},"a":2,"c":null},"other":{"flag":false,"cnt":9}})",
        R"({"dup":{"a":1,"b":false,"a":2,"c":[]},"plain":{"x":1,"y":2,"z":{}}})",
    }};

    for (const char* raw : kInputs) {
        const oj oracle = oj::parse(raw);
        const jk doc = jk::parse(raw);
        const jk cpy(doc);
        const jk second = jk::parse(raw);

        neojson::test::assert_equivalent(oracle, doc);
        neojson::test::assert_equivalent(oracle, cpy);
        EXPECT_EQ(doc, cpy);
        EXPECT_EQ(doc, second);
        EXPECT_FALSE(doc < cpy);
        EXPECT_FALSE(doc < second);
        EXPECT_EQ(doc.dump(2, '\t'), oracle.dump(2, '\t'));
        EXPECT_EQ(doc.dump(-1, ' ', true), oracle.dump(-1, ' ', true));
    }
}

TEST(LazyCow, DuplicateKeysAcrossThresholds) {
    static constexpr std::array<std::size_t, 9> kSizes = {{2, 3, 8, 9, 15, 16, 17, 32, 100}};

    for (const std::size_t n : kSizes) {
        const std::string raw = build_dup_object_text(n);
        const std::string wrapped = "{\"head\":1,\"wrap\":" + raw + ",\"tail\":[true,null]}";
        const std::string swapped = R"({"head":1,"other":2})";

        for (const std::string& input : {raw, wrapped}) {
            const oj oracle = oj::parse(input);
            const jk lazy_a = jk::parse(input);
            const jk lazy_b = jk::parse(input);
            const jk other = jk::parse(swapped);

            // Exercise pre-materialization operations on unmaterialized Lazy handles.
            EXPECT_EQ(lazy_a.empty(), oracle.empty());
            EXPECT_EQ(lazy_a.size(), oracle.size());
            EXPECT_EQ(lazy_a.contains("k0"), oracle.contains("k0"));
            EXPECT_EQ(lazy_a.contains("head"), oracle.contains("head"));
            EXPECT_EQ(lazy_a.contains("nonexistent_key"), oracle.contains("nonexistent_key"));
            EXPECT_EQ(lazy_a.count("k0"), oracle.count("k0"));
            EXPECT_EQ(lazy_a.dump(), oracle.dump());
            EXPECT_EQ(lazy_a.dump(2), oracle.dump(2));
            EXPECT_EQ(lazy_a == lazy_b, oracle == oracle);
            EXPECT_EQ(lazy_a < other, oracle < oj::parse(swapped));
            EXPECT_EQ(other < lazy_a, oj::parse(swapped) < oracle);

            neojson::test::assert_equivalent(oracle, lazy_a);

            jk detached = lazy_a;
            detached.materialize();
            neojson::test::assert_equivalent(oracle, detached);
            EXPECT_EQ(detached, lazy_a);
        }
    }
}

TEST(LazyCow, CustomIndentCharDeepDump) {
    // 300 nesting levels * 2 indent chars = 600 chars > 512 inline indent buffer.
    constexpr std::size_t kDepth = 300;
    std::string raw;
    for (std::size_t i = 0; i < kDepth; ++i) {
        raw += (i % 2 == 0) ? "{\"a\":" : "[";
    }
    raw += "[1,\"txt\"]";
    for (std::size_t i = kDepth; i > 0; --i) {
        raw += ((i - 1) % 2 == 0) ? "}" : "]";
    }

    const oj oracle = oj::parse(raw);
    jk lazy_doc = jk::parse(raw);

    const std::string oracle_tab = oracle.dump(2, '\t');
    const std::string lazy_tab = lazy_doc.dump(2, '\t');
    EXPECT_EQ(lazy_tab, oracle_tab);

    EXPECT_EQ(lazy_doc.dump(3, '.'), oracle.dump(3, '.'));

    lazy_doc.materialize();
    EXPECT_EQ(lazy_doc.dump(2, '\t'), oracle_tab);
    EXPECT_EQ(lazy_doc.dump(3, '.'), oracle.dump(3, '.'));
}

TEST(LazyCow, CowCopyIsolationAndZeroCopySteal) {
    const std::string raw = R"({
        "arr": [1, 2, 3, 4, 5],
        "obj": {"a": 1, "b": "two", "sub": [10, 20, 30, 40, 50]},
        "tags": ["alpha", "beta", "gamma", "delta", "epsilon"]
    })";
    const oj oracle_orig = oj::parse(raw);
    const std::string expected_dump = oracle_orig.dump();

    jk a = jk::parse(raw);
    jk b = a;
    const jk c = a;
    EXPECT_EQ(a, b);
    EXPECT_FALSE(a < b);
    EXPECT_EQ(a.dump(), expected_dump);

    // 1) Mutate via operator[] / at() / update() on detached COW copy.
    EXPECT_THROW((void)b.at("nonexistent_cow_key"), jk::out_of_range);
    {
        jk m = b;
        oj om = oracle_orig;
        m["arr"][0] = 999;
        om["arr"][0] = 999;
        m["new_k"] = "added";
        om["new_k"] = "added";
        m.at("obj").at("a") = 77;
        om.at("obj").at("a") = 77;
        m["obj"]["sub"].push_back(60);
        om["obj"]["sub"].push_back(60);
        m.emplace("emplaced", jk::array({1, 2, 3}));
        om.emplace("emplaced", oj::array({1, 2, 3}));
        m["tags"].emplace_back("zeta");
        om["tags"].emplace_back("zeta");
        m["tags"].insert(m["tags"].begin() + 1, "inserted");
        om["tags"].insert(om["tags"].begin() + 1, "inserted");
        m.update(jk::parse(R"({"obj":{"extra":true}})"), true);
        om.update(oj::parse(R"({"obj":{"extra":true}})"), true);
        neojson::test::assert_equivalent(om, m);
    }

    // 2) Range-insert from another Lazy container iterators & erasures & swaps.
    {
        jk m = b;
        oj om = oracle_orig;
        m["arr"].insert(m["arr"].end(), c["tags"].cbegin(), c["tags"].cend());
        om["arr"].insert(om["arr"].end(), oracle_orig["tags"].cbegin(), oracle_orig["tags"].cend());
        m.erase("tags");
        om.erase("tags");
        m["arr"].erase(0);
        om["arr"].erase(0);
        m["arr"].erase(m["arr"].begin() + 1, m["arr"].begin() + 3);
        om["arr"].erase(om["arr"].begin() + 1, om["arr"].begin() + 3);
        neojson::test::assert_equivalent(om, m);

        jk m_clear = b;
        oj om_clear = oracle_orig;
        m_clear["arr"].clear();
        om_clear["arr"].clear();
        m_clear["obj"].clear();
        om_clear["obj"].clear();
        neojson::test::assert_equivalent(om_clear, m_clear);

        jk m_swap = b;
        oj om_swap = oracle_orig;
        jk::array_t ext_arr = {jk(100), jk(200)};
        oj::array_t oext_arr = {oj(100), oj(200)};
        m_swap["arr"].swap(ext_arr);
        om_swap["arr"].swap(oext_arr);
        jk::object_t ext_obj;
        ext_obj["only"] = jk(1);
        oj::object_t oext_obj;
        oext_obj["only"] = oj(1);
        m_swap["obj"].swap(ext_obj);
        om_swap["obj"].swap(oext_obj);
        neojson::test::assert_equivalent(om_swap, m_swap);
    }

    // Originals must stay completely untouched!
    EXPECT_EQ(a.dump(), expected_dump);
    EXPECT_EQ(c.dump(), expected_dump);
    neojson::test::assert_equivalent(oracle_orig, a);
    neojson::test::assert_equivalent(oracle_orig, c);

    // 3) Single-owner zero-copy steal after const reads.
    {
        jk s = jk::parse(raw);
        oj os = oracle_orig;
        EXPECT_EQ(s.size(), os.size());
        EXPECT_TRUE(s.contains("arr"));
        EXPECT_EQ(std::as_const(s)["arr"].size(), 5u);
        s["arr"].push_back(6);
        os["arr"].push_back(6);
        s["added"] = true;
        os["added"] = true;
        neojson::test::assert_equivalent(os, s);
    }

    // 4) Lazy 2-element array passed inside initializer-list object constructor.
    {
        jk pair = jk::parse(R"(["k",[1,2,3,4,5]])");
        oj opair = oj::parse(R"(["k",[1,2,3,4,5]])");
        jk built = {pair};
        oj obuild = {opair};
        neojson::test::assert_equivalent(obuild, built);
    }
}

TEST(LazyCow, ExplicitMaterializeDetachesDeeply) {
    const std::string raw = R"({
        "a": {"b": [1, 2, {"c": "val", "d": [10, 20, 30, 40, 50]}]},
        "dup": {"x": 1, "y": 2, "x": 3},
        "empty_obj": {},
        "empty_arr": []
    })";
    const oj oracle = oj::parse(raw);

    jk doc = jk::parse(raw);
    const jk untouched = doc;
    doc.materialize();
    doc["a"]["b"][2]["c"] = "changed";

    oj modified = oracle;
    modified["a"]["b"][2]["c"] = "changed";

    neojson::test::assert_equivalent(modified, doc);
    neojson::test::assert_equivalent(oracle, untouched);

    // materialize() is a clean no-op on scalars, nulls, and empty containers.
    jk scalar = jk(123);
    jk null_j = nullptr;
    jk empty_o = jk::parse("{}");
    scalar.materialize();
    null_j.materialize();
    empty_o.materialize();
    EXPECT_EQ(scalar, 123);
    EXPECT_TRUE(null_j.is_null());
    EXPECT_TRUE(empty_o.is_object() && empty_o.empty());
}

TEST(LazyCow, ConcurrentConstReadsAndPromotionStress) {
    // Include >16 keys to exercise lookup_probes materialization + nested dup-key sub-object.
    std::string raw = R"({"dup":{"a":1,"b":2,"a":3},"arr":[1,2,3,4,5],"nested":{"inner":{"k":"v","nums":[10,20,30,40,50]}})";
    for (int i = 0; i < 24; ++i) {
        raw += ",\"key_" + std::to_string(i) + "\":" + std::to_string(i * 7);
    }
    raw.push_back('}');

    const oj oracle = oj::parse(raw);
    const std::string expected_compact = oracle.dump();
    const std::string expected_pretty = oracle.dump(2);

    constexpr int kRounds = 20;
    constexpr int kThreads = 8;

    for (int r = 0; r < kRounds; ++r) {
        const jk root = jk::parse(raw);
        const jk sibling = jk::parse(raw);
        std::vector<std::thread> readers;
        readers.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            readers.emplace_back([&, t]() {
                EXPECT_FALSE(root.empty());
                EXPECT_EQ(root.size(), oracle.size());
                // Cross lookup_probes threshold concurrently on >16-key object.
                for (int p = 0; p < 6; ++p) {
                    EXPECT_TRUE(root.contains("key_" + std::to_string((t + p) % 24)));
                    EXPECT_FALSE(root.contains("missing_" + std::to_string(t + p)));
                }
                EXPECT_EQ(root.dump(), expected_compact);
                EXPECT_EQ(root.dump(2), expected_pretty);
                EXPECT_EQ(root, sibling);
                EXPECT_FALSE(root < sibling);

                const jk cow = root;
                EXPECT_EQ(cow.at("dup").size(), 2u);
                EXPECT_EQ(cow.at("dup")["a"], 3);
                EXPECT_EQ(cow["arr"][4], 5);
                auto it = cow.find("key_19");
                ASSERT_NE(it, cow.cend());
                EXPECT_EQ(it.key(), "key_19");
                EXPECT_EQ(it.value(), 19 * 7);
                const auto* obj_ptr = cow.get_ptr<const jk::object_t*>();
                ASSERT_NE(obj_ptr, nullptr);
                EXPECT_EQ(obj_ptr->size(), oracle.size());

                std::size_t iterated = 0;
                for (auto cit = cow.cbegin(); cit != cow.cend(); ++cit) {
                    ++iterated;
                }
                EXPECT_EQ(iterated, oracle.size());
            });
        }
        for (std::thread& th : readers) {
            th.join();
        }
        neojson::test::assert_equivalent(oracle, root);
    }
}

TEST(LazyCow, ConcurrentCowMutateAndCrossThreadDestroyStress) {
    const std::string raw = R"({
        "list": [1, 2, 3, 4, 5, 6],
        "map": {"one": [10, 20, 30, 40, 50], "two": {"a": 1, "b": 2}},
        "msg": "shared_lazy_doc"
    })";
    const oj oracle = oj::parse(raw);

    constexpr int kThreads = 8;
    constexpr int kIters = 16;

    for (int iter = 0; iter < kIters; ++iter) {
        const jk master = jk::parse(raw);
        struct Mailbox {
            std::mutex mu;
            std::vector<jk> items;
        };
        std::array<Mailbox, kThreads> mailboxes;

        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int tid = 0; tid < kThreads; ++tid) {
            workers.emplace_back([&, tid]() {
                jk local = master;
                if (tid % 2 == 0) {
                    EXPECT_EQ(local.size(), oracle.size());
                    EXPECT_TRUE(local.contains("map"));
                    EXPECT_EQ(local.dump(), oracle.dump());
                    EXPECT_EQ(local["map"]["one"][4], 50);
                } else {
                    local["list"].push_back(tid);
                    local["map"]["two"]["extra"] = tid * 10;
                    if (tid == 3) {
                        local.materialize();
                    } else if (tid == 5) {
                        local["list"].clear();
                    }
                    EXPECT_NE(local, master);
                }

                const int next = (tid + 1) % kThreads;
                {
                    std::lock_guard<std::mutex> lk(mailboxes[next].mu);
                    mailboxes[next].items.push_back(std::move(local));
                }
                std::vector<jk> drained;
                {
                    std::lock_guard<std::mutex> lk(mailboxes[tid].mu);
                    drained.swap(mailboxes[tid].items);
                }
                for (const jk& incoming : drained) {
                    EXPECT_TRUE(incoming.is_object());
                }
            });
        }
        for (std::thread& w : workers) {
            w.join();
        }
        neojson::test::assert_equivalent(oracle, master);
    }
}

TEST(LazyCow, DeepNestingOn512KiBSmallStack) {
    constexpr std::size_t kDepth = 1100;
    constexpr std::size_t kSmallStack = 512u * 1024u;
    constexpr std::size_t kWalkStack = 4u * 1024u * 1024u;

    std::string deep_arr(kDepth, '[');
    deep_arr += "1,2";
    deep_arr.append(kDepth, ']');

    std::string deep_alt;
    for (std::size_t i = 0; i < kDepth; ++i) {
        deep_alt += (i % 2 == 0) ? "[" : "{\"k\":";
    }
    deep_alt += "42";
    for (std::size_t i = kDepth; i > 0; --i) {
        deep_alt += ((i - 1) % 2 == 0) ? "]" : "}";
    }

    jk::parse_options opts;
    opts.max_depth = kDepth + 64u;

    for (const std::string& raw : {deep_arr, deep_alt}) {
        // (a) Non-recursive Lazy parse + size/empty + iterative destroy on 512 KiB stack.
        const std::string err_a = neojson::test::run_with_stack(kSmallStack, [&]() {
            jk doc = jk::parse(raw, nullptr, true, false, opts);
            EXPECT_FALSE(doc.empty());
            EXPECT_EQ(doc.size(), 1u);
        });
        ASSERT_TRUE(err_a.empty()) << err_a;

        // (b) Const materialization down 1050 levels + iterative Lazy destruction via release_containers.
        const std::string err_b = neojson::test::run_with_stack(kSmallStack, [&]() {
            const std::size_t heap_uses_before = neojson::detail::destroy_heap_stack_uses();
            {
                const jk doc = jk::parse(raw, nullptr, true, false, opts);
                const jk* cur = &doc;
                for (std::size_t i = 0; i < 1050; ++i) {
                    if (cur->is_array()) {
                        cur = &((*cur)[0]);
                    } else {
                        cur = &((*cur)["k"]);
                    }
                }
                EXPECT_TRUE(cur->is_structured());
            }
            EXPECT_GT(neojson::detail::destroy_heap_stack_uses(), heap_uses_before);
        });
        ASSERT_TRUE(err_b.empty()) << err_b;

        // (c) Full iterative materialize() + iterative destroy on 512 KiB stack.
        const std::string err_c = neojson::test::run_with_stack(kSmallStack, [&]() {
            jk doc = jk::parse(raw, nullptr, true, false, opts);
            doc.materialize();
            EXPECT_EQ(doc.size(), 1u);
        });
        ASSERT_TRUE(err_c.empty()) << err_c;

        // (d) Recursive Lazy & materialized dump() / dump(2) at depth 1100 on 4 MiB walk stack.
        const std::string err_d = neojson::test::run_with_stack(kWalkStack, [&]() {
            jk doc = jk::parse(raw, nullptr, true, false, opts);
            const std::string lazy_compact = doc.dump();
            EXPECT_EQ(lazy_compact.size(), raw.size());
            EXPECT_FALSE(doc.dump(2).empty());
            doc.materialize();
            EXPECT_EQ(doc.dump(), lazy_compact);
            EXPECT_FALSE(doc.dump(2).empty());
        });
        ASSERT_TRUE(err_d.empty()) << err_d;
    }
}

TEST(LazyCow, PointerPatchDiffFlattenAndConversionsOnLazy) {
    const std::string src_raw = R"({
        "a": {"b": [10, 20, 30, 40, 50]},
        "c": {"x": 1, "y": "hello"},
        "nums": [1, 2, 3, 4, 5, 6]
    })";
    const std::string tgt_raw = R"({
        "a": {"b": [10, 99, 30, 40, 50]},
        "c": {"x": 1, "y": "world", "z": true},
        "nums": [1, 2, 3, 4, 5, 6]
    })";
    const std::string patch_raw = R"({"c":{"y":"merged","z":null},"extra":[7,8]})";

    const oj o_src = oj::parse(src_raw);
    const oj o_tgt = oj::parse(tgt_raw);
    const oj o_mp = oj::parse(patch_raw);

    const jk l_src = jk::parse(src_raw);
    const jk l_tgt = jk::parse(tgt_raw);
    const jk l_mp = jk::parse(patch_raw);

    // json_pointer & flatten/unflatten & value() & get<container>()
    EXPECT_EQ(l_src[jk::json_pointer("/a/b/2")], o_src[oj::json_pointer("/a/b/2")].get<int>());
    EXPECT_EQ(l_src.at(jk::json_pointer("/c/y")), "hello");
    EXPECT_TRUE(l_src.contains(jk::json_pointer("/c/x")));
    EXPECT_EQ(l_src.value(jk::json_pointer("/a/b/1"), -1), 20);
    EXPECT_EQ(l_src["c"].value("x", 0), 1);
    EXPECT_EQ(l_src["c"].value("missing", 99), 99);
    EXPECT_EQ(l_src["nums"].get<std::vector<int>>(), o_src["nums"].get<std::vector<int>>());

    neojson::test::assert_equivalent(o_src.flatten(), l_src.flatten());
    neojson::test::assert_equivalent(o_src.flatten().unflatten(), l_src.flatten().unflatten());

    // diff + patch + patch_inplace
    const oj o_diff = oj::diff(o_src, o_tgt);
    const jk l_diff = jk::diff(l_src, l_tgt);
    neojson::test::assert_equivalent(o_diff, l_diff);
    neojson::test::assert_equivalent(o_src.patch(o_diff), l_src.patch(l_diff));

    jk l_inplace = l_src;
    oj o_inplace = o_src;
    l_inplace.patch_inplace(l_diff);
    o_inplace.patch_inplace(o_diff);
    neojson::test::assert_equivalent(o_inplace, l_inplace);
    neojson::test::assert_equivalent(o_src, l_src);

    // merge_patch with Lazy source & Lazy patch
    jk l_merged = l_src;
    oj o_merged = o_src;
    l_merged.merge_patch(l_mp);
    o_merged.merge_patch(o_mp);
    neojson::test::assert_equivalent(o_merged, l_merged);
    neojson::test::assert_equivalent(o_src, l_src);

    // external_constructor overwrites over Lazy container release cleanly.
    const char* lazy_container = R"({"big":[1,2,3,4,5],"obj":{"k":"v"}})";
    {
        jk j = jk::parse(lazy_container);
        j = 42;
        EXPECT_EQ(j, 42);
    }
    {
        jk j = jk::parse(lazy_container);
        j = "replaced";
        EXPECT_EQ(j, "replaced");
    }
    {
        jk j = jk::parse(lazy_container);
        j = true;
        EXPECT_EQ(j, true);
    }
    {
        jk j = jk::parse(lazy_container);
        j = std::vector<int>{10, 20};
        EXPECT_EQ(j.size(), 2u);
    }
    {
        jk j = jk::parse(lazy_container);
        j = std::map<std::string, int>{{"m", 7}};
        EXPECT_EQ(j["m"], 7);
    }
}

TEST(LazyCow, ClearInPlaceStealAndSharedIsolation) {
    const std::string obj_raw = R"({"k0":1,"k1":2,"k2":[10,20,30,40,50],"dup":1,"dup":2})";
    jk l_obj = jk::parse(obj_raw);
    const jk::object_t* obj_ptr_before = std::as_const(l_obj).get_ptr<const jk::object_t*>();
    ASSERT_NE(obj_ptr_before, nullptr);
    l_obj.clear();
    EXPECT_TRUE(l_obj.is_object());
    EXPECT_TRUE(l_obj.empty());
    EXPECT_EQ(l_obj.size(), 0u);
    EXPECT_EQ(l_obj.dump(), "{}");
    EXPECT_EQ(l_obj.get_ptr<jk::object_t*>(), obj_ptr_before);

    const std::string arr_raw = R"([1,2,3,4,5,[6,7,8,9,10]])";
    jk l_arr = jk::parse(arr_raw);
    const jk::array_t* arr_ptr_before = std::as_const(l_arr).get_ptr<const jk::array_t*>();
    ASSERT_NE(arr_ptr_before, nullptr);
    l_arr.clear();
    EXPECT_TRUE(l_arr.is_array());
    EXPECT_TRUE(l_arr.empty());
    EXPECT_EQ(l_arr.size(), 0u);
    EXPECT_EQ(l_arr.dump(), "[]");
    EXPECT_EQ(l_arr.get_ptr<jk::array_t*>(), arr_ptr_before);

    // Clearing a shared lazy object leaves sibling copy untouched.
    const oj o_obj = oj::parse(obj_raw);
    jk s1 = jk::parse(obj_raw);
    jk s2 = s1;
    s1.clear();
    EXPECT_TRUE(s1.empty());
    EXPECT_EQ(s1.dump(), "{}");
    neojson::test::assert_equivalent(o_obj, s2);

    // Clearing an unmaterialized sole-owner lazy array/object also empties cleanly.
    jk u_obj = jk::parse(obj_raw);
    u_obj.clear();
    EXPECT_TRUE(u_obj.is_object());
    EXPECT_EQ(u_obj.dump(), "{}");
    jk u_arr = jk::parse(arr_raw);
    u_arr.clear();
    EXPECT_TRUE(u_arr.is_array());
    EXPECT_EQ(u_arr.dump(), "[]");
}

TEST(LazyCow, ConcurrentDetachWhileSiblingReleases) {
    const std::string raw = R"({
        "a": [1, 2, 3, 4, 5],
        "b": {"x": 10, "y": [100, 200, 300, 400, 500], "z": "hello"},
        "k0": 0, "k1": 1, "k2": 2, "k3": 3, "k4": 4, "k5": 5
    })";
    const oj o_base = oj::parse(raw);

    for (int iter = 0; iter < 64; ++iter) {
        jk j = jk::parse(raw);
        jk k = j;
        if ((iter & 1) != 0) {
            EXPECT_TRUE(std::as_const(j).contains("b"));
            EXPECT_EQ(std::as_const(j)["b"]["y"][2], 300);
        }
        std::atomic<bool> go{false};
        std::thread detacher([&]() {
            while (!go.load(std::memory_order_acquire)) {
            }
            const jk rhs = std::as_const(j)["b"];
            j["a"] = rhs;
            j["z"] = iter;
        });
        std::thread releaser([&]() {
            while (!go.load(std::memory_order_acquire)) {
            }
            jk doomed = std::move(k);
        });
        go.store(true, std::memory_order_release);
        detacher.join();
        releaser.join();

        oj o_expected = o_base;
        o_expected["a"] = o_expected["b"];
        o_expected["z"] = iter;
        neojson::test::assert_equivalent(o_expected, j);
    }

    // Sole-owner const-materialized self-assignment steals backing storage in place.
    jk sole = jk::parse(raw);
    const jk::object_t* sole_ptr_before = std::as_const(sole).get_ptr<const jk::object_t*>();
    ASSERT_NE(sole_ptr_before, nullptr);
    sole["a"] = std::as_const(sole)["b"];
    EXPECT_EQ(sole.get_ptr<jk::object_t*>(), sole_ptr_before);
    oj o_expected = o_base;
    o_expected["a"] = o_expected["b"];
    neojson::test::assert_equivalent(o_expected, sole);
}

TEST(LazyCow, NestedDuplicateOuterUniqueCachingAndLargeKeys) {
    // Outer object is unique while nested object contains duplicate keys.
    const std::string nested_dup = R"({
        "outer_a": {"d": 1, "d": 2, "e": 3},
        "outer_b": [1, 2, 3, 4, 5],
        "outer_c": {"x": 10, "y": {"k": 1, "k": 2, "m": 9}}
    })";
    const oj o_nested = oj::parse(nested_dup);
    const jk l_nested = jk::parse(nested_dup);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(l_nested.size(), o_nested.size());
        EXPECT_EQ(l_nested.dump(), o_nested.dump());
        EXPECT_EQ(l_nested.dump(2), o_nested.dump(2));
    }
    jk detached = l_nested;
    detached.materialize();
    neojson::test::assert_equivalent(o_nested, detached);

    // 130 distinct 16-byte keys intentionally colliding in bucket 0 (cap <= 512) to exercise
    // DupProbeTable's >=64-probe O(n log n) sorted fallback on both unique and duplicate inputs.
    std::vector<std::string> flood_keys;
    flood_keys.reserve(130);
    for (std::uint64_t idx = 0; flood_keys.size() < 130; ++idx) {
        std::string key(16, 'a');
        std::uint64_t t = idx;
        for (unsigned pos = 0; pos < 16 && t > 0; ++pos) {
            key[pos] = static_cast<char>('a' + (t % 26ULL));
            t /= 26ULL;
        }
        if ((neojson::detail::hash_bytes(key.data(), key.size()) & 511u) == 0u) {
            flood_keys.push_back(std::move(key));
        }
    }
    ASSERT_EQ(flood_keys.size(), 130u);

    std::string flood_unique_json = "{";
    for (std::size_t i = 0; i < flood_keys.size(); ++i) {
        if (i > 0) {
            flood_unique_json.push_back(',');
        }
        flood_unique_json.push_back('"');
        flood_unique_json += flood_keys[i];
        flood_unique_json += "\":" + std::to_string(i);
    }
    std::string flood_dup_json = flood_unique_json + ",\"" + flood_keys[0] + "\":99999}";
    flood_unique_json.push_back('}');

    for (const std::string& json_text : {flood_unique_json, flood_dup_json}) {
        const oj o_flood = oj::parse(json_text);
        const jk l_flood = jk::parse(json_text);
        EXPECT_EQ(l_flood.size(), o_flood.size());
        EXPECT_EQ(l_flood.dump(), o_flood.dump());
    }
}

TEST(LazyCow, SharedStringCowPinSwapClearAndMaterialize) {
    const std::string base_text(128, 'x');
    jk s1 = base_text;
    jk s2 = s1;
    const auto* ptr1 = std::as_const(s1).get_ptr<const jk::string_t*>();
    const auto* ptr2 = std::as_const(s2).get_ptr<const jk::string_t*>();
    ASSERT_NE(ptr1, nullptr);
    EXPECT_EQ(ptr1, ptr2);
    EXPECT_EQ(s1, s2);
    EXPECT_FALSE(s1 < s2);
    EXPECT_FALSE(s2 < s1);

    // Pinning via mutable get_ptr detaches from shared siblings and disables future sharing.
    jk::string_t* mut_ptr = s1.get_ptr<jk::string_t*>();
    ASSERT_NE(mut_ptr, nullptr);
    EXPECT_NE(mut_ptr, ptr2);
    *mut_ptr += "_pinned";
    EXPECT_EQ(s2.get<std::string>(), base_text);
    EXPECT_EQ(s1.get<std::string>(), base_text + "_pinned");

    jk s3 = s1;
    EXPECT_NE(std::as_const(s3).get_ptr<const jk::string_t*>(), mut_ptr);
    *mut_ptr += "_again";
    EXPECT_EQ(s3.get<std::string>(), base_text + "_pinned");
    EXPECT_EQ(s1.get<std::string>(), base_text + "_pinned_again");

    // Mutable get_ref also pins a sole-owner string against future copy sharing.
    jk solo = std::string(96, 'r');
    jk::string_t& solo_ref = solo.get_ref<jk::string_t&>();
    jk solo_copy = solo;
    EXPECT_NE(std::as_const(solo_copy).get_ptr<const jk::string_t*>(), &solo_ref);
    solo_ref += "_solo";
    EXPECT_EQ(solo_copy.get<std::string>(), std::string(96, 'r'));

    // swap(string_t&) detaches without mutating shared sibling.
    jk sw1 = std::string(80, 'k');
    jk sw2 = sw1;
    std::string external = "swapped_value";
    sw1.swap(external);
    EXPECT_EQ(sw1, "swapped_value");
    EXPECT_EQ(external, std::string(80, 'k'));
    EXPECT_EQ(sw2, std::string(80, 'k'));

    // clear() on shared string allocates fresh empty string without affecting sibling.
    jk cl1 = std::string(80, 'q');
    jk cl2 = cl1;
    cl1.clear();
    EXPECT_EQ(cl1, "");
    EXPECT_EQ(cl2, std::string(80, 'q'));
    cl2.clear();
    EXPECT_EQ(cl2, "");

    // Initializer-list key construction and push_back must not move-from shared string keys.
    jk shared_k1 = std::string(72, 'a');
    jk shared_k2 = std::string(72, 'b');
    jk obj = {{shared_k1, 42}};
    obj.push_back({shared_k2, 99});
    EXPECT_EQ(shared_k1, std::string(72, 'a'));
    EXPECT_EQ(shared_k2, std::string(72, 'b'));
    EXPECT_EQ(obj[shared_k1.get<std::string>()], 42);
    EXPECT_EQ(obj[shared_k2.get<std::string>()], 99);

    // materialize() detaches shared strings inside containers as well as root strings.
    jk root_str = std::string(90, 'm');
    jk root_str_copy = root_str;
    root_str.materialize();
    EXPECT_NE(std::as_const(root_str).get_ptr<const jk::string_t*>(),
              std::as_const(root_str_copy).get_ptr<const jk::string_t*>());
    EXPECT_EQ(root_str, root_str_copy);

    jk tree = {{"txt", std::string(90, 't')}, {"arr", {std::string(90, 'u'), 2}}};
    jk tree_copy = tree;
    EXPECT_EQ(std::as_const(tree)["txt"].get_ptr<const jk::string_t*>(),
              std::as_const(tree_copy)["txt"].get_ptr<const jk::string_t*>());
    tree_copy.materialize();
    EXPECT_NE(std::as_const(tree)["txt"].get_ptr<const jk::string_t*>(),
              std::as_const(tree_copy)["txt"].get_ptr<const jk::string_t*>());
    EXPECT_NE(std::as_const(tree)["arr"][0].get_ptr<const jk::string_t*>(),
              std::as_const(tree_copy)["arr"][0].get_ptr<const jk::string_t*>());
    EXPECT_EQ(tree, tree_copy);
}

TEST(LazyCow, ConcurrentSharedStringCopyAndMutateStress) {
    const std::string p1(96, 'p');
    const std::string p2(96, 'q');
    const jk shared_doc = {{"s1", p1}, {"s2", p2}};

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    for (int tid = 0; tid < 4; ++tid) {
        workers.emplace_back([&, tid]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int iter = 0; iter < 1500; ++iter) {
                jk local = shared_doc;
                const jk& c_local = local;
                const jk& cs1 = c_local["s1"];
                const jk& cs2 = c_local["s2"];
                ASSERT_EQ(cs1.get<std::string>(), p1);
                ASSERT_TRUE(cs1 < cs2);

                jk dup = cs1;
                if ((iter + tid) % 4 == 0) {
                    auto* mp = dup.get_ptr<jk::string_t*>();
                    ASSERT_NE(mp, nullptr);
                    *mp += "_x";
                    ASSERT_EQ(dup.get<std::string>(), p1 + "_x");
                } else if ((iter + tid) % 4 == 1) {
                    dup.clear();
                    ASSERT_EQ(dup.get<std::string>(), "");
                } else if ((iter + tid) % 4 == 2) {
                    std::string tmp = "alt";
                    dup.swap(tmp);
                    ASSERT_EQ(dup.get<std::string>(), "alt");
                    ASSERT_EQ(tmp, p1);
                } else {
                    dup = "reassigned";
                    ASSERT_EQ(dup.get<std::string>(), "reassigned");
                }
                ASSERT_EQ(cs1.get<std::string>(), p1);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& t : workers) {
        t.join();
    }
    EXPECT_EQ(shared_doc["s1"].get<std::string>(), p1);
    EXPECT_EQ(shared_doc["s2"].get<std::string>(), p2);
}

TEST(LazyCow, DirectYyjsonDumpEdgeCasesAndDeepStack) {
    // Control escapes (lowercase hex), 0x7F DEL verbatim, UTF-8, -0.0, and scientific bounds.
    std::string raw_edges = R"({"ctrl":")";
    raw_edges += R"(\b\t\n\f\r\"\\/\u0001\u001f)";
    raw_edges.push_back(static_cast<char>(0x7F));
    raw_edges += "\",\"utf8\":\"\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4\xF0\x9F\x8E\x89\"";
    raw_edges += R"(,"neg_zero":-0.0,"small":1e-5,"fixed":1e-4,"big":1e15})";
    const oj o_edges = oj::parse(raw_edges);
    const jk l_edges = jk::parse(raw_edges);
    EXPECT_EQ(l_edges.dump(), o_edges.dump());
    EXPECT_EQ(l_edges.dump(2), o_edges.dump(2));

    // Subtree direct dump and NaN/Inf mapped to null cleanly.
    jk with_nan = l_edges;
    with_nan["nan_v"] = std::numeric_limits<double>::quiet_NaN();
    with_nan["inf_v"] = std::numeric_limits<double>::infinity();
    const jk round_nan = jk::parse(with_nan.dump());
    EXPECT_TRUE(round_nan["nan_v"].is_null());
    EXPECT_TRUE(round_nan["inf_v"].is_null());
    EXPECT_EQ(round_nan["utf8"].dump(), o_edges["utf8"].dump());

    // Duplicate keys at root vs nested duplicate only (outer unique -> size() sets bit 0, then
    // direct dump must detect nested dup and fall back accurately).
    const std::string root_dup_raw = R"({"k":1,"k":2,"arr":[1,2,3]})";
    const oj o_root_dup = oj::parse(root_dup_raw);
    const jk l_root_dup = jk::parse(root_dup_raw);
    EXPECT_EQ(l_root_dup.size(), o_root_dup.size());
    EXPECT_EQ(l_root_dup.dump(), o_root_dup.dump());

    const std::string nested_dup_raw =
        R"({"a":1,"sub":{"d":10,"d":20,"e":30},"leaf":{"x":true,"y":"str"}})";
    const oj o_nested_dup = oj::parse(nested_dup_raw);
    const jk l_nested_dup = jk::parse(nested_dup_raw);
    for (int iter = 0; iter < 3; ++iter) {
        EXPECT_EQ(l_nested_dup.size(), o_nested_dup.size());
        EXPECT_EQ(l_nested_dup.dump(), o_nested_dup.dump());
    }
    EXPECT_EQ(std::as_const(l_nested_dup)["leaf"].dump(), o_nested_dup["leaf"].dump());

    // Direct iterative dump at depth 1100 on a 512 KiB stack must not stack overflow.
    constexpr std::size_t kDepth = 1100u;
    std::string deep_raw;
    for (std::size_t i = 0; i < kDepth; ++i) {
        deep_raw += (i % 2u == 0u) ? "[" : R"({"n":)";
    }
    deep_raw += "7";
    for (std::size_t i = kDepth; i > 0; --i) {
        deep_raw += ((i - 1u) % 2u == 0u) ? "]" : "}";
    }
    jk::parse_options opts;
    opts.max_depth = kDepth + 16u;
    const std::string err = neojson::test::run_with_stack(512u * 1024u, [&]() {
        const jk doc = jk::parse(deep_raw, nullptr, true, false, opts);
        EXPECT_EQ(doc.dump(), deep_raw);
    });
    ASSERT_TRUE(err.empty()) << err;
}

TEST(LazyCow, ConstIteratorAndReverseEdgeCases) {
    static_assert(sizeof(jk::iterator) == sizeof(void*) + sizeof(std::ptrdiff_t));
    static_assert(sizeof(jk::const_iterator) == sizeof(void*) + sizeof(std::ptrdiff_t));

    const std::string obj_raw =
        R"({"k0":0,"k1":1,"k2":2,"k3":3,"k4":4,"k5":5,"k6":6,"k7":7,"k8":8,"k9":9})";
    const std::string arr_raw = "[0,1,2,3,4,5,6,7,8,9,10,11]";
    const oj o_obj = oj::parse(obj_raw);
    const oj o_arr = oj::parse(arr_raw);
    const jk l_obj = jk::parse(obj_raw);
    const jk l_arr = jk::parse(arr_raw);

    const char* phase = "init";
    try {
        phase = "obj_fwd";
        std::vector<std::pair<std::string, int>> fwd_obj, rev_obj;
        for (auto it = l_obj.cbegin(); it != l_obj.cend(); ++it) {
            fwd_obj.emplace_back(it.key(), it.value().get<int>());
        }
        phase = "obj_rev";
        for (auto it = l_obj.crbegin(); it != l_obj.crend(); ++it) {
            rev_obj.emplace_back(it.key(), it.value().get<int>());
        }
        ASSERT_EQ(fwd_obj.size(), o_obj.size());
        std::reverse(rev_obj.begin(), rev_obj.end());
        EXPECT_EQ(fwd_obj, rev_obj);

        phase = "obj_items";
        std::size_t item_count = 0;
        for (const auto& kv : l_obj.items()) {
            neojson::test::assert_tree_equal(o_obj[kv.key()], kv.value());
            ++item_count;
        }
        EXPECT_EQ(item_count, o_obj.size());

        phase = "arr_fwd";
        std::vector<int> fwd_arr, rev_arr;
        for (auto it = l_arr.cbegin(); it != l_arr.cend(); ++it) {
            fwd_arr.push_back(it->get<int>());
        }
        phase = "arr_rev";
        for (auto it = l_arr.crbegin(); it != l_arr.crend(); ++it) {
            rev_arr.push_back(it->get<int>());
        }
        std::reverse(rev_arr.begin(), rev_arr.end());
        EXPECT_EQ(fwd_arr, rev_arr);
        phase = "arr_arith";
        EXPECT_EQ(l_arr.cend() - l_arr.cbegin(), static_cast<std::ptrdiff_t>(o_arr.size()));
        EXPECT_EQ(*(l_arr.cbegin() + 5), 5);
        EXPECT_EQ(*((l_arr.cend() - 1) - 3), 8);

        phase = "mut_convert";
        jk mut_obj = jk::parse(obj_raw);
        EXPECT_EQ(mut_obj.rbegin(), mut_obj.crbegin());
        EXPECT_EQ(mut_obj.crbegin(), mut_obj.rbegin());
        EXPECT_EQ(mut_obj.rend(), mut_obj.crend());
        EXPECT_EQ(mut_obj.crend(), mut_obj.rend());
        auto mit = mut_obj.begin();
        jk::const_iterator cit(mit);
        jk::const_iterator cit2;
        cit2 = mit;
        mut_obj["k0"] = 777;
        EXPECT_EQ(cit.key(), "k0");
        EXPECT_EQ(*cit, 777);
        EXPECT_EQ(*cit2, 777);

        phase = "shared_detach";
        jk shared_a = jk::parse(obj_raw);
        jk shared_b = shared_a;
        auto shared_cit = shared_a.cbegin();
        shared_a["k0"] = 888;
        EXPECT_EQ(shared_cit.key(), "k0");
        EXPECT_EQ(*shared_cit, 888);
        EXPECT_EQ(shared_b.at("k0"), 0);

        phase = "empty_and_scalar";
        const jk empty_obj = jk::object();
        const jk empty_arr = jk::array();
        const jk scalar = 42;
        EXPECT_EQ(empty_obj.cbegin(), empty_obj.cend());
        EXPECT_EQ(empty_obj.crbegin(), empty_obj.crend());
        EXPECT_EQ(empty_arr.cbegin(), empty_arr.cend());
        EXPECT_EQ(empty_arr.crbegin(), empty_arr.crend());
        EXPECT_EQ(scalar.cbegin() + 1, scalar.cend());
        EXPECT_EQ(*scalar.cbegin(), 42);
        EXPECT_EQ(*scalar.crbegin(), 42);
    } catch (const std::exception& e) {
        FAIL() << "Unexpected exception in phase [" << phase << "]: " << e.what();
    }

    // Out-of-range and default-constructed reverse iterators throw invalid_iterator.
    const jk::const_reverse_iterator empty_rev(jk::const_iterator{});
    const jk scalar = 42;
    EXPECT_THROW((void)*l_obj.crend(), jk::invalid_iterator);
    EXPECT_THROW((void)l_obj.crend().key(), jk::invalid_iterator);
    EXPECT_THROW((void)*empty_rev, jk::invalid_iterator);
    EXPECT_THROW((void)empty_rev.key(), jk::invalid_iterator);
    EXPECT_THROW((void)*scalar.crend(), jk::invalid_iterator);
}

} // namespace
