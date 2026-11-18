#include "differential.hpp"
#include "small_stack.hpp"

#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

using neojson::test::assert_equivalent;
using neojson::test::run_with_stack;

namespace {

using jk = neojson::json;
using oj = nlohmann::ordered_json;

constexpr std::size_t kDefault = neojson::parse_options::default_max_depth;
constexpr std::size_t kSmallStack = 512 * 1024;
// Enough for every recursive walk of a tree of the default depth in an unoptimised build with sanitizers (see
// parse_options::default_max_depth).
constexpr std::size_t kWalkStack = 4 * 1024 * 1024;

// `n` arrays inside each other around the number 1.
std::string nested_arrays(std::size_t n) {
    return std::string(n, '[') + "1" + std::string(n, ']');
}

// `n` objects inside each other: {"a":{"a":...{"a":1}...}}.
std::string nested_objects(std::size_t n) {
    std::string text;
    for (std::size_t i = 0; i < n; ++i) {
        text += "{\"a\":";
    }
    text += "1";
    text.append(n, '}');
    return text;
}

// `n` levels alternating between arrays and objects, outermost an array.
std::string nested_mixed(std::size_t n) {
    std::string open;
    open.reserve(n * 3);
    std::vector<char> closers;
    closers.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (i % 2 == 0) {
            open += '[';
            closers.push_back(']');
        } else {
            open += "{\"a\":";
            closers.push_back('}');
        }
    }
    std::string text = open + "1";
    text.append(closers.rbegin(), closers.rend());
    return text;
}

using Shape = std::string (*)(std::size_t);
const Shape kShapes[] = {nested_arrays, nested_objects, nested_mixed};
const char* const kShapeNames[] = {"arrays", "objects", "mixed"};

// The three ways a call reports a document it rejects.
void expect_rejected_for_nesting(const std::string& text, const jk::parse_options& options,
                                 const std::string& what) {
    SCOPED_TRACE(what);
    try {
        (void)jk::parse(text, nullptr, true, false, options);
        ADD_FAILURE() << "no exception";
    } catch (const jk::parse_error& e) {
        EXPECT_EQ(e.id, 120);
        EXPECT_NE(std::string(e.what()).find("nesting limit exceeded"), std::string::npos)
            << e.what();
    }
    EXPECT_TRUE(jk::parse(text, nullptr, false, false, options).is_discarded());
    EXPECT_TRUE(jk::parse(text, false, false, options).is_discarded());
    EXPECT_FALSE(jk::accept(text, false, options));
}

void expect_accepted_like_oracle(const std::string& text, const jk::parse_options& options) {
    ASSERT_TRUE(oj::accept(text));
    EXPECT_TRUE(jk::accept(text, false, options));
    assert_equivalent(oj::parse(text), jk::parse(text, nullptr, true, false, options));
}

TEST(ParseDepth, DefaultLimitIsPublishedAndUsedByDefault) {
    EXPECT_EQ(jk::parse_options{}.max_depth, kDefault);
    EXPECT_EQ(jk::parse_options::default_max_depth, kDefault);
    EXPECT_GE(kDefault, 256u);  // the documents of the existing tests
}

TEST(ParseDepth, DocumentAtTheDefaultLimitParsesLikeTheOracle) {
    for (std::size_t s = 0; s < 3; ++s) {
        SCOPED_TRACE(kShapeNames[s]);
        const std::string text = kShapes[s](kDefault);
        ASSERT_TRUE(oj::accept(text));
        EXPECT_TRUE(jk::accept(text));
        assert_equivalent(oj::parse(text), jk::parse(text));
    }
}

TEST(ParseDepth, DocumentOneLevelPastTheDefaultLimitIsAParseError) {
    for (std::size_t s = 0; s < 3; ++s) {
        const std::string text = kShapes[s](kDefault + 1);
        expect_rejected_for_nesting(text, jk::parse_options{}, kShapeNames[s]);
        // The calls without options use the default.
        EXPECT_FALSE(jk::accept(text));
        EXPECT_TRUE(jk::parse(text, nullptr, false).is_discarded());
        EXPECT_THROW((void)jk::parse(text), jk::parse_error);
    }
}

