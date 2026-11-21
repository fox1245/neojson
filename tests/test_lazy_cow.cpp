#include <gtest/gtest.h>

#include "differential.hpp"
#include "small_stack.hpp"

#include <array>
#include <cstdint>
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

} // namespace
