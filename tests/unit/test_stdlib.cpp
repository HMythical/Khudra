// Phase 7: the standard library is Khudra source, embedded in the toolchain.
#include "test_harness.h"

#include <string>

#include "bytecode/module.h"
#include "compiler.h"
#include "sema/stdlib_sources.h"
#include "vm/vm.h"

namespace {

struct StdRun {
    bool compiled = false;
    bool ran = false;
    std::string output;
    std::string diagnostics;
    std::string runtime_error;
};

StdRun run_source(const std::string& source, const std::string& input = {}) {
    StdRun result;
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

void check_output(const std::string& source, const std::string& expected) {
    StdRun result = run_source(source);
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

KHU_TEST(stdlib, is_embedded_and_parses_cleanly) {
    KHU_CHECK(khu::sema::stdlib_file_count() >= 4);

    khu::Compiler compiler;
    KHU_CHECK(compiler.load_stdlib());
    KHU_CHECK_EQ(compiler.diagnostics().render(), std::string(""));

    // Every file is named so diagnostics point somewhere real.
    bool saw_math = false;
    bool saw_io = false;
    bool saw_runtime = false;
    bool saw_core = false;
    for (std::size_t i = 0; i < khu::sema::stdlib_file_count(); ++i) {
        std::string path = khu::sema::stdlib_files()[i].path;
        if (path == "lib/math.khu") saw_math = true;
        if (path == "lib/io.khu") saw_io = true;
        if (path == "lib/khu.khu") saw_runtime = true;
        if (path == "lib/core.khu") saw_core = true;
    }
    KHU_CHECK(saw_math);
    KHU_CHECK(saw_io);
    KHU_CHECK(saw_runtime);
    KHU_CHECK(saw_core);
}

KHU_TEST(stdlib, declares_the_full_integer_width_matrix) {
    // Every width, and only matching widths.
    check_output(
        "public class T {\n"
        "    func main() {\n"
        "        int8 a = 1;\n"
        "        io.printLine(khuStdMath.add(a, 2));\n"
        "        int16 b = 300;\n"
        "        io.printLine(khuStdMath.subtract(b, 44));\n"
        "        int32 c = 7;\n"
        "        io.printLine(khuStdMath.multiply(c, 6));\n"
        "        int64 d = 100;\n"
        "        io.printLine(khuStdMath.divide(d, 4));\n"
        "        uint8 e = 200;\n"
        "        io.printLine(khuStdMath.remainder(e, 7));\n"
        "        uint16 f = 1000;\n"
        "        io.printLine(khuStdMath.add(f, 24));\n"
        "        uint32 g = 5;\n"
        "        io.printLine(khuStdMath.multiply(g, 5));\n"
        "        uint64 h = 9;\n"
        "        io.printLine(khuStdMath.subtract(h, 4));\n"
        "        float i = 1.5;\n"
        "        io.printLine(khuStdMath.add(i, 0.5));\n"
        "        dfloat j = 2.5;\n"
        "        io.printLine(khuStdMath.divide(j, 0.5));\n"
        "    }\n"
        "}\n",
        "3\n256\n42\n25\n4\n1024\n25\n5\n2\n5\n");
}

KHU_TEST(stdlib, has_no_remainder_for_floats) {
    // The matrix declares remainder for integers only, so a float call has no
    // match rather than silently doing something.
    StdRun result = run_source(
        "public class T {\n"
        "    func main() {\n"
        "        dfloat a = 5.0;\n"
        "        io.printLine(khuStdMath.remainder(a, 2.0));\n"
        "    }\n"
        "}\n");
    KHU_CHECK(!result.compiled);
    KHU_CHECK_CONTAINS(result.diagnostics, "no declaration of 'khuStdMath.remainder' matches");
}

KHU_TEST(stdlib, prints_every_builtin_type) {
    check_output(
        "public class T {\n"
        "    func main() {\n"
        "        io.print(\"text \");\n"
        "        io.print(true);\n"
        "        io.print(\" \");\n"
        "        io.print(42);\n"
        "        io.print(\" \");\n"
        "        io.printLine(1.25);\n"
        "    }\n"
        "}\n",
        "text true 42 1.25\n");
}

KHU_TEST(stdlib, reads_lines) {
    StdRun result = run_source(
        "public class T {\n"
        "    func main() {\n"
        "        string first = io.readLine();\n"
        "        string second = io.readLine();\n"
        "        io.printLine(second);\n"
        "        io.printLine(first);\n"
        "    }\n"
        "}\n",
        "alpha\nbeta\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("beta\nalpha\n"));
}

KHU_TEST(stdlib, exposes_the_khu_runtime_namespace) {
    // The members example.khu uses resolve and run.
    check_output(
        "public class T {\n"
        "    private Array descriptor = null;\n"
        "    private Procedures {\n"
        "        khu.stdlibLoadObject();\n"
        "    }\n"
        "    public T() {\n"
        "        this.descriptor = khu.getType();\n"
        "        this.descriptor = khu.LoadRuntimeType();\n"
        "    }\n"
        "    func main() {\n"
        "        io.printLine(this.descriptor == null);\n"
        "    }\n"
        "}\n",
        "true\n");
}

// The clock cannot have a golden, so what is asserted is what is actually true
// of it: that the monotonic clock does not go backwards, and that nowString has
// the shape it promises.
KHU_TEST(stdlib, the_clock_is_monotonic_and_formatted) {
    StdRun result = run_source(
        "bring khu::stdlib;\n\n"
        "public class T {\n"
        "    func main() {\n"
        "        int64 first = khuStdTime.monotonicNanos();\n"
        "        khuStdTime.sleep(2);\n"
        "        int64 second = khuStdTime.monotonicNanos();\n"
        "        io.printLine(second >= first);\n"
        "        io.printLine(khuStdTime.nowMillis() > 0);\n"
        "        io.printLine(khuStdTime.nowNanos() > 0);\n"
        "        string stamp = khuStdTime.nowString(true);\n"
        "        io.printLine(khuStdString.length(stamp));\n"
        "        io.printLine(khuStdString.charAt(stamp, 4));\n"
        "        io.printLine(khuStdString.charAt(stamp, 10));\n"
        "        io.printLine(khuStdString.endsWith(stamp, \"Z\"));\n"
        "        io.printLine(khuStdString.endsWith(khuStdTime.nowString(false), \"Z\"));\n"
        "        io.printLine(khuStdConv.canParseInt32(khuStdString.substring(stamp, 0, 4)));\n"
        "    }\n"
        "}\n");
    if (!result.compiled) {
        KHU_FAIL("did not compile:\n" + result.diagnostics);
        return;
    }
    KHU_CHECK(result.ran);
    // 20 bytes: `2026-08-30T14:03:07Z`, with '-' at 4 and 'T' at 10.
    KHU_CHECK_EQ(result.output, std::string("true\ntrue\ntrue\n20\n45\n84\ntrue\nfalse\ntrue\n"));
}

KHU_TEST(stdlib, rejects_a_native_member_on_a_class) {
    StdRun result = run_source(
        "public class T {\n"
        "    native func nope();\n"
        "    func main() { }\n"
        "}\n");
    KHU_CHECK(!result.compiled);
    KHU_CHECK_CONTAINS(result.diagnostics, "only a namespace member can be declared native");
}

KHU_TEST(stdlib, rejects_an_unbound_native_declaration) {
    StdRun result = run_source(
        "public namespace mine {\n"
        "    native func doesNotExist(int32 x) -> int32;\n"
        "}\n"
        "public class T { func main() { } }\n");
    KHU_CHECK(!result.compiled);
    KHU_CHECK_CONTAINS(result.diagnostics, "no runtime binding for 'mine.doesNotExist'");
}

KHU_TEST(stdlib, rejects_non_native_members_in_a_namespace) {
    StdRun with_body = run_source(
        "public namespace mine {\n"
        "    func hasABody() { }\n"
        "}\n"
        "public class T { func main() { } }\n");
    KHU_CHECK(!with_body.compiled);
    KHU_CHECK_CONTAINS(with_body.diagnostics, "must be declared native");

    StdRun with_field = run_source(
        "public namespace mine {\n"
        "    public int32 state = 0;\n"
        "}\n"
        "public class T { func main() { } }\n");
    KHU_CHECK(!with_field.compiled);
    KHU_CHECK_CONTAINS(with_field.diagnostics, "a namespace can only declare native functions");
}

KHU_TEST(stdlib, keeps_user_names_from_colliding_with_it) {
    StdRun result = run_source(
        "public class io { }\n"
        "public class T { func main() { } }\n");
    KHU_CHECK(!result.compiled);
    KHU_CHECK_CONTAINS(result.diagnostics,
                       "'io' is a built-in namespace and cannot be declared as a class");
}
