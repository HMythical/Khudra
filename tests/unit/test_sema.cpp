// Phase 2 acceptance: accept valid programs; reject calling a Procedure by
// name, freeing a pinned manual object, and external read/write of private
// members. Plus the type, memory-model and hierarchy rules those rest on.
#include "test_harness.h"

#include <string>

#include "compiler.h"
#include "sema/symbol.h"

namespace {

class Analysis {
public:
    explicit Analysis(std::string source) {
        std::uint32_t file = compiler_.add_buffer("test.khu", std::move(source));
        program_ = compiler_.analyze(file);
    }

    bool ok() const { return !compiler_.diagnostics().has_errors(); }
    std::string errors() const { return compiler_.diagnostics().render(); }
    std::size_t warning_count() const { return compiler_.diagnostics().warning_count(); }
    khu::sema::Program* program() const { return program_; }

private:
    khu::Compiler compiler_;
    khu::sema::Program* program_ = nullptr;
};

}  // namespace

#define KHU_ACCEPTS(source)                                                        \
    do {                                                                           \
        Analysis analysis((source));                                               \
        if (!analysis.ok()) KHU_FAIL("expected acceptance, got:\n" + analysis.errors()); \
    } while (false)

#define KHU_REJECTS(source, expected_message)                                      \
    do {                                                                           \
        Analysis analysis((source));                                               \
        if (analysis.ok()) {                                                       \
            KHU_FAIL("expected rejection mentioning " +                            \
                     ::khu::test::show(std::string(expected_message)));            \
        } else {                                                                   \
            KHU_CHECK_CONTAINS(analysis.errors(), (expected_message));             \
        }                                                                          \
    } while (false)

// ---------------------------------------------------------------------------
// Acceptance
// ---------------------------------------------------------------------------

KHU_TEST(sema, accepts_a_complete_program) {
    KHU_ACCEPTS(
        "bring khu::stdlib;\n"
        "public class Counter {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setStandard();\n"
        "    public int32 value = 0;\n"
        "    private bool ready = false;\n"
        "\n"
        "    private Procedures(int32 seed) {\n"
        "        io.printLine(\"materializing\");\n"
        "    }\n"
        "\n"
        "    public Counter(int32 seed) {\n"
        "        this.value = seed;\n"
        "        this.ready = true;\n"
        "    }\n"
        "\n"
        "    func bump(int32 by) -> int32 {\n"
        "        this.value = khuStdMath.add(this.value, by);\n"
        "        return this.value;\n"
        "    }\n"
        "\n"
        "    method reset() {\n"
        "        this.value = 0;\n"
        "    }\n"
        "}\n"
        "\n"
        "public class Program {\n"
        "    func main() {\n"
        "        Counter c = Counter(5);\n"
        "        c.bump(3);\n"
        "    }\n"
        "}\n");
}

KHU_TEST(sema, rejects_a_main_on_a_class_that_needs_allocation_arguments) {
    // `main` is an ordinary member, so running it materializes its class --
    // which only works when materialization takes no arguments.
    KHU_REJECTS(
        "public class T {\n"
        "    private Procedures(int32 seed) { }\n"
        "    func main() { }\n"
        "}\n",
        "cannot be materialized as the entry point");
}

KHU_TEST(sema, requires_one_materialization_signature_per_inheritance_chain) {
    KHU_ACCEPTS(
        "public class Base {\n"
        "    public Base(int32 seed) { }\n"
        "}\n"
        "public class Derived extends Base {\n"
        "    public Derived(int32 seed) { }\n"
        "}\n");

    KHU_REJECTS(
        "public class Base {\n"
        "    public Base(int32 seed) { }\n"
        "}\n"
        "public class Derived extends Base {\n"
        "    public Derived(bool flag) { }\n"
        "}\n",
        "declare different materialization parameters");
}

