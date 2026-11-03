#ifndef JSONKIT_TESTS_DIFFERENTIAL_HPP_
#define JSONKIT_TESTS_DIFFERENTIAL_HPP_

#include <cmath>
#include <cstdint>
#include <string>

#ifndef JSON_USE_GLOBAL_UDLS
#define JSON_USE_GLOBAL_UDLS 0
#endif

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "jsonkit/json.hpp"

namespace jsonkit::test {

inline void assert_tree_equal(const nlohmann::ordered_json& oracle,
                              const jsonkit::json& subject) {
    ASSERT_EQ(static_cast<std::uint8_t>(oracle.type()),
              static_cast<std::uint8_t>(subject.type()));
    EXPECT_STREQ(oracle.type_name(), subject.type_name());

    EXPECT_EQ(oracle.is_null(), subject.is_null());
    EXPECT_EQ(oracle.is_boolean(), subject.is_boolean());
    EXPECT_EQ(oracle.is_number(), subject.is_number());
    EXPECT_EQ(oracle.is_number_integer(), subject.is_number_integer());
    EXPECT_EQ(oracle.is_number_unsigned(), subject.is_number_unsigned());
    EXPECT_EQ(oracle.is_number_float(), subject.is_number_float());
    EXPECT_EQ(oracle.is_object(), subject.is_object());
    EXPECT_EQ(oracle.is_array(), subject.is_array());
    EXPECT_EQ(oracle.is_string(), subject.is_string());
    EXPECT_EQ(oracle.is_binary(), subject.is_binary());
    EXPECT_EQ(oracle.is_discarded(), subject.is_discarded());
    EXPECT_EQ(oracle.is_primitive(), subject.is_primitive());
    EXPECT_EQ(oracle.is_structured(), subject.is_structured());

    if (!oracle.is_discarded()) {
        EXPECT_EQ(oracle.empty(), subject.empty());
        EXPECT_EQ(oracle.size(), subject.size());
        EXPECT_EQ(oracle.max_size(), subject.max_size());
    }

    switch (subject.type()) {
        case jsonkit::json::value_t::null:
        case jsonkit::json::value_t::discarded:
        case jsonkit::json::value_t::binary:
            break;

        case jsonkit::json::value_t::boolean:
            EXPECT_EQ(oracle.get<bool>(), subject.get<bool>());
            break;

        case jsonkit::json::value_t::number_integer:
            EXPECT_EQ(oracle.get<std::int64_t>(), subject.get<std::int64_t>());
            break;

        case jsonkit::json::value_t::number_unsigned:
            EXPECT_EQ(oracle.get<std::uint64_t>(), subject.get<std::uint64_t>());
            break;

        case jsonkit::json::value_t::number_float: {
            const double od = oracle.get<double>();
            const double sd = subject.get<double>();
            if (std::isnan(od)) {
                EXPECT_TRUE(std::isnan(sd));
            } else {
                EXPECT_EQ(od, sd);
                EXPECT_EQ(std::signbit(od), std::signbit(sd));
            }
            break;
        }

        case jsonkit::json::value_t::string:
            EXPECT_EQ(oracle.get<std::string>(), subject.get<std::string>());
            break;

        case jsonkit::json::value_t::array: {
            ASSERT_EQ(oracle.size(), subject.size());
            for (std::size_t i = 0; i < oracle.size(); ++i) {
                assert_tree_equal(oracle[i], subject[i]);
            }
            break;
        }

        case jsonkit::json::value_t::object: {
            ASSERT_EQ(oracle.size(), subject.size());
            auto it_o = oracle.begin();
            auto it_s = subject.begin();
            for (; it_o != oracle.end() && it_s != subject.end(); ++it_o, ++it_s) {
                EXPECT_EQ(it_o.key(), it_s.key());
                assert_tree_equal(it_o.value(), it_s.value());
            }
            break;
        }
    }
}

template <typename FnOracle, typename FnSubject>
void expect_same_exception(FnOracle&& fn_oracle, FnSubject&& fn_subject) {
    bool oracle_threw = false;
    int oracle_id = 0;
    std::size_t oracle_byte = 0;
    std::string oracle_cat;
    std::string oracle_what;

    try {
        (void)fn_oracle();
    } catch (const nlohmann::ordered_json::parse_error& e) {
        oracle_threw = true;
        oracle_id = e.id;
        oracle_byte = e.byte;
        oracle_cat = "parse_error";
        oracle_what = e.what();
    } catch (const nlohmann::ordered_json::invalid_iterator& e) {
        oracle_threw = true;
        oracle_id = e.id;
        oracle_cat = "invalid_iterator";
        oracle_what = e.what();
    } catch (const nlohmann::ordered_json::type_error& e) {
        oracle_threw = true;
        oracle_id = e.id;
        oracle_cat = "type_error";
        oracle_what = e.what();
    } catch (const nlohmann::ordered_json::out_of_range& e) {
        oracle_threw = true;
        oracle_id = e.id;
        oracle_cat = "out_of_range";
        oracle_what = e.what();
    } catch (const nlohmann::ordered_json::other_error& e) {
        oracle_threw = true;
        oracle_id = e.id;
        oracle_cat = "other_error";
        oracle_what = e.what();
    } catch (const nlohmann::ordered_json::exception& e) {
        oracle_threw = true;
        oracle_id = e.id;
        oracle_cat = "exception";
        oracle_what = e.what();
    }

    bool subject_threw = false;
    int subject_id = 0;
    std::size_t subject_byte = 0;
    std::string subject_cat;
    std::string subject_what;

    try {
        (void)fn_subject();
    } catch (const jsonkit::json::parse_error& e) {
        subject_threw = true;
        subject_id = e.id;
        subject_byte = e.byte;
        subject_cat = "parse_error";
        subject_what = e.what();
    } catch (const jsonkit::json::invalid_iterator& e) {
        subject_threw = true;
        subject_id = e.id;
        subject_cat = "invalid_iterator";
        subject_what = e.what();
    } catch (const jsonkit::json::type_error& e) {
        subject_threw = true;
        subject_id = e.id;
        subject_cat = "type_error";
        subject_what = e.what();
    } catch (const jsonkit::json::out_of_range& e) {
        subject_threw = true;
        subject_id = e.id;
        subject_cat = "out_of_range";
        subject_what = e.what();
    } catch (const jsonkit::json::other_error& e) {
        subject_threw = true;
        subject_id = e.id;
        subject_cat = "other_error";
        subject_what = e.what();
    } catch (const jsonkit::json::exception& e) {
        subject_threw = true;
        subject_id = e.id;
        subject_cat = "exception";
        subject_what = e.what();
    }

    ASSERT_EQ(oracle_threw, subject_threw)
        << "Oracle what: " << oracle_what << "\nSubject what: " << subject_what;

    if (oracle_threw) {
        EXPECT_EQ(oracle_cat, subject_cat)
            << "Oracle what: " << oracle_what << "\nSubject what: " << subject_what;
        EXPECT_EQ(oracle_id, subject_id)
            << "Oracle what: " << oracle_what << "\nSubject what: " << subject_what;

        const std::string expected_prefix =
            "[json.exception." + oracle_cat + "." + std::to_string(oracle_id) + "] ";
        EXPECT_EQ(subject_what.substr(0, expected_prefix.size()), expected_prefix)
            << "Subject what: " << subject_what;

        if (oracle_cat == "parse_error") {
            EXPECT_GT(oracle_byte, 0u);
            EXPECT_EQ(oracle_byte, subject_byte)
                << "Oracle what: " << oracle_what << "\nSubject what: " << subject_what;
        }
        EXPECT_EQ(oracle_what, subject_what);
    }
}

inline void assert_dump_option_equal(const nlohmann::ordered_json& oracle,
                                     const jsonkit::json& subject, int indent,
                                     char indent_char, bool ensure_ascii,
                                     nlohmann::ordered_json::error_handler_t eh_o,
                                     jsonkit::json::error_handler_t eh_s) {
    std::string out_o;
    std::string out_s;
    expect_same_exception(
        [&]() { out_o = oracle.dump(indent, indent_char, ensure_ascii, eh_o); },
        [&]() { out_s = subject.dump(indent, indent_char, ensure_ascii, eh_s); });
    EXPECT_EQ(out_o, out_s);
}

inline void assert_equivalent(const nlohmann::ordered_json& oracle,
                              const jsonkit::json& subject) {
    assert_tree_equal(oracle, subject);

    if (!oracle.is_discarded()) {
        assert_dump_option_equal(oracle, subject, -1, ' ', false,
                                 nlohmann::ordered_json::error_handler_t::strict,
                                 jsonkit::json::error_handler_t::strict);
        assert_dump_option_equal(oracle, subject, 2, ' ', false,
                                 nlohmann::ordered_json::error_handler_t::strict,
                                 jsonkit::json::error_handler_t::strict);
        assert_dump_option_equal(oracle, subject, 4, '\t', true,
                                 nlohmann::ordered_json::error_handler_t::strict,
                                 jsonkit::json::error_handler_t::strict);
        assert_dump_option_equal(oracle, subject, -1, ' ', false,
                                 nlohmann::ordered_json::error_handler_t::replace,
                                 jsonkit::json::error_handler_t::replace);
        assert_dump_option_equal(oracle, subject, -1, ' ', true,
                                 nlohmann::ordered_json::error_handler_t::replace,
                                 jsonkit::json::error_handler_t::replace);
        assert_dump_option_equal(oracle, subject, -1, ' ', false,
                                 nlohmann::ordered_json::error_handler_t::ignore,
                                 jsonkit::json::error_handler_t::ignore);
    }
}

} // namespace jsonkit::test

#endif // JSONKIT_TESTS_DIFFERENTIAL_HPP_
