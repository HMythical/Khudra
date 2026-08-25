#include "test_harness.h"

#include <cstdio>
#include <cstring>

namespace khu::test {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

std::vector<Failure>& current_failures() {
    static std::vector<Failure> failures;
    return failures;
}

void record_failure(const char* file, int line, const std::string& message) {
    std::string text = std::string(file) + ":" + std::to_string(line) + ": " + message;
    current_failures().push_back(Failure{std::move(text)});
}

static bool matches(const TestCase& test, std::string_view filter) {
    if (filter.empty()) return true;
    std::string full = std::string(test.suite) + "." + test.name;
    return full.compare(0, filter.size(), filter) == 0;
}

int run_all(std::string_view filter) {
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;

    for (const TestCase& test : registry()) {
        if (!matches(test, filter)) {
            ++skipped;
            continue;
        }
        current_failures().clear();
        test.run();
        if (current_failures().empty()) {
            ++passed;
            std::printf("  ok   %s.%s\n", test.suite, test.name);
        } else {
            ++failed;
            std::printf("  FAIL %s.%s\n", test.suite, test.name);
            for (const Failure& failure : current_failures()) {
                std::printf("    %s\n", failure.message.c_str());
            }
        }
    }

    std::printf("\n%zu passed, %zu failed", passed, failed);
    if (skipped) std::printf(", %zu filtered out", skipped);
    std::printf("\n");
    return failed == 0 ? 0 : 1;
}

}  // namespace khu::test

int main(int argc, char** argv) {
    std::string_view filter = argc > 1 ? argv[1] : "";
    std::printf("khudra unit tests\n");
    return khu::test::run_all(filter);
}
