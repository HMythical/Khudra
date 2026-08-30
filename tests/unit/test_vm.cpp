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

// ---------------------------------------------------------------------------
// Phase 4: objects, fields and dispatch
// ---------------------------------------------------------------------------

KHU_TEST(objects, materialize_and_read_back_fields) {
    check_output(
        "public class Point {\n"
        "    public int32 x = 0;\n"
        "    public int32 y = 0;\n"
        "    public Point(int32 a, int32 b) {\n"
        "        this.x = a;\n"
        "        this.y = b;\n"
        "    }\n"
        "}\n"
        "public class T {\n"
        "    func main() {\n"
        "        Point p = Point(3, 4);\n"
        "        io.print(p.x);\n"
        "        io.print(\",\");\n"
        "        io.printLine(p.y);\n"
        "        p.x = 9;\n"
        "        io.printLine(p.x);\n"
        "    }\n"
        "}\n",
        "3,4\n9\n");
}

KHU_TEST(objects, apply_field_initializers_before_the_constructor) {
    check_output(
        "public class Boxed {\n"
        "    public int32 count = 7;\n"
        "    public string label = \"initial\";\n"
        "    public bool flag = true;\n"
        "    public Boxed() {\n"
        "        io.printLine(this.count);\n"
        "        io.printLine(this.label);\n"
        "        this.label = \"constructed\";\n"
        "    }\n"
        "}\n"
        "public class T {\n"
        "    func main() {\n"
        "        Boxed b = Boxed();\n"
        "        io.printLine(b.label);\n"
        "        io.printLine(b.flag);\n"
        "    }\n"
        "}\n",
        "7\ninitial\nconstructed\ntrue\n");
}

KHU_TEST(objects, default_uninitialized_fields_to_null_and_zero) {
    check_output(
        "public class Bare {\n"
        "    public int32 number;\n"
        "    public bool flag;\n"
        "    public Bare other;\n"
        "}\n"
        "public class T {\n"
        "    func main() {\n"
        "        Bare b = Bare();\n"
        "        io.printLine(b.number);\n"
        "        io.printLine(b.flag);\n"
        "        io.printLine(b.other == null);\n"
        "    }\n"
        "}\n",
        "0\nfalse\ntrue\n");
}

KHU_TEST(objects, run_procedures_before_the_constructor) {
    check_output(
        "public class Ordered {\n"
        "    private Procedures(int32 seed) {\n"
        "        io.print(\"procedures \");\n"
        "        io.printLine(seed);\n"
        "    }\n"
        "    public Ordered(int32 seed) {\n"
        "        io.print(\"constructor \");\n"
        "        io.printLine(seed);\n"
        "    }\n"
        "}\n"
        "public class T {\n"
        "    func main() {\n"
        "        Ordered o = Ordered(5);\n"
        "        io.printLine(\"site resumes\");\n"
        "    }\n"
        "}\n",
        "procedures 5\nconstructor 5\nsite resumes\n");
}

KHU_TEST(objects, dispatch_virtually_on_the_receivers_class) {
    check_output(
        "public class Base {\n"
        "    func speak() -> string { return \"base\"; }\n"
        "    func echo() -> string { return this.speak(); }\n"
        "}\n"
        "public class Middle extends Base {\n"
        "    func speak() -> string { return \"middle\"; }\n"
        "}\n"
        "public class Leaf extends Middle { }\n"
        "public class T {\n"
        "    func main() {\n"
        "        Base a = Base();\n"
        "        Base b = Middle();\n"
        "        Base c = Leaf();\n"
        "        io.printLine(a.echo());\n"
        "        io.printLine(b.echo());\n"
        "        io.printLine(c.echo());\n"
        "    }\n"
        "}\n",
        "base\nmiddle\nmiddle\n");
}

KHU_TEST(objects, lay_base_fields_out_first) {
    check_output(
        "public class Base {\n"
        "    public int32 a = 1;\n"
        "    public int32 b = 2;\n"
        "}\n"
        "public class Derived extends Base {\n"
        "    public int32 c = 3;\n"
        "}\n"
        "public class T {\n"
        "    func main() {\n"
        "        Derived d = Derived();\n"
        "        Base viewed = d;\n"
        "        d.c = 30;\n"
        "        viewed.a = 10;\n"
        "        io.print(d.a);\n"
        "        io.print(d.b);\n"
        "        io.printLine(d.c);\n"
        "    }\n"
        "}\n",
        "10230\n");
}

KHU_TEST(objects, chain_procedures_and_constructors_base_first) {
    check_output(
        "public class Base {\n"
        "    private Procedures { io.printLine(\"base procedures\"); }\n"
        "    public Base() { io.printLine(\"base constructor\"); }\n"
        "}\n"
        "public class Derived extends Base {\n"
        "    private Procedures { io.printLine(\"derived procedures\"); }\n"
        "    public Derived() { io.printLine(\"derived constructor\"); }\n"
        "}\n"
        "public class T {\n"
        "    func main() { Derived d = Derived(); }\n"
        "}\n",
        "base procedures\nderived procedures\nbase constructor\nderived constructor\n");
}

KHU_TEST(objects, trap_on_reaching_through_null) {
    RunResult result = run_source(
        "public class Node { public int32 v = 0; }\n"
        "public class T {\n"
        "    func main() {\n"
        "        Node n = null;\n"
        "        io.printLine(n.v);\n"
        "    }\n"
        "}\n");
    KHU_CHECK(result.compiled);
    KHU_CHECK(!result.ran);
    KHU_CHECK_CONTAINS(result.runtime_error, "the object is not instantiated yet");
}

