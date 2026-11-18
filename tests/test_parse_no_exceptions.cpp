// Parsing with exceptions turned off: a text that cannot be parsed gives a discarded value, no exception is thrown
// for it, and the value is the one nlohmann::ordered_json returns. With exceptions on, the same texts still throw what
// the oracle throws.

#include "differential.hpp"

#include <string>
#include <vector>

namespace {

using jk = neojson::json;
using oj = nlohmann::ordered_json;

std::vector<std::string> invalid_texts() {
    return {"",          " ",         "[",        "{",        "]",        "}",       "[1,",       "[1 2]",
            "[,]",       "{,}",       "{\"a\"}",  "{\"a\":}", "{\"a\":1,}", "tru",    "nul",       "nulll",
            "\"abc",     "\"\\x\"",   "01",       "1.",       "-",        "1e",      "[1]x",      "{} {}",
            "//c\n1",    "/*c*/1",    "\xff",     "\"\xc3\"", "[\"a\tb\"]", "'a'",   "[1,2,]",    "{\"a\":1 \"b\":2}",
            std::string("[1,\0,2]", 8), std::string("{\"a\":\0}", 8), std::string("\0", 1)};
}

TEST(ParseWithoutExceptions, AnUnparsableTextGivesADiscardedValueAndNoException) {
    for (const std::string& text : invalid_texts()) {
        SCOPED_TRACE("text " + text);
        for (const bool comments : {false, true}) {
            oj oracle;
            jk subject;
            ASSERT_NO_THROW(oracle = oj::parse(text, nullptr, false, comments));
            ASSERT_NO_THROW(subject = jk::parse(text, nullptr, false, comments));
            EXPECT_EQ(oracle.is_discarded(), subject.is_discarded());
            EXPECT_EQ(oracle.type_name(), std::string(subject.type_name()));
            if (oracle.is_discarded()) {
                EXPECT_TRUE(subject.is_discarded());
                EXPECT_EQ(subject.dump(), oracle.dump());
            }
            EXPECT_EQ(oj::accept(text, comments), jk::accept(text, comments));
        }
        // With a callback.
        const oj::parser_callback_t keep_oracle = [](int, oj::parse_event_t, oj&) { return true; };
        const jk::parser_callback_t keep_subject = [](int, jk::parse_event_t, jk&) { return true; };
        EXPECT_EQ(oj::parse(text, keep_oracle, false).is_discarded(), jk::parse(text, keep_subject, false).is_discarded());
    }
}

TEST(ParseWithoutExceptions, TheSameTextsStillThrowWhatTheOracleThrowsWhenExceptionsAreOn) {
    for (const std::string& text : invalid_texts()) {
        SCOPED_TRACE("text " + text);
        neojson::test::expect_same_exception([&] { return oj::parse(text); }, [&] { return jk::parse(text); });
    }
}

TEST(ParseWithoutExceptions, ADocumentNestedPastTheLimitIsDiscardedWithoutAnException) {
    const std::string text = std::string(50, '[') + std::string(50, ']');
    const jk::parse_options options{10};
    jk j;
    ASSERT_NO_THROW(j = jk::parse(text, nullptr, false, false, options));
    EXPECT_TRUE(j.is_discarded());
    EXPECT_FALSE(jk::accept(text, false, options));
    // The same document at a limit that holds it is parsed.
    EXPECT_TRUE(jk::parse(text, nullptr, false, false, jk::parse_options{50}).is_array());
    // With exceptions on, the nesting error is still thrown.
    EXPECT_THROW((void)jk::parse(text, nullptr, true, false, options), jk::parse_error);
}

} // namespace
