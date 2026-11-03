#include "jsonkit/json.hpp"

#include "yyjson.h"

namespace jsonkit {
namespace detail {
namespace dtoa_impl {

template <typename Target, typename Source>
Target reinterpret_bits(const Source source) {
    static_assert(sizeof(Target) == sizeof(Source), "size mismatch");
    Target target;
    std::memcpy(&target, &source, sizeof(Source));
    return target;
}

struct diyfp {
    static constexpr int kPrecision = 64;

    std::uint64_t f = 0;
    int e = 0;

    constexpr diyfp(std::uint64_t f_, int e_) noexcept : f(f_), e(e_) {}

    static diyfp sub(const diyfp& x, const diyfp& y) noexcept {
        assert(x.e == y.e);
        assert(x.f >= y.f);
        return {x.f - y.f, x.e};
    }

    static diyfp mul(const diyfp& x, const diyfp& y) noexcept {
        static_assert(kPrecision == 64, "internal error");

        const std::uint64_t u_lo = x.f & 0xFFFFFFFFu;
        const std::uint64_t u_hi = x.f >> 32u;
        const std::uint64_t v_lo = y.f & 0xFFFFFFFFu;
        const std::uint64_t v_hi = y.f >> 32u;

        const std::uint64_t p0 = u_lo * v_lo;
        const std::uint64_t p1 = u_lo * v_hi;
        const std::uint64_t p2 = u_hi * v_lo;
        const std::uint64_t p3 = u_hi * v_hi;

        const std::uint64_t p0_hi = p0 >> 32u;
        const std::uint64_t p1_lo = p1 & 0xFFFFFFFFu;
        const std::uint64_t p1_hi = p1 >> 32u;
        const std::uint64_t p2_lo = p2 & 0xFFFFFFFFu;
        const std::uint64_t p2_hi = p2 >> 32u;

        std::uint64_t Q = p0_hi + p1_lo + p2_lo;
        Q += std::uint64_t{1} << (64u - 32u - 1u);

        const std::uint64_t h = p3 + p2_hi + p1_hi + (Q >> 32u);
        return {h, x.e + y.e + 64};
    }

    static diyfp normalize(diyfp x) noexcept {
        assert(x.f != 0);
        while ((x.f >> 63u) == 0) {
            x.f <<= 1u;
            x.e--;
        }
        return x;
    }