TEST(ParseDepth, ParseErrorNamesTheLevelThatPassedTheLimit) {
    try {
        (void)jk::parse("[[[1]]]", nullptr, true, false, jk::parse_options{2});
        FAIL() << "no exception";
    } catch (const jk::parse_error& e) {
        EXPECT_STREQ(e.what(),
                     "[json.exception.parse_error.120] parse error at line 1, column 3: nesting "
                     "limit exceeded; arrays and objects may be nested at most 2 levels deep");
        EXPECT_EQ(e.byte, 3u);
    }
    try {
        (void)jk::parse("{\"a\":\n  {\"b\": [1]}}", nullptr, true, false, jk::parse_options{2});
        FAIL() << "no exception";
    } catch (const jk::parse_error& e) {
        EXPECT_EQ(e.id, 120);
        EXPECT_NE(std::string(e.what()).find("line 2, column 9"), std::string::npos) << e.what();
    }
}

TEST(ParseDepth, CustomLimitWorksInBothDirections) {
    for (std::size_t limit : {1u, 2u, 5u, 63u, 64u, 65u, 300u}) {
        for (std::size_t s = 0; s < 3; ++s) {
            SCOPED_TRACE(std::string(kShapeNames[s]) + " limit " + std::to_string(limit));
            const jk::parse_options options{limit};
            expect_accepted_like_oracle(kShapes[s](limit), options);
            expect_rejected_for_nesting(kShapes[s](limit + 1), options, "one past the limit");
        }
    }
    // A limit above the default admits what the default rejects.
    const std::size_t deep = 1200;
    ASSERT_GT(deep, kDefault) << "raise `deep` together with the default";
    for (std::size_t s = 0; s < 3; ++s) {
        SCOPED_TRACE(kShapeNames[s]);
        const std::string text = kShapes[s](deep);
        EXPECT_FALSE(jk::accept(text));
        expect_accepted_like_oracle(text, jk::parse_options{deep});
    }
}

TEST(ParseDepth, EmptyContainersAreLevels) {
    const jk::parse_options three{3};
    EXPECT_TRUE(jk::accept("[[[]]]", false, three));
    EXPECT_FALSE(jk::accept("[[[[]]]]", false, three));
    EXPECT_TRUE(jk::accept("[[{}]]", false, three));
    EXPECT_FALSE(jk::accept("[[{\"a\":[]}]]", false, three));
    EXPECT_TRUE(jk::accept("[[], [], [[]], {}]", false, three));
}

TEST(ParseDepth, SmallLimits) {
    // A value that is not an array or an object has no level.
    EXPECT_TRUE(jk::accept("1", false, jk::parse_options{0}));
    EXPECT_TRUE(jk::accept("\"[[[\"", false, jk::parse_options{0}));
    EXPECT_FALSE(jk::accept("[]", false, jk::parse_options{0}));
    EXPECT_FALSE(jk::accept("{}", false, jk::parse_options{0}));
    EXPECT_TRUE(jk::accept("[]", false, jk::parse_options{1}));
    EXPECT_FALSE(jk::accept("[[]]", false, jk::parse_options{1}));
    EXPECT_FALSE(jk::accept("{\"a\":[]}", false, jk::parse_options{1}));
    EXPECT_TRUE(jk::accept("[1,2,{\"a\":1},\"[[[\"]", false, jk::parse_options{2}));
    expect_rejected_for_nesting("[]", jk::parse_options{0}, "limit 0");
}