KHU_TEST(sema, records_the_entry_point) {
    Analysis with_main(
        "public class A {\n"
        "    func main() { }\n"
        "}\n");
    KHU_CHECK(with_main.ok());
    KHU_CHECK(with_main.program()->main_function != nullptr);
    KHU_CHECK(with_main.program()->root_class != nullptr);

    // With no main, the first top-level class materializes instead.
    Analysis root_only("public class First { }\npublic class Second { }\n");
    KHU_CHECK(root_only.ok());
    KHU_CHECK(root_only.program()->main_function == nullptr);
    KHU_CHECK_EQ(std::string(root_only.program()->root_class->name), std::string("First"));
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

KHU_TEST(sema, canonicalizes_type_aliases) {
    // i32/int32, u8/byte/uint8 and int/int32 are the same type, so mixing the
    // spellings is fine while mixing the widths is not.
    KHU_ACCEPTS(
        "public class A {\n"
        "    func go() {\n"
        "        i32 a = 1;\n"
        "        int32 b = a;\n"
        "        int c = b;\n"
        "        u8 d = 1;\n"
        "        byte e = d;\n"
        "        uint8 f = e;\n"
        "    }\n"
        "}\n");
}

KHU_TEST(sema, rejects_implicit_widening_and_narrowing) {
    KHU_REJECTS(
        "public class A {\n"
        "    func go() {\n"
        "        int32 small = 1;\n"
        "        int64 wide = small;\n"
        "    }\n"
        "}\n",
        "cannot convert int32 to int64");

    KHU_REJECTS(
        "public class A {\n"
        "    func go() {\n"
        "        int64 wide = 1;\n"
        "        int32 small = wide;\n"
        "    }\n"
        "}\n",
        "no implicit widening or narrowing");

    KHU_REJECTS(
        "public class A {\n"
        "    func go() {\n"
        "        int32 a = 1;\n"
        "        int64 b = 1;\n"
        "        int64 c = a + b;\n"
        "    }\n"
        "}\n",
        "cannot convert");
}

KHU_TEST(sema, accepts_explicit_conversion) {
    KHU_ACCEPTS(
        "public class A {\n"
        "    func go() {\n"
        "        int32 small = 1;\n"
        "        int64 wide = khuStdMath.convertTo(int64, small);\n"
        "        int64 sum = khuStdMath.add(wide, 2);\n"
        "        float narrow = khuStdMath.convertTo(float, 1.5);\n"
        "    }\n"
        "}\n");

    KHU_REJECTS(
        "public class A {\n"
        "    func go() {\n"
        "        int64 wide = khuStdMath.convertTo(1, 2);\n"
        "    }\n"
        "}\n",
        "first argument of khuStdMath.convertTo must be a type");
}

KHU_TEST(sema, defaults_integer_literals_to_int32_and_adapts_to_context) {
    KHU_ACCEPTS(
        "public class A {\n"
        "    public int64 wide = 7;\n"
        "    public uint8 small = 200;\n"
        "    func go() {\n"
        "        int32 natural = 7;\n"
        "        int64 adapted = 7;\n"
        "        uint64 also = 7;\n"
        "    }\n"
        "}\n");

    KHU_REJECTS(
        "public class A {\n"
        "    public uint8 tooBig = 300;\n"
        "}\n",
        "integer literal 300 does not fit in uint8");
}

KHU_TEST(sema, treats_null_as_uninstantiated_reference) {
    KHU_ACCEPTS(
        "public class Node { public int32 v = 0; }\n"
        "public class A {\n"
        "    private Node next = null;\n"
        "    private Array items = null;\n"
        "    private string text = null;\n"
        "    func go() {\n"
        "        Node local = null;\n"
        "        bool empty = local == null;\n"
        "    }\n"
        "}\n");

    KHU_REJECTS(
        "public class A {\n"
        "    public int32 x = null;\n"
        "}\n",
        "cannot convert null to int32");
}

KHU_TEST(sema, checks_conditions_and_returns) {
    KHU_REJECTS(
        "public class A {\n"
        "    func go() {\n"
        "        if (1) { }\n"
        "    }\n"
        "}\n",
        "an if condition must be bool, found int32");

    KHU_REJECTS(
        "public class A {\n"
        "    func go() -> int32 {\n"
        "        io.printLine(\"no return\");\n"
        "    }\n"
        "}\n",
        "control can reach the end of its body without returning a value");

    KHU_REJECTS(
        "public class A {\n"
        "    func go() {\n"
        "        return 1;\n"
        "    }\n"
        "}\n",
        "returns void, so it cannot return a value");

    // No arrow means void, and `-> void` means the same thing.
    KHU_ACCEPTS(
        "public class A {\n"
        "    func a() { return; }\n"
        "    func b() -> void { return; }\n"
        "    func c() -> int32 { if (true) { return 1; } else { return 2; } }\n"
        "}\n");
}

// ---------------------------------------------------------------------------
// Access control
// ---------------------------------------------------------------------------

KHU_TEST(sema, rejects_external_reads_of_private_fields) {
    KHU_REJECTS(
        "public class Secret { private int32 hidden = 0; public int32 shown = 0; }\n"
        "public class Reader {\n"
        "    func go(Secret s) -> int32 {\n"
        "        return s.hidden;\n"
        "    }\n"
        "}\n",
        "cannot read private field 'Secret.hidden' from another object");
}

KHU_TEST(sema, rejects_external_writes_to_private_fields) {
    KHU_REJECTS(
        "public class Secret { private int32 hidden = 0; }\n"
        "public class Writer {\n"
        "    func go(Secret s) {\n"
        "        s.hidden = 1;\n"
        "    }\n"
        "}\n",
        "cannot modify private field 'Secret.hidden' from another object");
}

KHU_TEST(sema, allows_public_members_from_other_objects) {
    KHU_ACCEPTS(
        "public class Open {\n"
        "    public int32 shown = 0;\n"
        "    func read() -> int32 { return this.shown; }\n"
        "}\n"
        "public class User {\n"
        "    func go(Open o) -> int32 {\n"
        "        o.shown = 2;\n"
        "        return o.read();\n"
        "    }\n"
        "}\n");
}

KHU_TEST(sema, private_access_is_per_object_not_per_class) {
    // Stricter than Java on purpose: KHU-PLAN.md says private members are not
    // reachable "from other loaded objects", so another instance of the same
    // class is still outside.
    KHU_REJECTS(
        "public class Pair {\n"
        "    private int32 hidden = 0;\n"
        "    func copyFrom(Pair other) {\n"
        "        this.hidden = other.hidden;\n"
        "    }\n"
        "}\n",
        "cannot read private field 'Pair.hidden' from another object");
}

KHU_TEST(sema, applies_method_visibility_defaults_to_access_control) {
    // `method` is private by default, so it is unreachable from another object.
    KHU_REJECTS(
        "public class Owner {\n"
        "    method hidden() { }\n"
        "    func shown() { }\n"
        "}\n"
        "public class Caller {\n"
        "    func go(Owner o) {\n"
        "        o.hidden();\n"
        "    }\n"
        "}\n",
        "cannot call private method 'Owner.hidden' from another object");

    KHU_ACCEPTS(
        "public class Owner {\n"
        "    public method shown() { }\n"
        "    method hidden() { }\n"
        "    func useOwn() { this.hidden(); }\n"
        "}\n"
        "public class Caller {\n"
        "    func go(Owner o) { o.shown(); }\n"
        "}\n");
}

// ---------------------------------------------------------------------------
// Memory model
// ---------------------------------------------------------------------------

KHU_TEST(sema, requires_a_valid_strategy_initializer) {
    KHU_ACCEPTS(
        "public class A {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "}\n");

    KHU_REJECTS(
        "public class A {\n"
        "    public MemoryAllocationTypeObject type = null;\n"
        "}\n",
        "must be initialized with MemoryAllocationTypeObject.setStandard()");

    KHU_REJECTS(
        "public class A {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setWhatever();\n"
        "}\n",
        "must be initialized with MemoryAllocationTypeObject.setStandard()");
}

KHU_TEST(sema, rejects_two_strategy_fields) {
    KHU_REJECTS(
        "public class A {\n"
        "    public MemoryAllocationTypeObject a = MemoryAllocationTypeObject.setStandard();\n"
        "    public MemoryAllocationTypeObject b = MemoryAllocationTypeObject.setManual();\n"
        "}\n",
        "declares more than one MemoryAllocationTypeObject field");
}

KHU_TEST(sema, rejects_post_construction_strategy_reassignment) {
    KHU_REJECTS(
        "public class A {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setStandard();\n"
        "    func switchIt() {\n"
        "        this.type = MemoryAllocationTypeObject.setManual();\n"
        "    }\n"
        "}\n",
        "the memory strategy is fixed at materialization and cannot be reassigned");
}

KHU_TEST(sema, rejects_freeing_a_collected_object) {
    KHU_REJECTS(
        "public class Managed {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setStandard();\n"
        "}\n"
        "public class A {\n"
        "    func go() {\n"
        "        Managed m = Managed();\n"
        "        free(m);\n"
        "    }\n"
        "}\n",
        "cannot free 'Managed': it is garbage collected");
}

KHU_TEST(sema, rejects_freeing_a_pinned_manual_object) {
    KHU_REJECTS(
        "public class Node {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "    public int32 v = 0;\n"
        "}\n"
        "public class List {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setStandard();\n"
        "    public Node head = null;\n"
        "    func go() {\n"
        "        Node n = Node();\n"
        "        this.head = n;\n"
        "        free(n);\n"
        "    }\n"
        "}\n",
        "cannot free pinned object 'n': it is still held by 'this.head'");
}

KHU_TEST(sema, accepts_a_free_once_the_pin_is_cleared) {
    KHU_ACCEPTS(
        "public class Node {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "    public int32 v = 0;\n"
        "}\n"
        "public class List {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setStandard();\n"
        "    public Node head = null;\n"
        "    func go() {\n"
        "        Node n = Node();\n"
        "        this.head = n;\n"
        "        this.head = null;\n"
        "        free(n);\n"
        "    }\n"
        "}\n");
}

KHU_TEST(sema, rejects_a_double_release) {
    KHU_REJECTS(
        "public class Node {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "}\n"
        "public class A {\n"
        "    func go() {\n"
        "        Node n = Node();\n"
        "        free(n);\n"
        "        dispose(n);\n"
        "    }\n"
        "}\n",
        "is released twice");
}

