// Differential verification for number formatting (Grisu2 128-bit mul, clz normalize, power-of-10
// table, cached power constants, two-digit uint formatting), SWAR/LUT string escaping across
// alignment boundaries, and DumpBuffer heap growth with non-default formatting options.

#include "differential.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using jk = neojson::json;
using oj = nlohmann::ordered_json;

std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30u)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27u)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31u);
}

TEST(DumpNumbersAndSwar, DoubleDumpMatchesOracleAcross100kRandomAndBoundaryValues) {
    const std::vector<double> special = {
        0.0,
        -0.0,
        1.0,
        -1.0,
        0.1,
        0.2,
        0.3,
        0.1 + 0.2,
        1e-4,
        1e-5,
        1e14,
        1e15,
        1e16,
        1.0868302770500545e-181,
        1.2345678901234567e172,
        9.876543210987654e148,
        1e-308,
        1e-309,
        1e308,
        42.0,
        -42.0,
        1234567.0,
        -1234567.0,
        999999999999998.0,
        999999999999999.0,
        -999999999999999.0,
        1000000000000000.0,
        -1000000000000000.0,
        9007199254740991.0,   // 2^53 - 1
        -9007199254740991.0,
        9007199254740992.0,   // 2^53
        -9007199254740992.0,
        std::numeric_limits<double>::min(),
        -std::numeric_limits<double>::min(),
        std::numeric_limits<double>::denorm_min(),
        -std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::max(),
        -std::numeric_limits<double>::max(),
        std::numeric_limits<double>::epsilon(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),
    };
    auto check_neojson_float_roundtrip = [](const double d, const std::string& dumped,
                                            const std::string& oracle_dumped) {
        if (!std::isfinite(d)) {
            EXPECT_EQ(dumped, "null");
            EXPECT_EQ(oracle_dumped, "null");
            return;
        }
        EXPECT_LE(dumped.size(), oracle_dumped.size()) << "d=" << d << " dumped=" << dumped
                                                       << " oracle=" << oracle_dumped;
        const jk reparsed = jk::parse(dumped);
        ASSERT_TRUE(reparsed.is_number()) << "dumped=" << dumped;
        const double rd = reparsed.get<double>();
        std::uint64_t bits_in = 0;
        std::uint64_t bits_out = 0;
        std::memcpy(&bits_in, &d, sizeof(bits_in));
        std::memcpy(&bits_out, &rd, sizeof(bits_out));
        EXPECT_EQ(bits_out, bits_in) << "dumped=" << dumped;
        EXPECT_EQ(reparsed.dump(), dumped);
        const std::size_t epos = dumped.find('e');
        if (epos != std::string::npos) {
            ASSERT_LT(epos + 3, dumped.size()) << "dumped=" << dumped;
            EXPECT_TRUE(dumped[epos + 1] == '+' || dumped[epos + 1] == '-');
            EXPECT_TRUE(dumped[epos + 2] >= '0' && dumped[epos + 2] <= '9');
            EXPECT_TRUE(dumped[epos + 3] >= '0' && dumped[epos + 3] <= '9');
        }
    };

    for (const double d : special) {
        const std::string dumped = jk(d).dump();
        const std::string oracle = oj(d).dump();
        check_neojson_float_roundtrip(d, dumped, oracle);
        if (d == 0.0 || d == 1.0 || d == -1.0 || d == 0.1 || d == 0.2 || d == 0.3 ||
            d == 1e-4 || d == 1e-5 || d == 1e14 || d == 1e15 || d == 1e16 || d == 42.0 ||
            d == -42.0 || d == 999999999999999.0 || d == 1000000000000000.0 || !std::isfinite(d)) {
            EXPECT_EQ(dumped, oracle) << "val=" << d;
        }
    }

    std::uint64_t rng = 0x517CC1B727220A95ULL;
    oj o_batch = oj::array();
    jk s_batch = jk::array();
    for (int i = 0; i < 100000; ++i) {
        const std::uint64_t bits = splitmix64(rng);
        double d = 0.0;
        std::memcpy(&d, &bits, sizeof(d));
        if (i < 5000) {
            check_neojson_float_roundtrip(d, jk(d).dump(), oj(d).dump());
        }
        o_batch.push_back(d);
        s_batch.push_back(d);
    }
    const std::string batch_compact = s_batch.dump();
    const std::string batch_pretty = s_batch.dump(2);
    const jk lazy_batch = jk::parse(batch_compact);
    EXPECT_EQ(lazy_batch.dump(), batch_compact);
    const oj oracle_reparsed = oj::parse(o_batch.dump());
    neojson::test::assert_tree_equal(oracle_reparsed, lazy_batch);
    neojson::test::assert_tree_equal(oracle_reparsed, jk::parse(batch_pretty));
}

