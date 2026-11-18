// Parsing with a callback. nlohmann::ordered_json is the specification: for the same text and the same callback,
// neojson makes the same calls (depth, event, value, in the same order) and returns the same tree. The callbacks here
// decide from the events alone (their number, their kind, the depth, a seed), so both libraries must show them the
// same sequence; they keep, drop, and replace what they are given, including with a discarded value. The inputs are
// all small documents of a fixed grammar, documents with repeated keys, and larger documents (objects above the member
// count at which the plain parse path sorts keys) generated from a seed.

#include "differential.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {

using jk = neojson::json;
using oj = nlohmann::ordered_json;

// The two libraries number their events alike.
static_assert(static_cast<int>(jk::parse_event_t::object_start) == static_cast<int>(oj::parse_event_t::object_start));
static_assert(static_cast<int>(jk::parse_event_t::object_end) == static_cast<int>(oj::parse_event_t::object_end));
static_assert(static_cast<int>(jk::parse_event_t::array_start) == static_cast<int>(oj::parse_event_t::array_start));
static_assert(static_cast<int>(jk::parse_event_t::array_end) == static_cast<int>(oj::parse_event_t::array_end));
static_assert(static_cast<int>(jk::parse_event_t::key) == static_cast<int>(oj::parse_event_t::key));
static_assert(static_cast<int>(jk::parse_event_t::value) == static_cast<int>(oj::parse_event_t::value));

constexpr int kObjectStart = 0;
constexpr int kObjectEnd = 1;
constexpr int kArrayStart = 2;
constexpr int kArrayEnd = 3;
constexpr int kKey = 4;
constexpr int kValue = 5;
constexpr int kKinds = 6;