    static diyfp normalize_to(const diyfp& x, const int target_exponent) noexcept {
        const int delta = x.e - target_exponent;
        assert(delta >= 0);
        assert(((x.f << delta) >> delta) == x.f);
        return {x.f << delta, target_exponent};
    }
};

struct boundaries {
    diyfp w;
    diyfp minus;
    diyfp plus;
};

template <typename FloatType>
boundaries compute_boundaries(FloatType value) {
    assert(std::isfinite(value));
    assert(value > 0);

    static constexpr int kPrecision = std::numeric_limits<FloatType>::digits;
    static constexpr int kBias =
        std::numeric_limits<FloatType>::max_exponent - 1 + (kPrecision - 1);
    static constexpr int kMinExp = 1 - kBias;
    static constexpr std::uint64_t kHiddenBit = std::uint64_t{1} << (kPrecision - 1);

    using bits_type = std::uint64_t;
    const bits_type bits = reinterpret_bits<bits_type>(value);
    const std::uint64_t e = bits >> (kPrecision - 1);
    const std::uint64_t f = bits & (kHiddenBit - 1);

    const diyfp v = (e == 0) ? diyfp(f, kMinExp) : diyfp(kHiddenBit + f, static_cast<int>(e) - kBias);

    diyfp plus = diyfp::normalize(diyfp(2 * v.f + 1, v.e - 1));
    diyfp minus = (f == 0 && e > 1) ? diyfp(4 * v.f - 1, v.e - 2) : diyfp(2 * v.f - 1, v.e - 1);
    minus = diyfp::normalize_to(minus, plus.e);

    return {diyfp::normalize_to(v, plus.e), minus, plus};
}

struct cached_power {
    std::uint64_t f;
    int e;
    int k;
};

inline cached_power get_cached_power_for_binary_exponent(int e) {
    static constexpr int kCachedPowersMinDecExp = -300;
    static constexpr int kCachedPowersDecStep = 8;

    static constexpr std::array<cached_power, 79> kCachedPowers = {{
        {0xAB70FE17C79AC6CAULL, -1060, -300}, {0xFF77B1FCBEBCDC4FULL, -1034, -292},
        {0xBE5691EF416BD60CULL, -1007, -284}, {0x8DD01FAD907FFC3CULL, -980, -276},
        {0xD3515C2831559A83ULL, -954, -268},  {0x9D71AC8FADA6C9B5ULL, -927, -260},
        {0xEA9C227723EE8BCBULL, -901, -252},  {0xAECC49914078536DULL, -874, -244},
        {0x823C12795DB6CE57ULL, -847, -236},  {0xC21094364DFB5637ULL, -821, -228},
        {0x9096EA6F3848984FULL, -794, -220},  {0xD77485CB25823AC7ULL, -768, -212},
        {0xA086CFCD97BF97F4ULL, -741, -204},  {0xEF340A98172AACE5ULL, -715, -196},
        {0xB23867FB2A35B28EULL, -688, -188},  {0x84C8D4DFD2C63F3BULL, -661, -180},
        {0xC5DD44271AD3CDFAULL, -635, -172},  {0x936B9FCEBB25C996ULL, -608, -164},
        {0xDBAC6C247D62A584ULL, -582, -156},  {0xA3AB66580D5FDAAFULL, -555, -148},
        {0xF3E2F893DEC3F126ULL, -529, -140},  {0xB5B5ADA8AAFF80B8ULL, -502, -132},
        {0x87625F056C7C4A8BULL, -475, -124},  {0xC9BCFF6034C13053ULL, -449, -116},
        {0x964E858C91BA2655ULL, -422, -108},  {0xDFF9772470297EBDULL, -396, -100},
        {0xA6DFBD9FB8E5B88FULL, -369, -92},   {0xF8A95FCF88747D94ULL, -343, -84},
        {0xB94470938FA89BCFULL, -316, -76},   {0x8A08F0F8BF0F156BULL, -289, -68},
        {0xCDB02555653131B6ULL, -263, -60},   {0x993FE2C6D07B7FACULL, -236, -52},
        {0xE45C10C42A2B3B06ULL, -210, -44},   {0xAA242499697392D3ULL, -183, -36},
        {0xFD87B5F28300CA0EULL, -157, -28},   {0xBCE5086492111AEBULL, -130, -20},
        {0x8CBCCC096F5088CCULL, -103, -12},   {0xD1B71758E219652CULL, -77, -4},
        {0x9C40000000000000ULL, -50, 4},      {0xE8D4A51000000000ULL, -24, 12},
        {0xAD78EBC5AC620000ULL, 3, 20},       {0x813F3978F8940984ULL, 30, 28},
        {0xC097CE7BC90715B3ULL, 56, 36},      {0x8F7E32CE7BEA5C70ULL, 83, 44},
        {0xD5D238A4ABE98068ULL, 109, 52},     {0x9F4F2726179A2245ULL, 136, 60},
        {0xED63A231D4C4FB27ULL, 162, 68},     {0xB0DE65388CC8ADA8ULL, 189, 76},
        {0x83C7088E1AAB65DBULL, 216, 84},     {0xC45D1DF942711D9AULL, 242, 92},
        {0x924D692CA61BE758ULL, 269, 100},    {0xDA01EE641A708DEAULL, 295, 108},
        {0xA26DA3999AEF774AULL, 322, 116},    {0xF209787BB47D6B85ULL, 348, 124},
        {0xB454E4A179DD1877ULL, 375, 132},    {0x865B86925B9BC5C2ULL, 402, 140},
        {0xC83553C5C8965D3DULL, 428, 148},    {0x952AB45CFA97A0B3ULL, 455, 156},
        {0xDE469FBD99A05FE3ULL, 481, 164},    {0xA59BC234DB398C25ULL, 508, 172},
        {0xF6C69A72A3989F5CULL, 534, 180},    {0xB7DCFB5354E9BECEULL, 561, 188},
        {0x88FCF317F22241E2ULL, 588, 196},    {0xCC20CE9BD35C78A5ULL, 614, 204},
        {0x98165AF37B2153DFULL, 641, 212},    {0xE2A0B5DC971F303AULL, 667, 220},
        {0xA8D9D1535CE3B396ULL, 694, 228},    {0xFB9B7CD9A4A7443CULL, 720, 236},
        {0xBB764C4CA7A44410ULL, 747, 244},    {0x8BAB8EEFB6409C1AULL, 774, 252},
        {0xD01FEF10A657842CULL, 800, 260},    {0x9B10A4E5E9913129ULL, 827, 268},
        {0xE7109BFBA19C0C9DULL, 853, 276},    {0xAC2820D9623BF429ULL, 880, 284},
        {0x80444B5E7AA7CF85ULL, 907, 292},     {0xBF21E44003ACDD2DULL, 933, 300},
        {0x8E679C2F5E44FF8FULL, 960, 308},    {0xD433179D9C8CB841ULL, 986, 316},
        {0x9E19DB92B4E31BA9ULL, 1013, 324},
    }};

    static constexpr int kAlpha = -60;
    const int f = kAlpha - e - 1;
    const int k = (f * 78913) / (1 << 18) + (f > 0 ? 1 : 0);
    const int index =
        (-kCachedPowersMinDecExp + k + (kCachedPowersDecStep - 1)) / kCachedPowersDecStep;
    assert(index >= 0 && static_cast<std::size_t>(index) < kCachedPowers.size());
    return kCachedPowers[static_cast<std::size_t>(index)];
}

inline int find_largest_pow10(const std::uint32_t n, std::uint32_t& pow10) {
    if (n >= 1000000000) {
        pow10 = 1000000000;
        return 10;
    }
    if (n >= 100000000) {
        pow10 = 100000000;
        return 9;
    }
    if (n >= 10000000) {
        pow10 = 10000000;
        return 8;
    }
    if (n >= 1000000) {
        pow10 = 1000000;
        return 7;
    }
    if (n >= 100000) {
        pow10 = 100000;
        return 6;
    }
    if (n >= 10000) {
        pow10 = 10000;
        return 5;
    }
    if (n >= 1000) {
        pow10 = 1000;
        return 4;
    }
    if (n >= 100) {
        pow10 = 100;
        return 3;
    }
    if (n >= 10) {
        pow10 = 10;
        return 2;
    }
    pow10 = 1;
    return 1;
}

inline void grisu2_round(char* buf, int len, std::uint64_t dist, std::uint64_t delta,
                         std::uint64_t rest, std::uint64_t ten_k) {
    assert(len >= 1);
    assert(dist <= delta);
    assert(ten_k > 0);

    while (rest < dist && delta - rest >= ten_k &&
           (rest + ten_k < dist || dist - rest > rest + ten_k - dist)) {
        assert(buf[len - 1] != '0');
        buf[len - 1]--;
        rest += ten_k;
    }
}

inline void grisu2_digit_gen(char* buffer, int& length, int& decimal_exponent, diyfp M_minus,
                             diyfp w, diyfp M_plus) {
    static constexpr int kAlpha = -60;
    static constexpr int kGamma = -32;
    (void)kAlpha;
    (void)kGamma;

    assert(M_plus.e >= kAlpha);
    assert(M_plus.e <= kGamma);
    assert(M_minus.e == M_plus.e);
    assert(w.e == M_plus.e);

    std::uint64_t delta = diyfp::sub(M_plus, M_minus).f;
    std::uint64_t dist = diyfp::sub(M_plus, w).f;

    const diyfp one(std::uint64_t{1} << -M_plus.e, M_plus.e);
    auto p1 = static_cast<std::uint32_t>(M_plus.f >> -one.e);
    std::uint64_t p2 = M_plus.f & (one.f - 1);

    std::uint32_t pow10 = 0;
    const int k = find_largest_pow10(p1, pow10);

    int n = k;
    while (n > 0) {
        const std::uint32_t d = p1 / pow10;
        const std::uint32_t r = p1 % pow10;

        assert(d <= 9);
        buffer[length++] = static_cast<char>('0' + d);

        const std::uint64_t rest = (static_cast<std::uint64_t>(r) << -one.e) + p2;
        if (rest <= delta) {
            decimal_exponent += n - 1;
            grisu2_round(buffer, length, dist, delta, rest,
                         static_cast<std::uint64_t>(pow10) << -one.e);
            return;
        }

        p1 = r;
        pow10 /= 10;
        --n;
    }

    assert(p1 == 0);
    int m = 0;
    for (;;) {
        assert(p2 <= (std::numeric_limits<std::uint64_t>::max)() / 10);
        p2 *= 10;
        const auto d = static_cast<std::uint32_t>(p2 >> -one.e);
        const std::uint64_t r = p2 & (one.f - 1);

        assert(d <= 9);
        buffer[length++] = static_cast<char>('0' + d);

        p2 = r;
        --m;
        delta *= 10;
        dist *= 10;
        if (p2 <= delta) {
            decimal_exponent += m;
            grisu2_round(buffer, length, dist, delta, p2, one.f);
            return;
        }
    }
}

inline void grisu2(char* buf, int& len, int& decimal_exponent, diyfp m_minus, diyfp v,
                   diyfp m_plus) {
    assert(m_plus.e == m_minus.e);
    assert(m_plus.e == v.e);

    const cached_power cached = get_cached_power_for_binary_exponent(m_plus.e);
    const diyfp c_minus_k(cached.f, cached.e);

    const diyfp w = diyfp::mul(v, c_minus_k);
    diyfp w_minus = diyfp::mul(m_minus, c_minus_k);
    diyfp w_plus = diyfp::mul(m_plus, c_minus_k);

    w_minus.f++;
    w_plus.f--;

    decimal_exponent = -cached.k;
    grisu2_digit_gen(buf, len, decimal_exponent, w_minus, w, w_plus);
}

template <typename FloatType>
void grisu2(char* buf, int& len, int& decimal_exponent, FloatType value) {
    const boundaries w = compute_boundaries(static_cast<double>(value));
    grisu2(buf, len, decimal_exponent, w.minus, w.w, w.plus);
}

inline char* append_exponent(char* buf, int e) {
    assert(e > -1000);
    assert(e < 1000);

    if (e < 0) {
        e = -e;
        *buf++ = '-';
    } else {
        *buf++ = '+';
    }

    auto k = static_cast<std::uint32_t>(e);
    if (k < 10) {
        *buf++ = '0';
        *buf++ = static_cast<char>('0' + k);
    } else if (k < 100) {
        *buf++ = static_cast<char>('0' + k / 10);
        k %= 10;
        *buf++ = static_cast<char>('0' + k);
    } else {
        *buf++ = static_cast<char>('0' + k / 100);
        k %= 100;
        *buf++ = static_cast<char>('0' + k / 10);
        k %= 10;
        *buf++ = static_cast<char>('0' + k);
    }

    return buf;
}

inline char* format_buffer(char* buf, int len, int decimal_exponent, int min_exp, int max_exp) {
    assert(min_exp < 0);
    assert(max_exp > 0);

    const int k = decimal_exponent;
    const int n = len + k;

    if (k >= 0 && n <= max_exp) {
        std::fill_n(buf + len, k, '0');
        buf[n] = '.';
        buf[n + 1] = '0';
        return buf + (static_cast<std::size_t>(n) + 2);
    }

    if (0 < k && k < 0) {
        // unreachable
    }

    if (0 < n && n <= max_exp) {
        assert(k < 0);
        std::memmove(buf + (static_cast<std::size_t>(n) + 1), buf + n,
                     static_cast<std::size_t>(len - n));
        buf[n] = '.';
        return buf + (static_cast<std::size_t>(len) + 1U);
    }

    if (min_exp < n && n <= 0) {
        std::memmove(buf + (2 + static_cast<std::size_t>(-n)), buf, static_cast<std::size_t>(len));
        buf[0] = '0';
        buf[1] = '.';
        std::fill_n(buf + 2, -n, '0');
        return buf + (2U + static_cast<std::size_t>(-n) + static_cast<std::size_t>(len));
    }

    if (len > 1) {
        std::memmove(buf + 2, buf + 1, static_cast<std::size_t>(len - 1));
        buf[1] = '.';
        buf += len + 1;
    } else {
        buf += 1;
    }

    *buf++ = 'e';
    return append_exponent(buf, n - 1);
}

inline void append_float(std::string& out, double val) {
    if (std::isnan(val) || std::isinf(val)) {
        out.append("null", 4);
        return;
    }

    const bool is_negative = std::signbit(val);
    if (is_negative) {
        val = -val;
    }

    if (val == 0.0) {
        if (is_negative) {
            out.append("-0.0", 4);
        } else {
            out.append("0.0", 3);
        }
        return;
    }

    std::array<char, 64> number_buffer{{}};
    char* p = number_buffer.data();
    if (is_negative) {
        *p++ = '-';
    }

    int length = 0;
    int decimal_exponent = 0;
    grisu2(p, length, decimal_exponent, val);

    constexpr int kMinExp = -4;
    constexpr int kMaxExp = std::numeric_limits<double>::digits10;
    char* end = format_buffer(p, length, decimal_exponent, kMinExp, kMaxExp);
    out.append(number_buffer.data(), static_cast<std::size_t>(end - number_buffer.data()));
}

} // namespace dtoa_impl

namespace utf8_impl {

static constexpr std::uint8_t UTF8_ACCEPT = 0;
static constexpr std::uint8_t UTF8_REJECT = 1;

inline std::uint8_t decode(std::uint8_t& state, std::uint32_t& codep,
                           const std::uint8_t byte) noexcept {
    static const std::array<std::uint8_t, 400> utf8d = {{
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 00..1f
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 20..3f
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 40..5f
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 60..7f
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, // 80..9f
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, // a0..bf
        8, 8, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // c0..df
        0xA, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x4, 0x3, 0x3,                 // e0..ef
        0xB, 0x6, 0x6, 0x6, 0x5, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8,                 // f0..ff
        0x0, 0x1, 0x2, 0x3, 0x5, 0x8, 0x7, 0x1, 0x1, 0x1, 0x4, 0x6, 0x1, 0x1, 0x1, 0x1,                 // s0..s0
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, // s1..s2
        1, 2, 1, 1, 1, 1, 1, 2, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, // s3..s4
        1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 1, 3, 1, 1, 1, 1, 1, 1, // s5..s6
        1, 3, 1, 1, 1, 1, 1, 3, 1, 3, 1, 1, 1, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1  // s7..s8
    }};

    const std::uint8_t type = utf8d[byte];
    codep = (state != UTF8_ACCEPT)
                ? (byte & 0x3Fu) | (codep << 6u)
                : (0xFFu >> type) & (byte);

    std::size_t index = 256u + static_cast<std::size_t>(state) * 16u + static_cast<std::size_t>(type);
    state = utf8d[index];
    return state;
}

inline void append_u4(std::string& out, std::uint16_t cp) {
    static constexpr char kHex[] = "0123456789abcdef";
    char buf[6] = {'\\', 'u',
                   kHex[(cp >> 12u) & 0x0Fu],
                   kHex[(cp >> 8u) & 0x0Fu],
                   kHex[(cp >> 4u) & 0x0Fu],
                   kHex[cp & 0x0Fu]};
    out.append(buf, 6);
}

inline void throw_invalid_utf8(std::size_t idx, std::uint8_t byte) {
    static constexpr char kHexUpper[] = "0123456789ABCDEF";
    char hex[5] = {'0', 'x', kHexUpper[byte >> 4u], kHexUpper[byte & 0x0Fu], '\0'};
    throw type_error::create(
        316, "invalid UTF-8 byte at index " + std::to_string(idx) + ": " + hex);
}

inline void throw_incomplete_utf8(std::uint8_t byte) {
    static constexpr char kHexUpper[] = "0123456789ABCDEF";
    char hex[5] = {'0', 'x', kHexUpper[byte >> 4u], kHexUpper[byte & 0x0Fu], '\0'};
    throw type_error::create(316, std::string("incomplete UTF-8 string; last byte: ") + hex);
}

inline void dump_escaped(std::string& out, const std::string& s, const bool ensure_ascii,
                         const error_handler_t error_handler) {
    std::uint32_t codepoint = 0;
    std::uint8_t state = UTF8_ACCEPT;
    std::size_t undumped_chars = 0;

    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto byte = static_cast<std::uint8_t>(s[i]);

        switch (decode(state, codepoint, byte)) {
            case UTF8_ACCEPT: {
                switch (codepoint) {
                    case 0x08:
                        out.append("\\b", 2);
                        break;
                    case 0x09:
                        out.append("\\t", 2);
                        break;
                    case 0x0A:
                        out.append("\\n", 2);
                        break;
                    case 0x0C:
                        out.append("\\f", 2);
                        break;
                    case 0x0D:
                        out.append("\\r", 2);
                        break;
                    case 0x22:
                        out.append("\\\"", 2);
                        break;
                    case 0x5C:
                        out.append("\\\\", 2);
                        break;
                    default: {
                        if (codepoint <= 0x1F || (ensure_ascii && codepoint >= 0x7F)) {
                            if (codepoint <= 0xFFFF) {
                                append_u4(out, static_cast<std::uint16_t>(codepoint));
                            } else {
                                append_u4(out, static_cast<std::uint16_t>(0xD7C0u + (codepoint >> 10u)));
                                append_u4(out, static_cast<std::uint16_t>(0xDC00u + (codepoint & 0x3FFu)));
                            }
                        } else {
                            out.append(&s[i - undumped_chars], undumped_chars + 1);
                        }
                        break;
                    }
                }
                undumped_chars = 0;
                break;
            }

            case UTF8_REJECT: {
                switch (error_handler) {
                    case error_handler_t::strict:
                        throw_invalid_utf8(i, byte);
                        break;
                    case error_handler_t::ignore:
                    case error_handler_t::replace: {
                        if (undumped_chars > 0) {
                            --i;
                        }
                        undumped_chars = 0;
                        state = UTF8_ACCEPT;

                        if (error_handler == error_handler_t::replace) {
                            if (ensure_ascii) {
                                out.append("\\ufffd", 6);
                            } else {
                                out.append("\xEF\xBF\xBD", 3);
                            }
                        }
                        break;
                    }
                }
                break;
            }

            default: {
                ++undumped_chars;
                break;
            }
        }
    }

