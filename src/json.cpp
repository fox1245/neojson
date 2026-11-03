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

struct diag_position_t {
    std::size_t chars_read_total = 0;
    std::size_t chars_read_current_line = 0;
    std::size_t lines_read = 0;
};

enum class diag_token_type {
    uninitialized,
    literal_true,
    literal_false,
    literal_null,
    value_string,
    value_unsigned,
    value_integer,
    value_float,
    begin_array,
    begin_object,
    end_array,
    end_object,
    name_separator,
    value_separator,
    parse_error,
    end_of_input,
    literal_or_value
};

const char* diag_token_type_name(const diag_token_type t) noexcept {
    switch (t) {
        case diag_token_type::uninitialized:
            return "<uninitialized>";
        case diag_token_type::literal_true:
            return "true literal";
        case diag_token_type::literal_false:
            return "false literal";
        case diag_token_type::literal_null:
            return "null literal";
        case diag_token_type::value_string:
            return "string literal";
        case diag_token_type::value_unsigned:
        case diag_token_type::value_integer:
        case diag_token_type::value_float:
            return "number literal";
        case diag_token_type::begin_array:
            return "'['";
        case diag_token_type::begin_object:
            return "'{'";
        case diag_token_type::end_array:
            return "']'";
        case diag_token_type::end_object:
            return "'}'";
        case diag_token_type::name_separator:
            return "':'";
        case diag_token_type::value_separator:
            return "','";
        case diag_token_type::parse_error:
            return "<parse error>";
        case diag_token_type::end_of_input:
            return "end of input";
        case diag_token_type::literal_or_value:
            return "'[', '{', or a literal";
        default:
            return "unknown token";
    }
}

class diagnostic_lexer {
public:
    using char_type = char;
    using char_int_type = std::char_traits<char>::int_type;

    diagnostic_lexer(std::string_view input, bool ignore_comments) noexcept
        : input_(input), ignore_comments_(ignore_comments) {}

    constexpr json::number_float_t get_number_float() const noexcept {
        return value_float_;
    }

    constexpr diag_position_t get_position() const noexcept {
        return position_;
    }

    constexpr std::size_t accepted_prefix_size() const noexcept {
        return current_ == '\0' ? cursor_ - 1 : cursor_;
    }

    std::string get_token_string() const {
        std::string result;
        for (const auto c : token_string_) {
            if (static_cast<unsigned char>(c) <= '\x1F') {
                std::array<char, 9> cs{{}};
                static_cast<void>(std::snprintf(cs.data(), cs.size(), "<U+%.4X>",
                                                static_cast<unsigned char>(c)));
                result += cs.data();
            } else {
                result.push_back(c);
            }
        }
        return result;
    }

    constexpr const char* get_error_message() const noexcept {
        return error_message_;
    }

    diag_token_type scan() {
        if (position_.chars_read_total == 0 && !skip_bom()) {
            error_message_ = "invalid BOM; must be 0xEF 0xBB 0xBF if given";
            return diag_token_type::parse_error;
        }

        skip_whitespace();

        while (ignore_comments_ && current_ == '/') {
            if (!scan_comment()) {
                return diag_token_type::parse_error;
            }
            skip_whitespace();
        }

        switch (current_) {
            case '[':
                return diag_token_type::begin_array;
            case ']':
                return diag_token_type::end_array;
            case '{':
                return diag_token_type::begin_object;
            case '}':
                return diag_token_type::end_object;
            case ':':
                return diag_token_type::name_separator;
            case ',':
                return diag_token_type::value_separator;

            case 't':
                return scan_literal("true", 4, diag_token_type::literal_true);
            case 'f':
                return scan_literal("false", 5, diag_token_type::literal_false);
            case 'n':
                return scan_literal("null", 4, diag_token_type::literal_null);

            case '\"':
                return scan_string();

            case '-':
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                return scan_number();

            case '\0':
            case std::char_traits<char>::eof():
                return diag_token_type::end_of_input;

            default:
                error_message_ = "invalid literal";
                return diag_token_type::parse_error;
        }
    }

private:
    char_int_type get_character() noexcept {
        if (cursor_ < input_.size()) {
            return std::char_traits<char>::to_int_type(input_[cursor_++]);
        }
        return std::char_traits<char>::eof();
    }

