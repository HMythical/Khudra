// The shared formatting and parsing helpers (src/vm/format.h).
//
// These are the single source of truth for turning a number into text and text
// back into a number, so they are what keeps `io.print` under the VM and under
// a built binary printing the same bytes. The rules they implement are stated
// in format.h; these tests are those rules written as assertions.
#include "test_harness.h"

#include <string>

#include "vm/format.h"

namespace format = khu::vm::format;

KHU_TEST(format, renders_integers_by_signedness) {
    KHU_CHECK_EQ(format::format_int(0), std::string("0"));
    KHU_CHECK_EQ(format::format_int(-1), std::string("-1"));
    KHU_CHECK_EQ(format::format_int(INT64_MIN), std::string("-9223372036854775808"));
    KHU_CHECK_EQ(format::format_uint(0), std::string("0"));
    KHU_CHECK_EQ(format::format_uint(UINT64_MAX), std::string("18446744073709551615"));
}

KHU_TEST(format, renders_floats_with_six_significant_digits) {
    KHU_CHECK_EQ(format::format_float(1.5, false), std::string("1.5"));
    KHU_CHECK_EQ(format::format_float(1.0, false), std::string("1"));
    KHU_CHECK_EQ(format::format_float(1.0 / 3.0, false), std::string("0.333333"));
    KHU_CHECK_EQ(format::format_float(1e20, false), std::string("1e+20"));
    // The width is not part of the rendering: a float and a dfloat holding the
    // same value print the same text.
    KHU_CHECK_EQ(format::format_float(0.5, true), format::format_float(0.5, false));
}

KHU_TEST(format, renders_non_finite_values_without_a_nan_sign) {
    double zero = 0.0;
    KHU_CHECK_EQ(format::format_float(1.0 / zero, false), std::string("inf"));
    KHU_CHECK_EQ(format::format_float(-1.0 / zero, false), std::string("-inf"));
    KHU_CHECK_EQ(format::format_float(zero / zero, false), std::string("nan"));
    KHU_CHECK_EQ(format::format_float(-(zero / zero), false), std::string("nan"));
}

KHU_TEST(format, renders_bools_as_keywords) {
    KHU_CHECK_EQ(format::format_bool(true), std::string("true"));
    KHU_CHECK_EQ(format::format_bool(false), std::string("false"));
}

KHU_TEST(format, parses_a_whole_signed_integer_or_nothing) {
    std::int64_t value = 0;
    KHU_CHECK(format::parse_int("42", 32, value));
    KHU_CHECK_EQ(value, static_cast<std::int64_t>(42));
    KHU_CHECK(format::parse_int("-42", 32, value));
    KHU_CHECK_EQ(value, static_cast<std::int64_t>(-42));
    KHU_CHECK(format::parse_int("+7", 32, value));
    KHU_CHECK_EQ(value, static_cast<std::int64_t>(7));

    // A partial match is not a match, and whitespace is not skipped.
    KHU_CHECK(!format::parse_int("42x", 32, value));
    KHU_CHECK(!format::parse_int(" 42", 32, value));
    KHU_CHECK(!format::parse_int("42 ", 32, value));
    KHU_CHECK(!format::parse_int("", 32, value));
    KHU_CHECK(!format::parse_int("-", 32, value));
    KHU_CHECK(!format::parse_int("0x10", 32, value));
}

KHU_TEST(format, refuses_an_integer_that_does_not_fit_the_width) {
    std::int64_t value = 0;
    KHU_CHECK(format::parse_int("127", 8, value));
    KHU_CHECK(!format::parse_int("128", 8, value));
    KHU_CHECK(format::parse_int("-128", 8, value));
    KHU_CHECK(!format::parse_int("-129", 8, value));

    KHU_CHECK(format::parse_int("-9223372036854775808", 64, value));
    KHU_CHECK_EQ(value, INT64_MIN);
    KHU_CHECK(!format::parse_int("9223372036854775808", 64, value));
}

KHU_TEST(format, parses_unsigned_integers_without_a_sign) {
    std::uint64_t value = 0;
    KHU_CHECK(format::parse_uint("255", 8, value));
    KHU_CHECK_EQ(value, static_cast<std::uint64_t>(255));
    KHU_CHECK(!format::parse_uint("256", 8, value));
    KHU_CHECK(!format::parse_uint("-1", 8, value));
    KHU_CHECK(format::parse_uint("18446744073709551615", 64, value));
    KHU_CHECK(!format::parse_uint("18446744073709551616", 64, value));
}

KHU_TEST(format, parses_the_float_grammar) {
    double value = 0.0;
    KHU_CHECK(format::parse_float("1.5", false, value));
    KHU_CHECK_EQ(value, 1.5);
    KHU_CHECK(format::parse_float("-2", false, value));
    KHU_CHECK_EQ(value, -2.0);
    KHU_CHECK(format::parse_float(".5", false, value));
    KHU_CHECK_EQ(value, 0.5);
    KHU_CHECK(format::parse_float("2.0e-3", false, value));
    KHU_CHECK_EQ(value, 0.002);

    KHU_CHECK(!format::parse_float("1.5e", false, value));
    KHU_CHECK(!format::parse_float(".", false, value));
    KHU_CHECK(!format::parse_float("", false, value));
    KHU_CHECK(!format::parse_float("1,5", false, value));
    KHU_CHECK(!format::parse_float("inf", false, value));
}

KHU_TEST(format, rounds_a_parsed_float_through_thirty_two_bits) {
    double wide = 0.0;
    double narrow = 0.0;
    KHU_CHECK(format::parse_float("0.1", false, wide));
    KHU_CHECK(format::parse_float("0.1", true, narrow));
    KHU_CHECK(wide != narrow);
    KHU_CHECK_EQ(narrow, static_cast<double>(0.1f));
}

KHU_TEST(format, is_numeric_matches_the_float_grammar) {
    KHU_CHECK(format::is_numeric("0"));
    KHU_CHECK(format::is_numeric("-3.25e7"));
    KHU_CHECK(!format::is_numeric("twelve"));
    KHU_CHECK(!format::is_numeric("12 "));
    KHU_CHECK(!format::is_numeric(""));
}

KHU_TEST(format, classifies_bytes_as_ascii_only) {
    KHU_CHECK(format::is_digit_byte('7'));
    KHU_CHECK(!format::is_digit_byte('a'));
    KHU_CHECK(format::is_letter_byte('a'));
    KHU_CHECK(format::is_letter_byte('Z'));
    KHU_CHECK(!format::is_letter_byte(0xe9));  // a byte of a UTF-8 code point
    KHU_CHECK(format::is_whitespace_byte(' '));
    KHU_CHECK(format::is_whitespace_byte('\n'));
    KHU_CHECK(!format::is_whitespace_byte('.'));
    KHU_CHECK_EQ(static_cast<int>(format::to_upper_byte('a')), static_cast<int>('A'));
    KHU_CHECK_EQ(static_cast<int>(format::to_upper_byte('1')), static_cast<int>('1'));
    KHU_CHECK_EQ(static_cast<int>(format::to_lower_byte('Z')), static_cast<int>('z'));
    KHU_CHECK_EQ(static_cast<int>(format::to_lower_byte(0xe9)), 0xe9);
}