std::uint64_t mix(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

enum class Mode {
    keep_all,
    drop_kind,         // drop every event of one kind
    drop_odd_of_kind,  // drop every other event of one kind (the 1st, 3rd, ... of that kind is kept)
    drop_even_of_kind, // drop the 2nd, 4th, ... event of one kind
    drop_at_depth,     // drop every event at one depth
    drop_numbers,      // drop value events that are numbers
    random_drop,       // drop each event with a probability given by `one_in`
    discard_by_value,  // value events: replace by a discarded value and keep
    discard_at_end,    // end events: replace the container by a discarded value and keep
    random_mixed,      // drop, keep, replace by discarded or by a number, also on start events
};

struct Policy {
    Mode mode = Mode::keep_all;
    int kind = 0;
    int depth = 0;
    unsigned one_in = 2;
    std::uint64_t seed = 0;

    std::size_t seen = 0;
    std::size_t per_kind[kKinds] = {};

    template <typename J>
    bool decide(int event_depth, int event, J& parsed) {
        const std::size_t n = seen++;
        const std::size_t of_kind = per_kind[event]++;
        const std::uint64_t h = mix(seed * 1000003u + n);
        switch (mode) {
            case Mode::keep_all:
                return true;
            case Mode::drop_kind:
                return event != kind;
            case Mode::drop_odd_of_kind:
                return event != kind || of_kind % 2 == 0;
            case Mode::drop_even_of_kind:
                return event != kind || of_kind % 2 == 1;
            case Mode::drop_at_depth:
                return event_depth != depth;
            case Mode::drop_numbers:
                return !(event == kValue && parsed.is_number());
            case Mode::random_drop:
                return h % one_in != 0;
            case Mode::discard_by_value:
                if (event == kValue && h % 3 == 0) {
                    parsed = J(J::value_t::discarded);
                }
                return true;
            case Mode::discard_at_end:
                if ((event == kObjectEnd || event == kArrayEnd) && h % 3 == 0) {
                    parsed = J(J::value_t::discarded);
                }
                return true;
            case Mode::random_mixed: {
                switch (h % 11) {
                    case 0:
                    case 1:
                    case 2:
                        return false;
                    case 3:
                        if (event == kValue || event == kObjectEnd || event == kArrayEnd) {
                            parsed = J(J::value_t::discarded);
                        }
                        return true;
                    case 4:
                        if (event == kValue || event == kObjectStart || event == kArrayStart) {
                            parsed = J(static_cast<std::int64_t>(h % 100));
                        }
                        return true;
                    default:
                        return true;
                }
            }
        }
        return true;
    }
};

template <typename J>
J run_parse(const std::string& text, Policy policy, std::string& trace) {
    auto callback = [&](int depth, typename J::parse_event_t event, J& parsed) {
        trace += std::to_string(depth);
        trace += ',';
        trace += std::to_string(static_cast<int>(event));
        trace += ',';
        trace += parsed.dump();
        trace += ';';
        return policy.template decide<J>(depth, static_cast<int>(event), parsed);
    };
    return J::parse(text, callback);
}

void expect_like_oracle(const std::string& text, const Policy& policy) {
    SCOPED_TRACE("text " + text.substr(0, 200) + " mode " + std::to_string(static_cast<int>(policy.mode)) + " kind " +
                 std::to_string(policy.kind) + " depth " + std::to_string(policy.depth) + " one_in " +
                 std::to_string(policy.one_in) + " seed " + std::to_string(policy.seed));
    std::string trace_oracle;
    std::string trace_subject;
    const oj oracle = run_parse<oj>(text, policy, trace_oracle);
    const jk subject = run_parse<jk>(text, policy, trace_subject);
    ASSERT_EQ(trace_oracle, trace_subject);
    neojson::test::assert_equivalent(oracle, subject);
}

std::vector<Policy> policies(std::uint64_t seed, bool full) {
    std::vector<Policy> out;
    out.push_back(Policy{});
    for (int k = 0; k < kKinds; ++k) {
        Policy p;
        p.kind = k;
        p.mode = Mode::drop_kind;
        out.push_back(p);
        p.mode = Mode::drop_odd_of_kind;
        out.push_back(p);
        p.mode = Mode::drop_even_of_kind;
        out.push_back(p);
    }
    for (int d : {0, 1, 2}) {
        Policy p;
        p.mode = Mode::drop_at_depth;
        p.depth = d;
        out.push_back(p);
    }
    {
        Policy p;
        p.mode = Mode::drop_numbers;
        out.push_back(p);
        p.mode = Mode::discard_by_value;
        p.seed = seed;
        out.push_back(p);
        p.mode = Mode::discard_at_end;
        out.push_back(p);
    }
    if (full) {
        for (unsigned one_in : {2u, 3u, 8u}) {
            for (std::uint64_t s = 0; s < 3; ++s) {
                Policy p;
                p.mode = Mode::random_drop;
                p.one_in = one_in;
                p.seed = seed * 7 + s;
                out.push_back(p);
            }
        }
        for (std::uint64_t s = 0; s < 6; ++s) {
            Policy p;
            p.mode = Mode::random_mixed;
            p.seed = seed * 13 + s;
            out.push_back(p);
        }
    }
    return out;
}

// Every array of up to three elements and every object of up to three members (keys "a" and "b", so they repeat)
// over a small set of values.
std::vector<std::string> small_documents() {
    const std::vector<std::string> values = {"1", "null", "[]", "{}", "[1]", "{\"a\":1}"};
    std::vector<std::string> docs;
    docs.push_back("[]");
    docs.push_back("{}");
    std::vector<std::vector<std::string>> arrays = {{}};
    for (std::size_t len = 1; len <= 3; ++len) {
        std::vector<std::vector<std::string>> next;
        for (const auto& prefix : arrays) {
            if (prefix.size() + 1 != len) {
                continue;
            }
            for (const auto& v : values) {
                auto extended = prefix;
                extended.push_back(v);
                next.push_back(extended);
            }
        }
        for (const auto& elems : next) {
            std::string text = "[";
            for (std::size_t i = 0; i < elems.size(); ++i) {
                text += (i == 0 ? "" : ",") + elems[i];
            }
            docs.push_back(text + "]");
        }
        arrays.insert(arrays.end(), next.begin(), next.end());
    }
    // Objects: each member is a (key, value) pair.
    std::vector<std::string> members;
    for (const char* key : {"\"a\"", "\"b\""}) {
        for (const auto& v : values) {
            members.push_back(std::string(key) + ":" + v);
        }
    }
    std::vector<std::vector<std::size_t>> shapes = {{}};
    std::vector<std::vector<std::size_t>> frontier = {{}};
    for (std::size_t len = 1; len <= 3; ++len) {
        std::vector<std::vector<std::size_t>> next;
        for (const auto& prefix : frontier) {
            for (std::size_t m = 0; m < members.size(); ++m) {
                auto extended = prefix;
                extended.push_back(m);
                next.push_back(extended);
            }
        }
        for (const auto& shape : next) {
            std::string text = "{";
            for (std::size_t i = 0; i < shape.size(); ++i) {
                text += (i == 0 ? "" : ",") + members[shape[i]];
            }
            docs.push_back(text + "}");
        }
        frontier = next;
    }
    return docs;
}

TEST(ParseCallback, EverySmallDocumentUnderEveryPolicy) {
    const std::vector<Policy> all = policies(1, false);
    for (const std::string& doc : small_documents()) {
        for (const Policy& p : all) {
            expect_like_oracle(doc, p);
            if (HasFatalFailure()) {
                return;
            }
        }
    }
}

TEST(ParseCallback, SmallDocumentsNestedInOthers) {
    const std::vector<std::string> docs = small_documents();
    const std::vector<Policy> all = policies(2, true);
    for (std::size_t i = 0; i < docs.size(); i += 5) {
        const std::string& d = docs[i];
        const std::string wrapped[] = {"[" + d + ",1]",           "[1," + d + "]",
                                       "{\"a\":" + d + ",\"b\":2}", "{\"a\":1,\"b\":" + d + ",\"a\":2}",
                                       "{\"a\":" + d + ",\"a\":" + d + "}", "[" + d + "," + d + "]"};
        for (const std::string& text : wrapped) {
            for (const Policy& p : all) {
                expect_like_oracle(text, p);
                if (HasFatalFailure()) {
                    return;
                }
            }
        }
    }
}

// A random document. Objects draw their keys from a small pool, so that keys repeat; `big` makes some objects hold
// more members than the count at which the plain parse path sorts keys.
std::string random_value(std::mt19937_64& rng, int depth, bool big) {
    const unsigned kind = static_cast<unsigned>(rng() % (depth >= 3 ? 4 : 8));
    switch (kind) {
        case 0:
            return std::to_string(static_cast<int>(rng() % 50));
        case 1:
            return "null";
        case 2:
            return "\"s" + std::to_string(rng() % 5) + "\"";
        case 3:
            return rng() % 2 == 0 ? "true" : "[]";
        case 4:
        case 5: {
            std::string text = "[";
            const std::size_t len = static_cast<std::size_t>(rng() % 5);
            for (std::size_t i = 0; i < len; ++i) {
                text += (i == 0 ? "" : ",") + random_value(rng, depth + 1, big);
            }
            return text + "]";
        }
        default: {
            std::string text = "{";
            std::size_t len = static_cast<std::size_t>(rng() % 5);
            std::size_t pool = 3;
            if (big && depth == 0) {
                len = neojson::detail::kObjectIndexThreshold + 1 + static_cast<std::size_t>(rng() % 60);
                pool = 40;
            }
            for (std::size_t i = 0; i < len; ++i) {
                text += (i == 0 ? "" : ",") + std::string("\"k") + std::to_string(rng() % pool) + "\":" +
                        (big && depth == 0 && rng() % 3 != 0 ? std::to_string(i) : random_value(rng, depth + 1, big));
            }
            return text + "}";
        }
    }
}

TEST(ParseCallback, RandomSmallDocuments) {
    std::mt19937_64 rng(20240601);
    for (std::size_t i = 0; i < 400; ++i) {
        const std::string text = random_value(rng, 0, false);
        for (const Policy& p : policies(i, true)) {
            expect_like_oracle(text, p);
            if (HasFatalFailure()) {
                return;
            }
        }
    }
}

TEST(ParseCallback, RandomObjectsAboveTheIndexThreshold) {
    std::mt19937_64 rng(77);
    for (std::size_t i = 0; i < 60; ++i) {
        // The root object is large; it sits at the root, in an array and in an object.
        std::string text;
        do {
            text = random_value(rng, 0, true);
        } while (text.front() != '{');
        const std::string texts[] = {text, "[" + text + ",1," + text + "]", "{\"w\":" + text + ",\"w\":" + text + "}"};
        for (const std::string& t : texts) {
            for (const Policy& p : policies(i, true)) {
                expect_like_oracle(t, p);
                if (HasFatalFailure()) {
                    return;
                }
            }
        }
    }
}

TEST(ParseCallback, ObjectOfManyMembersAndChildObjectsDroppingPlaceholders) {
    // Primitive members whose values are dropped leave a discarded member behind; each child object that ends removes
    // the first one. Members and children alternate, with repeated keys, above the index threshold.
    const std::size_t n = 3 * neojson::detail::kObjectIndexThreshold;
    std::string text = "{";
    for (std::size_t i = 0; i < n; ++i) {
        if (i != 0) {
            text += ',';
        }
        const std::size_t key = (i * 7) % (n / 2);
        text += "\"k" + std::to_string(key) + "\":";
        text += (i % 3 == 0) ? "{\"x\":" + std::to_string(i) + "}" : std::to_string(i);
    }
    text += '}';
    for (const Policy& p : policies(9, true)) {
        expect_like_oracle(text, p);
        if (HasFatalFailure()) {
            return;
        }
    }
    Policy drop_numbers;
    drop_numbers.mode = Mode::drop_numbers;
    expect_like_oracle(text, drop_numbers);
}

} // namespace