    char_int_type get() {
        ++position_.chars_read_total;
        ++position_.chars_read_current_line;

        if (next_unget_) {
            next_unget_ = false;
        } else {
            current_ = get_character();
        }

        if (current_ != std::char_traits<char>::eof()) {
            token_string_.push_back(std::char_traits<char>::to_char_type(current_));
        }

        if (current_ == '\n') {
            ++position_.lines_read;
            position_.chars_read_current_line = 0;
        }

        return current_;
    }

    void unget() {
        next_unget_ = true;
        --position_.chars_read_total;

        if (position_.chars_read_current_line == 0) {
            if (position_.lines_read > 0) {
                --position_.lines_read;
            }
        } else {
            --position_.chars_read_current_line;
        }

        if (current_ != std::char_traits<char>::eof()) {
            assert(!token_string_.empty());
            token_string_.pop_back();
        }
    }

    void reset() noexcept {
        token_buffer_.clear();
        token_string_.clear();
        token_string_.push_back(std::char_traits<char>::to_char_type(current_));
    }

    void add(char_int_type c) {
        token_buffer_.push_back(static_cast<char>(c));
    }

    bool skip_bom() {
        if (get() == 0xEF) {
            return get() == 0xBB && get() == 0xBF;
        }
        unget();
        return true;
    }

    void skip_whitespace() {
        do {
            get();
        } while (current_ == ' ' || current_ == '\t' || current_ == '\n' || current_ == '\r');
    }

    int get_codepoint() {
        assert(current_ == 'u');
        int codepoint = 0;

        for (const auto factor : {12u, 8u, 4u, 0u}) {
            get();
            if (current_ >= '0' && current_ <= '9') {
                codepoint += static_cast<int>((static_cast<unsigned int>(current_) - 0x30u)
                                              << factor);
            } else if (current_ >= 'A' && current_ <= 'F') {
                codepoint += static_cast<int>((static_cast<unsigned int>(current_) - 0x37u)
                                              << factor);
            } else if (current_ >= 'a' && current_ <= 'f') {
                codepoint += static_cast<int>((static_cast<unsigned int>(current_) - 0x57u)
                                              << factor);
            } else {
                return -1;
            }
        }
        return codepoint;
    }

    bool next_byte_in_range(std::initializer_list<char_int_type> ranges) {
        for (auto range = ranges.begin(); range != ranges.end(); ++range) {
            get();
            if (*range <= current_ && current_ <= *(++range)) {
                // valid continuation byte
            } else {
                error_message_ = "invalid string: ill-formed UTF-8 byte";
                return false;
            }
        }
        return true;
    }