    if (state != UTF8_ACCEPT) {
        switch (error_handler) {
            case error_handler_t::strict:
                if (!s.empty()) {
                    throw_incomplete_utf8(static_cast<std::uint8_t>(s.back()));
                } else {
                    throw_incomplete_utf8(0);
                }
                break;
            case error_handler_t::ignore:
                break;
            case error_handler_t::replace:
                if (ensure_ascii) {
                    out.append("\\ufffd", 6);
                } else {
                    out.append("\xEF\xBF\xBD", 3);
                }
                break;
        }
    }
}

} // namespace utf8_impl

namespace {

void append_uint(std::string& out, std::uint64_t v) {
    char buf[24];
    char* p = buf + sizeof(buf);
    do {
        *--p = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    out.append(p, static_cast<std::size_t>((buf + sizeof(buf)) - p));
}

void append_int(std::string& out, std::int64_t v) {
    if (v < 0) {
        out.push_back('-');
        const auto u = static_cast<std::uint64_t>(-(v + 1)) + 1u;
        append_uint(out, u);
    } else {
        append_uint(out, static_cast<std::uint64_t>(v));
    }
}

void dump_value(std::string& out, const json& val, const bool pretty_print,
                const bool ensure_ascii, const error_handler_t error_handler,
                const unsigned int indent_step, std::string& indent_string,
                const unsigned int current_indent) {
    switch (val.type()) {
        case value_t::null:
            out.append("null", 4);
            return;

        case value_t::boolean:
            if (val.get<bool>()) {
                out.append("true", 4);
            } else {
                out.append("false", 5);
            }
            return;

        case value_t::number_integer:
            append_int(out, *val.get_ptr<const json::number_integer_t*>());
            return;

        case value_t::number_unsigned:
            append_uint(out, *val.get_ptr<const json::number_unsigned_t*>());
            return;

        case value_t::number_float:
            dtoa_impl::append_float(out, *val.get_ptr<const json::number_float_t*>());
            return;

        case value_t::string:
            out.push_back('"');
            utf8_impl::dump_escaped(
                out, *val.get_ptr<const json::string_t*>(), ensure_ascii, error_handler);
            out.push_back('"');
            return;

        case value_t::array: {
            const auto* arr = val.get_ptr<const json::array_t*>();
            if (arr->empty()) {
                out.append("[]", 2);
                return;
            }

            if (pretty_print) {
                out.append("[\n", 2);
                const unsigned int new_indent = current_indent + indent_step;
                if (indent_string.size() < new_indent) {
                    indent_string.resize(indent_string.size() * 2, ' ');
                }
                for (std::size_t i = 0; i < arr->size(); ++i) {
                    out.append(indent_string.data(), new_indent);
                    dump_value(out, (*arr)[i], true, ensure_ascii, error_handler, indent_step,
                               indent_string, new_indent);
                    if (i + 1 < arr->size()) {
                        out.append(",\n", 2);
                    } else {
                        out.push_back('\n');
                    }
                }
                out.append(indent_string.data(), current_indent);
                out.push_back(']');
            } else {
                out.push_back('[');
                for (std::size_t i = 0; i < arr->size(); ++i) {
                    if (i > 0) {
                        out.push_back(',');
                    }
                    dump_value(out, (*arr)[i], false, ensure_ascii, error_handler, indent_step,
                               indent_string, current_indent);
                }
                out.push_back(']');
            }
            return;
        }

        case value_t::object: {
            const auto* obj = val.get_ptr<const json::object_t*>();
            if (obj->empty()) {
                out.append("{}", 2);
                return;
            }

            if (pretty_print) {
                out.append("{\n", 2);
                const unsigned int new_indent = current_indent + indent_step;
                if (indent_string.size() < new_indent) {
                    indent_string.resize(indent_string.size() * 2, ' ');
                }
                for (std::size_t i = 0; i < obj->size(); ++i) {
                    out.append(indent_string.data(), new_indent);
                    out.push_back('"');
                    utf8_impl::dump_escaped(out, (*obj)[i].first, ensure_ascii, error_handler);
                    out.append("\": ", 3);
                    dump_value(out, (*obj)[i].second, true, ensure_ascii, error_handler,
                               indent_step, indent_string, new_indent);
                    if (i + 1 < obj->size()) {
                        out.append(",\n", 2);
                    } else {
                        out.push_back('\n');
                    }
                }
                out.append(indent_string.data(), current_indent);
                out.push_back('}');
            } else {
                out.push_back('{');
                for (std::size_t i = 0; i < obj->size(); ++i) {
                    if (i > 0) {
                        out.push_back(',');
                    }
                    out.push_back('"');
                    utf8_impl::dump_escaped(out, (*obj)[i].first, ensure_ascii, error_handler);
                    out.append("\":", 2);
                    dump_value(out, (*obj)[i].second, false, ensure_ascii, error_handler,
                               indent_step, indent_string, current_indent);
                }
                out.push_back('}');
            }
            return;
        }

        case value_t::discarded:
            out.append("<discarded>", 11);
            return;

        case value_t::binary:
        default:
            return;
    }
}

json materialize_yyjson(yyjson_val* val) {
    switch (yyjson_get_tag(val)) {
        case YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE:
            return json(nullptr);

        case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE:
            return json(true);

        case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE:
            return json(false);

        case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT:
            return json(static_cast<json::number_unsigned_t>(unsafe_yyjson_get_uint(val)));

        case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT:
            return json(static_cast<json::number_integer_t>(unsafe_yyjson_get_sint(val)));

        case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL:
            return json(static_cast<json::number_float_t>(unsafe_yyjson_get_real(val)));

        case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE:
        case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
            return json(json::string_t(unsafe_yyjson_get_str(val), unsafe_yyjson_get_len(val)));

        case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE: {
            json res = json::value_t::array;
            auto* arr = res.get_ptr<json::array_t*>();
            const std::size_t len = unsafe_yyjson_get_len(val);
            arr->reserve(len);
            yyjson_arr_iter iter = yyjson_arr_iter_with(val);
            yyjson_val* elem = nullptr;
            while ((elem = yyjson_arr_iter_next(&iter)) != nullptr) {
                arr->push_back(materialize_yyjson(elem));
            }
            return res;
        }

        case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE: {
            json res = json::value_t::object;
            auto* obj = res.get_ptr<json::object_t*>();
            const std::size_t len = unsafe_yyjson_get_len(val);
            obj->reserve(len);
            yyjson_obj_iter iter = yyjson_obj_iter_with(val);
            yyjson_val* key = nullptr;
            while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
                yyjson_val* v = yyjson_obj_iter_get_val(key);
                std::string_view k_sv(unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
                (*obj)[k_sv] = materialize_yyjson(v);
            }
            return res;
        }

        default:
            return json(nullptr);
    }
}

} // namespace
} // namespace detail

json::string_t json::dump(const int indent, const char indent_char, const bool ensure_ascii,
                          const error_handler_t error_handler) const {
    string_t out;
    out.reserve(detail::kDumpInitialBufferCapacity);
    std::string indent_string(512, indent_char);
    if (indent >= 0) {
        detail::dump_value(out, *this, true, ensure_ascii, error_handler,
                           static_cast<unsigned int>(indent), indent_string, 0);
    } else {
        detail::dump_value(out, *this, false, ensure_ascii, error_handler, 0, indent_string, 0);
    }
    return out;
}

std::ostream& operator<<(std::ostream& o, const json& j) {
    const bool pretty_print = (o.width() > 0);
    const auto indentation = (pretty_print ? o.width() : 0);
    o.width(0);
    const char fill_char = o.fill();
    o << j.dump(pretty_print ? static_cast<int>(indentation) : -1, fill_char);
    return o;
}

json json::parse(std::string_view s, parser_callback_t /*cb*/, const bool allow_exceptions,
                 const bool ignore_comments) {
    const auto nul_pos = s.find('\0');
    if (nul_pos != std::string_view::npos) {
        s = s.substr(0, nul_pos);
    }

    yyjson_read_flag flg = YYJSON_READ_ALLOW_BOM;
    if (ignore_comments) {
        flg |= YYJSON_READ_ALLOW_COMMENTS;
    }

    yyjson_read_err err{};
    // yyjson_read_opts does not mutate the input buffer when YYJSON_READ_INSITU is not set.
    yyjson_doc* doc =
        yyjson_read_opts(const_cast<char*>(s.data()), s.size(), flg, nullptr, &err);
    if (doc == nullptr) {
        if (!allow_exceptions) {
            return json(value_t::discarded);
        }
        if (err.code == YYJSON_READ_ERROR_INVALID_NUMBER && err.msg != nullptr &&
            std::string_view(err.msg) == "number is infinity when parsed as double") {
            std::size_t idx = err.pos;
            while (idx < s.size()) {
                const char c = s[idx];
                if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' ||
                    c == '-') {
                    ++idx;
                } else {
                    break;
                }
            }
            const std::string token(s.substr(err.pos, idx - err.pos));
            throw out_of_range::create(406, "number overflow parsing '" + token + "'");
        }
        const std::size_t byte_pos = err.pos + 1;
        std::size_t line = 1;
        std::size_t col = 0;
        const std::size_t limit = std::min(err.pos, s.size());
        for (std::size_t idx = 0; idx < limit; ++idx) {
            if (s[idx] == '\n') {
                ++line;
                col = 0;
            } else {
                ++col;
            }
        }
        ++col;
        const char* msg = err.msg != nullptr ? err.msg : "syntax error";
        throw parse_error::create(101, byte_pos, line, col, msg);
    }