KHU_TEST(sema, builds_reference_maps_from_the_target_strategy) {
    Analysis analysis(
        "public class Manual {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "}\n"
        "public class Managed {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setStandard();\n"
        "}\n"
        "public class Holder {\n"
        "    public int32 scalar = 0;\n"
        "    public Manual manualRef = null;\n"
        "    public Managed managedRef = null;\n"
        "    public Array items = null;\n"
        "}\n");
    KHU_CHECK(analysis.ok());

    khu::sema::ClassSymbol* holder = analysis.program()->find_class("Holder");
    KHU_CHECK(holder != nullptr);
    if (!holder) return;
    KHU_CHECK(holder->find_field("scalar")->ref_kind == khu::sema::RefKind::Raw);
    KHU_CHECK(holder->find_field("manualRef")->ref_kind == khu::sema::RefKind::ManualRef);
    KHU_CHECK(holder->find_field("managedRef")->ref_kind == khu::sema::RefKind::ManagedRef);
    KHU_CHECK(holder->find_field("items")->ref_kind == khu::sema::RefKind::ManagedRef);
}

KHU_TEST(sema, resolves_allocation_site_strategy_overrides) {
    Analysis analysis(
        "public class Node {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setStandard();\n"
        "}\n"
        "public class A {\n"
        "    func go() {\n"
        "        Node a = Node();\n"
        "        Node b = Node(manual);\n"
        "        Node c = Node(standard);\n"
        "    }\n"
        "}\n");
    KHU_CHECK(analysis.ok());
    if (!analysis.ok()) return;

    khu::sema::ClassSymbol* holder = analysis.program()->find_class("A");
    const auto* body = holder->methods[0]->decl->body;
    auto strategy_of = [&](std::size_t index) {
        const auto* local = body->statements[index]->as<khu::ast::VarDeclStmt>();
        return local->init->info->alloc_strategy;
    };
    KHU_CHECK(strategy_of(0) == khu::sema::Strategy::Gc);
    KHU_CHECK(strategy_of(1) == khu::sema::Strategy::Manual);  // the site wins
    KHU_CHECK(strategy_of(2) == khu::sema::Strategy::Gc);
}

