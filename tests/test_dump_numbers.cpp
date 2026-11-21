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
        std::numeric_limits<double>::min(),
        std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::max(),
        -std::numeric_limits<double>::max(),
        std::numeric_limits<double>::epsilon(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),
    };
    for (const double d : special) {
        EXPECT_EQ(oj(d).dump(), jk(d).dump()) << "val=" << d;
    }

    std::uint64_t rng = 0x517CC1B727220A95ULL;
    oj o_batch = oj::array();
    jk s_batch = jk::array();
    for (int i = 0; i < 100000; ++i) {
        const std::uint64_t bits = splitmix64(rng);
        double d = 0.0;
        std::memcpy(&d, &bits, sizeof(d));
        if (i < 5000) {
            ASSERT_EQ(oj(d).dump(), jk(d).dump()) << "i=" << i << " bits=" << bits;
        }
        o_batch.push_back(d);
        s_batch.push_back(d);
    }
    EXPECT_EQ(o_batch.dump(), s_batch.dump());
    EXPECT_EQ(o_batch.dump(2), s_batch.dump(2));
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

} // namespace