    diag_token_type scan_string() {
        reset();
        assert(current_ == '\"');

        while (true) {
            switch (get()) {
                case std::char_traits<char>::eof():
                    error_message_ = "invalid string: missing closing quote";
                    return diag_token_type::parse_error;

                case '\"':
                    return diag_token_type::value_string;

                case '\\': {
                    switch (get()) {
                        case '\"':
                        case '\\':
                        case '/':
                        case 'b':
                        case 'f':
                        case 'n':
                        case 'r':
                        case 't':
                            break;

                        case 'u': {
                            const int codepoint1 = get_codepoint();
                            if (codepoint1 == -1) {
                                error_message_ =
                                    "invalid string: '\\u' must be followed by 4 hex digits";
                                return diag_token_type::parse_error;
                            }

                            if (0xD800 <= codepoint1 && codepoint1 <= 0xDBFF) {
                                if (get() == '\\' && get() == 'u') {
                                    const int codepoint2 = get_codepoint();
                                    if (codepoint2 == -1) {
                                        error_message_ =
                                            "invalid string: '\\u' must be followed by 4 hex digits";
                                        return diag_token_type::parse_error;
                                    }
                                    if (!(0xDC00 <= codepoint2 && codepoint2 <= 0xDFFF)) {
                                        error_message_ =
                                            "invalid string: surrogate U+D800..U+DBFF must be "
                                            "followed by U+DC00..U+DFFF";
                                        return diag_token_type::parse_error;
                                    }
                                } else {
                                    error_message_ =
                                        "invalid string: surrogate U+D800..U+DBFF must be "
                                        "followed by U+DC00..U+DFFF";
                                    return diag_token_type::parse_error;
                                }
                            } else if (0xDC00 <= codepoint1 && codepoint1 <= 0xDFFF) {
                                error_message_ =
                                    "invalid string: surrogate U+DC00..U+DFFF must follow "
                                    "U+D800..U+DBFF";
                                return diag_token_type::parse_error;
                            }
                            break;
                        }

                        default:
                            error_message_ = "invalid string: forbidden character after backslash";
                            return diag_token_type::parse_error;
                    }
                    break;
                }

                case 0x00:
                    error_message_ =
                        "invalid string: control character U+0000 (NUL) must be escaped to \\u0000";
                    return diag_token_type::parse_error;
                case 0x01:
                    error_message_ =
                        "invalid string: control character U+0001 (SOH) must be escaped to \\u0001";
                    return diag_token_type::parse_error;
                case 0x02:
                    error_message_ =
                        "invalid string: control character U+0002 (STX) must be escaped to \\u0002";
                    return diag_token_type::parse_error;
                case 0x03:
                    error_message_ =
                        "invalid string: control character U+0003 (ETX) must be escaped to \\u0003";
                    return diag_token_type::parse_error;
                case 0x04:
                    error_message_ =
                        "invalid string: control character U+0004 (EOT) must be escaped to \\u0004";
                    return diag_token_type::parse_error;
                case 0x05:
                    error_message_ =
                        "invalid string: control character U+0005 (ENQ) must be escaped to \\u0005";
                    return diag_token_type::parse_error;
                case 0x06:
                    error_message_ =
                        "invalid string: control character U+0006 (ACK) must be escaped to \\u0006";
                    return diag_token_type::parse_error;
                case 0x07:
                    error_message_ =
                        "invalid string: control character U+0007 (BEL) must be escaped to \\u0007";
                    return diag_token_type::parse_error;
                case 0x08:
                    error_message_ =
                        "invalid string: control character U+0008 (BS) must be escaped to \\u0008 "
                        "or \\b";
                    return diag_token_type::parse_error;
                case 0x09:
                    error_message_ =
                        "invalid string: control character U+0009 (HT) must be escaped to \\u0009 "
                        "or \\t";
                    return diag_token_type::parse_error;
                case 0x0A:
                    error_message_ =
                        "invalid string: control character U+000A (LF) must be escaped to \\u000A "
                        "or \\n";
                    return diag_token_type::parse_error;
                case 0x0B:
                    error_message_ =
                        "invalid string: control character U+000B (VT) must be escaped to \\u000B";
                    return diag_token_type::parse_error;
                case 0x0C:
                    error_message_ =
                        "invalid string: control character U+000C (FF) must be escaped to \\u000C "
                        "or \\f";
                    return diag_token_type::parse_error;
                case 0x0D:
                    error_message_ =
                        "invalid string: control character U+000D (CR) must be escaped to \\u000D "
                        "or \\r";
                    return diag_token_type::parse_error;
                case 0x0E:
                    error_message_ =
                        "invalid string: control character U+000E (SO) must be escaped to \\u000E";
                    return diag_token_type::parse_error;
                case 0x0F:
                    error_message_ =
                        "invalid string: control character U+000F (SI) must be escaped to \\u000F";
                    return diag_token_type::parse_error;
                case 0x10:
                    error_message_ =
                        "invalid string: control character U+0010 (DLE) must be escaped to \\u0010";
                    return diag_token_type::parse_error;
                case 0x11:
                    error_message_ =
                        "invalid string: control character U+0011 (DC1) must be escaped to \\u0011";
                    return diag_token_type::parse_error;
                case 0x12:
                    error_message_ =
                        "invalid string: control character U+0012 (DC2) must be escaped to \\u0012";
                    return diag_token_type::parse_error;
                case 0x13:
                    error_message_ =
                        "invalid string: control character U+0013 (DC3) must be escaped to \\u0013";
                    return diag_token_type::parse_error;
                case 0x14:
                    error_message_ =
                        "invalid string: control character U+0014 (DC4) must be escaped to \\u0014";
                    return diag_token_type::parse_error;
                case 0x15:
                    error_message_ =
                        "invalid string: control character U+0015 (NAK) must be escaped to \\u0015";
                    return diag_token_type::parse_error;
                case 0x16:
                    error_message_ =
                        "invalid string: control character U+0016 (SYN) must be escaped to \\u0016";
                    return diag_token_type::parse_error;
                case 0x17:
                    error_message_ =
                        "invalid string: control character U+0017 (ETB) must be escaped to \\u0017";
                    return diag_token_type::parse_error;
                case 0x18:
                    error_message_ =
                        "invalid string: control character U+0018 (CAN) must be escaped to \\u0018";
                    return diag_token_type::parse_error;
                case 0x19:
                    error_message_ =
                        "invalid string: control character U+0019 (EM) must be escaped to \\u0019";
                    return diag_token_type::parse_error;
                case 0x1A:
                    error_message_ =
                        "invalid string: control character U+001A (SUB) must be escaped to \\u001A";
                    return diag_token_type::parse_error;
                case 0x1B:
                    error_message_ =
                        "invalid string: control character U+001B (ESC) must be escaped to \\u001B";
                    return diag_token_type::parse_error;
                case 0x1C:
                    error_message_ =
                        "invalid string: control character U+001C (FS) must be escaped to \\u001C";
                    return diag_token_type::parse_error;
                case 0x1D:
                    error_message_ =
                        "invalid string: control character U+001D (GS) must be escaped to \\u001D";
                    return diag_token_type::parse_error;
                case 0x1E:
                    error_message_ =
                        "invalid string: control character U+001E (RS) must be escaped to \\u001E";
                    return diag_token_type::parse_error;
                case 0x1F:
                    error_message_ =
                        "invalid string: control character U+001F (US) must be escaped to \\u001F";
                    return diag_token_type::parse_error;

                case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
                case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E:
                case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
                case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C:
                case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43:
                case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A:
                case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x51:
                case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58:
                case 0x59: case 0x5A: case 0x5B: case 0x5D: case 0x5E: case 0x5F: case 0x60:
                case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
                case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E:
                case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:
                case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C:
                case 0x7D: case 0x7E: case 0x7F:
                    break;

                case 0xC2: case 0xC3: case 0xC4: case 0xC5: case 0xC6: case 0xC7: case 0xC8:
                case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF:
                case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6:
                case 0xD7: case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD:
                case 0xDE: case 0xDF:
                    if (!next_byte_in_range({0x80, 0xBF})) {
                        return diag_token_type::parse_error;
                    }
                    break;

                case 0xE0:
                    if (!next_byte_in_range({0xA0, 0xBF, 0x80, 0xBF})) {
                        return diag_token_type::parse_error;
                    }
                    break;

                case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7:
                case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xEE: case 0xEF:
                    if (!next_byte_in_range({0x80, 0xBF, 0x80, 0xBF})) {
                        return diag_token_type::parse_error;
                    }
                    break;

                case 0xED:
                    if (!next_byte_in_range({0x80, 0x9F, 0x80, 0xBF})) {
                        return diag_token_type::parse_error;
                    }
                    break;

                case 0xF0:
                    if (!next_byte_in_range({0x90, 0xBF, 0x80, 0xBF, 0x80, 0xBF})) {
                        return diag_token_type::parse_error;
                    }
                    break;

                case 0xF1: case 0xF2: case 0xF3:
                    if (!next_byte_in_range({0x80, 0xBF, 0x80, 0xBF, 0x80, 0xBF})) {
                        return diag_token_type::parse_error;
                    }
                    break;

                case 0xF4:
                    if (!next_byte_in_range({0x80, 0x8F, 0x80, 0xBF, 0x80, 0xBF})) {
                        return diag_token_type::parse_error;
                    }
                    break;

                default:
                    error_message_ = "invalid string: ill-formed UTF-8 byte";
                    return diag_token_type::parse_error;
            }
        }
    }

