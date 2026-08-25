// Phase 3 acceptance: methods, calls and control flow execute end to end.
#include "test_harness.h"

#include <string>

#include "bytecode/disassembler.h"
#include "bytecode/module.h"
#include "compiler.h"
#include "vm/vm.h"

namespace {

struct RunResult {
    bool compiled = false;
    bool ran = false;
    std::string output;
    std::string diagnostics;
    std::string runtime_error;
};

// Compiles a source string and runs its entry point, capturing io.* output.
RunResult run_source(const std::string& source, const std::string& input = {}) {
    RunResult result;
    khu::Compiler compiler;
    std::uint32_t file = compiler.add_buffer("test.khu", source);

    khu::bytecode::Module module;
    result.compiled = compiler.compile(file, module);
    result.diagnostics = compiler.diagnostics().render();
    if (!result.compiled) return result;

    khu::vm::Vm vm(module);
    vm.set_output_sink(&result.output);
    if (!input.empty()) vm.set_input(input);
    result.ran = vm.run();
    result.runtime_error = vm.error();
    return result;
}

// Wraps `body` in a class with a main, so the tests stay readable.
std::string program(const std::string& body) {
    return "public class T {\n    func main() {\n" + body + "\n    }\n}\n";
}

void check_output(const std::string& source, const std::string& expected) {
    RunResult result = run_source(source);
    if (!result.compiled) {
        KHU_FAIL("did not compile:\n" + result.diagnostics);
        return;
    }
    if (!result.ran) {
        KHU_FAIL("runtime error:\n" + result.runtime_error);
        return;
    }
    KHU_CHECK_EQ(result.output, expected);
}

}  // namespace

KHU_TEST(vm, runs_arithmetic_across_widths) {
    check_output(program("        io.printLine(khuStdMath.add(20, 22));\n"
                         "        io.printLine(khuStdMath.subtract(10, 25));\n"
                         "        io.printLine(khuStdMath.multiply(6, 7));\n"
                         "        io.printLine(khuStdMath.divide(45, 6));\n"
                         "        io.printLine(khuStdMath.remainder(45, 7));"),
                 "42\n-15\n42\n7\n3\n");
}

KHU_TEST(vm, wraps_on_overflow) {
    // Overflow wraps by default (KHU-PLAN.md, Type rules).
    check_output(program("        uint8 a = 255;\n"
                         "        io.printLine(khuStdMath.add(a, 1));\n"
                         "        int8 b = 127;\n"
                         "        io.printLine(khuStdMath.add(b, 1));\n"
                         "        int32 c = 2147483647;\n"
                         "        io.printLine(khuStdMath.add(c, 1));"),
                 "0\n-128\n-2147483648\n");
}

KHU_TEST(vm, evaluates_operators_and_precedence) {
    check_output(program("        io.printLine(1 + 2 * 3 - 4);\n"
                         "        io.printLine((1 + 2) * 3);\n"
                         "        io.printLine(-7 / 2);\n"
                         "        io.printLine(-7 % 2);\n"
                         "        io.printLine(1 << 10);\n"
                         "        io.printLine(-16 >> 2);\n"
                         "        io.printLine(12 & 10);\n"
                         "        io.printLine(12 | 10);\n"
                         "        io.printLine(12 ^ 10);\n"
                         "        io.printLine(~0);"),
                 "3\n9\n-3\n-1\n1024\n-4\n8\n14\n6\n-1\n");
}

KHU_TEST(vm, evaluates_unsigned_shifts_and_division_separately) {
    check_output(program("        uint32 a = 4294967280;\n"
                         "        io.printLine(a >> 2);\n"
                         "        io.printLine(a / 3);\n"
                         "        io.printLine(a % 7);"),
                 "1073741820\n1431655760\n2\n");
}

KHU_TEST(vm, converts_between_widths_explicitly) {
    check_output(program("        int32 small = -5;\n"
                         "        io.printLine(khuStdMath.convertTo(int64, small));\n"
                         "        io.printLine(khuStdMath.convertTo(uint8, 300));\n"
                         "        io.printLine(khuStdMath.convertTo(int32, 3.99));\n"
                         "        io.printLine(khuStdMath.convertTo(dfloat, 7));"),
                 "-5\n44\n3\n7\n");
}

