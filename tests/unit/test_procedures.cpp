// Phase 6 acceptance: the Procedures block runs on GC and manual instances,
// before the allocation site resumes, receives its arguments, and is not
// invocable by name -- driven by the C engine in utils/proc_engine.c.
#include "test_harness.h"

#include <cstdint>
#include <string>

extern "C" {
#include "proc_engine.h"
}

#include "bytecode/module.h"
#include "compiler.h"
#include "vm/vm.h"

namespace {

struct ProcRun {
    bool compiled = false;
    bool ran = false;
    std::string output;
    std::string diagnostics;
    std::string runtime_error;
    std::uint64_t materializations = 0;
};

ProcRun run_program(const std::string& source) {
    ProcRun result;
    khu::Compiler compiler;
    std::uint32_t file = compiler.add_buffer("proc.khu", source);

    khu::bytecode::Module module;
    result.compiled = compiler.compile(file, module);
    result.diagnostics = compiler.diagnostics().render();
    if (!result.compiled) return result;

    khu_proc_reset_stats();
    khu::vm::Vm vm(module);
    vm.set_output_sink(&result.output);
    result.ran = vm.run();
    result.runtime_error = vm.error();
    result.materializations = khu_proc_materialize_count();
    return result;
}

void check_output(const std::string& source, const std::string& expected) {
    ProcRun result = run_program(source);
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

KHU_TEST(procedures, run_before_the_allocation_site_resumes) {
    check_output(
        "public class Loaded {\n"
        "    private Procedures {\n"
        "        io.printLine(\"1 procedures\");\n"
        "    }\n"
        "    public Loaded() {\n"
        "        io.printLine(\"2 constructor\");\n"
        "    }\n"
        "}\n"
        "public class T {\n"
        "    func main() {\n"
        "        Loaded a = Loaded();\n"
        "        io.printLine(\"3 site resumes\");\n"
        "    }\n"
        "}\n",
        "1 procedures\n2 constructor\n3 site resumes\n");
}

KHU_TEST(procedures, run_on_both_gc_and_manual_instances) {
    check_output(
        "public class Collected {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setStandard();\n"
        "    private Procedures { io.printLine(\"collected instance loaded\"); }\n"
        "}\n"
        "public class Owned {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "    private Procedures { io.printLine(\"manual instance loaded\"); }\n"
        "}\n"
        "public class T {\n"
        "    func main() {\n"
        "        Collected c = Collected();\n"
        "        Owned m = Owned();\n"
        "        // A site override still runs the block.\n"
        "        Collected forced = Collected(manual);\n"
        "        free(m);\n"
        "        free(forced);\n"
        "    }\n"
        "}\n",
        "collected instance loaded\nmanual instance loaded\ncollected instance loaded\n");
}

KHU_TEST(procedures, receive_the_allocation_site_arguments) {
    check_output(
        "public class Sprite {\n"
        "    public int32 x = 0;\n"
        "    public int32 y = 0;\n"
        "    private Procedures(int32 px, int32 py) {\n"
        "        io.print(\"procedures got \");\n"
        "        io.print(px);\n"
        "        io.print(\",\");\n"
        "        io.printLine(py);\n"
        "    }\n"
        "    public Sprite(int32 px, int32 py) {\n"
        "        this.x = px;\n"
        "        this.y = py;\n"
        "    }\n"
        "}\n"
        "public class T {\n"
        "    func main() {\n"
        "        Sprite s = Sprite(3, 9);\n"
        "        io.print(s.x);\n"
        "        io.print(\",\");\n"
        "        io.printLine(s.y);\n"
        "    }\n"
        "}\n",
        "procedures got 3,9\n3,9\n");
}

KHU_TEST(procedures, may_call_outward_and_materialize_other_objects) {
    // Nested allocations run depth-first: the inner object is fully
    // materialized before the outer block's next statement.
    check_output(
        "public class Inner {\n"
        "    private Procedures { io.printLine(\"  inner procedures\"); }\n"
        "    public Inner() { io.printLine(\"  inner constructor\"); }\n"
        "}\n"
        "public class Helper {\n"
        "    func assist() -> int32 { return 42; }\n"
        "}\n"
        "public class Outer {\n"
        "    private Procedures {\n"
        "        io.printLine(\"outer procedures start\");\n"
        "        Inner i = Inner();\n"
        "        Helper h = Helper();\n"
        "        io.printLine(h.assist());\n"
        "        io.printLine(\"outer procedures end\");\n"
        "    }\n"
        "}\n"
        "public class T {\n"
        "    func main() { Outer o = Outer(); }\n"
        "}\n",
        "outer procedures start\n  inner procedures\n  inner constructor\n42\n"
        "outer procedures end\n");
}

KHU_TEST(procedures, run_exactly_once_per_instance) {
    ProcRun result = run_program(
        "public class Counted {\n"
        "    private Procedures { io.printLine(\"loaded\"); }\n"
        "}\n"
        "public class T {\n"
        "    func main() {\n"
        "        Counted a = Counted();\n"
        "        Counted b = Counted();\n"
        "        io.printLine(a.equalsNothing());\n"
        "    }\n"
        "    func unused() { }\n"
        "}\n");
    // The bad call is a compile error; what matters is that it is a *name*
    // error, not a second run of anything.
    KHU_CHECK(!result.compiled);
    KHU_CHECK_CONTAINS(result.diagnostics, "has no function or method 'equalsNothing'");

    ProcRun clean = run_program(
        "public class Counted {\n"
        "    private Procedures { io.printLine(\"loaded\"); }\n"
        "}\n"
        "public class T {\n"
        "    func touch(Counted c) { }\n"
        "    func main() {\n"
        "        Counted a = Counted();\n"
        "        this.touch(a);\n"
        "        this.touch(a);\n"
        "    }\n"
        "}\n");
    KHU_CHECK(clean.ran);
    // One instance, one run -- calling into it later never re-runs the block.
    KHU_CHECK_EQ(clean.output, std::string("loaded\n"));
    // Two materializations: the entry object and the Counted.
    KHU_CHECK_EQ(clean.materializations, static_cast<std::uint64_t>(2));
}

KHU_TEST(procedures, cannot_be_invoked_by_name) {
    ProcRun bare = run_program(
        "public class T {\n"
        "    private Procedures { }\n"
        "    func main() { Procedures(); }\n"
        "}\n");
    KHU_CHECK(!bare.compiled);
    KHU_CHECK_CONTAINS(bare.diagnostics, "not invocable by name");

    ProcRun through_object = run_program(
        "public class A { private Procedures { } }\n"
        "public class T {\n"
        "    func main() {\n"
        "        A a = A();\n"
        "        a.Procedures();\n"
        "    }\n"
        "}\n");
    KHU_CHECK(!through_object.compiled);
    KHU_CHECK_CONTAINS(through_object.diagnostics, "not invocable by name");
}

KHU_TEST(procedures, abort_the_whole_materialization_when_a_block_fails) {
    // The constructor must not run, and the reference must never reach the
    // allocation site (docs/procedures.md, section 5).
    ProcRun result = run_program(
        "public class Fragile {\n"
        "    private Procedures {\n"
        "        io.printLine(\"procedures start\");\n"
        "        int32 zero = 0;\n"
        "        io.printLine(khuStdMath.divide(1, zero));\n"
        "        io.printLine(\"procedures end\");\n"
        "    }\n"
        "    public Fragile() {\n"
        "        io.printLine(\"constructor should not run\");\n"
        "    }\n"
        "}\n"
        "public class T {\n"
        "    func main() {\n"
        "        Fragile f = Fragile();\n"
        "        io.printLine(\"site should not resume\");\n"
        "    }\n"
        "}\n");
    KHU_CHECK(result.compiled);
    KHU_CHECK(!result.ran);
    KHU_CHECK_EQ(result.output, std::string("procedures start\n"));
    KHU_CHECK_CONTAINS(result.runtime_error, "division by zero");
    // Nothing completed, so nothing was counted as materialized.
    KHU_CHECK_EQ(result.materializations, static_cast<std::uint64_t>(1));  // just the entry object
}

KHU_TEST(procedures, guard_against_a_materialization_cycle) {
    ProcRun result = run_program(
        "public class A { private Procedures { B b = B(); } }\n"
        "public class B { private Procedures { A a = A(); } }\n"
        "public class T {\n"
        "    func main() { A a = A(); }\n"
        "}\n");
    KHU_CHECK(result.compiled);
    KHU_CHECK(!result.ran);
    KHU_CHECK_CONTAINS(result.runtime_error, "materialization nested too deeply");
    // The trace is capped rather than printing every frame of the recursion.
    KHU_CHECK_CONTAINS(result.runtime_error, "more frames");
}

KHU_TEST(procedures, chain_base_first_through_inheritance) {
    check_output(
        "public class Base {\n"
        "    private Procedures { io.printLine(\"base procedures\"); }\n"
        "    public Base() { io.printLine(\"base constructor\"); }\n"
        "}\n"
        "public class Middle extends Base {\n"
        "    private Procedures { io.printLine(\"middle procedures\"); }\n"
        "}\n"
        "public class Leaf extends Middle {\n"
        "    private Procedures { io.printLine(\"leaf procedures\"); }\n"
        "    public Leaf() { io.printLine(\"leaf constructor\"); }\n"
        "}\n"
        "public class T {\n"
        "    func main() { Leaf l = Leaf(); }\n"
        "}\n",
        "base procedures\nmiddle procedures\nleaf procedures\n"
        "base constructor\nleaf constructor\n");
}

KHU_TEST(procedures, fire_for_the_root_class_entry_form) {
    // With no `main`, the first top-level class is materialized and its
    // Procedures block fires -- and nothing is called afterwards.
    check_output(
        "public class Root {\n"
        "    private Procedures { io.printLine(\"root loaded\"); }\n"
        "    public Root() { io.printLine(\"root constructed\"); }\n"
        "    func neverCalled() { io.printLine(\"should not appear\"); }\n"
        "}\n",
        "root loaded\nroot constructed\n");
}

KHU_TEST(procedures, engine_tracks_depth_and_returns_to_zero) {
    ProcRun result = run_program(
        "public class Inner { private Procedures { } }\n"
        "public class Outer { private Procedures { Inner i = Inner(); } }\n"
        "public class T {\n"
        "    func main() { Outer o = Outer(); }\n"
        "}\n");
    KHU_CHECK(result.ran);
    // Nesting unwinds completely: entry object, Outer, Inner.
    KHU_CHECK_EQ(khu_proc_depth(), static_cast<std::uint32_t>(0));
    KHU_CHECK_EQ(result.materializations, static_cast<std::uint64_t>(3));
}