    bool scan_comment() {
        switch (get()) {
            case '/': {
                while (true) {
                    switch (get()) {
                        case '\n':
                        case '\r':
                        case std::char_traits<char>::eof():
                        case '\0':
                            return true;
                        default:
                            break;
                    }
                }
            }

            case '*': {
                while (true) {
                    switch (get()) {
                        case std::char_traits<char>::eof():
                        case '\0':
                            error_message_ = "invalid comment; missing closing '*/'";
                            return false;

                        case '*': {
                            switch (get()) {
                                case '/':
                                    return true;
                                default:
                                    unget();
                                    continue;
                            }
                        }

                        default:
                            continue;
                    }
                }
            }

            default:
                error_message_ = "invalid comment; expecting '/' or '*' after '/'";
                return false;
        }
    }

    diag_token_type scan_number() {
        reset();
        diag_token_type number_type = diag_token_type::value_unsigned;

        switch (current_) {
            case '-':
                add(current_);
                goto scan_number_minus;
            case '0':
                add(current_);
                goto scan_number_zero;
            case '1': case '2': case '3': case '4': case '5':
            case '6': case '7': case '8': case '9':
                add(current_);
                goto scan_number_any1;
            default:
                assert(false);
                return diag_token_type::parse_error;
        }

    scan_number_minus:
        number_type = diag_token_type::value_integer;
        switch (get()) {
            case '0':
                add(current_);
                goto scan_number_zero;
            case '1': case '2': case '3': case '4': case '5':
            case '6': case '7': case '8': case '9':
                add(current_);
                goto scan_number_any1;
            default:
                error_message_ = "invalid number; expected digit after '-'";
                return diag_token_type::parse_error;
        }

    scan_number_zero:
        switch (get()) {
            case '.':
                add('.');
                goto scan_number_decimal1;
            case 'e':
            case 'E':
                add(current_);
                goto scan_number_exponent;
            default:
                goto scan_number_done;
        }

    scan_number_any1:
        switch (get()) {
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                add(current_);
                goto scan_number_any1;
            case '.':
                add('.');
                goto scan_number_decimal1;
            case 'e':
            case 'E':
                add(current_);
                goto scan_number_exponent;
            default:
                goto scan_number_done;
        }

    scan_number_decimal1:
        number_type = diag_token_type::value_float;
        switch (get()) {
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                add(current_);
                goto scan_number_decimal2;
            default:
                error_message_ = "invalid number; expected digit after '.'";
                return diag_token_type::parse_error;
        }

    scan_number_decimal2:
        switch (get()) {
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                add(current_);
                goto scan_number_decimal2;
            case 'e':
            case 'E':
                add(current_);
                goto scan_number_exponent;
            default:
                goto scan_number_done;
        }

    scan_number_exponent:
        number_type = diag_token_type::value_float;
        switch (get()) {
            case '+':
            case '-':
                add(current_);
                goto scan_number_sign;
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                add(current_);
                goto scan_number_any2;
            default:
                error_message_ = "invalid number; expected '+', '-', or digit after exponent";
                return diag_token_type::parse_error;
        }

    scan_number_sign:
        switch (get()) {
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                add(current_);
                goto scan_number_any2;
            default:
                error_message_ = "invalid number; expected digit after exponent sign";
                return diag_token_type::parse_error;
        }

    scan_number_any2:
        switch (get()) {
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                add(current_);
                goto scan_number_any2;
            default:
                goto scan_number_done;
        }

    scan_number_done:
        unget();

        char* endptr = nullptr;
        errno = 0;

        if (number_type == diag_token_type::value_unsigned) {
            const auto x = std::strtoull(token_buffer_.c_str(), &endptr, 10);
            if (errno == 0) {
                const auto vu = static_cast<json::number_unsigned_t>(x);
                if (vu == x) {
                    return diag_token_type::value_unsigned;
                }
            }
        } else if (number_type == diag_token_type::value_integer) {
            const auto x = std::strtoll(token_buffer_.c_str(), &endptr, 10);
            if (errno == 0) {
                const auto vi = static_cast<json::number_integer_t>(x);
                if (vi == x) {
                    return diag_token_type::value_integer;
                }
            }
        }

        value_float_ = std::strtod(token_buffer_.c_str(), &endptr);
        return diag_token_type::value_float;
    }