KHU_TEST(vm, evaluates_comparisons_and_short_circuit_logic) {
    check_output(program("        io.printLine(1 < 2);\n"
                         "        io.printLine(2 <= 2);\n"
                         "        io.printLine(3 > 4);\n"
                         "        io.printLine(3 == 3);\n"
                         "        io.printLine(3 != 3);\n"
                         "        io.printLine(true && false);\n"
                         "        io.printLine(false || true);\n"
                         "        io.printLine(!true);"),
                 "true\ntrue\nfalse\ntrue\nfalse\nfalse\ntrue\nfalse\n");
}

KHU_TEST(vm, short_circuits_without_evaluating_the_other_side) {
    // The right operand must not run when the left already decides.
    check_output(
        "public class T {\n"
        "    func loud() -> bool {\n"
        "        io.printLine(\"evaluated\");\n"
        "        return true;\n"
        "    }\n"
        "    func main() {\n"
        "        io.printLine(false && this.loud());\n"
        "        io.printLine(true || this.loud());\n"
        "        io.printLine(true && this.loud());\n"
        "    }\n"
        "}\n",
        "false\ntrue\nevaluated\ntrue\n");
}

KHU_TEST(vm, executes_control_flow) {
    check_output(program("        int32 total = 0;\n"
                         "        int32 i = 0;\n"
                         "        while (i < 5) {\n"
                         "            if (i % 2 == 0) {\n"
                         "                total = khuStdMath.add(total, i);\n"
                         "            } else {\n"
                         "                total = khuStdMath.subtract(total, i);\n"
                         "            }\n"
                         "            i = khuStdMath.add(i, 1);\n"
                         "        }\n"
                         "        io.printLine(total);"),
                 "2\n");
}

KHU_TEST(vm, calls_methods_and_returns_values) {
    check_output(
        "public class T {\n"
        "    func twice(int32 x) -> int32 { return khuStdMath.multiply(x, 2); }\n"
        "    method secret(int32 x) -> int32 { return khuStdMath.add(x, 1); }\n"
        "    func main() {\n"
        "        io.printLine(this.twice(21));\n"
        "        io.printLine(this.secret(41));\n"
        "        io.printLine(this.twice(this.secret(2)));\n"
        "    }\n"
        "}\n",
        "42\n42\n6\n");
}

KHU_TEST(vm, supports_recursion) {
    check_output(
        "public class T {\n"
        "    func fib(int32 n) -> int32 {\n"
        "        if (n < 2) { return n; }\n"
        "        return khuStdMath.add(this.fib(khuStdMath.subtract(n, 1)),\n"
        "                              this.fib(khuStdMath.subtract(n, 2)));\n"
        "    }\n"
        "    func main() { io.printLine(this.fib(20)); }\n"
        "}\n",
        "6765\n");
}

KHU_TEST(vm, prints_strings_and_bools) {
    check_output(program("        io.print(\"a\");\n"
                         "        io.print(\"b\");\n"
                         "        io.printLine(\"\");\n"
                         "        io.printLine(true);\n"
                         "        io.printLine(1.5);"),
                 "ab\ntrue\n1.5\n");
}

KHU_TEST(vm, reads_input_lines) {
    RunResult result = run_source(program("        io.printLine(io.readLine());\n"
                                          "        io.printLine(io.readLine());"),
                                  "first\nsecond\n");
    KHU_CHECK(result.compiled);
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("first\nsecond\n"));
}

KHU_TEST(vm, traps_on_division_by_zero_with_a_source_location) {
    RunResult result = run_source(program("        int32 zero = 0;\n"
                                          "        io.printLine(khuStdMath.divide(1, zero));"));
    KHU_CHECK(result.compiled);
    KHU_CHECK(!result.ran);
    KHU_CHECK_CONTAINS(result.runtime_error, "runtime error: division by zero");
    KHU_CHECK_CONTAINS(result.runtime_error, "test.khu:4:");
    KHU_CHECK_CONTAINS(result.runtime_error, "at T.main");
}