TEST(ParseDepth, BracketsInsideStringsAndCommentsAreNotLevels) {
    const std::string text = R"(["[[[[[[[[", {"[[[[[": "]]]]]"}])";
    EXPECT_TRUE(jk::accept(text, false, jk::parse_options{2}));
    const std::string commented = "[ /* [[[[[[[[ */ 1 ] // [[[[\n";
    EXPECT_TRUE(jk::accept(commented, true, jk::parse_options{1}));
}

TEST(ParseDepth, DocumentsWithNulBytesAreBounded) {
    // The text is read up to the first NUL byte; what follows it is not part of the document.
    const std::string text("[[[1]]]\0[[[[[[[[[[[[", 20);
    ASSERT_EQ(oj::accept(text), jk::accept(text));
    EXPECT_TRUE(jk::accept(text, false, jk::parse_options{3}));
    EXPECT_FALSE(jk::accept(text, false, jk::parse_options{2}));
    EXPECT_TRUE(jk::parse(text, nullptr, true, false, jk::parse_options{3}).is_array());
    try {
        (void)jk::parse(text, nullptr, true, false, jk::parse_options{2});
        FAIL() << "no exception";
    } catch (const jk::parse_error& e) {
        EXPECT_EQ(e.id, 120);
    }
    EXPECT_TRUE(jk::parse(text, nullptr, false, false, jk::parse_options{2}).is_discarded());
    EXPECT_TRUE(jk::parse(text, nullptr, false, false, jk::parse_options{3}).is_array());
}

TEST(ParseDepth, MalformedDocumentKeepsItsSyntaxError) {
    // The syntax error comes first in the text.
    expect_rejected_for_nesting("[[[", jk::parse_options{2}, "nesting before the end of input");
    try {
        (void)jk::parse("[[x", nullptr, true, false, jk::parse_options{5});
        FAIL() << "no exception";
    } catch (const jk::parse_error& e) {
        EXPECT_EQ(e.id, 101);
    }
    try {
        (void)jk::parse("[[1,,]", nullptr, true, false, jk::parse_options{1});
        FAIL() << "no exception";
    } catch (const jk::parse_error& e) {
        EXPECT_EQ(e.id, 120);  // the second '[' is reached before the second ','
    }
    try {
        (void)jk::parse("[[1,,]", nullptr, true, false, jk::parse_options{2});
        FAIL() << "no exception";
    } catch (const jk::parse_error& e) {
        EXPECT_EQ(e.id, 101);
    }
}

TEST(ParseDepth, CallbackParseIsBounded) {
    int calls = 0;
    auto cb = [&calls](int, jk::parse_event_t, jk&) {
        ++calls;
        return true;
    };
    const std::string at_limit = nested_mixed(40);
    EXPECT_TRUE(jk::parse(at_limit, cb, true, false, jk::parse_options{40}).is_array());
    EXPECT_GT(calls, 0);
    calls = 0;
    EXPECT_THROW((void)jk::parse(at_limit, cb, true, false, jk::parse_options{39}),
                 jk::parse_error);
    EXPECT_TRUE(jk::parse(at_limit, cb, false, false, jk::parse_options{39}).is_discarded());
    EXPECT_EQ(calls, 0);

    // 100,000 levels with a callback, on a small stack.
    const std::string huge = nested_arrays(100000);
    EXPECT_EQ(run_with_stack(kSmallStack, [&] {
                  EXPECT_TRUE(jk::parse(huge, cb, false).is_discarded());
                  EXPECT_THROW((void)jk::parse(huge, cb), jk::parse_error);
              }),
              "");
}

TEST(ParseDepth, EveryEntryPointUsesTheLimit) {
    const std::string text = nested_arrays(10);
    const jk::parse_options options{9};

    EXPECT_THROW((void)jk::parse(std::string_view(text), nullptr, true, false, options),
                 jk::parse_error);
    EXPECT_THROW((void)jk::parse(text.begin(), text.end(), nullptr, true, false, options),
                 jk::parse_error);
    EXPECT_TRUE(jk::parse(text.begin(), text.end(), false, false, options).is_discarded());
    std::istringstream is1(text);
    EXPECT_THROW((void)jk::parse(is1, nullptr, true, false, options), jk::parse_error);
    std::istringstream is2(text);
    EXPECT_TRUE(jk::parse(is2, false, false, options).is_discarded());
    EXPECT_THROW((void)jk::parse(std::istringstream(text), nullptr, true, false, options),
                 jk::parse_error);
    EXPECT_TRUE(jk::parse(std::istringstream(text), false, false, options).is_discarded());

    EXPECT_FALSE(jk::accept(std::string_view(text), false, options));
    EXPECT_FALSE(jk::accept(text.begin(), text.end(), false, options));
    std::istringstream is3(text);
    EXPECT_FALSE(jk::accept(is3, false, options));
    EXPECT_FALSE(jk::accept(std::istringstream(text), false, options));

    // The same inputs inside the limit.
    const jk::parse_options ten{10};
    EXPECT_TRUE(jk::parse(std::string_view(text), nullptr, true, false, ten).is_array());
    EXPECT_TRUE(jk::parse(text.begin(), text.end(), nullptr, true, false, ten).is_array());
    std::istringstream is4(text);
    EXPECT_TRUE(jk::parse(is4, nullptr, true, false, ten).is_array());
    std::istringstream is5(text);
    EXPECT_TRUE(jk::accept(is5, false, ten));

    // The entry points that take no options use the default limit.
    const std::string past_default = nested_arrays(kDefault + 1);
    std::istringstream is6(past_default);
    EXPECT_THROW((void)jk::parse(is6), jk::parse_error);
    std::istringstream is7(past_default);
    jk target;
    EXPECT_THROW(is7 >> target, jk::parse_error);
    EXPECT_THROW((void)neojson::literals::operator""_json(past_default.data(), past_default.size()),
                 jk::parse_error);
    EXPECT_FALSE(jk::accept(past_default.begin(), past_default.end()));
}

TEST(ParseDepth, ExistingSignaturesStillCompile) {
    // Every call form of the nlohmann/json API, without the added parameter.
    const std::string text = "[1,[2]]";
    EXPECT_TRUE(jk::parse(text).is_array());
    EXPECT_TRUE(jk::parse(text, nullptr).is_array());
    EXPECT_TRUE(jk::parse(text, nullptr, true).is_array());
    EXPECT_TRUE(jk::parse(text, nullptr, true, true).is_array());
    EXPECT_TRUE(jk::parse(text, true).is_array());
    EXPECT_TRUE(jk::parse(text, false, true).is_array());
    EXPECT_TRUE(jk::parse(text.begin(), text.end()).is_array());
    EXPECT_TRUE(jk::parse(text.begin(), text.end(), nullptr, true, false).is_array());
    EXPECT_TRUE(jk::parse(text.begin(), text.end(), true, false).is_array());
    std::istringstream is(text);
    EXPECT_TRUE(jk::parse(is).is_array());
    EXPECT_TRUE(jk::accept(text));
    EXPECT_TRUE(jk::accept(text, true));
    EXPECT_TRUE(jk::accept(text, {}));
    EXPECT_TRUE(jk::accept(text.begin(), text.end()));
    std::istringstream is2(text);
    EXPECT_TRUE(jk::accept(is2, false));
}

// The documents the limit rejects reach no recursive code, so they are safe on a small stack. Parsing a document at
// the limit needs no deep call stack either; walking the finished tree does, so that test uses kWalkStack.
TEST(ParseDepth, HugeDocumentsAreRejectedOnASmallStack) {
    for (std::size_t levels : {100000u, 1000000u}) {
        for (std::size_t s = 0; s < 3; ++s) {
            SCOPED_TRACE(std::string(kShapeNames[s]) + " " + std::to_string(levels));
            const std::string text = kShapes[s](levels);
            const std::string error = run_with_stack(kSmallStack, [&] {
                try {
                    (void)jk::parse(text);
                    ADD_FAILURE() << "no exception";
                } catch (const jk::parse_error& e) {
                    EXPECT_EQ(e.id, 120);
                    EXPECT_NE(std::string(e.what()).find("nesting limit exceeded"),
                              std::string::npos);
                }
                EXPECT_TRUE(jk::parse(text, nullptr, false).is_discarded());
                EXPECT_FALSE(jk::accept(text));
                std::istringstream is(text);
                EXPECT_FALSE(jk::accept(is));
                EXPECT_TRUE(jk::parse(text, nullptr, false, false, jk::parse_options{10})
                                .is_discarded());
            });
            EXPECT_EQ(error, "");
        }
    }
}

// Parsing a document far deeper than its call stack could hold works when the limit allows it, because the builders
// keep their own stack. The trees are destroyed afterwards on the (larger) stack of the test thread.
TEST(ParseDepth, ParsingNeedsNoDeepCallStack) {
    const std::size_t levels = 10000;
    const jk::parse_options options{levels};
    const jk::parser_callback_t keep = [](int, jk::parse_event_t, jk&) { return true; };
    for (std::size_t s = 0; s < 3; ++s) {
        SCOPED_TRACE(kShapeNames[s]);
        const std::string text = kShapes[s](levels);
        jk plain;
        jk with_callback;
        EXPECT_EQ(run_with_stack(kSmallStack,
                                 [&] {
                                     plain = jk::parse(text, nullptr, true, false, options);
                                     with_callback = jk::parse(text, keep, true, false, options);
                                     EXPECT_TRUE(jk::accept(text, false, options));
                                 }),
                  "");
        // Walk down to the innermost value of each tree.
        for (const jk* root : {&plain, &with_callback}) {
            ASSERT_TRUE(root->is_structured());
            const jk* node = root;
            std::size_t seen = 0;
            while (node->is_structured()) {
                ASSERT_EQ(node->size(), 1u);
                node = node->is_array() ? &(*node)[0] : &node->begin().value();
                ++seen;
            }
            EXPECT_EQ(seen, levels);
            EXPECT_EQ(node->get<int>(), 1);
        }
    }
}

TEST(ParseDepth, DocumentAtTheDefaultLimitParsesOnASmallStack) {
    for (std::size_t s = 0; s < 3; ++s) {
        SCOPED_TRACE(kShapeNames[s]);
        const std::string text = kShapes[s](kDefault);
        // Parsing (with and without a callback) keeps its own stack.
        const std::string parse_error_text = run_with_stack(kSmallStack, [&] {
            EXPECT_TRUE(jk::parse(text).is_structured());
            const jk::parser_callback_t keep = [](int, jk::parse_event_t, jk&) { return true; };
            EXPECT_TRUE(jk::parse(text, keep).is_structured());
            EXPECT_TRUE(jk::accept(text));
        });
        EXPECT_EQ(parse_error_text, "");
    }
}

TEST(ParseDepth, DocumentAtTheDefaultLimitIsWalkedOnAModerateStack) {
    for (std::size_t s = 0; s < 3; ++s) {
        SCOPED_TRACE(kShapeNames[s]);
        const std::string text = kShapes[s](kDefault);
        const std::string error = run_with_stack(kWalkStack, [&] {
            jk parsed = jk::parse(text);
            const std::string dumped = parsed.dump();
            const std::string pretty = parsed.dump(1);
            EXPECT_EQ(dumped, jk::parse(dumped).dump());
            EXPECT_FALSE(pretty.empty());
            jk copy = parsed;
            EXPECT_TRUE(copy == parsed);
            EXPECT_FALSE(copy != parsed);
            copy = jk();
            // parsed and copy are destroyed with the thread's frame
        });
        EXPECT_EQ(error, "");
    }
}

} // namespace
