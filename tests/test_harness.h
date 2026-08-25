// Minimal self-contained unit-test harness.
//
// No external dependencies by design (KHU-PLAN.md, Testing): a test is a free
// function registered at static-init time; `khudra_unit_tests` runs them all
// and reports failures with file:line.
//
//   KHU_TEST(lexer, scans_keywords) {
//       KHU_CHECK_EQ(scan("class").kind, TokenKind::KwClass);
//   }
#ifndef KHU_TESTS_TEST_HARNESS_H
#define KHU_TESTS_TEST_HARNESS_H

#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace khu::test {

struct TestCase {
    const char* suite;
    const char* name;
    void (*run)();
};

// The registry is a function-local static so registration order across
// translation units is well defined.
std::vector<TestCase>& registry();

// Set by KHU_CHECK_* when an assertion inside the running test fails.
struct Failure {
    std::string message;
};

std::vector<Failure>& current_failures();

void record_failure(const char* file, int line, const std::string& message);

struct Registrar {
    Registrar(const char* suite, const char* name, void (*run)()) {
        registry().push_back(TestCase{suite, name, run});
    }
};

template <typename T>
std::string show(const T& value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

inline std::string show(const std::string& value) { return "\"" + value + "\""; }
inline std::string show(std::string_view value) { return "\"" + std::string(value) + "\""; }
inline std::string show(const char* value) { return value ? "\"" + std::string(value) + "\"" : "null"; }
inline std::string show(bool value) { return value ? "true" : "false"; }

// Runs the whole suite. `filter` matches "suite" or "suite.name" prefixes.
int run_all(std::string_view filter);

}  // namespace khu::test

#define KHU_TEST(suite_name, test_name)                                                \
    static void khu_test_##suite_name##_##test_name();                                 \
    static ::khu::test::Registrar khu_test_registrar_##suite_name##_##test_name(       \
        #suite_name, #test_name, &khu_test_##suite_name##_##test_name);                \
    static void khu_test_##suite_name##_##test_name()

#define KHU_FAIL(message) ::khu::test::record_failure(__FILE__, __LINE__, (message))

#define KHU_CHECK(condition)                                                           \
    do {                                                                               \
        if (!(condition)) KHU_FAIL("expected: " #condition);                           \
    } while (false)

#define KHU_CHECK_EQ(actual, expected)                                                 \
    do {                                                                               \
        auto khu_actual = (actual);                                                    \
        auto khu_expected = (expected);                                                \
        if (!(khu_actual == khu_expected)) {                                           \
            KHU_FAIL(std::string(#actual) + " == " + #expected + "\n      actual:   " + \
                     ::khu::test::show(khu_actual) + "\n      expected: " +            \
                     ::khu::test::show(khu_expected));                                 \
        }                                                                              \
    } while (false)

#define KHU_CHECK_NE(actual, unexpected)                                               \
    do {                                                                               \
        auto khu_actual = (actual);                                                    \
        auto khu_unexpected = (unexpected);                                            \
        if (khu_actual == khu_unexpected) {                                            \
            KHU_FAIL(std::string(#actual) + " != " + #unexpected + "\n      both:     " + \
                     ::khu::test::show(khu_actual));                                   \
        }                                                                              \
    } while (false)

// String containment -- the workhorse for diagnostic-text assertions.
#define KHU_CHECK_CONTAINS(haystack, needle)                                           \
    do {                                                                               \
        std::string khu_haystack{(haystack)};                                          \
        std::string khu_needle{(needle)};                                              \
        if (khu_haystack.find(khu_needle) == std::string::npos) {                      \
            KHU_FAIL(std::string(#haystack) + " should contain " +                     \
                     ::khu::test::show(khu_needle) + "\n      actual:   " +            \
                     ::khu::test::show(khu_haystack));                                 \
        }                                                                              \
    } while (false)

#endif  // KHU_TESTS_TEST_HARNESS_H