KHU_TEST(sema, rejects_strategy_tokens_away_from_allocation_sites) {
    KHU_REJECTS(
        "public class A {\n"
        "    func take(int32 x) { }\n"
        "    func go() {\n"
        "        this.take(manual);\n"
        "    }\n"
        "}\n",
        "only valid as the first argument of an allocation site");
}

// ---------------------------------------------------------------------------
// Procedures
// ---------------------------------------------------------------------------

KHU_TEST(sema, rejects_calling_a_procedure_by_name) {
    KHU_REJECTS(
        "public class A {\n"
        "    private Procedures { }\n"
        "    func go() {\n"
        "        Procedures();\n"
        "    }\n"
        "}\n",
        "a Procedures block is not invocable by name");

    KHU_REJECTS(
        "public class A {\n"
        "    private Procedures { }\n"
        "}\n"
        "public class B {\n"
        "    func go(A a) {\n"
        "        a.Procedures();\n"
        "    }\n"
        "}\n",
        "a Procedures block is not invocable by name");

    KHU_REJECTS(
        "public class A {\n"
        "    private Procedures { }\n"
        "    func go() {\n"
        "        this.Procedures();\n"
        "    }\n"
        "}\n",
        "a Procedures block is not invocable by name");
}

KHU_TEST(sema, rejects_a_second_procedures_block) {
    KHU_REJECTS(
        "public class A {\n"
        "    private Procedures { }\n"
        "    public Procedures { }\n"
        "}\n",
        "already declares a Procedures block");
}

