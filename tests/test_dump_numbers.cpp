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

TEST(DumpNumbersAndSwar, FloatPairRunsAndFractionalSwarMatchOracleBitExact) {
    auto expect_double_bits_eq = [](const double a, const double b, const char* ctx) {
        std::uint64_t ua = 0u;
        std::uint64_t ub = 0u;
        std::memcpy(&ua, &a, sizeof(ua));
        std::memcpy(&ub, &b, sizeof(ub));
        EXPECT_EQ(ua, ub) << ctx << " a=" << a << " b=" << b;
    };

    const std::vector<std::string> boundary_tokens = {
        "0.0", "-0.0", "0.5", "-0.5", "0.25", "0.125", "5.0", "50.0", "500.0",
        "1234567.0", "1234567.8", "1234567.89", "-65.625", "-65.613616999999977",
        "43.420273000000009", "0.00000125", "123456789012345.0", "12345678901234.5",
        "1234567890123.45", "123456789012.345", "999999999999999.0",
        "1.0e-20", "-2.5e+20", "1.25e-10", "3.5e300", "-4.5e-300",
        "2.2250738585072014e-308", "4.9406564584124654e-324",
        "1.7976931348623157e+308",
        "1.23456789012345678901", "-98.7654321098765432109",
    };

    // Exercise native SIMD parser (>= 32B) and short-tail boundary (end - p < 8).
    std::string tail_doc = "[0.0,0.0,0.0,0.0,0.0,0.0,0.0,1.25]";
    {
        const jk sj = jk::parse(tail_doc);
        const oj o = oj::parse(tail_doc);
        ASSERT_EQ(sj.size(), o.size());
        for (std::size_t i = 0u; i < o.size(); ++i) {
            expect_double_bits_eq(sj[i].get<double>(), o[i].get<double>(), "tail_doc");
        }
    }

    std::string compact = "[";
    std::string spaced = "[ ";
    auto append_pair = [&](const std::string& a, const std::string& b, const bool first) {
        if (!first) {
            compact += ',';
            spaced += " , ";
        }
        compact += '[';
        compact += a;
        compact += ',';
        compact += b;
        compact += ']';

        spaced += "[ ";
        spaced += a;
        spaced += " , ";
        spaced += b;
        spaced += " ]";
    };

    std::size_t pair_idx = 0u;
    for (std::size_t i = 0u; i < boundary_tokens.size(); i += 2u) {
        const std::string& a = boundary_tokens[i];
        const std::string& b = boundary_tokens[(i + 1u) < boundary_tokens.size() ? (i + 1u) : 0u];
        append_pair(a, b, pair_idx++ == 0u);
    }

    // 25,000 PRNG pairs = 50,000 floats across 1..19+ total digits and 1..18 frac digits.
    std::uint64_t rng = 0x243F6A8885A308D3ULL;
    auto gen_decimal = [&]() -> std::string {
        const std::uint64_t r0 = splitmix64(rng);
        const std::uint64_t r1 = splitmix64(rng);
        const std::uint64_t r2 = splitmix64(rng);
        std::string out;
        if ((r0 & 1u) != 0u) {
            out.push_back('-');
        }
        const int int_mode = static_cast<int>((r0 >> 1u) % 5u);
        if (int_mode == 0) {
            out.push_back('0');
        } else if (int_mode == 1) {
            out += std::to_string(1u + (r0 >> 8u) % 99u);
        } else if (int_mode == 2) {
            out += std::to_string(100u + (r0 >> 8u) % 900u);
        } else if (int_mode == 3) {
            out += std::to_string(1u + (r0 >> 8u) % 99999999ULL);
        } else {
            out += std::to_string(100000000000ULL + (r0 >> 8u) % 900000000000ULL);
        }
        out.push_back('.');
        const int lz = static_cast<int>((r1 & 3u) == 0u ? ((r1 >> 2u) % 5u) : 0u);
        for (int z = 0; z < lz; ++z) {
            out.push_back('0');
        }
        const int frac_len = 1 + static_cast<int>((r1 >> 5u) % 18u);
        const std::string raw_digits =
            std::to_string(r2 % 1000000000000000000ULL + 100000000000000000ULL);
        out.append(raw_digits, 0, static_cast<std::size_t>(frac_len));
        return out;
    };

    for (int i = 0; i < 25000; ++i) {
        const std::string a = gen_decimal();
        const std::string b = gen_decimal();
        if (!compact.empty()) {
            compact += ',';
        }
        compact += '[';
        compact += a;
        compact += ',';
        compact += b;
        compact += ']';
        if (i < 1000) {
            spaced += " ,\n  [ ";
            spaced += a;
            spaced += " , ";
            spaced += b;
            spaced += " ]";
        }
    }
    compact += ']';
    spaced += " ]";

    auto verify_array_of_pairs = [&](const std::string& json_text, const char* label) {
        const oj oracle = oj::parse(json_text);
        const jk lazy = jk::parse(json_text);
        ASSERT_EQ(lazy.size(), oracle.size()) << label;
        for (std::size_t i = 0u; i < oracle.size(); ++i) {
            ASSERT_EQ(lazy[i].size(), oracle[i].size()) << label << " i=" << i;
            expect_double_bits_eq(
                lazy[i][0].get<double>(), oracle[i][0].get<double>(), label);
            expect_double_bits_eq(
                lazy[i][1].get<double>(), oracle[i][1].get<double>(), label);
        }
        const std::string dumped = lazy.dump();
        const jk reparsed = jk::parse(dumped);
        ASSERT_EQ(reparsed.size(), oracle.size()) << label;
        for (std::size_t i = 0u; i < oracle.size(); ++i) {
            expect_double_bits_eq(
                reparsed[i][0].get<double>(), oracle[i][0].get<double>(), label);
            expect_double_bits_eq(
                reparsed[i][1].get<double>(), oracle[i][1].get<double>(), label);
        }
    };

    verify_array_of_pairs(compact, "compact_50k");
    verify_array_of_pairs(spaced, "spaced_2k");

    // Non-pair / mixed fallback paths in parse_num_pair_run and write_num_pair_run_flat.
    const std::string mixed_json =
        "[[-65.5,43.25],[1],[2.5,3,4.75],[1,999999999999999999],[-12,34.5],"
        "[1.5,\"x\"],[1,[2]],[\"x\",1.5],[0.0000000000,-0.0000000001]]";
    neojson::test::assert_equivalent(oj::parse(mixed_json), jk::parse(mixed_json));
    EXPECT_EQ(jk::parse(mixed_json).dump(), oj::parse(mixed_json).dump());

    // Malformed float syntax and truncated EOFs must be rejected identically even inside >= 32B pair payloads.
    const std::vector<std::string> invalid_pair_inputs = {
        "[[-1.0,2.0],[-3.0,4.0],[-5.,6.0]]",
        "[[-1.0,2.0],[-3.0,4.0],[-5.e2,6.0]]",
        "[[-1.0,2.0],[-3.0,4.0],[1.0e,2.0]]",
        "[[-1.0,2.0],[-3.0,4.0],[-.,6.0]]",
        "[[-1.0,2.0],[-3.0,4.0],[01.5,6.0]]",
        "[[-1.0,2.0],[-3.0,4.0],[-3.0,4.0],[1.2,]]",
        "[[-1.0,2.0],[-3.0,4.0],[1.2,3.]]",
        "[[-1.0,2.0],[-3.0,4.0],[-3.0,4.0],",
        "[[-1.0,2.0],[-3.0,4.0],[-3.0,4.0],[1.0,",
        "[[-1.0,2.0],[-3.0,4.0],[-3.0,4.0],[1.0,2.0",
        "[0,0,0,0,0,0,0,0,0,0,0,0,0,0,[1]",
    };
    for (const std::string& bad : invalid_pair_inputs) {
        ASSERT_GE(bad.size(), 32u) << bad;
        EXPECT_FALSE(jk::accept(bad)) << bad;
        EXPECT_FALSE(oj::accept(bad)) << bad;
    }
}