TEST(DumpNumbersAndSwar, SignedAndUnsignedIntegersMatchOracle) {
    const std::vector<std::int64_t> signed_vals = {
        std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::min() + 1,
        -1000000000000000000LL,
        -10000,
        -1000,
        -100,
        -99,
        -10,
        -9,
        -1,
        0,
        1,
        9,
        10,
        11,
        99,
        100,
        101,
        999,
        1000,
        9999,
        10000,
        999999,
        1000000,
        1000000000LL,
        std::numeric_limits<std::int64_t>::max() - 1,
        std::numeric_limits<std::int64_t>::max(),
    };
    for (const std::int64_t v : signed_vals) {
        EXPECT_EQ(oj(v).dump(), jk(v).dump()) << "v=" << v;
    }

    std::vector<std::uint64_t> unsigned_vals = {
        0u, 1u, 9u, 10u, 99u, 100u, 101u, 9999u, 10000u,
        std::numeric_limits<std::uint64_t>::max() - 1u,
        std::numeric_limits<std::uint64_t>::max(),
    };
    std::uint64_t p = 1;
    for (int e = 0; e <= 19; ++e) {
        if (e > 0) {
            unsigned_vals.push_back(p - 1);
        }
        unsigned_vals.push_back(p);
        unsigned_vals.push_back(p + 1);
        if (e < 19) {
            p *= 10u;
        }
    }
    for (const std::uint64_t v : unsigned_vals) {
        EXPECT_EQ(oj(v).dump(), jk(v).dump()) << "u=" << v;
    }
}

TEST(DumpNumbersAndSwar, SwarStringBoundariesAndBufferGrowthMatchOracle) {
    const std::vector<std::string> markers = {
        std::string(1, '\0'),
        std::string(1, '\x01'),
        std::string(1, '\x1F'),
        "\"",
        "\\",
        "\n",
        "\t",
        "\x7F",
        "\xC3\xA9",
        "\xE2\x82\xAC",
        "\xF0\x9F\x98\x80",
        "\xFF",
        "\xE2\x82",
    };

    for (std::size_t len = 0; len <= 33; ++len) {
        const std::string clean(len, 'a');
        EXPECT_EQ(oj(clean).dump(), jk(clean).dump());
        EXPECT_EQ(oj(clean).dump(-1, ' ', true), jk(clean).dump(-1, ' ', true));
    }

    const std::vector<std::pair<jk::error_handler_t, oj::error_handler_t>> handlers = {
        {jk::error_handler_t::strict, oj::error_handler_t::strict},
        {jk::error_handler_t::ignore, oj::error_handler_t::ignore},
        {jk::error_handler_t::replace, oj::error_handler_t::replace},
    };
    for (std::size_t prefix = 0; prefix <= 16; ++prefix) {
        for (const std::string& m : markers) {
            const std::string text = std::string(prefix, 'x') + m + std::string(9, 'y');
            for (const bool ensure_ascii : {false, true}) {
                for (const auto& [s_eh, o_eh] : handlers) {
                    const oj o_val(text);
                    const jk s_val(text);
                    neojson::test::assert_dump_option_equal(
                        o_val, s_val, -1, ' ', ensure_ascii, o_eh, s_eh);
                }
            }
        }
    }

    // Exercise DumpBuffer growth past inline 4 KiB and pretty-indent growth past 512 chars.
    oj o_root = oj::object();
    jk s_root = jk::object();
    for (int i = 0; i < 400; ++i) {
        const std::string k = "key_" + std::to_string(i);
        const std::string v = std::string(40, static_cast<char>('a' + (i % 26))) + "\"\\";
        o_root[k] = v;
        s_root[k] = v;
    }
    EXPECT_EQ(o_root.dump(), s_root.dump());
    EXPECT_EQ(o_root.dump(4, '\t', true), s_root.dump(4, '\t', true));

    oj o_deep = 42;
    jk s_deep = 42;
    for (int i = 0; i < 150; ++i) {
        o_deep = oj::array({o_deep});
        s_deep = jk::array({s_deep});
    }
    EXPECT_EQ(o_deep.dump(4, '\t'), s_deep.dump(4, '\t'));
}

