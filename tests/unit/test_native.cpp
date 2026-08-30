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
    std::string runtime_error;
};

BackendRun run_on_vm(const khu::bytecode::Module& module, const std::string& input) {
    BackendRun result;
    khu::vm::Vm vm(module);
    vm.set_output_sink(&result.output);
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
    // and the frame is one array the collector can walk.
    KHU_CHECK_CONTAINS(emitted.source, "static int khu_m0(KhuValue self, const KhuValue* argv,");
    KHU_CHECK_CONTAINS(emitted.source, "KhuValue V[3];");
    KHU_CHECK_CONTAINS(emitted.source,
                       "khu_rt_frame_enter(&F, V, 0u, 1u, 3u, self, argv, 0u);");
    KHU_CHECK_CONTAINS(emitted.source, "V[1] = khu_rt_constants[");
    KHU_CHECK_CONTAINS(emitted.source, "khu_val_arith(KHU_A_ADD, 3u, &V[1], &V[2], &V[1])");
    // The safepoint publishes where we are and how much of the frame is live.
    KHU_CHECK_CONTAINS(emitted.source, "F.height = 2u;");
    KHU_CHECK_CONTAINS(emitted.source, "khu_rt_call_native(&F, 2u, 1u, &V[1], &V[1], 0)");
    KHU_CHECK_CONTAINS(emitted.source, "KHU_RETURN(&F);");
    KHU_CHECK_CONTAINS(emitted.source, "const KhuNativeMethod khu_native_methods[] = {");
    KHU_CHECK_CONTAINS(emitted.source, "const uint32_t khu_native_method_count = 1u;");
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
