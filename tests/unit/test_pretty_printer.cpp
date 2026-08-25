// Phase 1 acceptance: example.khu lexes, parses and pretty-prints losslessly.
//
// "Losslessly" here means the printer is structure-preserving and text-stable:
//   parse(src) -> print -> parse -> print
// reparses to an equal tree and reproduces byte-identical text. Comments and
// original whitespace are not preserved -- the printer is a canonical formatter.
#include "test_harness.h"

#include <string>

#include "compiler.h"
#include "parser/pretty_printer.h"
#include "util/file.h"

using khu::Compiler;
using khu::parser::PrettyPrinter;

namespace {

struct RoundTrip {
    bool parsed_cleanly = false;
    bool reparsed_cleanly = false;
    bool text_stable = false;
    bool tree_equal = false;
    std::string first;
    std::string second;
    std::string errors;
};

RoundTrip round_trip(const std::string& source) {
    RoundTrip result;

    Compiler first_pass;
    std::uint32_t file = first_pass.add_buffer("input.khu", source);
    khu::ast::CompilationUnit* first_unit = first_pass.parse(file);
    result.parsed_cleanly = !first_pass.diagnostics().has_errors();
    result.errors = first_pass.diagnostics().render();
    if (!result.parsed_cleanly) return result;

    PrettyPrinter first_printer;
    result.first = first_printer.print(*first_unit);

    Compiler second_pass;
    std::uint32_t reprinted = second_pass.add_buffer("reprinted.khu", result.first);
    khu::ast::CompilationUnit* second_unit = second_pass.parse(reprinted);
    result.reparsed_cleanly = !second_pass.diagnostics().has_errors();
    result.errors += second_pass.diagnostics().render();
    if (!result.reparsed_cleanly) return result;

    PrettyPrinter second_printer;
    result.second = second_printer.print(*second_unit);
    result.text_stable = result.first == result.second;
    result.tree_equal = khu::parser::ast_equal(*first_unit, *second_unit);
    return result;
}

void check_round_trip(const std::string& source) {
    RoundTrip result = round_trip(source);
    if (!result.parsed_cleanly) {
        KHU_FAIL("source did not parse cleanly:\n" + result.errors);
        return;
    }
    if (!result.reparsed_cleanly) {
        KHU_FAIL("pretty-printed output did not reparse:\n" + result.errors + "\n--- printed ---\n" +
                 result.first);
        return;
    }
    if (!result.text_stable) {
        KHU_FAIL("pretty printing is not idempotent\n--- first ---\n" + result.first +
                 "\n--- second ---\n" + result.second);
    }
    if (!result.tree_equal) {
        KHU_FAIL("reparsed tree differs from the original\n--- printed ---\n" + result.first);
    }
}

std::string read_project_file(const char* relative) {
    std::string contents;
    std::string path = std::string(KHU_PROJECT_ROOT) + "/" + relative;
    khu::util::FileError status = khu::util::read_file(path, contents);
    if (status != khu::util::FileError::None) return {};
    return contents;
}

}  // namespace

KHU_TEST(pretty_printer, round_trips_the_reference_example) {
    std::string source = read_project_file("example.khu");
    KHU_CHECK(!source.empty());
    if (source.empty()) return;
    check_round_trip(source);
}

KHU_TEST(pretty_printer, prints_the_reference_example_readably) {
    std::string source = read_project_file("example.khu");
    if (source.empty()) return;

    Compiler compiler;
    std::uint32_t file = compiler.add_buffer("example.khu", source);
    khu::ast::CompilationUnit* unit = compiler.parse(file);
    KHU_CHECK(!compiler.diagnostics().has_errors());

    PrettyPrinter printer;
    std::string text = printer.print(*unit);

    KHU_CHECK_CONTAINS(text, "bring khu::stdlib;");
    KHU_CHECK_CONTAINS(text, "public class Example {");
    KHU_CHECK_CONTAINS(
        text, "public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setStandard();");
    KHU_CHECK_CONTAINS(text, "private Array char = null;");
    KHU_CHECK_CONTAINS(text, "private Procedures {");
    KHU_CHECK_CONTAINS(text, "public Example() {");
    KHU_CHECK_CONTAINS(text, "func math(int x, int y) -> int32 {");
    KHU_CHECK_CONTAINS(text, "private i32 result = khuStdMath.add(x, y);");
    KHU_CHECK_CONTAINS(text, "method foo(int x, float y, dfloat z) -> int64 {");
    KHU_CHECK_CONTAINS(text, "khuStdMath.convertTo(int64, y);");
}

