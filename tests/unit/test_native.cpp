// The native backend: what the emitter writes, and that it agrees with the VM.
//
// The differential tests here run a program twice in one process -- once
// through KhudraVm, once through the emitted C -- and compare the output and
// the trap text. That is the property the whole backend exists to keep
// (docs/native.md, section 5): a program that printed byte-identical output
// under the VM must print byte-identical output under both native modes.
//
// They need a host C compiler. On a machine without one the backend's answer is
// NoCompiler and the test reports that it could not run rather than failing --
// the same fallback `khudra run --native` makes.
#include "test_harness.h"

#include <string>

#include "bytecode/module.h"
#include "bytecode/opcode.h"
#include "bytecode/verifier.h"
#include "cemit/cemit.h"
#include "compiler.h"
#include "host/native_backend.h"
#include "host/toolchain.h"
#include "util/file.h"
#include "vm/vm.h"

namespace {

bool compile_source(const std::string& source, khu::bytecode::Module& module,
                    std::string& diagnostics) {
    khu::Compiler compiler;
    std::uint32_t file = compiler.add_buffer("test.khu", source);
    bool built = compiler.compile(file, module);
    diagnostics = compiler.diagnostics().render();
    return built;
}

struct BackendRun {
    bool ran = false;
    std::string output;
    // What the program wrote to standard error, as distinct from how it died.
    std::string error_output;
    std::string runtime_error;
};

BackendRun run_on_vm(const khu::bytecode::Module& module, const std::string& input) {
    BackendRun result;
    khu::vm::Vm vm(module);
    vm.set_output_sink(&result.output);
    vm.set_error_sink(&result.error_output);
    if (!input.empty()) vm.set_input(input);
    result.ran = vm.run();
    result.runtime_error = vm.error();
    return result;
}

// Returns false when the machine has no C compiler, so a caller can say so
// instead of failing.
bool run_on_native(const khu::bytecode::Module& module, const std::string& input,
                   BackendRun& result, std::string& backend_error) {
    khu::native::JitCapture capture;
    capture.input = input;
    int exit_code = 0;
    switch (khu::native::run_jit(module, exit_code, backend_error, &capture)) {
        case khu::native::NativeStatus::NoCompiler:
            return false;
        case khu::native::NativeStatus::Failed:
            result.ran = false;
            result.runtime_error = backend_error;
            return true;
        case khu::native::NativeStatus::Ok:
            break;
    }
    result.ran = exit_code == 0;
    result.output = capture.output;
    result.error_output = capture.error_output;
    result.runtime_error = capture.runtime_error;
    return true;
}

// Runs one image under both backends, asserting that nothing observable
// differs.
void check_module_identical(const khu::bytecode::Module& module, const std::string& input = {}) {
    BackendRun expected = run_on_vm(module, input);
    BackendRun actual;
    std::string backend_error;
    if (!run_on_native(module, input, actual, backend_error)) return;  // no host compiler

    KHU_CHECK_EQ(actual.output, expected.output);
    KHU_CHECK_EQ(actual.error_output, expected.error_output);
    KHU_CHECK_EQ(actual.runtime_error, expected.runtime_error);
    KHU_CHECK_EQ(actual.ran, expected.ran);
}

// Compiles `source` once and runs it under both backends.
void check_identical(const std::string& source, const std::string& input = {}) {
    khu::bytecode::Module module;
    std::string diagnostics;
    if (!compile_source(source, module, diagnostics)) {
        KHU_FAIL("did not compile:\n" + diagnostics);
        return;
    }
    check_module_identical(module, input);
}

// Wraps `body` in a class with a main, matching the VM tests' shape.
std::string program(const std::string& body) {
    return "bring khu::stdlib;\n\npublic class T {\n    func main() {\n" + body +
           "\n    }\n}\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// The emitter
// ---------------------------------------------------------------------------

KHU_TEST(native_emit, lowers_a_method_to_a_readable_c_function) {
    khu::bytecode::Module module;
    std::string diagnostics;
    const std::string source =
        "bring khu::stdlib;\n"
        "\n"
        "public class Snap {\n"
        "    func main() {\n"
        "        int32 total = khuStdMath.add(2, 3);\n"
        "        io.printLine(total);\n"
        "    }\n"
        "}\n";
    if (!compile_source(source, module, diagnostics)) {
        KHU_FAIL("did not compile:\n" + diagnostics);
        return;
    }

    khu::native::EmitResult emitted = khu::native::emit_c(module, khu::native::EmitOptions{});
    KHU_CHECK_EQ(emitted.error, std::string());

    // The operand stack is gone: every push is an assignment to a fixed slot,
    // and the frame is one array the collector can walk. The method's index
    // depends on how many methods the standard library brought with it, so it
    // is found rather than assumed.
    std::string main_index;
    for (std::size_t i = 0; i < module.methods.size(); ++i) {
        if (module.string_at(module.methods[i].name) == "main") {
            main_index = std::to_string(i);
        }
    }
    KHU_CHECK(!main_index.empty());
    KHU_CHECK_CONTAINS(emitted.source,
                       "static int khu_m" + main_index + "(KhuValue self, const KhuValue* argv,");
    KHU_CHECK_CONTAINS(emitted.source, "KhuValue V[3];");
    KHU_CHECK_CONTAINS(emitted.source, "khu_rt_frame_enter(&F, V, " + main_index +
                                           "u, 1u, 3u, self, argv, 0u);");
    KHU_CHECK_CONTAINS(emitted.source, "V[1] = khu_rt_constants[");
    KHU_CHECK_CONTAINS(emitted.source, "khu_val_arith(KHU_A_ADD, 3u, &V[1], &V[2], &V[1])");
    // The safepoint publishes where we are and how much of the frame is live.
    KHU_CHECK_CONTAINS(emitted.source, "F.height = 2u;");
    KHU_CHECK_CONTAINS(emitted.source, "khu_rt_call_native(&F, 2u, 1u, &V[1], &V[1], 0)");
    KHU_CHECK_CONTAINS(emitted.source, "KHU_RETURN(&F);");
    KHU_CHECK_CONTAINS(emitted.source, "const KhuNativeMethod khu_native_methods[] = {");
}

KHU_TEST(native_emit, is_reproducible) {
    khu::bytecode::Module module;
    std::string diagnostics;
    if (!compile_source(program("        io.printLine(khuStdMath.add(1, 2));"), module,
                        diagnostics)) {
        KHU_FAIL("did not compile:\n" + diagnostics);
        return;
    }
    khu::native::EmitResult first = khu::native::emit_c(module, khu::native::EmitOptions{});
    khu::native::EmitResult second = khu::native::emit_c(module, khu::native::EmitOptions{});
    KHU_CHECK_EQ(first.error, std::string());
    KHU_CHECK_EQ(first.source == second.source, true);
}

KHU_TEST(native_emit, lowers_control_flow_to_labels_and_gotos) {
    khu::bytecode::Module module;
    std::string diagnostics;
    if (!compile_source(program("        int32 i = 0;\n"
                                "        while (i < 3) {\n"
                                "            io.printLine(i);\n"
                                "            i = khuStdMath.add(i, 1);\n"
                                "        }"),
                        module, diagnostics)) {
        KHU_FAIL("did not compile:\n" + diagnostics);
        return;
    }
    khu::native::EmitResult emitted = khu::native::emit_c(module, khu::native::EmitOptions{});
    KHU_CHECK_EQ(emitted.error, std::string());
    KHU_CHECK_CONTAINS(emitted.source, "goto L");
    KHU_CHECK_CONTAINS(emitted.source, "if (!khu_truthy(");
    KHU_CHECK_CONTAINS(emitted.source, "khu_val_compare(KHU_C_LT,");
}

KHU_TEST(native_emit, embeds_the_image_only_when_asked) {
    khu::bytecode::Module module;
    std::string diagnostics;
    if (!compile_source(program("        io.printLine(1);"), module, diagnostics)) {
        KHU_FAIL("did not compile:\n" + diagnostics);
        return;
    }

    khu::native::EmitResult bare = khu::native::emit_c(module, khu::native::EmitOptions{});
    KHU_CHECK_CONTAINS(bare.source, "const uint32_t khu_native_image_size = 0u;");

    khu::native::EmitOptions options;
    options.image = khu::bytecode::serialize(module);
    khu::native::EmitResult carried = khu::native::emit_c(module, options);
    KHU_CHECK_CONTAINS(carried.source,
                       "const uint32_t khu_native_image_size = " +
                           std::to_string(options.image.size()) + "u;");
}

KHU_TEST(native_emit, output_compiles_under_a_strict_compiler_mode) {
    std::string compiler = khu::native::find_c_compiler();
    if (compiler.empty()) return;  // nothing to check on a machine with no cc

    khu::bytecode::Module module;
    std::string diagnostics;
    if (!compile_source(program("        int32 i = 0;\n"
                                "        while (i < 3) {\n"
                                "            io.printLine(khuStdMath.multiply(i, 2));\n"
                                "            i = khuStdMath.add(i, 1);\n"
                                "        }"),
                        module, diagnostics)) {
        KHU_FAIL("did not compile:\n" + diagnostics);
        return;
    }
    khu::native::EmitResult emitted = khu::native::emit_c(module, khu::native::EmitOptions{});
    KHU_CHECK_EQ(emitted.error, std::string());

    khu::native::TempDir scratch;
    if (!scratch.ok()) {
        KHU_FAIL("cannot create a scratch directory");
        return;
    }
    std::string path = scratch.file("snapshot.c");
    scratch.track(path);
    if (!khu::util::write_file(path, emitted.source)) {
        KHU_FAIL("cannot write " + path);
        return;
    }

    // -Werror: the emitted C is a deliverable, not a place warnings accumulate.
    std::string report;
    int status = khu::native::run_tool({compiler, "-std=c11", "-O1", "-Wall", "-Wextra",
                                        "-Wpedantic", "-Werror", "-c", path, "-o", "/dev/null"},
                                       report);
    if (status != 0) KHU_FAIL("the emitted C did not compile cleanly:\n" + report);
}

// ---------------------------------------------------------------------------
// Differential: the VM and the native backend, same program, same output
// ---------------------------------------------------------------------------

KHU_TEST(native_diff, arithmetic_across_widths) {
    check_identical(program("        io.printLine(khuStdMath.add(20, 22));\n"
                            "        io.printLine(khuStdMath.subtract(10, 25));\n"
                            "        io.printLine(khuStdMath.multiply(6, 7));\n"
                            "        io.printLine(khuStdMath.divide(45, 6));\n"
                            "        io.printLine(khuStdMath.remainder(45, 7));"));
}

KHU_TEST(native_diff, overflow_wraps_the_same_way) {
    check_identical(program("        uint8 a = 255;\n"
                            "        io.printLine(khuStdMath.add(a, 1));\n"
                            "        int8 b = 127;\n"
                            "        io.printLine(khuStdMath.add(b, 1));\n"
                            "        int32 c = 2147483647;\n"
                            "        io.printLine(khuStdMath.add(c, 1));\n"
                            "        int64 d = 9223372036854775807;\n"
                            "        io.printLine(khuStdMath.add(d, 1));"));
}

KHU_TEST(native_diff, operators_and_precedence) {
    check_identical(program("        io.printLine(1 + 2 * 3);\n"
                            "        io.printLine((1 + 2) * 3);\n"
                            "        io.printLine(-4 + 10);\n"
                            "        io.printLine(7 % 3);\n"
                            "        io.printLine(10 / 4);"));
}

KHU_TEST(native_diff, comparisons_and_booleans) {
    check_identical(program("        io.printLine(1 < 2);\n"
                            "        io.printLine(2 <= 2);\n"
                            "        io.printLine(3 > 4);\n"
                            "        io.printLine(4 >= 4);\n"
                            "        io.printLine(5 == 5);\n"
                            "        io.printLine(5 != 5);\n"
                            "        io.printLine(true);\n"
                            "        io.printLine(false);"));
}

KHU_TEST(native_diff, bitwise_shifts_and_logical_negation) {
    check_identical(program("        int32 a = 12;\n"
                            "        int32 b = 10;\n"
                            "        io.printLine(a & b);\n"
                            "        io.printLine(a | b);\n"
                            "        io.printLine(a ^ b);\n"
                            "        io.printLine(~a);\n"
                            "        io.printLine(a << 3);\n"
                            "        io.printLine(a >> 2);\n"
                            "        io.printLine(!(a < b));\n"
                            "        io.printLine(a >= b);"));
}

KHU_TEST(native_diff, shifts_wrap_their_count_by_width) {
    // The count is taken modulo the operand's width, and a signed right shift
    // keeps its sign -- both are places a C translation could quietly diverge.
    check_identical(program("        uint8 small = 200;\n"
                            "        io.printLine(small << 9);\n"
                            "        io.printLine(small >> 1);\n"
                            "        int8 negative = -8;\n"
                            "        io.printLine(negative >> 2);\n"
                            "        int64 wide = -1;\n"
                            "        io.printLine(wide >> 63);\n"
                            "        uint64 unsignedWide = 18446744073709551615;\n"
                            "        io.printLine(unsignedWide >> 60);"));
}

KHU_TEST(native_diff, division_edge_cases) {
    check_identical(program("        int32 low = -2147483648;\n"
                            "        io.printLine(low / -1);\n"
                            "        io.printLine(low % -1);\n"
                            "        io.printLine(-7 / 2);\n"
                            "        io.printLine(-7 % 2);"));
}

KHU_TEST(native_diff, floating_point_rounding) {
    check_identical(program("        float f = 1.5;\n"
                            "        dfloat d = 2.25;\n"
                            "        io.printLine(f);\n"
                            "        io.printLine(d);\n"
                            "        io.printLine(f + f);\n"
                            "        io.printLine(d / 4.0);"));
}

// The standard library's natives run in the host, not in the emitted C, so a
// disagreement here would mean the two backends were calling different code --
// which is exactly what src/vm/natives.cpp exists to prevent.
KHU_TEST(native_diff, math_utilities_at_their_edges) {
    check_identical(program("        int8 floorOfInt8 = -128;\n"
                            "        io.printLine(khuStdMath.abs(floorOfInt8));\n"
                            "        io.printLine(khuStdMath.neg(floorOfInt8));\n"
                            "        io.printLine(khuStdMath.signum(floorOfInt8));\n"
                            "        uint8 wide = 200;\n"
                            "        uint8 narrow = 4;\n"
                            "        io.printLine(khuStdMath.min(wide, narrow));\n"
                            "        io.printLine(khuStdMath.max(wide, narrow));\n"
                            "        io.printLine(khuStdMath.clamp(wide, narrow, narrow));\n"
                            "        io.printLine(khuStdMath.gcd(0, 0));\n"
                            "        io.printLine(khuStdMath.gcd(-48, 18));\n"
                            "        io.printLine(khuStdMath.lcm(0, 6));\n"
                            "        int64 big = 4000000000;\n"
                            "        io.printLine(khuStdMath.lcm(big, big));"));
}

KHU_TEST(native_diff, integer_pow_wraps_and_truncates) {
    check_identical(program("        io.printLine(khuStdMath.pow(2, 31));\n"
                            "        io.printLine(khuStdMath.pow(2, 32));\n"
                            "        io.printLine(khuStdMath.pow(-3, 3));\n"
                            "        io.printLine(khuStdMath.pow(7, 0));\n"
                            "        io.printLine(khuStdMath.pow(2, -3));\n"
                            "        io.printLine(khuStdMath.pow(1, -3));\n"
                            "        io.printLine(khuStdMath.pow(-1, -3));\n"
                            "        io.printLine(khuStdMath.pow(-1, -4));\n"
                            "        uint8 base = 3;\n"
                            "        uint8 exponent = 6;\n"
                            "        io.printLine(khuStdMath.pow(base, exponent));"));
}

// A float transcendental is computed at 64 bits and rounded back to 32, so it
// has to round the same way in both backends -- and differently from the
// dfloat it was computed alongside.
KHU_TEST(native_diff, transcendentals_round_to_their_width) {
    check_identical(program("        float narrow = 0.5;\n"
                            "        dfloat wide = 0.5;\n"
                            "        io.printLine(khuStdMath.sqrt(narrow));\n"
                            "        io.printLine(khuStdMath.sqrt(wide));\n"
                            "        io.printLine(khuStdMath.exp(narrow));\n"
                            "        io.printLine(khuStdMath.exp(wide));\n"
                            "        io.printLine(khuStdMath.pow(wide, wide));\n"
                            "        io.printLine(khuStdMath.atan2(wide, wide));\n"
                            "        io.printLine(khuStdMath.pi());\n"
                            "        io.printLine(khuStdMath.e());"));
}

// Domain errors are values, not traps: they render through the same formatter
// on both sides.
KHU_TEST(native_diff, float_domain_errors_are_values) {
    check_identical(program("        io.printLine(khuStdMath.sqrt(-1.0));\n"
                            "        io.printLine(khuStdMath.log(0.0));\n"
                            "        io.printLine(khuStdMath.log(-1.0));\n"
                            "        io.printLine(khuStdMath.asin(2.0));\n"
                            "        io.printLine(khuStdMath.fmod(1.0, 0.0));"));
}

// And a native that does trap traps identically, with the same message and the
// same stack trace.
KHU_TEST(native_diff, a_native_trap_reads_the_same_on_both_backends) {
    check_identical(program("        io.printLine(khuStdMath.clamp(5, 10, 0));"));
}

KHU_TEST(native_diff, a_negative_power_of_zero_is_a_division_by_zero) {
    check_identical(program("        io.printLine(khuStdMath.pow(0, -1));"));
}

// A native that produces a *string* allocates it in the backend's own runtime
// store, so this is where the two stores have to behave the same way.
KHU_TEST(native_diff, conversions_round_trip_through_runtime_strings) {
    check_identical(program("        io.printLine(khuStdConv.toString(-98765));\n"
                            "        io.printLine(khuStdConv.parseInt32(khuStdConv.toString(7)));\n"
                            "        io.printLine(khuStdConv.toString(1.5));\n"
                            "        float narrow = 0.1;\n"
                            "        io.printLine(khuStdConv.toString(narrow));\n"
                            "        io.printLine(khuStdConv.parseFloat(\"0.1\"));\n"
                            "        io.printLine(khuStdConv.parseDFloat(\"0.1\"));\n"
                            "        io.printLine(khuStdConv.toChar(65));"));
}

KHU_TEST(native_diff, a_failed_parse_answers_with_the_same_sentinel) {
    check_identical(program("        io.printLine(khuStdConv.parseInt8(\"128\"));\n"
                            "        io.printLine(khuStdConv.parseInt64(\"9223372036854775808\"));\n"
                            "        io.printLine(khuStdConv.parseUInt16(\"-1\"));\n"
                            "        io.printLine(khuStdConv.parseUInt64(\"18446744073709551616\"));\n"
                            "        io.printLine(khuStdConv.parseDFloat(\"1.5e\"));\n"
                            "        io.printLine(khuStdConv.parseFloat(\"\"));\n"
                            "        io.printLine(khuStdConv.isNumeric(\"12 \"));"));
}

// Every string operation allocates its result in the backend's own store, and
// the ones that answer a sentinel have to answer the same sentinel.
KHU_TEST(native_diff, string_operations_agree_including_their_edges) {
    check_identical(program("        string text = \"Hello, Khudra\";\n"
                            "        io.printLine(khuStdString.length(text));\n"
                            "        io.printLine(khuStdString.charAt(text, 999));\n"
                            "        io.printLine(khuStdString.charAt(text, -1));\n"
                            "        io.printLine(khuStdString.substring(text, 7, 999));\n"
                            "        io.printLine(khuStdString.substring(text, 9, 3));\n"
                            "        io.printLine(khuStdString.indexOf(text, \"zzz\"));\n"
                            "        io.printLine(khuStdString.indexOf(text, \"\"));\n"
                            "        io.printLine(khuStdString.lastIndexOf(text, \"l\"));\n"
                            "        io.printLine(khuStdString.compareTo(\"abc\", \"abcd\"));\n"
                            "        io.printLine(khuStdString.replaceAll(\"aaa\", \"aa\", \"b\"));\n"
                            "        io.printLine(khuStdString.replaceAll(\"a-b\", \"\", \"+\"));\n"
                            "        io.printLine(khuStdString.repeat(\"ab\", -2));\n"
                            "        io.printLine(khuStdString.trim(\"  x  \"));\n"
                            "        io.printLine(khuStdString.concat(text, \"!\"));"));
}

// A string operation on a null reference is not an index out of range: there is
// no string there at all, and it traps the same way a null field read does.
KHU_TEST(native_diff, a_string_operation_on_null_traps_identically) {
    check_identical(
        "bring khu::stdlib;\n\n"
        "public class T {\n"
        "    public string held;\n"
        "    func main() {\n"
        "        io.printLine(khuStdString.length(this.held));\n"
        "    }\n"
        "}\n");
}

// Arrays are the one place where allocation, indexing and the write barrier all
// meet, and the element type is erased at run time -- so both backends have to
// agree about a block of tagged values with no descriptor behind it.
KHU_TEST(native_diff, typed_arrays_read_and_write_the_same) {
    check_identical(program(
        "        Array<int32> counts = khuStdCollection.arrayCreate(int32, 4);\n"
        "        io.printLine(khuStdCollection.arraySize(counts));\n"
        "        io.printLine(counts[0]);\n"
        "        counts[2] = 7;\n"
        "        io.printLine(counts[2]);\n"
        "        int32 kept = (counts[1] = 9);\n"
        "        io.printLine(kept);\n"
        "        Array<string> names = khuStdCollection.arrayCreate(string, 2);\n"
        "        io.printLine(names[0]);\n"
        "        names[0] = \"x\";\n"
        "        io.printLine(names[0]);\n"
        "        Array<dfloat> ratios = khuStdCollection.arrayCreate(dfloat, 1);\n"
        "        io.printLine(ratios[0]);\n"
        "        Array<bool> flags = khuStdCollection.arrayCreate(bool, 1);\n"
        "        io.printLine(flags[0]);\n"
        "        Array<Array<int32>> grid = khuStdCollection.arrayCreate(Array<int32>, 1);\n"
        "        grid[0] = counts;\n"
        "        io.printLine(grid[0][2]);\n"
        "        Array bare = counts;\n"
        "        io.printLine(khuStdCollection.arraySize(bare));\n"
        "        string word = \"Khudra\";\n"
        "        io.printLine(word[1]);"));
}

KHU_TEST(native_diff, an_index_out_of_range_traps_identically) {
    check_identical(program(
        "        Array<int32> counts = khuStdCollection.arrayCreate(int32, 2);\n"
        "        io.printLine(counts[5]);"));
}

KHU_TEST(native_diff, a_negative_index_traps_identically) {
    check_identical(program(
        "        Array<int32> counts = khuStdCollection.arrayCreate(int32, 2);\n"
        "        counts[-1] = 0;"));
}

KHU_TEST(native_diff, indexing_a_null_array_traps_identically) {
    check_identical(
        "bring khu::stdlib;\n\n"
        "public class T {\n"
        "    public Array<int32> held;\n"
        "    func main() {\n"
        "        io.printLine(this.held[0]);\n"
        "    }\n"
        "}\n");
}

KHU_TEST(native_diff, a_negative_array_length_traps_identically) {
    check_identical(program(
        "        io.printLine(khuStdCollection.arraySize("
        "khuStdCollection.arrayCreate(int32, -1)));"));
}

// An array holds references, and the collector has to find them through it: an
// array is traced by tag rather than through a reference map, because it has
// no class descriptor to carry one.
KHU_TEST(native_diff, the_collector_walks_arrays) {
    check_identical(
        "bring khu::stdlib;\n\n"
        "public class Cell {\n"
        "    public int32 value = 0;\n"
        "    public Cell(int32 v) { this.value = v; }\n"
        "}\n\n"
        "public class T {\n"
        "    func main() {\n"
        "        Array<Cell> kept = khuStdCollection.arrayCreate(Cell, 4);\n"
        "        kept[0] = Cell(1);\n"
        "        kept[3] = Cell(2);\n"
        "        int32 i = 0;\n"
        "        while (i < 400) {\n"
        "            Cell garbage = Cell(i);\n"
        "            i = khuStdMath.add(i, 1);\n"
        "        }\n"
        "        io.printLine(kept[0].value);\n"
        "        io.printLine(kept[3].value);\n"
        "    }\n"
        "}\n");
}

// A generic class is compiled once and its slots are erased, so what the two
// backends see is a class whose fields hold whatever an instantiation put
// there. Both have to trace and pin those slots by tag.
KHU_TEST(native_diff, generic_classes_behave_the_same) {
    check_identical(
        "bring khu::stdlib;\n\n"
        "public class Box<T> {\n"
        "    private T held;\n"
        "    public func put(T value) { this.held = value; }\n"
        "    public func get() -> T { return this.held; }\n"
        "}\n\n"
        "public class T {\n"
        "    func main() {\n"
        "        Box<int32> numbers = Box<int32>();\n"
        "        io.printLine(numbers.get());\n"
        "        numbers.put(42);\n"
        "        io.printLine(numbers.get());\n"
        "        Box<string> words = Box<string>();\n"
        "        words.put(\"hi\");\n"
        "        io.printLine(words.get());\n"
        "        Box<dfloat> ratios = Box<dfloat>();\n"
        "        ratios.put(1.5);\n"
        "        io.printLine(ratios.get());\n"
        "    }\n"
        "}\n");
}

// An erased slot holding a manual object still pins it: the pin comes from the
// tag on the value, not from a static answer in the reference map.
KHU_TEST(native_diff, an_erased_slot_pins_a_manual_object) {
    check_identical(
        "bring khu::stdlib;\n\n"
        "public class Node {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "    public int32 value = 0;\n"
        "    public Node(int32 v) { this.value = v; }\n"
        "}\n\n"
        "public class Box<T> {\n"
        "    private T held;\n"
        "    public func put(T value) { this.held = value; }\n"
        "    public func get() -> T { return this.held; }\n"
        "}\n\n"
        "public class T {\n"
        "    func main() {\n"
        "        Box<Node> box = Box<Node>();\n"
        "        Node n = Node(7);\n"
        "        box.put(n);\n"
        "        io.printLine(box.get().value);\n"
        "        free(n);\n"
        "    }\n"
        "}\n");
}

// Result and Option are ordinary Khudra classes in the standard library, so
// this is also the test that a stdlib class behaves the same under both
// backends -- including when it ends the program.
KHU_TEST(native_diff, results_carry_a_value_or_a_failure) {
    check_identical(program(
        "        Result<int32> good = Result<int32>().withValue(7);\n"
        "        io.printLine(good.ok());\n"
        "        io.printLine(good.value());\n"
        "        io.printLine(good.unwrapOr(0));\n"
        "        Result<int32> bad = Result<int32>().withError(khuErrors.parse(), \"bad\");\n"
        "        io.printLine(bad.ok());\n"
        "        io.printLine(bad.errorCode());\n"
        "        io.printLine(bad.errorMessage());\n"
        "        io.printLine(bad.unwrapOr(-1));\n"
        "        Option<string> none = Option<string>();\n"
        "        io.printLine(none.isEmpty());\n"
        "        io.printLine(none.orElse(\"absent\"));\n"
        "        Parse parse = Parse();\n"
        "        io.printLine(parse.toInt32(\"42\").value());\n"
        "        io.printLine(parse.toInt32(\"x\").errorMessage());\n"
        "        io.printLine(parse.toUInt8(\"-1\").ok());"));
}

// khuErrors.fail is the one way Khudra source raises a fatal error, and the
// trap it produces has to be the same on both backends down to the frame the
// standard library contributes to the trace.
KHU_TEST(native_diff, reading_a_failed_result_traps_identically) {
    check_identical(program(
        "        Result<int32> bad = Result<int32>().withError(khuErrors.parse(), \"bad\");\n"
        "        io.printLine(bad.value());"));
}

KHU_TEST(native_diff, reading_an_empty_option_traps_identically) {
    check_identical(program("        io.printLine(Option<int32>().value());"));
}

// The containers are Khudra code over Array<T>, so this is the test that a
// program's data structures -- growth, rehashing, tombstones, the collector
// walking through all of it -- behave the same under both backends.
KHU_TEST(native_diff, containers_agree_through_growth_and_removal) {
    check_identical(program(
        "        List<int32> items = List<int32>();\n"
        "        int32 i = 0;\n"
        "        while (i < 40) {\n"
        "            items.add(khuStdMath.multiply(i, i));\n"
        "            i = khuStdMath.add(i, 1);\n"
        "        }\n"
        "        io.printLine(items.size());\n"
        "        io.printLine(items.get(39));\n"
        "        io.printLine(items.removeAt(0));\n"
        "        io.printLine(items.get(0));\n"
        "        io.printLine(items.indexOf(100));\n"
        "        io.printLine(items.remove(100));\n"
        "        io.printLine(items.size());\n"
        "        Stack<int32> stack = Stack<int32>();\n"
        "        stack.push(1);\n"
        "        stack.push(2);\n"
        "        io.printLine(stack.pop());\n"
        "        Queue<int32> queue = Queue<int32>();\n"
        "        int32 q = 0;\n"
        "        while (q < 30) {\n"
        "            queue.enqueue(q);\n"
        "            if (khuStdMath.remainder(q, 3) == 0) { queue.dequeue(); }\n"
        "            q = khuStdMath.add(q, 1);\n"
        "        }\n"
        "        io.printLine(queue.size());\n"
        "        io.printLine(queue.peek());"));
}

// A Map's layout is decided by khuStdCollection.hash, so its bucket order --
// and therefore keys() -- is only reproducible if the two backends hash
// identically.
KHU_TEST(native_diff, a_map_lays_out_the_same_table_on_both_backends) {
    check_identical(program(
        "        Map<int32, int32> squares = Map<int32, int32>();\n"
        "        int32 i = 0;\n"
        "        while (i < 40) {\n"
        "            squares.put(i, khuStdMath.multiply(i, i));\n"
        "            i = khuStdMath.add(i, 1);\n"
        "        }\n"
        "        io.printLine(squares.size());\n"
        "        io.printLine(squares.remove(7));\n"
        "        io.printLine(squares.containsKey(7));\n"
        "        io.printLine(squares.get(39));\n"
        "        List<int32> keys = squares.keys();\n"
        "        int32 k = 0;\n"
        "        while (k < keys.size()) {\n"
        "            io.print(keys.get(k));\n"
        "            io.print(\" \");\n"
        "            k = khuStdMath.add(k, 1);\n"
        "        }\n"
        "        io.printLine(\"\");\n"
        "        Map<string, int32> named = Map<string, int32>();\n"
        "        named.put(\"ada\", 1);\n"
        "        named.put(\"alan\", 2);\n"
        "        io.printLine(named.get(\"ada\"));\n"
        "        io.printLine(named.getOrElse(\"nobody\", -1));"));
}

KHU_TEST(native_diff, a_container_read_out_of_range_traps_identically) {
    check_identical(program("        List<int32> items = List<int32>();\n"
                            "        items.add(1);\n"
                            "        io.printLine(items.get(5));"));
}

KHU_TEST(native_diff, an_empty_container_traps_identically) {
    check_identical(program("        io.printLine(Stack<int32>().pop());"));
}

KHU_TEST(native_diff, a_missing_map_key_traps_identically) {
    check_identical(program("        io.printLine(Map<string, int32>().get(\"absent\"));"));
}

// The typed reads are readLine plus the shared parser, so the sentinel a bad
// line produces has to be the same one on both backends.
KHU_TEST(native_diff, typed_reads_agree_including_their_sentinels) {
    check_identical(program("        io.printLine(io.readInt32());\n"
                            "        io.printLine(io.readInt32());\n"
                            "        io.printLine(io.readUInt8());\n"
                            "        io.printLine(io.readDFloat());\n"
                            "        io.printLine(io.readBool());\n"
                            "        io.printLine(io.readBool());\n"
                            "        io.printLine(io.readLine());\n"
                            "        io.printLine(io.readInt32());"),
                    "42\nnonsense\n-1\n2.5\ntrue\nyes\ntail\n");
}

// readChar and readByte read from the same position readLine does, and they
// disagree only about how they report the end of input.
KHU_TEST(native_diff, byte_reads_share_the_line_readers_position) {
    check_identical(program("        io.printLine(io.readChar());\n"
                            "        io.printLine(io.readLine());\n"
                            "        io.printLine(io.readByte());\n"
                            "        io.printLine(io.readByte());\n"
                            "        io.printLine(io.readChar());"),
                    "abc\n");
}

// The two streams are separate, and both are compared.
KHU_TEST(native_diff, the_error_stream_agrees_too) {
    check_identical(program("        io.printLine(\"one\");\n"
                            "        khuStdErr.errPrintLine(\"two\");\n"
                            "        khuStdErr.errPrint(3);\n"
                            "        khuStdErr.errPrint(\" \");\n"
                            "        khuStdErr.errPrintLine(1.5);\n"
                            "        khuStdErr.errWriteString(\"raw\\n\");\n"
                            "        io.writeString(\"four\\n\");\n"
                            "        io.flush();\n"
                            "        khuStdErr.flush();"));
}

KHU_TEST(native_diff, describe_renders_what_print_has_no_overload_for) {
    check_identical(
        "bring khu::stdlib;\n\n"
        "public class Cell { public int32 value = 0; }\n\n"
        "public class T {\n"
        "    func main() {\n"
        "        io.printLine(io.describe(Cell()));\n"
        "        io.printLine(io.describe(khuStdCollection.arrayCreate(int32, 3)));\n"
        "        io.printLine(io.describe(7));\n"
        "        io.printLine(io.describe(null));\n"
        "        io.printLine(io.describe(1.5));\n"
        "    }\n"
        "}\n");
}

// Buffers are the raw side of the memory model: both backends allocate from the
// same C allocator, so both see the same header, the same length checks and the
// same leak counters.
KHU_TEST(native_diff, manual_buffers_agree) {
    check_identical(program(
        "        io.printLine(khuStdMem.liveBlocks());\n"
        "        *byte buffer = khuStdMem.alloc(16);\n"
        "        io.printLine(khuStdMem.sizeOf(buffer));\n"
        "        io.printLine(buffer[0]);\n"
        "        buffer[0] = 65;\n"
        "        buffer[15] = 90;\n"
        "        io.printLine(buffer[0]);\n"
        "        io.printLine(buffer[15]);\n"
        "        khuStdMem.fill(buffer, 3, 8);\n"
        "        io.printLine(buffer[7]);\n"
        "        *byte other = khuStdMem.alloc(16);\n"
        "        io.printLine(khuStdMem.compare(buffer, other, 16));\n"
        "        khuStdMem.copy(other, buffer, 16);\n"
        "        io.printLine(khuStdMem.compare(buffer, other, 16));\n"
        "        io.printLine(khuStdMem.refEquals(buffer, other));\n"
        "        buffer = khuStdMem.realloc(buffer, 4);\n"
        "        io.printLine(khuStdMem.sizeOf(buffer));\n"
        "        *int32 words = khuStdMem.alloc(16);\n"
        "        words[2] = -70000;\n"
        "        io.printLine(words[2]);\n"
        "        io.printLine(words[0]);\n"
        "        khuStdMem.release(words);\n"
        "        khuStdMem.release(other);\n"
        "        khuStdMem.release(buffer);\n"
        "        io.printLine(khuStdMem.liveBlocks());\n"
        "        io.printLine(khuStdMem.liveBytes());"));
}

KHU_TEST(native_diff, a_buffer_operation_past_the_end_traps_identically) {
    check_identical(program("        *byte buffer = khuStdMem.alloc(4);\n"
                            "        khuStdMem.fill(buffer, 1, 8);"));
}

KHU_TEST(native_diff, releasing_a_buffer_twice_traps_identically) {
    check_identical(program("        *byte buffer = khuStdMem.alloc(4);\n"
                            "        khuStdMem.release(buffer);\n"
                            "        khuStdMem.release(buffer);"));
}

KHU_TEST(native_diff, reaching_through_a_null_pointer_traps_identically) {
    check_identical(program("        *byte buffer = null;\n"
                            "        io.printLine(buffer[0]);"));
}

KHU_TEST(native_diff, overlapping_copy_is_refused_identically) {
    check_identical(program("        *byte buffer = khuStdMem.alloc(8);\n"
                            "        khuStdMem.copy(buffer, buffer, 8);"));
}

// The generator is the same state machine on both sides, so a seeded program
// produces the same sequence under either. That is the whole reason it is
// deterministic.
KHU_TEST(native_diff, the_random_stream_is_the_same_on_both_backends) {
    check_identical(program("        io.printLine(khuStdRandom.nextInt());\n"
                            "        khuStdRandom.seed(1234);\n"
                            "        int32 i = 0;\n"
                            "        while (i < 8) {\n"
                            "            io.printLine(khuStdRandom.nextInt());\n"
                            "            io.printLine(khuStdRandom.nextInt(100));\n"
                            "            io.printLine(khuStdRandom.nextInt64());\n"
                            "            io.printLine(khuStdRandom.nextFloat());\n"
                            "            io.printLine(khuStdRandom.nextDouble());\n"
                            "            io.printLine(khuStdRandom.nextBool());\n"
                            "            i = khuStdMath.add(i, 1);\n"
                            "        }\n"
                            "        *byte buffer = khuStdMem.alloc(16);\n"
                            "        khuStdRandom.nextBytes(buffer, 16);\n"
                            "        io.printLine(buffer[0]);\n"
                            "        io.printLine(buffer[15]);\n"
                            "        khuStdMem.release(buffer);"));
}

KHU_TEST(native_diff, an_empty_random_bound_traps_identically) {
    check_identical(program("        io.printLine(khuStdRandom.nextInt(0));"));
}

KHU_TEST(native_diff, control_flow) {
    check_identical(program("        int32 i = 0;\n"
                            "        int32 total = 0;\n"
                            "        while (i < 10) {\n"
                            "            if (i % 2 == 0) {\n"
                            "                total = khuStdMath.add(total, i);\n"
                            "            }\n"
                            "            i = khuStdMath.add(i, 1);\n"
                            "        }\n"
                            "        io.printLine(total);"));
}

KHU_TEST(native_diff, strings_and_reference_equality) {
    check_identical(program("        string a = \"khudra\";\n"
                            "        string b = \"khudra\";\n"
                            "        io.printLine(a);\n"
                            "        io.printLine(a == b);\n"
                            "        io.printLine(a != b);"));
}

KHU_TEST(native_diff, objects_procedures_and_virtual_dispatch) {
    check_identical(
        "bring khu::stdlib;\n"
        "\n"
        "public class Base {\n"
        "    public int32 tag = 1;\n"
        "    private Procedures {\n"
        "        io.printLine(\"loading a Base\");\n"
        "    }\n"
        "    public Base() {\n"
        "        io.printLine(\"constructing a Base\");\n"
        "    }\n"
        "    func describe() -> int32 {\n"
        "        return this.tag;\n"
        "    }\n"
        "}\n"
        "\n"
        "public class Derived extends Base {\n"
        "    func describe() -> int32 {\n"
        "        return khuStdMath.add(this.tag, 41);\n"
        "    }\n"
        "}\n"
        "\n"
        "public class Runner {\n"
        "    func main() {\n"
        "        Base one = Base();\n"
        "        Base two = Derived();\n"
        "        io.printLine(one.describe());\n"
        "        io.printLine(two.describe());\n"
        "    }\n"
        "}\n");
}

KHU_TEST(native_diff, traps_report_the_same_position_and_trace) {
    check_identical(
        "bring khu::stdlib;\n"
        "\n"
        "public class Trap {\n"
        "    private int32 zero = 0;\n"
        "    func divide(int32 n) -> int32 {\n"
        "        return n / this.zero;\n"
        "    }\n"
        "    func main() {\n"
        "        io.printLine(\"before\");\n"
        "        io.printLine(this.divide(10));\n"
        "        io.printLine(\"after\");\n"
        "    }\n"
        "}\n");
}

KHU_TEST(native_diff, a_null_field_read_traps_identically) {
    check_identical(
        "bring khu::stdlib;\n"
        "\n"
        "public class Leaf {\n"
        "    public int32 value = 7;\n"
        "}\n"
        "\n"
        "public class Reader {\n"
        "    private Leaf leaf = null;\n"
        "    func main() {\n"
        "        io.printLine(this.leaf.value);\n"
        "    }\n"
        "}\n");
}

KHU_TEST(native_diff, runaway_recursion_traps_identically) {
    check_identical(
        "bring khu::stdlib;\n"
        "\n"
        "public class Deep {\n"
        "    func down(int32 n) -> int32 {\n"
        "        return this.down(khuStdMath.add(n, 1));\n"
        "    }\n"
        "    func main() {\n"
        "        io.printLine(this.down(0));\n"
        "    }\n"
        "}\n");
}

KHU_TEST(native_diff, reads_input_the_same_way) {
    check_identical(program("        string line = io.readLine();\n"
                            "        io.printLine(line);\n"
                            "        io.printLine(io.readLine());"),
                    "first\nsecond\n");
}

KHU_TEST(native_diff, a_root_class_program_has_no_main) {
    // The other entry form: no `main`, so materializing the root class is the
    // whole program. Both backends have to make that choice the same way.
    check_identical(
        "bring khu::stdlib;\n"
        "\n"
        "public class OnlyProcedures {\n"
        "    private Procedures {\n"
        "        io.printLine(\"the root class was materialized\");\n"
        "    }\n"
        "}\n");
}

KHU_TEST(native_diff, manual_memory_and_pinning) {
    check_identical(
        "bring khu::stdlib;\n"
        "\n"
        "public class Node {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "    public int32 value = 0;\n"
        "    public Node next = null;\n"
        "    public Node(int32 seed) {\n"
        "        this.value = seed;\n"
        "    }\n"
        "}\n"
        "\n"
        "public class Owner {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setStandard();\n"
        "    public Node head = null;\n"
        "    func main() {\n"
        "        Node first = Node(1);\n"
        "        this.head = first;\n"
        "        io.printLine(this.head.value);\n"
        "        this.head = null;\n"
        "        free(first);\n"
        "        Node scratch = Node(manual, 9);\n"
        "        io.printLine(scratch.value);\n"
        "        free(scratch);\n"
        "        io.printLine(\"released\");\n"
        "    }\n"
        "}\n");
}

KHU_TEST(native_diff, freeing_twice_traps_identically) {
    // Routed through a helper so the release happens at run time: sema catches
    // a double release it can see, and this one it cannot.
    check_identical(
        "bring khu::stdlib;\n"
        "\n"
        "public class Chunk {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "    public int32 value = 3;\n"
        "}\n"
        "\n"
        "public class Doubler {\n"
        "    func release(Chunk doomed) {\n"
        "        free(doomed);\n"
        "    }\n"
        "    func main() {\n"
        "        Chunk chunk = Chunk();\n"
        "        this.release(chunk);\n"
        "        this.release(chunk);\n"
        "        io.printLine(\"unreachable\");\n"
        "    }\n"
        "}\n");
}

KHU_TEST(native_diff, the_collector_runs_under_native_execution) {
    // Enough churn to force many cycles, with a live reference held across
    // every allocation safepoint: if the scanned-locals region were wrong, the
    // total would not survive.
    check_identical(
        "bring khu::stdlib;\n"
        "\n"
        "public class Cell {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setStandard();\n"
        "    public int32 value = 0;\n"
        "    public Cell next = null;\n"
        "}\n"
        "\n"
        "public class Churn {\n"
        "    func build(int32 depth) -> Cell {\n"
        "        Cell head = null;\n"
        "        int32 i = 0;\n"
        "        while (i < depth) {\n"
        "            Cell fresh = Cell();\n"
        "            fresh.value = i;\n"
        "            fresh.next = head;\n"
        "            head = fresh;\n"
        "            i = khuStdMath.add(i, 1);\n"
        "        }\n"
        "        return head;\n"
        "    }\n"
        "    func total(Cell head) -> int32 {\n"
        "        int32 sum = 0;\n"
        "        Cell walk = head;\n"
        "        while (walk != null) {\n"
        "            sum = khuStdMath.add(sum, walk.value);\n"
        "            walk = walk.next;\n"
        "        }\n"
        "        return sum;\n"
        "    }\n"
        "    func main() {\n"
        "        int32 round = 0;\n"
        "        Cell kept = this.build(50);\n"
        "        while (round < 400) {\n"
        "            Cell garbage = this.build(30);\n"
        "            round = khuStdMath.add(round, 1);\n"
        "        }\n"
        "        io.printLine(this.total(kept));\n"
        "    }\n"
        "}\n");
}


// ---------------------------------------------------------------------------
// Opcodes the front end does not currently emit
// ---------------------------------------------------------------------------
//
// `dup_x1`, `alloc`, `manualalloc`, `pin`, `unpin` and `halt` are in the
// instruction set and the interpreter runs them, but no program in examples/ or
// tests/integration/ produces one, so the differential suite never sees them.
// A hand-assembled image closes that gap: it is a legal .kbc, it goes through
// the verifier like any other, and both backends have to agree on it.

namespace {

struct Assembler {
    khu::util::Array<std::uint8_t> bytes;

    void op(khu::bytecode::Op value) { bytes.push(static_cast<std::uint8_t>(value)); }
    void u8(std::uint32_t value) { bytes.push(static_cast<std::uint8_t>(value & 0xffu)); }
    void u16(std::uint32_t value) {
        u8(value);
        u8(value >> 8);
    }
};

}  // namespace

KHU_TEST(native_diff, opcodes_no_khudra_program_emits) {
    using khu::bytecode::Op;
    using khu::bytecode::TypeTag;

    khu::bytecode::Module module;
    module.source_file = module.intern_string("synthetic.khu");
    std::uint32_t synth_name = module.intern_string("Synth");
    std::uint32_t chunk_name = module.intern_string("Chunk");
    std::uint32_t main_name = module.intern_string("main");
    std::uint32_t value_name = module.intern_string("value");
    std::uint32_t seven = module.intern_int(TypeTag::Int32, 7);
    std::uint32_t nine = module.intern_int(TypeTag::Int32, 9);
    std::uint32_t never = module.intern_string("halt did not stop the method");

    Assembler code;
    code.op(Op::Alloc);        code.u16(0);   // a fresh collected Synth
    code.op(Op::Dup);
    code.op(Op::Pin);                          // pin it, then let it go again
    code.op(Op::Dup);
    code.op(Op::Unpin);
    code.op(Op::Dup);
    code.op(Op::LoadConst);    code.u16(seven);
    code.op(Op::PutField);     code.u16(0);
    code.op(Op::Dup);
    code.op(Op::GetField);     code.u16(0);
    code.op(Op::CallNative);   code.u16(2); code.u8(1);   // io.printLine -> 7
    code.op(Op::Pop);
    code.op(Op::LoadConst);    code.u16(seven);
    code.op(Op::LoadConst);    code.u16(nine);
    code.op(Op::DupX1);                        // [7, 9] -> [9, 7, 9]
    code.op(Op::CallNative);   code.u16(2); code.u8(1);   // -> 9
    code.op(Op::CallNative);   code.u16(2); code.u8(1);   // -> 7
    code.op(Op::CallNative);   code.u16(2); code.u8(1);   // -> 9
    code.op(Op::ManualAlloc);  code.u16(1);   // a manual Chunk, released at once
    code.op(Op::Free);
    code.op(Op::Halt);
    // Everything past `halt` must stay unexecuted under both backends.
    code.op(Op::LoadConst);    code.u16(never);
    code.op(Op::CallNative);   code.u16(2); code.u8(1);
    code.op(Op::Return);

    khu::bytecode::MethodEntry& main = module.methods.emplace();
    main.name = main_name;
    main.owner_class = 0;
    main.param_count = 0;
    main.frame_size = 0;
    main.return_type = TypeTag::Void;
    main.code = code.bytes;

    khu::bytecode::ClassEntry& synth = module.classes.emplace();
    synth.name = synth_name;
    synth.flags = khu::bytecode::kClassGc;
    khu::bytecode::FieldEntry& field = synth.fields.emplace();
    field.name = value_name;
    field.type = TypeTag::Int32;
    field.offset = 0;
    synth.vtable.push(0);

    khu::bytecode::ClassEntry& chunk = module.classes.emplace();
    chunk.name = chunk_name;
    chunk.flags = khu::bytecode::kClassManual;

    module.main_method = 0;
    module.root_class = 0;

    // It has to be a legal image, or the two backends would only agree on
    // rejecting it.
    std::string report;
    if (!khu::bytecode::verify(module, report)) {
        KHU_FAIL("the hand-assembled image is not valid:\n" + report);
        return;
    }

    // The emitter must lower all six, not quietly skip any.
    khu::native::EmitResult emitted = khu::native::emit_c(module, khu::native::EmitOptions{});
    KHU_CHECK_EQ(emitted.error, std::string());
    KHU_CHECK_CONTAINS(emitted.source, "khu_rt_alloc(&F, 0u, 0,");
    KHU_CHECK_CONTAINS(emitted.source, "khu_rt_alloc(&F, 1u, 1,");
    KHU_CHECK_CONTAINS(emitted.source, "khu_rt_pin(&F, &V[1], 1)");
    KHU_CHECK_CONTAINS(emitted.source, "khu_rt_pin(&F, &V[1], 0)");
    KHU_CHECK_CONTAINS(emitted.source, "khu_rt_free(&F, &V[0])");
    KHU_CHECK_CONTAINS(emitted.source, "unreachable */");

    BackendRun reference = run_on_vm(module, {});
    if (!reference.ran) {
        KHU_FAIL("the VM refused the hand-assembled image:\n" + reference.runtime_error);
        return;
    }
    KHU_CHECK_EQ(reference.output, std::string("7\n9\n7\n9\n"));

    check_module_identical(module);
}

// ---------------------------------------------------------------------------
// Inline C: the statement splices raw text into the native lowering, so there
// is no bytecode-VM equivalent to compare against. The tests pin the emitted
// aliases and text, and -- where a host compiler exists -- that the native
// backend runs the block and the VM refuses it.
// ---------------------------------------------------------------------------

KHU_TEST(native_emit, splices_inline_c_and_aliases_the_locals) {
    khu::bytecode::Module module;
    std::string diagnostics;
    if (!compile_source(program("        int32 value = 0;\n"
                                "        inline_c {\n"
                                "            *value = khu_normalize_int(KHU_T_INT32, value->v.as_int);\n"
                                "        }\n"),
                        module, diagnostics)) {
        KHU_FAIL("did not compile:\n" + diagnostics);
        return;
    }
    KHU_CHECK(module.has_inline());

    khu::native::EmitResult emitted = khu::native::emit_c(module, khu::native::EmitOptions{});
    KHU_CHECK_EQ(emitted.error, std::string());
    KHU_CHECK_CONTAINS(emitted.source, "KhuValue* const value = &V[");
    KHU_CHECK_CONTAINS(emitted.source, "/* inline_c */");
    KHU_CHECK_CONTAINS(emitted.source, "khu_normalize_int(KHU_T_INT32, value->v.as_int);");
}

KHU_TEST(native_emit, skips_an_alias_that_collides_with_the_host_signature) {
    khu::bytecode::Module module;
    std::string diagnostics;
    // `out` is the emitted method's result pointer, so its alias must not be
    // declared -- the inline text below reads the Khudra local through V[]
    // instead, and the whole function must still compile.
    if (!compile_source(program("        int32 out = 0;\n"
                                "        inline_c {\n"
                                "            V[0] = *out;\n"
                                "        }\n"),
                        module, diagnostics)) {
        KHU_FAIL("did not compile:\n" + diagnostics);
        return;
    }
    khu::native::EmitResult emitted = khu::native::emit_c(module, khu::native::EmitOptions{});
    KHU_CHECK_EQ(emitted.error, std::string());
    KHU_CHECK_CONTAINS(emitted.source, "/* inline_c */");
    // The collision name does not become an alias; the frame still exists.
    KHU_CHECK_CONTAINS(emitted.source, "KhuValue V[");
    KHU_CHECK(emitted.source.find("KhuValue* const out") == std::string::npos);
}

KHU_TEST(native_emit, inline_c_compiles_under_a_strict_compiler_mode) {
    std::string compiler = khu::native::find_c_compiler();
    if (compiler.empty()) return;  // nothing to check on a machine with no cc

    khu::bytecode::Module module;
    std::string diagnostics;
    if (!compile_source(program("        int32 value = 0;\n"
                                "        inline_c {\n"
                                "            *value = khu_normalize_int(KHU_T_INT32, value->v.as_int + 1);\n"
                                "        }\n"
                                "        io.printLine(value);\n"),
                        module, diagnostics)) {
        KHU_FAIL("did not compile:\n" + diagnostics);
        return;
    }
    khu::native::EmitResult emitted = khu::native::emit_c(module, khu::native::EmitOptions{});
    KHU_CHECK_EQ(emitted.error, std::string());

    khu::native::TempDir scratch;
    if (!scratch.ok()) {
        KHU_FAIL("cannot create a scratch directory");
        return;
    }
    std::string path = scratch.file("inline.c");
    scratch.track(path);
    if (!khu::util::write_file(path, emitted.source)) {
        KHU_FAIL("cannot write " + path);
        return;
    }
    std::string report;
    int status = khu::native::run_tool({compiler, "-std=c11", "-O1", "-Wall", "-Wextra",
                                        "-Wpedantic", "-Werror", "-c", path, "-o", "/dev/null"},
                                       report);
    if (status != 0) KHU_FAIL("the emitted inline C did not compile cleanly:\n" + report);
}

KHU_TEST(native_inline, runs_raw_c_while_the_vm_refuses_it) {
    khu::bytecode::Module module;
    std::string diagnostics;
    const std::string source = program(
        "        int32 x = 0;\n"
        "        int32 y = 2;\n"
        "        inline_c {\n"
        "            *x = khu_normalize_int(KHU_T_INT32, x->v.as_int + y->v.as_int * 21);\n"
        "        }\n"
        "        io.printLine(x);\n");
    if (!compile_source(source, module, diagnostics)) {
        KHU_FAIL("did not compile:\n" + diagnostics);
        return;
    }
    KHU_CHECK(module.has_inline());

    // The bytecode VM will not execute raw C.
    BackendRun vm = run_on_vm(module, {});
    KHU_CHECK(!vm.ran);
    KHU_CHECK_CONTAINS(vm.runtime_error, "native-only");

    // The native backend splices the C in and gets exactly what it declared.
    BackendRun native;
    std::string backend_error;
    if (!run_on_native(module, {}, native, backend_error)) return;  // no host compiler
    KHU_CHECK(native.ran);
    KHU_CHECK_EQ(native.error_output, std::string(""));
    KHU_CHECK_EQ(native.output, std::string("42\n"));
}

KHU_TEST(native_emit, wraps_inline_asm_in_a_volatile_extended_asm_statement) {
    khu::bytecode::Module module;
    std::string diagnostics;
    if (!compile_source(program("        inline_asm {\n"
                                "            \"nop\"\n"
                                "        }\n"),
                        module, diagnostics)) {
        KHU_FAIL("did not compile:\n" + diagnostics);
        return;
    }
    KHU_CHECK(module.has_inline());

    khu::native::EmitResult emitted = khu::native::emit_c(module, khu::native::EmitOptions{});
    KHU_CHECK_EQ(emitted.error, std::string());
    KHU_CHECK_CONTAINS(emitted.source, "/* inline_asm */");
    KHU_CHECK_CONTAINS(emitted.source, "__asm__ volatile(");
    KHU_CHECK_CONTAINS(emitted.source, "\"nop\"");
    KHU_CHECK_CONTAINS(emitted.source, ");");
}

KHU_TEST(native_inline, runs_asm_that_writes_a_local_through_its_alias) {
    khu::bytecode::Module module;
    std::string diagnostics;
    // x is a `KhuValue* const` alias into the frame; extended asm writes the
    // Int32 tag and the 64-bit payload through its memory operands. The note
    // that the emitted asm is volatile only holds when the text reaches the
    // translation unit verbatim -- which is what this test pins.
    const std::string source = program(
        "        int32 x = 0;\n"
        "        inline_asm {\n"
        "            \"movb %2, %0\\n\\t\"\n"
        "            \"movq %3, %1\\n\\t\"\n"
        "            : \"=m\"(x->tag), \"=m\"(x->v.as_int)\n"
        "            : \"i\"(KHU_T_INT32), \"i\"(42)\n"
        "            : \"memory\"\n"
        "        }\n"
        "        io.printLine(x);\n");
    if (!compile_source(source, module, diagnostics)) {
        KHU_FAIL("did not compile:\n" + diagnostics);
        return;
    }
    KHU_CHECK(module.has_inline());

    // The bytecode VM will not execute raw asm.
    BackendRun vm = run_on_vm(module, {});
    KHU_CHECK(!vm.ran);
    KHU_CHECK_CONTAINS(vm.runtime_error, "native-only");

    BackendRun native;
    std::string backend_error;
    if (!run_on_native(module, {}, native, backend_error)) return;  // no host compiler
    KHU_CHECK(native.ran);
    KHU_CHECK_EQ(native.error_output, std::string(""));
    KHU_CHECK_EQ(native.output, std::string("42\n"));
}

KHU_TEST(native_emit, inline_asm_compiles_under_a_strict_compiler_mode) {
    std::string compiler = khu::native::find_c_compiler();
    if (compiler.empty()) return;  // nothing to check on a machine with no cc

    khu::bytecode::Module module;
    std::string diagnostics;
    if (!compile_source(program("        int32 value = 0;\n"
                                "        inline_asm {\n"
                                "            \"movb $0, %0\\n\\t\"\n"
                                "            : \"=m\"(value->tag)\n"
                                "            : : \"memory\"\n"
                                "        }\n"
                                "        io.printLine(value);\n"),
                        module, diagnostics)) {
        KHU_FAIL("did not compile:\n" + diagnostics);
        return;
    }
    khu::native::EmitResult emitted = khu::native::emit_c(module, khu::native::EmitOptions{});
    KHU_CHECK_EQ(emitted.error, std::string());

    khu::native::TempDir scratch;
    if (!scratch.ok()) {
        KHU_FAIL("cannot create a scratch directory");
        return;
    }
    std::string path = scratch.file("inline_asm.c");
    scratch.track(path);
    if (!khu::util::write_file(path, emitted.source)) {
        KHU_FAIL("cannot write " + path);
        return;
    }
    std::string report;
    int status = khu::native::run_tool({compiler, "-std=c11", "-O1", "-Wall", "-Wextra",
                                        "-Wpedantic", "-Werror", "-c", path, "-o", "/dev/null"},
                                       report);
    if (status != 0) KHU_FAIL("the emitted inline asm did not compile cleanly:\n" + report);
}