    diag_token_type scan_literal(const char* literal_text, const std::size_t length,
                                 diag_token_type return_type) {
        assert(std::char_traits<char>::to_char_type(current_) == literal_text[0]);
        for (std::size_t i = 1; i < length; ++i) {
            if (std::char_traits<char>::to_char_type(get()) != literal_text[i]) {
                error_message_ = "invalid literal";
                return diag_token_type::parse_error;
            }
        }
        return return_type;
    }

    std::string_view input_;
    std::size_t cursor_ = 0;
    const bool ignore_comments_ = false;
    char_int_type current_ = std::char_traits<char>::eof();
    bool next_unget_ = false;
    diag_position_t position_{};
    std::vector<char> token_string_{};
    std::string token_buffer_{};
    const char* error_message_ = "";
    json::number_float_t value_float_ = 0.0;
};

class diagnostic_parser {
public:
    diagnostic_parser(std::string_view input, bool ignore_comments)
        : lexer_(input, ignore_comments) {
        get_token();
    }

    bool diagnose(bool allow_exceptions) {
        const bool ok = sax_parse_internal(allow_exceptions);
        if (!ok) {
            return false;
        }
        if (get_token() != diag_token_type::end_of_input) {
            return fail_parse_error(
                allow_exceptions,
                exception_message(diag_token_type::end_of_input, "value"));
        }
        return true;
    }