KHU_TEST(sema, rejects_returning_a_value_from_a_procedure) {
    KHU_REJECTS(
        "public class A {\n"
        "    private Procedures {\n"
        "        return 1;\n"
        "    }\n"
        "}\n",
        "a Procedures block cannot return a value");

    // A bare `return;` leaves the block early and is fine.
    KHU_ACCEPTS(
        "public class A {\n"
        "    private Procedures {\n"
        "        return;\n"
        "    }\n"
        "}\n");
}

KHU_TEST(sema, binds_allocation_arguments_to_procedure_parameters) {
    KHU_ACCEPTS(
        "public class Sprite {\n"
        "    private Procedures(int32 x, int32 y) { }\n"
        "}\n"
        "public class A {\n"
        "    func go() {\n"
        "        Sprite s = Sprite(1, 2);\n"
        "    }\n"
        "}\n");

    KHU_REJECTS(
        "public class Sprite {\n"
        "    private Procedures(int32 x, int32 y) { }\n"
        "}\n"
        "public class A {\n"
        "    func go() {\n"
        "        Sprite s = Sprite(1);\n"
        "    }\n"
        "}\n",
        "materializing 'Sprite' expects 2 arguments, found 1");

    KHU_REJECTS(
        "public class Sprite {\n"
        "    private Procedures(int32 x) { }\n"
        "}\n"
        "public class A {\n"
        "    func go() {\n"
        "        Sprite s = Sprite(true);\n"
        "    }\n"
        "}\n",
        "cannot convert bool to int32");
}

KHU_TEST(sema, rejects_disagreeing_procedure_and_constructor_parameters) {
    // Both are bound to the same allocation-site arguments.
    KHU_REJECTS(
        "public class Sprite {\n"
        "    private Procedures(int32 x) { }\n"
        "    public Sprite(bool flag) { }\n"
        "}\n",
        "declare different parameters, but both are bound to the same allocation-site arguments");
}

KHU_TEST(sema, lets_a_procedure_call_outward) {
    KHU_ACCEPTS(
        "public class Helper {\n"
        "    func assist() -> int32 { return 1; }\n"
        "}\n"
        "public class A {\n"
        "    private Procedures {\n"
        "        io.printLine(\"outward call\");\n"
        "        Helper h = Helper();\n"
        "        h.assist();\n"
        "    }\n"
        "}\n");
}

KHU_TEST(sema, warns_when_a_procedure_reads_constructor_state) {
    // The Procedures block runs before the constructor body, so the field still
    // holds its default here.
    Analysis analysis(
        "public class A {\n"
        "    private int32 later;\n"
        "    private Procedures {\n"
        "        io.printLine(this.later);\n"
        "    }\n"
        "    public A() {\n"
        "        this.later = 7;\n"
        "    }\n"
        "}\n");
    KHU_CHECK(analysis.ok());
    KHU_CHECK_EQ(analysis.warning_count(), static_cast<std::size_t>(1));
    KHU_CHECK_CONTAINS(analysis.errors(),
                       "is assigned by the constructor, which runs after this Procedures block");
}

// ---------------------------------------------------------------------------
// Hierarchy
// ---------------------------------------------------------------------------