KHU_TEST(vm, traps_on_runaway_recursion) {
    RunResult result = run_source(
        "public class T {\n"
        "    func spin(int32 n) -> int32 { return this.spin(n); }\n"
        "    func main() { io.printLine(this.spin(1)); }\n"
        "}\n");
    KHU_CHECK(result.compiled);
    KHU_CHECK(!result.ran);
    KHU_CHECK_CONTAINS(result.runtime_error, "call depth limit");
}

KHU_TEST(vm, reports_object_opcodes_as_a_later_phase) {
    // Materialization needs runtime objects; the message has to say so plainly
    // rather than crashing.
    RunResult result = run_source(
        "public class Node { public int32 v = 0; }\n"
        "public class T {\n"
        "    func main() { Node n = Node(); }\n"
        "}\n");
    KHU_CHECK(result.compiled);
    KHU_CHECK(!result.ran);
    KHU_CHECK_CONTAINS(result.runtime_error, "needs runtime objects");
}

KHU_TEST(codegen, emits_a_line_table_for_stack_traces) {
    khu::Compiler compiler;
    std::uint32_t file = compiler.add_buffer("lines.khu", program("        io.printLine(1);\n"
                                                                  "        io.printLine(2);"));
    khu::bytecode::Module module;
    KHU_CHECK(compiler.compile(file, module));
    if (compiler.diagnostics().has_errors()) return;

    const khu::bytecode::MethodEntry& main = module.methods[0];
    KHU_CHECK(main.lines.size() >= 2);
    // Offsets increase, and each maps to a real line.
    for (std::size_t i = 1; i < main.lines.size(); ++i) {
        KHU_CHECK(main.lines[i].offset >= main.lines[i - 1].offset);
        KHU_CHECK(main.lines[i].line > 0);
    }
}

KHU_TEST(codegen, records_class_layout_and_reference_maps_in_the_image) {
    khu::Compiler compiler;
    std::uint32_t file = compiler.add_buffer(
        "layout.khu",
        "public class Manual {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "}\n"
        "public class Holder {\n"
        "    public int32 scalar = 0;\n"
        "    private Manual owned = null;\n"
        "    func main() { }\n"
        "}\n");
    khu::bytecode::Module module;
    KHU_CHECK(compiler.compile(file, module));
    if (!compiler.diagnostics().has_errors()) {
        KHU_CHECK_EQ(module.classes.size(), static_cast<std::size_t>(2));
        const khu::bytecode::ClassEntry& manual = module.classes[0];
        KHU_CHECK_EQ(manual.flags, static_cast<std::uint32_t>(khu::bytecode::kClassManual));

        const khu::bytecode::ClassEntry& holder = module.classes[1];
        KHU_CHECK_EQ(holder.flags, static_cast<std::uint32_t>(khu::bytecode::kClassGc));
        KHU_CHECK_EQ(holder.fields.size(), static_cast<std::size_t>(2));
        KHU_CHECK_EQ(holder.fields[0].ref_kind, static_cast<std::uint8_t>(khu::bytecode::kRefRaw));
        KHU_CHECK_EQ(holder.fields[0].is_public, static_cast<std::uint8_t>(1));
        // A slot holding a manual class is a manual-ref: the collector pins it
        // rather than tracing it.
        KHU_CHECK_EQ(holder.fields[1].ref_kind,
                     static_cast<std::uint8_t>(khu::bytecode::kRefManual));
        KHU_CHECK_EQ(holder.fields[1].is_public, static_cast<std::uint8_t>(0));
    }
}

KHU_TEST(codegen, emits_materialize_with_the_resolved_strategy) {
    khu::Compiler compiler;
    std::uint32_t file = compiler.add_buffer(
        "alloc.khu",
        "public class Node { public int32 v = 0; }\n"
        "public class T {\n"
        "    func main() {\n"
        "        Node a = Node();\n"
        "        Node b = Node(manual);\n"
        "    }\n"
        "}\n");
    khu::bytecode::Module module;
    KHU_CHECK(compiler.compile(file, module));
    if (compiler.diagnostics().has_errors()) return;

    std::string listing;
    for (const khu::bytecode::MethodEntry& method : module.methods) {
        if (module.string_at(method.name) == "main") {
            listing = khu::bytecode::disassemble_method(module, method);
        }
    }
    KHU_CHECK_CONTAINS(listing, "materialize     0, standard");
    KHU_CHECK_CONTAINS(listing, "materialize     0, manual");
}