    constexpr std::size_t accepted_prefix_size() const noexcept {
        return lexer_.accepted_prefix_size();
    }

private:
    diag_token_type get_token() {
        return last_token_ = lexer_.scan();
    }

    bool fail_parse_error(bool allow_exceptions, const std::string& msg) {
        if (allow_exceptions) {
            const auto pos = lexer_.get_position();
            throw json::parse_error::create(101, pos.chars_read_total, pos.lines_read + 1,
                                            pos.chars_read_current_line, msg);
        }
        return false;
    }

    bool sax_parse_internal(bool allow_exceptions) {
        std::vector<bool> states;
        bool skip_to_state_evaluation = false;

        while (true) {
            if (!skip_to_state_evaluation) {
                switch (last_token_) {
                    case diag_token_type::begin_object: {
                        if (get_token() == diag_token_type::end_object) {
                            break;
                        }
                        if (last_token_ != diag_token_type::value_string) {
                            return fail_parse_error(
                                allow_exceptions,
                                exception_message(diag_token_type::value_string, "object key"));
                        }
                        if (get_token() != diag_token_type::name_separator) {
                            return fail_parse_error(
                                allow_exceptions,
                                exception_message(diag_token_type::name_separator,
                                                  "object separator"));
                        }
                        states.push_back(false);
                        get_token();
                        continue;
                    }

                    case diag_token_type::begin_array: {
                        if (get_token() == diag_token_type::end_array) {
                            break;
                        }
                        states.push_back(true);
                        continue;
                    }

                    case diag_token_type::value_float: {
                        const auto res = lexer_.get_number_float();
                        if (!std::isfinite(res)) {
                            if (allow_exceptions) {
                                throw json::out_of_range::create(
                                    406, "number overflow parsing '" + lexer_.get_token_string() +
                                             "'");
                            }
                            return false;
                        }
                        break;
                    }

                    case diag_token_type::literal_false:
                    case diag_token_type::literal_null:
                    case diag_token_type::literal_true:
                    case diag_token_type::value_integer:
                    case diag_token_type::value_string:
                    case diag_token_type::value_unsigned:
                        break;

                    case diag_token_type::parse_error:
                        return fail_parse_error(
                            allow_exceptions,
                            exception_message(diag_token_type::uninitialized, "value"));

                    case diag_token_type::end_of_input: {
                        if (lexer_.get_position().chars_read_total == 1) {
                            return fail_parse_error(
                                allow_exceptions,
                                "attempting to parse an empty input; check that your input string "
                                "or stream contains the expected JSON");
                        }
                        return fail_parse_error(
                            allow_exceptions,
                            exception_message(diag_token_type::literal_or_value, "value"));
                    }

                    case diag_token_type::uninitialized:
                    case diag_token_type::end_array:
                    case diag_token_type::end_object:
                    case diag_token_type::name_separator:
                    case diag_token_type::value_separator:
                    case diag_token_type::literal_or_value:
                    default:
                        return fail_parse_error(
                            allow_exceptions,
                            exception_message(diag_token_type::literal_or_value, "value"));
                }
            } else {
                skip_to_state_evaluation = false;
            }

            if (states.empty()) {
                return true;
            }

            if (states.back()) {
                if (get_token() == diag_token_type::value_separator) {
                    get_token();
                    continue;
                }
                if (last_token_ == diag_token_type::end_array) {
                    states.pop_back();
                    skip_to_state_evaluation = true;
                    continue;
                }
                return fail_parse_error(
                    allow_exceptions,
                    exception_message(diag_token_type::end_array, "array"));
            }

            if (get_token() == diag_token_type::value_separator) {
                if (get_token() != diag_token_type::value_string) {
                    return fail_parse_error(
                        allow_exceptions,
                        exception_message(diag_token_type::value_string, "object key"));
                }
                if (get_token() != diag_token_type::name_separator) {
                    return fail_parse_error(
                        allow_exceptions,
                        exception_message(diag_token_type::name_separator, "object separator"));
                }
                get_token();
                continue;
            }

            if (last_token_ == diag_token_type::end_object) {
                states.pop_back();
                skip_to_state_evaluation = true;
                continue;
            }

            return fail_parse_error(
                allow_exceptions,
                exception_message(diag_token_type::end_object, "object"));
        }
    }