KHU_TEST(sema, resolves_inheritance_and_builds_vtables) {
    Analysis analysis(
        "public class Base {\n"
        "    public int32 a = 0;\n"
        "    func speak() -> int32 { return 1; }\n"
        "    func onlyBase() { }\n"
        "}\n"
        "public class Derived extends Base {\n"
        "    public int32 b = 0;\n"
        "    func speak() -> int32 { return 2; }\n"
        "    func extra() { }\n"
        "}\n");
    KHU_CHECK(analysis.ok());
    if (!analysis.ok()) return;

    khu::sema::ClassSymbol* derived = analysis.program()->find_class("Derived");
    khu::sema::ClassSymbol* base = analysis.program()->find_class("Base");
    KHU_CHECK(derived->base == base);
    // Base fields come first in the layout.
    KHU_CHECK_EQ(derived->layout.size(), static_cast<std::size_t>(2));
    KHU_CHECK_EQ(std::string(derived->layout[0]->name), std::string("a"));
    KHU_CHECK_EQ(derived->layout[0]->slot, static_cast<std::uint32_t>(0));
    KHU_CHECK_EQ(derived->layout[1]->slot, static_cast<std::uint32_t>(1));

    // The override replaces the inherited entry; new methods append.
    KHU_CHECK_EQ(derived->vtable.size(), static_cast<std::size_t>(3));
    KHU_CHECK_EQ(std::string(derived->vtable[0]->name), std::string("speak"));
    KHU_CHECK(derived->vtable[0]->owner == derived);
    KHU_CHECK(derived->vtable[1]->owner == base);
    KHU_CHECK_EQ(std::string(derived->vtable[2]->name), std::string("extra"));
}

KHU_TEST(sema, accepts_a_derived_reference_where_a_base_is_expected) {
    KHU_ACCEPTS(
        "public class Base { public int32 a = 0; }\n"
        "public class Derived extends Base { }\n"
        "public class A {\n"
        "    func go() {\n"
        "        Base b = Derived();\n"
        "        int32 v = b.a;\n"
        "    }\n"
        "}\n");

    KHU_REJECTS(
        "public class Base { }\n"
        "public class Derived extends Base { }\n"
        "public class A {\n"
        "    func go() {\n"
        "        Derived d = Base();\n"
        "    }\n"
        "}\n",
        "cannot convert Base to Derived");
}

KHU_TEST(sema, rejects_broken_hierarchies) {
    KHU_REJECTS("public class A extends Missing { }\n", "unknown base class 'Missing'");
    KHU_REJECTS("public class A extends A { }\n", "cannot extend itself");
    KHU_REJECTS("public class A extends B { }\npublic class B extends A { }\n",
                "inheritance cycle");
    KHU_REJECTS(
        "public class Base { public int32 x = 0; }\n"
        "public class Derived extends Base { public int32 x = 0; }\n",
        "shadows a field inherited from 'Base'");
    KHU_REJECTS(
        "public class Base { func f() -> int32 { return 1; } }\n"
        "public class Derived extends Base { func f() -> int64 { return 1; } }\n",
        "changes the return type");
}

KHU_TEST(sema, rejects_duplicate_declarations) {
    KHU_REJECTS("public class A { }\npublic class A { }\n", "class 'A' is already declared");
    KHU_REJECTS("public class A { public int32 x = 0; public int32 x = 0; }\n",
                "field 'x' is already declared");
    KHU_REJECTS("public class A { func f() { } func f() { } }\n",
                "is already declared with the same parameter types");
    KHU_REJECTS("public class A { public A() { } public A() { } }\n",
                "already declares a constructor");
    KHU_REJECTS(
        "public class A {\n"
        "    func go() {\n"
        "        int32 x = 1;\n"
        "        int32 x = 2;\n"
        "    }\n"
        "}\n",
        "'x' is already declared in this scope");
}

KHU_TEST(sema, reports_unknown_names_with_locations) {
    Analysis analysis(
        "public class A {\n"
        "    func go() {\n"
        "        missing = 1;\n"
        "    }\n"
        "}\n");
    KHU_CHECK(!analysis.ok());
    KHU_CHECK_CONTAINS(analysis.errors(), "test.khu:3:9: error: unknown name 'missing'");

    KHU_REJECTS(
        "public class A {\n"
        "    func go() { nope(); }\n"
        "}\n",
        "unknown function 'nope'");
    KHU_REJECTS("public class A { public Missing x = null; }\n", "unknown type 'Missing'");
    KHU_REJECTS(
        "public class A {\n"
        "    func go() { khu.notThere(); }\n"
        "}\n",
        "namespace 'khu' has no member 'notThere'");
}

KHU_TEST(sema, rejects_overloads_that_only_differ_by_width) {
    KHU_REJECTS(
        "public class A {\n"
        "    func go() {\n"
        "        int32 a = 1;\n"
        "        int64 b = 1;\n"
        "        int32 c = khuStdMath.add(a, b);\n"
        "    }\n"
        "}\n",
        "no declaration of 'khuStdMath.add' matches (int32, int64)");
}