TEST(DumpNumbersAndSwar, Utf8MultibyteStreamingAndErrorsMatchOracle) {
    const std::vector<std::string> valid_cases = {
        // Long multi-byte runs (Korean, Japanese, Arabic, emoji) with/without ASCII & escapes
        "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4\xEB\xAC\xB8\xEC\x9E\xA5",
        "\xE3\x81\x8D\xE3\x82\x87\xE3\x81\x86\xE3\x81\xAF\xE3\x81\x84\xE3\x81\x84\xE5\xA4\xA9\xE6\xB0\x97",
        "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9",
        "\xF0\x9F\x98\x80\xF0\x9F\x9A\x80\xF0\x9F\x8E\x89\xE2\x9C\xA8",
        "prefix_abcdefgh_\xE3\x81\x82\n\t\"\\\x01\xED\x95\x9C_suffix",
        // Exact RFC 3629 boundary code points
        "\xC2\x80\xDF\xBF\xE0\xA0\x80\xED\x9F\xBF\xEE\x80\x80\xEF\xBF\xBF\xF0\x90\x80\x80\xF4\x8F\xBF\xBF",
        // 0x7F DEL + multi-byte mix
        "abc\x7F\xC2\x80\x7F\xF4\x8F\xBF\xBFxyz",
    };

    for (const std::string& s : valid_cases) {
        neojson::test::assert_equivalent(oj(s), jk(s));
    }

    // Object keys with clean ASCII, escaped ASCII, multi-byte UTF-8, and mixed values (key-colon fusion)
    {
        oj o_obj = oj::object();
        jk s_obj = jk::object();
        for (std::size_t idx = 0; idx < valid_cases.size(); ++idx) {
            const std::string k = "key_" + std::to_string(idx) + "_" + valid_cases[idx];
            o_obj[k] = valid_cases[idx];
            s_obj[k] = valid_cases[idx];
        }
        o_obj["plain_ascii_key_123456789"] = "plain_ascii_val_123456789";
        s_obj["plain_ascii_key_123456789"] = "plain_ascii_val_123456789";
        // Large expansion (> 6x input via repeated escapes) to force DumpBuffer growth mid-stream
        std::string many_escapes;
        for (int i = 0; i < 1200; ++i) {
            many_escapes += "\x01\"\\\n";
        }
        o_obj["many_escapes"] = many_escapes;
        s_obj["many_escapes"] = many_escapes;
        neojson::test::assert_equivalent(o_obj, s_obj);
    }

    // Invalid and truncated UTF-8 sequences must match nlohmann::ordered_json strict error index & byte
    const std::vector<std::string> invalid_tails = {
        "\x80",
        "\xC0\xAF",
        "\xC1\xBF",
        "\xE0\x80\xAF",
        "\xE0\x9F\xBF",
        "\xED\xA0\x80",
        "\xED\xBF\xBF",
        "\xF0\x80\x80\x80",
        "\xF0\x8F\xBF\xBF",
        "\xF4\x90\x80\x80",
        "\xF5\x80\x80\x80",
        "\xFF",
        "\xC2",
        "\xE2\x82",
        "\xE2\x82 ",
        "\xF0\x9F\x98",
        "\xF0\x9F\x98z",
    };
    const std::vector<std::string> prefixes = {
        "",
        "0123456789abcdef",
        "esc\n\x01\xE3\x81\x82",
    };
    for (const std::string& pre : prefixes) {
        for (const std::string& bad : invalid_tails) {
            const std::string sample = pre + bad;
            neojson::test::assert_equivalent(oj(sample), jk(sample));
            oj o_k = oj::object();
            jk s_k = jk::object();
            o_k[sample] = 42;
            s_k[sample] = 42;
            neojson::test::assert_equivalent(o_k, s_k);
        }
    }
}

} // namespace