KHU_TEST(pretty_printer, round_trips_control_flow_and_memory_syntax) {
    check_round_trip(
        "bring khu::stdlib;\n"
        "bring khu::io;\n"
        "\n"
        "public class Node extends Base {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "    private Node next = null;\n"
        "    public uint64 count = 0;\n"
        "\n"
        "    public Procedures(int32 seed) {\n"
        "        this.count = seed;\n"
        "    }\n"
        "\n"
        "    public Node() { }\n"
        "\n"
        "    func walk(int32 limit) -> void {\n"
        "        Node other = Node(manual, 1);\n"
        "        Node third = Node(standard);\n"
        "        while (limit > 0) {\n"
        "            if (limit == 1) {\n"
        "                free(other);\n"
        "            } else if (limit == 2) {\n"
        "                dispose(third);\n"
        "            } else {\n"
        "                limit = limit - 1;\n"
        "            }\n"
        "        }\n"
        "        return;\n"
        "    }\n"
        "}\n");
}

KHU_TEST(pretty_printer, preserves_parentheses_where_they_matter) {
    Compiler compiler;
    std::uint32_t file = compiler.add_buffer(
        "p.khu",
        "public class A {\n"
        "    func go() {\n"
        "        int32 a = (1 + 2) * 3;\n"
        "        int32 b = 1 - (2 - 3);\n"
        "        int32 c = -(1 + 2);\n"
        "        bool d = !(a == b);\n"
        "        int32 e = (a & b) == c;\n"
        "    }\n"
        "}\n");
    khu::ast::CompilationUnit* unit = compiler.parse(file);
    KHU_CHECK(!compiler.diagnostics().has_errors());

    PrettyPrinter printer;
    std::string text = printer.print(*unit);
    KHU_CHECK_CONTAINS(text, "int32 a = (1 + 2) * 3;");
    KHU_CHECK_CONTAINS(text, "int32 b = 1 - (2 - 3);");
    KHU_CHECK_CONTAINS(text, "int32 c = -(1 + 2);");
    KHU_CHECK_CONTAINS(text, "bool d = !(a == b);");
    KHU_CHECK_CONTAINS(text, "int32 e = (a & b) == c;");
}

KHU_TEST(pretty_printer, round_trips_literals) {
    check_round_trip(
        "public class L {\n"
        "    public int32 dec = 1000;\n"
        "    public int32 hex = 255;\n"
        "    public float f = 1.5;\n"
        "    public dfloat g = 2.0;\n"
        "    public dfloat tiny = 0.001;\n"
        "    public string s = \"a\\nb\\t\\\"c\\\\\";\n"
        "    public bool t = true;\n"
        "    public bool f2 = false;\n"
        "    public Array none = null;\n"
        "}\n");
}

KHU_TEST(pretty_printer, preserves_type_alias_spelling) {
    Compiler compiler;
    std::uint32_t file = compiler.add_buffer("t.khu",
                                             "public class A {\n"
                                             "    public i32 a = 0;\n"
                                             "    public int32 b = 0;\n"
                                             "    public u8 c = 0;\n"
                                             "    public byte d = 0;\n"
                                             "    private *byte raw = null;\n"
                                             "}\n");
    khu::ast::CompilationUnit* unit = compiler.parse(file);
    KHU_CHECK(!compiler.diagnostics().has_errors());

    PrettyPrinter printer;
    std::string text = printer.print(*unit);
    KHU_CHECK_CONTAINS(text, "public i32 a = 0;");
    KHU_CHECK_CONTAINS(text, "public int32 b = 0;");
    KHU_CHECK_CONTAINS(text, "public u8 c = 0;");
    KHU_CHECK_CONTAINS(text, "public byte d = 0;");
    KHU_CHECK_CONTAINS(text, "private *byte raw = null;");
}