KHU_TEST(objects, honour_the_allocation_site_strategy) {
    check_output(
        "public class Node {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "    public int32 v = 0;\n"
        "}\n"
        "public class T {\n"
        "    func main() {\n"
        "        Node n = Node();\n"
        "        n.v = 5;\n"
        "        io.printLine(n.v);\n"
        "        free(n);\n"
        "        io.printLine(\"released\");\n"
        "    }\n"
        "}\n",
        "5\nreleased\n");
}

KHU_TEST(objects, trap_on_use_after_free) {
    // Freeing a manual object leaves its chunk in the class arena, so a stale
    // alias still points at readable memory. The runtime must trap rather than
    // let the field read through the released object silently succeed.
    RunResult result = run_source(
        "public class Node {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "    public int32 v = 42;\n"
        "}\n"
        "public class T {\n"
        "    func main() {\n"
        "        Node n = Node();\n"
        "        Node alias = n;\n"
        "        free(n);\n"
        "        io.printLine(alias.v);\n"
        "    }\n"
        "}\n");
    KHU_CHECK(result.compiled);
    KHU_CHECK(!result.ran);
    KHU_CHECK_CONTAINS(result.runtime_error, "use of an object after it was released");
}

KHU_TEST(objects, reject_freeing_a_collected_object_before_it_runs) {
    // A reference's class is known statically even through a parameter, so the
    // checker catches this and the program never reaches the VM. The VM keeps
    // the same check as a guard against a hand-written image.
    RunResult result = run_source(
        "public class Managed { public int32 v = 0; }\n"
        "public class T {\n"
        "    func release(Managed m) { free(m); }\n"
        "    func main() { this.release(Managed()); }\n"
        "}\n");
    KHU_CHECK(!result.compiled);
    KHU_CHECK_CONTAINS(result.diagnostics, "it is garbage collected");
}

KHU_TEST(objects, refuse_to_free_a_pinned_object_at_runtime) {
    // The pin count lives in the object header, so the VM enforces it even
    // where the checker's intra-method analysis cannot see the assignment --
    // here the store happens inside another method.
    RunResult result = run_source(
        "public class Node {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "    public int32 v = 0;\n"
        "}\n"
        "public class Holder {\n"
        "    public Node held = null;\n"
        "    func keep(Node n) { this.held = n; }\n"
        "    func main() {\n"
        "        Node n = Node();\n"
        "        this.keep(n);\n"
        "        free(n);\n"
        "    }\n"
        "}\n");
    KHU_CHECK(result.compiled);
    KHU_CHECK(!result.ran);
    KHU_CHECK_CONTAINS(result.runtime_error, "it is still held by 'Holder.held'");
}

KHU_TEST(objects, track_the_heap) {
    khu::Compiler compiler;
    std::uint32_t file = compiler.add_buffer(
        "heap.khu",
        "public class Managed { public int32 v = 0; }\n"
        "public class Owned {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "}\n"
        "public class T {\n"
        "    func main() {\n"
        "        Managed a = Managed();\n"
        "        Managed b = Managed();\n"
        "        Owned c = Owned();\n"
        "    }\n"
        "}\n");
    khu::bytecode::Module module;
    KHU_CHECK(compiler.compile(file, module));
    if (compiler.diagnostics().has_errors()) return;

    khu::vm::Vm vm(module);
    std::string output;
    vm.set_output_sink(&output);
    KHU_CHECK(vm.run());
    // Two Managed instances plus the entry T instance are collected; Owned is
    // manual.
    KHU_CHECK_EQ(vm.heap().managed_count(), static_cast<std::size_t>(3));
    KHU_CHECK_EQ(vm.heap().manual_count(), static_cast<std::size_t>(1));
}

KHU_TEST(codegen, emits_a_line_table_for_stack_traces) {
    khu::Compiler compiler;
    std::uint32_t file = compiler.add_buffer("lines.khu", program("        io.printLine(1);\n"
                                                                  "        io.printLine(2);"));
    khu::bytecode::Module module;
    KHU_CHECK(compiler.compile(file, module));
    if (compiler.diagnostics().has_errors()) return;

    // Method 0 belongs to whichever class the standard library declared first,
    // so `main` is found by name.
    const khu::bytecode::MethodEntry* found = nullptr;
    for (const khu::bytecode::MethodEntry& method : module.methods) {
        if (module.string_at(method.name) == "main") found = &method;
    }
    if (!found) {
        KHU_FAIL("the image has no 'main'");
        return;
    }
    const khu::bytecode::MethodEntry& main = *found;
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
        // The image also carries the standard library's own classes, so the
        // program's are found by name rather than by index.
        const khu::bytecode::ClassEntry* manual_entry = nullptr;
        const khu::bytecode::ClassEntry* holder_entry = nullptr;
        for (const khu::bytecode::ClassEntry& entry : module.classes) {
            std::string_view name = module.string_at(entry.name);
            if (name == "Manual") manual_entry = &entry;
            if (name == "Holder") holder_entry = &entry;
        }
        if (!manual_entry || !holder_entry) {
            KHU_FAIL("the image does not carry both program classes");
            return;
        }
        const khu::bytecode::ClassEntry& manual = *manual_entry;
        KHU_CHECK_EQ(manual.flags, static_cast<std::uint32_t>(khu::bytecode::kClassManual));

        const khu::bytecode::ClassEntry& holder = *holder_entry;
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
    // The class id depends on how many classes the standard library brought
    // with it, so the listing is checked for the annotated form.
    KHU_CHECK_CONTAINS(listing, ", standard    ; Node");
    KHU_CHECK_CONTAINS(listing, ", manual    ; Node");
}