    struct DocGuard {
        yyjson_doc* d;
        ~DocGuard() { yyjson_doc_free(d); }
    } guard{doc};

    yyjson_val* root = yyjson_doc_get_root(doc);
    return detail::materialize_yyjson(root);
}

json json::parse(std::istream& i, parser_callback_t cb, const bool allow_exceptions,
                 const bool ignore_comments) {
    std::string buf((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
    return parse(std::string_view(buf), cb, allow_exceptions, ignore_comments);
}

bool json::accept(std::string_view s, const bool ignore_comments) {
    const auto nul_pos = s.find('\0');
    if (nul_pos != std::string_view::npos) {
        s = s.substr(0, nul_pos);
    }

    yyjson_read_flag flg = YYJSON_READ_ALLOW_BOM;
    if (ignore_comments) {
        flg |= YYJSON_READ_ALLOW_COMMENTS;
    }

    yyjson_read_err err{};
    yyjson_doc* doc =
        yyjson_read_opts(const_cast<char*>(s.data()), s.size(), flg, nullptr, &err);
    if (doc == nullptr) {
        return false;
    }
    yyjson_doc_free(doc);
    return true;
}

bool json::accept(std::istream& i, const bool ignore_comments) {
    std::string buf((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
    return accept(std::string_view(buf), ignore_comments);
}

} // namespace jsonkit