    std::string exception_message(const diag_token_type expected,
                                  const std::string& context) const {
        std::string error_msg = "syntax error ";
        if (!context.empty()) {
            error_msg += "while parsing " + context + " ";
        }
        error_msg += "- ";
        if (last_token_ == diag_token_type::parse_error) {
            error_msg += std::string(lexer_.get_error_message()) + "; last read: '" +
                         lexer_.get_token_string() + "'";
        } else {
            error_msg += std::string("unexpected ") + diag_token_type_name(last_token_);
        }
        if (expected != diag_token_type::uninitialized) {
            error_msg += std::string("; expected ") + diag_token_type_name(expected);
        }
        return error_msg;
    }

    diagnostic_lexer lexer_;
    diag_token_type last_token_ = diag_token_type::uninitialized;
};

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
    yyjson_read_flag flg = YYJSON_READ_ALLOW_BOM;
    if (ignore_comments) {
        flg |= YYJSON_READ_ALLOW_COMMENTS;
    }

    if (s.find('\0') != std::string_view::npos) {
        detail::diagnostic_parser diag(s, ignore_comments);
        if (!diag.diagnose(allow_exceptions)) {
            return json(value_t::discarded);
        }
        const std::string_view prefix = s.substr(0, diag.accepted_prefix_size());
        std::string clean_prefix;
        const char* data = prefix.data();
        const std::size_t size = prefix.size();
        if (prefix.find('\0') != std::string_view::npos) {
            clean_prefix.assign(prefix);
            std::replace(clean_prefix.begin(), clean_prefix.end(), '\0', '\n');
            data = clean_prefix.data();
        }
        yyjson_doc* doc =
            yyjson_read_opts(const_cast<char*>(data), size, flg, nullptr, nullptr);
        if (doc == nullptr) {
            return json(value_t::discarded);
        }
        struct DocGuard {
            yyjson_doc* d;
            ~DocGuard() { yyjson_doc_free(d); }
        } guard{doc};
        return detail::materialize_yyjson(yyjson_doc_get_root(doc));
    }

    // yyjson_read_opts does not mutate the input buffer when YYJSON_READ_INSITU is not set.
    yyjson_doc* doc =
        yyjson_read_opts(const_cast<char*>(s.data()), s.size(), flg, nullptr, nullptr);
    if (doc == nullptr) {
        detail::diagnostic_parser diag(s, ignore_comments);
        (void)diag.diagnose(allow_exceptions);
        return json(value_t::discarded);
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
    yyjson_read_flag flg = YYJSON_READ_ALLOW_BOM;
    if (ignore_comments) {
        flg |= YYJSON_READ_ALLOW_COMMENTS;
    }

    if (s.find('\0') != std::string_view::npos) {
        detail::diagnostic_parser diag(s, ignore_comments);
        if (!diag.diagnose(false)) {
            return false;
        }
        const std::string_view prefix = s.substr(0, diag.accepted_prefix_size());
        std::string clean_prefix;
        const char* data = prefix.data();
        const std::size_t size = prefix.size();
        if (prefix.find('\0') != std::string_view::npos) {
            clean_prefix.assign(prefix);
            std::replace(clean_prefix.begin(), clean_prefix.end(), '\0', '\n');
            data = clean_prefix.data();
        }
        yyjson_doc* doc =
            yyjson_read_opts(const_cast<char*>(data), size, flg, nullptr, nullptr);
        if (doc == nullptr) {
            return false;
        }
        yyjson_doc_free(doc);
        return true;
    }

    yyjson_doc* doc =
        yyjson_read_opts(const_cast<char*>(s.data()), s.size(), flg, nullptr, nullptr);
    if (doc == nullptr) {
        detail::diagnostic_parser diag(s, ignore_comments);
        (void)diag.diagnose(false);
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