TEST(DumpNumbersAndSwar, SimdParser16ByteBoundaryUtf8AndIndentParity) {
    // 1. Key and string value lengths 0..68 covering 16B and 32B vector boundaries.
    for (std::size_t len = 0u; len <= 68u; ++len) {
        const std::string ascii_key = "k" + std::string(len, 'x');
        const std::string ascii_val(len, 'a');
        const std::string doc1 =
            "{\"" + ascii_key + "\":\"" + ascii_val + "\",\"tail\":\"01234567890123456789\"}";
        ASSERT_GE(doc1.size(), 32u);
        neojson::test::assert_equivalent(oj::parse(doc1), jk::parse(doc1));
        EXPECT_EQ(jk::parse(doc1).dump(), oj::parse(doc1).dump());

        const std::string esc_doc =
            "{\"k" + std::string(len, 'b') + "\":\"" + std::string(len, 'c') +
            "\\\"\\\\\\n\\r\\t\\u00e9\\uD83D\\uDE00tail\"}";
        neojson::test::assert_equivalent(oj::parse(esc_doc), jk::parse(esc_doc));
        EXPECT_EQ(jk::parse(esc_doc).dump(), oj::parse(esc_doc).dump());

        // 2, 3, and 4-byte UTF-8 straddling every offset across 16B and 32B boundaries:
        // both entered mid-string (`utf8_doc`) and entered at offset 0 (`utf8_stream`)
        // so parse_string_utf8 processes prev != zero chunks AND p[-1..-3] tail retreats,
        // plus escape-entered parse_string_slow (`slow_retreat`) across c13..c15 / c29..c31.
        const std::string pad_before(len, 'p');
        const std::string u2 = "\xC3\xA9";          // e-acute
        const std::string u3 = "\xE2\x82\xAC";      // euro sign
        const std::string u4 = "\xF0\x9F\x98\x80";  // grinning face
        const std::string utf8_doc =
            "{\"u" + std::to_string(len) + "\":\"" + pad_before + u2 + "q" + u3 + "r" + u4 +
            "\",\"" + pad_before + u3 + "\":\"" + pad_before + u4 + u2 + "z\",\"pad\":\"" +
            std::string(36u, 'm') + "\"}";
        ASSERT_GE(utf8_doc.size(), 32u);
        neojson::test::assert_equivalent(oj::parse(utf8_doc), jk::parse(utf8_doc));
        EXPECT_EQ(jk::parse(utf8_doc).dump(), oj::parse(utf8_doc).dump());

        const std::string utf8_stream =
            "{\"" + u2 + pad_before + u3 + "a\":\"" + u2 + pad_before + u4 + u3 + u2 +
            std::string(8u, 'q') + "\",\"tail\":\"" + std::string(36u, 'm') + "\"}";
        ASSERT_GE(utf8_stream.size(), 32u);
        neojson::test::assert_equivalent(oj::parse(utf8_stream), jk::parse(utf8_stream));
        EXPECT_EQ(jk::parse(utf8_stream).dump(), oj::parse(utf8_stream).dump());

        const std::string slow_retreat =
            "{\"s\":\"\\n" + std::string(len, 's') + u2 + u3 + u4 + std::string(20u, 't') +
            "\"}";
        ASSERT_GE(slow_retreat.size(), 32u);
        neojson::test::assert_equivalent(oj::parse(slow_retreat), jk::parse(slow_retreat));
        EXPECT_EQ(jk::parse(slow_retreat).dump(), oj::parse(slow_retreat).dump());
    }

    // Valid multi-byte UTF-8 characters split at the final 16B/32B loop exit (<16B / <32B left
    // in string OR EOF), exercising parse_string_utf8 p[-1..-3] retreat back into scalar tail.
    const std::string u2 = "\xC3\xA9";
    const std::string u3 = "\xE2\x82\xAC";
    const std::string u4 = "\xF0\x9F\x98\x80";
    for (const std::size_t chunk_ofs : {13u, 14u, 15u, 16u, 29u, 30u, 31u, 32u}) {
        const std::string str_core = u2 + std::string(chunk_ofs - 2u, 'a');
        for (const std::string& mb : {u2, u3, u4}) {
            for (const std::string& suf : {std::string(""), std::string("z"), std::string("\xC3\xA9")}) {
                // Closing quote right after mb+suf leaves <16B / <32B at EOF inside string.
                const std::string s = str_core + mb + suf;
                const std::string valid_tail = "{\"pad\":\"" + std::string(20u, 'v') + "\",\"u\":\"" + s + "\"}";
                ASSERT_GE(valid_tail.size(), 32u);
                neojson::test::assert_equivalent(oj::parse(valid_tail), jk::parse(valid_tail));
                EXPECT_EQ(jk::parse(valid_tail).dump(), oj::parse(valid_tail).dump());

                // Same split sequence truncated at EOF (missing closing quote) must fail cleanly.
                const std::string eof_split = "{\"pad\":\"" + std::string(20u, 'v') + "\",\"u\":\"" + s;
                ASSERT_GE(eof_split.size(), 32u);
                EXPECT_FALSE(jk::accept(eof_split));
                EXPECT_FALSE(oj::accept(eof_split));
            }
        }
    }

    // 2. Malformed UTF-8 and raw control bytes across offsets 0..33 (all >= 32B),
    //    both ASCII-prefixed and UTF-8-streamed (`\xC3\xA9` prefix so prev != zero across boundaries).
    const std::vector<std::size_t> offsets = {0u, 1u, 13u, 14u, 15u, 16u, 17u, 30u, 31u, 32u};
    const std::vector<std::string> bad_seqs = {
        "\xC0\x80",                 // overlong 2-byte
        "\xE0\x80\x80",             // overlong 3-byte
        "\xF0\x80\x80\x80",         // overlong 4-byte
        "\xED\xA0\x80",             // UTF-16 high surrogate
        "\xF4\x90\x80\x80",         // > U+10FFFF
        "\x80",                     // stray continuation
        "\xC2",                     // truncated 2-byte before closing quote or escape
        "\xE2\x82",                 // truncated 3-byte before closing quote or escape
        "\xF0\x9F\x98",             // truncated 4-byte before closing quote or escape
        std::string(1u, '\x00'),    // raw NUL control byte
        std::string(1u, '\x1F'),    // raw 0x1F control byte
    };
    for (const std::size_t ofs : offsets) {
        for (const bool stream_utf8 : {false, true}) {
            const std::string prefix = stream_utf8 ? (u2 + std::string(ofs, 'a')) : std::string(ofs, 'a');
            for (const std::string& seq : bad_seqs) {
                const std::string close_quote =
                    "{\"s\":\"" + prefix + seq + "\",\"tail\":\"012345678901234567890123456789\"}";
                ASSERT_GE(close_quote.size(), 32u);
                EXPECT_FALSE(jk::accept(close_quote)) << "ofs=" << ofs;
                EXPECT_FALSE(oj::accept(close_quote)) << "ofs=" << ofs;

                const std::string before_esc =
                    "{\"s\":\"" + prefix + seq + "\\n\",\"tail\":\"012345678901234567890123456789\"}";
                ASSERT_GE(before_esc.size(), 32u);
                EXPECT_FALSE(jk::accept(before_esc)) << "ofs=" << ofs;
                EXPECT_FALSE(oj::accept(before_esc)) << "ofs=" << ofs;

                const std::string before_quote_end =
                    "{\"padding\":\"01234567890123456789\",\"s\":\"" + prefix + seq + "\"";
                ASSERT_GE(before_quote_end.size(), 32u);
                EXPECT_FALSE(jk::accept(before_quote_end)) << "ofs=" << ofs;
                EXPECT_FALSE(oj::accept(before_quote_end)) << "ofs=" << ofs;
            }
            const std::string eof_trunc =
                "{\"padding\":\"01234567890123456789\",\"s\":\"" + prefix + "\xE2\x82";
            ASSERT_GE(eof_trunc.size(), 32u);
            EXPECT_FALSE(jk::accept(eof_trunc)) << "ofs=" << ofs;
            EXPECT_FALSE(oj::accept(eof_trunc)) << "ofs=" << ofs;
        }
    }

    // 3. Indentation slot widths across 0..40 spaces (hitting 16B and 32B boundaries),
    //    plus IndentSlot re-learning on `\n` / `,\n` indent changes & space-then-tab fallback.
    const std::vector<int> indents = {0, 1, 2, 4, 8, 15, 16, 17, 31, 32, 33, 40};
    for (const int ind : indents) {
        const std::string sp(static_cast<std::size_t>(ind), ' ');
        const std::string pretty =
            "{\n" + sp + "\"first\": [\n" + sp + sp + "1,\n" + sp + sp + "2,\n" + sp + sp +
            "\"val_" + std::to_string(ind) + "\"\n" + sp + "],\n" + sp + "\"nested\": {\n" +
            sp + sp + "\"ok\": true,\n" + sp + sp + "\"pad\": \"0123456789abcdef\"\n" + sp +
            "}\n}\n";
        ASSERT_GE(pretty.size(), 32u);
        neojson::test::assert_equivalent(oj::parse(pretty), jk::parse(pretty));
        EXPECT_EQ(jk::parse(pretty).dump(), oj::parse(pretty).dump());
    }
    const std::string relearn_ws =
        "{\n" +
        std::string(17u, ' ') + "\"a\": [\n" + std::string(4u, ' ') + "1,\n" +
        std::string(15u, ' ') + "2,\n" + std::string(33u, ' ') + "3\n" +
        std::string(4u, ' ') + "],\n" +
        std::string(4u, ' ') + "\"b\": 1,\n" +
        std::string(15u, ' ') + "\"c\": 2,\n" +
        std::string(33u, ' ') + "\"d\": 3,\n" +
        "  \t  \"e\": 4,\n" +
        std::string(16u, ' ') + "\"f\": \"abcdef0123456789\"\n" +
        "}\n";
    ASSERT_GE(relearn_ws.size(), 32u);
    neojson::test::assert_equivalent(oj::parse(relearn_ws), jk::parse(relearn_ws));
    EXPECT_EQ(jk::parse(relearn_ws).dump(), oj::parse(relearn_ws).dump());

    const std::string mixed_ws =
        "{\r\n\t \t  \"k1\" :\r\n\t\t[ 1 , 2 ,\r\n\t\t\t\"abcdef0123456789\" ]\r\n}";
    ASSERT_GE(mixed_ws.size(), 32u);
    neojson::test::assert_equivalent(oj::parse(mixed_ws), jk::parse(mixed_ws));
    EXPECT_EQ(jk::parse(mixed_ws).dump(), oj::parse(mixed_ws).dump());
}

} // namespace
