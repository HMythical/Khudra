// Phase 0: source locations and diagnostics must carry file:line:col.
#include "test_harness.h"

#include "diag/diagnostic.h"
#include "diag/source_manager.h"

using khu::diag::DiagnosticEngine;
using khu::diag::Severity;
using khu::diag::SourceLocation;
using khu::diag::SourceManager;

KHU_TEST(source_manager, maps_locations_back_to_lines) {
    SourceManager sources;
    std::uint32_t file = sources.add_buffer("example.khu",
                                            "bring khu::stdlib;\n"
                                            "\n"
                                            "public class Example{\n");

    KHU_CHECK_EQ(std::string(sources.path(file)), std::string("example.khu"));
    KHU_CHECK_EQ(std::string(sources.line_text(SourceLocation{file, 1, 1, 0})),
                 std::string("bring khu::stdlib;"));
    KHU_CHECK_EQ(std::string(sources.line_text(SourceLocation{file, 3, 1, 0})),
                 std::string("public class Example{"));
    KHU_CHECK(sources.line_text(SourceLocation{file, 99, 1, 0}).empty());
    KHU_CHECK_EQ(sources.format(SourceLocation{file, 3, 14, 0}),
                 std::string("example.khu:3:14"));
}

KHU_TEST(source_manager, tolerates_crlf_sources) {
    SourceManager sources;
    std::uint32_t file = sources.add_buffer("crlf.khu", "public class A{\r\n}\r\n");
    KHU_CHECK_EQ(std::string(sources.line_text(SourceLocation{file, 1, 1, 0})),
                 std::string("public class A{"));
}

KHU_TEST(diagnostics, collects_multiple_and_counts_by_severity) {
    SourceManager sources;
    std::uint32_t file = sources.add_buffer("a.khu", "public class A{}\n");
    DiagnosticEngine diagnostics(sources);

    KHU_CHECK(!diagnostics.has_errors());
    diagnostics.warning(SourceLocation{file, 1, 1, 0}, "unused import");
    diagnostics.error(SourceLocation{file, 1, 14, 13}, "expected a class body");

    KHU_CHECK_EQ(diagnostics.error_count(), static_cast<std::size_t>(1));
    KHU_CHECK_EQ(diagnostics.warning_count(), static_cast<std::size_t>(1));
    KHU_CHECK_EQ(diagnostics.size(), static_cast<std::size_t>(2));
    KHU_CHECK(diagnostics.has_errors());

    diagnostics.clear();
    KHU_CHECK(!diagnostics.has_errors());
    KHU_CHECK_EQ(diagnostics.size(), static_cast<std::size_t>(0));
}

KHU_TEST(diagnostics, renders_location_source_line_and_caret) {
    SourceManager sources;
    std::uint32_t file = sources.add_buffer("example.khu", "public class Example{\n");
    DiagnosticEngine diagnostics(sources);

    diagnostics.error(SourceLocation{file, 1, 14, 13}, "class 'Example' has no body")
        .note(SourceLocation{file, 1, 1, 0}, "declared here");

    std::string text = diagnostics.render();
    KHU_CHECK_CONTAINS(text, "example.khu:1:14: error: class 'Example' has no body");
    KHU_CHECK_CONTAINS(text, "public class Example{");
    KHU_CHECK_CONTAINS(text, "^");
    KHU_CHECK_CONTAINS(text, "example.khu:1:1: note: declared here");
}

KHU_TEST(diagnostics, renders_without_a_location) {
    SourceManager sources;
    DiagnosticEngine diagnostics(sources);
    diagnostics.error(SourceLocation{}, "no input file");
    KHU_CHECK_CONTAINS(diagnostics.render(), "khudra: error: no input file");
}
