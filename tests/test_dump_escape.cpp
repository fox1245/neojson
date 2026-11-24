// dump of strings against nlohmann/json: every pair and triple of the fragments below, as a string value and as an
// object key, through assert_equivalent (which dumps with ensure_ascii on and off and with each error handler). The
// fragments cover the bytes that are copied as they are, the escapes, every length of a UTF-8 sequence, and sequences
// that are invalid or cut off, so that runs of copied bytes start and end next to each kind.

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "differential.hpp"

namespace {

using neojson::test::assert_equivalent;

const std::vector<std::string>& fragments() {
    static const std::vector<std::string> f = {
        "",
        "a",
        "plain text",
        "\"",
        "\\",
        "/",
        std::string("\0", 1),
        "\x01",
        "\x08",
        "\t",
        "\n",
        "\x0C",
        "\r",
        "\x1F",
        " ",
        "~",
        "\x7F",
        "\xC3\xA9",          // 2 bytes
        "\xE2\x82\xAC",      // 3 bytes
        "\xEF\xBF\xBF",      // 3 bytes, U+FFFF
        "\xF0\x9F\x98\x80",  // 4 bytes
        "\xF4\x8F\xBF\xBF",  // 4 bytes, U+10FFFF
        "\x80",              // a continuation byte alone
        "\xFF",
        "\xC0\xAF",          // overlong
        "\xED\xA0\x80",      // surrogate
        "\xF4\x90\x80\x80",  // above U+10FFFF
        "\xC3",              // cut off
        "\xE2\x82",          // cut off
        "\xF0\x9F\x98",      // cut off
    };
    return f;
}

void check(const std::string& s) {
    {
        nlohmann::ordered_json o = s;
        neojson::json j = s;
        assert_equivalent(o, j);
    }
    {
        nlohmann::ordered_json o = nlohmann::ordered_json::object();
        neojson::json j = neojson::json::object();
        o[s] = 1;
        j[s] = 1;
        assert_equivalent(o, j);
    }
}

TEST(DumpEscape, PairsOfFragments) {
    for (const auto& a : fragments()) {
        for (const auto& b : fragments()) {
            check(a + b);
            if (::testing::Test::HasFailure()) return;
        }
    }
}

TEST(DumpEscape, TriplesOfFragments) {
    for (const auto& a : fragments()) {
        for (const auto& b : fragments()) {
            for (const auto& c : fragments()) {
                check(a + b + c);
                if (::testing::Test::HasFailure()) return;
            }
        }
    }
}

TEST(DumpEscape, RandomBytes) {
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> len(0, 40);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> kind(0, 3);
    for (int n = 0; n < 4000; ++n) {
        std::string s;
        const int l = len(rng);
        for (int i = 0; i < l; ++i) {
            // Mostly printable ASCII, so that long runs of copied bytes occur between the other bytes.
            s.push_back(static_cast<char>(kind(rng) == 0 ? byte(rng) : 0x20 + byte(rng) % 0x5F));
        }
        check(s);
        if (::testing::Test::HasFailure()) return;
    }
}

TEST(DumpEscape, LazyDirectFlatBoundariesAndAllEscapes) {
    auto verify_raw = [](const std::string& raw) {
        const auto oracle = nlohmann::ordered_json::parse(raw);
        const auto candidate = neojson::json::parse(raw);
        EXPECT_EQ(candidate.dump(), oracle.dump());
    };

    // 1. Every control byte 0x00..0x1F, quote, backslash, slash-escape, and multi-byte UTF-8
    // across prefix lengths 0..64 and suffix lengths 0..33 (exercising 32B AVX2, 16B SSE2/NEON, and <16B tails).
    std::vector<std::string> esc_tokens;
    constexpr char kHex[] = "0123456789abcdef";
    for (int b = 0; b < 0x20; ++b) {
        std::string tok = "\\u00";
        tok.push_back(kHex[(b >> 4) & 0xF]);
        tok.push_back(kHex[b & 0xF]);
        esc_tokens.push_back(tok);
    }
    esc_tokens.push_back("\\\"");
    esc_tokens.push_back("\\\\");
    esc_tokens.push_back("\\/"); // decoded to '/' with NOESC cleared
    esc_tokens.push_back("\xC3\xA9");
    esc_tokens.push_back("\xE2\x82\xAC");
    esc_tokens.push_back("\xF0\x9F\x98\x80");

    for (std::size_t tok_i = 0; tok_i < esc_tokens.size(); ++tok_i) {
        for (std::size_t pre = 0; pre <= 64u; ++pre) {
            if (pre > 35u && pre < 62u && tok_i > 3u) continue;
            const std::size_t suf = (tok_i + pre) % 34u;
            const std::string inner = std::string(pre, 'a') + esc_tokens[tok_i] + std::string(suf, 'z');
            verify_raw("{\"" + inner + "\":\"ok\",\"k\":\"" + inner + "\"}");
            verify_raw("[\"" + inner + "\"]");
            if (::testing::Test::HasFailure()) return;
        }
    }
    for (std::size_t suf = 0; suf <= 33u; ++suf) {
        const std::string s0 = std::string(15u, 'p') + "\\u0007" + std::string(suf, 'q');
        const std::string s1 = std::string(31u, 'r') + "\\n" + std::string(suf, 's');
        verify_raw("{\"a\":\"" + s0 + "\",\"" + s1 + "\":\"" + s1 + "\"}");
        if (::testing::Test::HasFailure()) return;
    }

    // 2. Consecutive escape runs placed at SIMD/scalar boundary offsets.
    const std::string run = "\\n\\n\\t\\\"\\\\\\u0000\\u0001\\u001f\\/\\b\\f\\r";
    for (const std::size_t off : {std::size_t{0}, std::size_t{14}, std::size_t{15}, std::size_t{16},
                                  std::size_t{30}, std::size_t{31}, std::size_t{32}, std::size_t{33}}) {
        const std::string inner = std::string(off, 'x') + run + std::string((off + 7u) % 20u, 'y');
        verify_raw("{\"" + inner + "\":\"" + inner + "\"}");
        if (::testing::Test::HasFailure()) return;
    }

    // 3. Long escaped strings crossing stage buffer boundaries (8192 / 16384) and long NOESC strings as final entries.
    for (const std::size_t len : {std::size_t{9000}, std::size_t{18000}}) {
        std::string inner(len, 'm');
        inner.replace(17, 6, "\\u0001");
        inner.replace(8188, 4, "\\\"\\\\");
        if (len > 16395) {
            inner.replace(16380, 6, "\\u001e");
        }
        verify_raw("{\"tail\":\"" + inner + "\"}");
        verify_raw("[\"" + inner + "\"]");
        verify_raw("{\"" + inner + "\":\"" + inner + "\"}");
        if (::testing::Test::HasFailure()) return;
    }
    for (const std::size_t noesc_len : {std::size_t{300}, std::size_t{8200}, std::size_t{17000}}) {
        const std::string plain(noesc_len, 'k');
        verify_raw("{\"" + plain + "\":\"" + plain + "\"}");
        verify_raw("[\"" + plain + "\"]");
        if (::testing::Test::HasFailure()) return;
    }

    // 4. Empty string and pure slash-escape string.
    verify_raw("{\"empty\":\"\",\"slash\":\"\\/\"}");
    verify_raw("[\"\",\"\\/\"]");
}

}  // namespace
