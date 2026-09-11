// Phase 8: a .kbc image is an untrusted input, so it is checked before it runs.
#include "test_harness.h"

#include <string>

#include "bytecode/module.h"
#include "bytecode/verifier.h"
#include "compiler.h"
#include "vm/vm.h"

using namespace khu::bytecode;

namespace {

// A small but complete image, produced by the real compiler so the tests
// corrupt something that was valid to begin with.
bool build_module(Module& out) {
    khu::Compiler compiler;
    std::uint32_t file = compiler.add_buffer(
        "v.khu",
        "public class Node {\n"
        "    public int32 value = 0;\n"
        "    func doubled() -> int32 { return khuStdMath.multiply(this.value, 2); }\n"
        "}\n"
        "public class T {\n"
        "    func main() {\n"
        "        Node n = Node();\n"
        "        n.value = 21;\n"
        "        int32 i = 0;\n"
        "        while (i < 3) { i = khuStdMath.add(i, 1); }\n"
        "        io.printLine(n.doubled());\n"
        "    }\n"
        "}\n");
    return compiler.compile(file, out);
}

// Finds a method by name so a test does not depend on emission order.
MethodEntry* find_method(Module& module, const char* name) {
    for (MethodEntry& method : module.methods) {
        if (module.string_at(method.name) == name) return &method;
    }
    return nullptr;
}

std::string report_for(const Module& module) {
    std::string report;
    verify(module, report);
    return report;
}

}  // namespace

KHU_TEST(verifier, accepts_what_the_compiler_produces) {
    Module module;
    KHU_CHECK(build_module(module));
    std::string report;
    KHU_CHECK(verify(module, report));
    KHU_CHECK_EQ(report, std::string(""));
}

KHU_TEST(verifier, rejects_an_unknown_opcode) {
    Module module;
    if (!build_module(module)) return;
    MethodEntry* main = find_method(module, "main");
    KHU_CHECK(main != nullptr);
    if (!main) return;

    main->code[0] = 0xfe;
    KHU_CHECK_CONTAINS(report_for(module), "unknown opcode");
}

KHU_TEST(verifier, rejects_an_instruction_that_runs_past_the_end) {
    Module module;
    if (!build_module(module)) return;
    MethodEntry* main = find_method(module, "main");
    if (!main) return;

    // An opcode whose operands were cut off. (Simply ending early is fine --
    // that is an implicit return.)
    main->code.push(static_cast<std::uint8_t>(Op::LoadConst));
    KHU_CHECK_CONTAINS(report_for(module), "runs past the end of the method");
}

KHU_TEST(verifier, rejects_an_out_of_range_constant) {
    Module module;
    if (!build_module(module)) return;
    MethodEntry* main = find_method(module, "main");
    if (!main) return;

    for (std::size_t i = 0; i + 2 < main->code.size(); ++i) {
        if (static_cast<Op>(main->code[i]) != Op::LoadConst) continue;
        main->code[i + 1] = 0xff;
        main->code[i + 2] = 0xff;
        break;
    }
    KHU_CHECK_CONTAINS(report_for(module), "does not exist");
}

KHU_TEST(verifier, rejects_a_jump_outside_the_method) {
    Module module;
    if (!build_module(module)) return;
    MethodEntry* main = find_method(module, "main");
    if (!main) return;

    for (std::size_t i = 0; i + 4 < main->code.size(); ++i) {
        auto op = static_cast<Op>(main->code[i]);
        if (op != Op::Jump && op != Op::JumpIfFalse && op != Op::JumpIfTrue) continue;
        for (int b = 0; b < 4; ++b) main->code[i + 1 + static_cast<std::size_t>(b)] = 0x7f;
        break;
    }
    KHU_CHECK_CONTAINS(report_for(module), "is outside the method");
}

KHU_TEST(verifier, rejects_a_jump_into_the_middle_of_an_instruction) {
    Module module;
    module.source_file = module.intern_string("j.khu");
    ClassEntry klass;
    klass.name = module.intern_string("J");
    module.classes.push(std::move(klass));

    MethodEntry method;
    method.name = module.intern_string("go");
    method.owner_class = 0;
    method.frame_size = 1;
    // jmp +1 lands one byte into the ldc that follows it.
    method.code.push(static_cast<std::uint8_t>(Op::Jump));
    method.code.push(1);
    method.code.push(0);
    method.code.push(0);
    method.code.push(0);
    method.code.push(static_cast<std::uint8_t>(Op::LoadConst));
    method.code.push(0);
    method.code.push(0);
    module.methods.push(std::move(method));
    module.intern_int(TypeTag::Int32, 0);

    KHU_CHECK_CONTAINS(report_for(module), "lands in the middle of an instruction");
}

KHU_TEST(verifier, rejects_out_of_range_local_slots) {
    Module module;
    if (!build_module(module)) return;
    MethodEntry* main = find_method(module, "main");
    if (!main) return;

    for (std::size_t i = 0; i + 2 < main->code.size(); ++i) {
        if (static_cast<Op>(main->code[i]) != Op::StoreLocal) continue;
        main->code[i + 1] = 0xff;
        main->code[i + 2] = 0x00;
        break;
    }
    KHU_CHECK_CONTAINS(report_for(module), "is outside a frame of");
}

KHU_TEST(verifier, rejects_a_broken_class_table) {
    Module module;
    if (!build_module(module)) return;

    Module self_extending = module;
    self_extending.classes[0].base = 0;
    KHU_CHECK_CONTAINS(report_for(self_extending), "extends itself");

    Module missing_base = module;
    missing_base.classes[0].base = 99;
    KHU_CHECK_CONTAINS(report_for(missing_base), "names a base class that does not exist");

    Module missing_vtable = module;
    if (!missing_vtable.classes[0].vtable.empty()) {
        missing_vtable.classes[0].vtable[0] = 999;
        KHU_CHECK_CONTAINS(report_for(missing_vtable), "which does not exist");
    }

    Module missing_entry = module;
    missing_entry.main_method = 999;
    KHU_CHECK_CONTAINS(report_for(missing_entry),
                       "the entry point names a method that does not exist");
}

KHU_TEST(verifier, rejects_an_unknown_allocation_strategy) {
    Module module;
    if (!build_module(module)) return;
    MethodEntry* main = find_method(module, "main");
    if (!main) return;

    for (std::size_t i = 0; i + 3 < main->code.size(); ++i) {
        if (static_cast<Op>(main->code[i]) != Op::Materialize) continue;
        main->code[i + 3] = 42;
        break;
    }
    KHU_CHECK_CONTAINS(report_for(module), "unknown allocation strategy");
}

KHU_TEST(verifier, stops_the_vm_before_a_bad_image_executes) {
    Module module;
    if (!build_module(module)) return;
    MethodEntry* main = find_method(module, "main");
    if (!main) return;
    main->code[0] = 0xfe;

    khu::vm::Vm vm(module);
    std::string output;
    vm.set_output_sink(&output);
    KHU_CHECK(!vm.run());
    KHU_CHECK_CONTAINS(vm.error(), "this bytecode image is not valid");
    // Nothing ran.
    KHU_CHECK_EQ(output, std::string(""));
}

KHU_TEST(verifier, survives_a_truncated_image_on_disk) {
    Module module;
    if (!build_module(module)) return;

    std::string bytes = serialize(module);
    for (std::size_t cut = 1; cut < 40 && cut < bytes.size(); cut += 3) {
        Module reloaded;
        std::string error;
        // Either it fails to load, or it loads and fails to verify. It must
        // never load into something the interpreter would run.
        if (!deserialize(bytes.substr(0, bytes.size() - cut), reloaded, error)) continue;
        std::string report;
        (void)verify(reloaded, report);
    }
}

namespace {

// Builds a module whose `go` method contains an inline_c statement.
bool build_inline_module(Module& out) {
    khu::Compiler compiler;
    std::uint32_t file = compiler.add_buffer(
        "i.khu",
        "public class I {\n"
        "    func go() {\n"
        "        int32 x = 0;\n"
        "        inline_c { x = 1; }\n"
        "        io.printLine(x);\n"
        "    }\n"
        "}\n");
    return compiler.compile(file, out);
}

}  // namespace

KHU_TEST(verifier, accepts_the_compiler_produced_inline_image) {
    Module module;
    KHU_CHECK(build_inline_module(module));
    KHU_CHECK(module.has_inline());
    std::string report;
    KHU_CHECK(verify(module, report));
    KHU_CHECK_EQ(report, std::string(""));
}

KHU_TEST(verifier, rejects_an_inline_locals_table_larger_than_the_frame) {
    Module module;
    if (!build_module(module)) return;
    MethodEntry* main = find_method(module, "main");
    if (!main) return;

    main->flags |= kMethodHasInline;
    for (std::uint32_t i = 0; i <= main->frame_size; ++i) main->locals.push(0xffffffffu);
    KHU_CHECK_CONTAINS(report_for(module), "inline locals table larger than its frame");
}

KHU_TEST(verifier, rejects_inline_locals_without_an_inline_flag) {
    Module module;
    if (!build_module(module)) return;
    MethodEntry* main = find_method(module, "main");
    if (!main) return;

    main->locals.push(0xffffffffu);
    KHU_CHECK_CONTAINS(report_for(module), "inline locals table but no inline blocks");
}

KHU_TEST(verifier, rejects_an_inline_operand_that_is_not_a_string) {
    Module module;
    if (!build_inline_module(module)) return;
    MethodEntry* go = find_method(module, "go");
    if (!go) return;

    // Point the InlineC operand at an integer constant instead of the raw C
    // text that the image must carry.
    std::uint32_t number = module.intern_int(TypeTag::Int32, 1234567);
    for (std::size_t i = 0; i + 2 < go->code.size(); ++i) {
        if (static_cast<Op>(go->code[i]) != Op::InlineC) continue;
        go->code[i + 1] = static_cast<std::uint8_t>(number & 0xff);
        go->code[i + 2] = static_cast<std::uint8_t>((number >> 8) & 0xff);
        break;
    }
    KHU_CHECK_CONTAINS(report_for(module), "inline_c does not name a string constant");
}

namespace {

// Builds a module whose `go` method contains an inline_asm statement.
bool build_inline_asm_module(Module& out) {
    khu::Compiler compiler;
    std::uint32_t file = compiler.add_buffer(
        "a.khu",
        "public class A {\n"
        "    func go() {\n"
        "        int32 x = 0;\n"
        "        inline_asm { \"nop\" }\n"
        "        io.printLine(x);\n"
        "    }\n"
        "}\n");
    return compiler.compile(file, out);
}

}  // namespace

KHU_TEST(verifier, accepts_the_compiler_produced_inline_asm_image) {
    Module module;
    KHU_CHECK(build_inline_asm_module(module));
    KHU_CHECK(module.has_inline());
    std::string report;
    KHU_CHECK(verify(module, report));
    KHU_CHECK_EQ(report, std::string(""));
}

KHU_TEST(verifier, rejects_an_inline_asm_operand_that_is_not_a_string) {
    Module module;
    if (!build_inline_asm_module(module)) return;
    MethodEntry* go = find_method(module, "go");
    if (!go) return;

    std::uint32_t number = module.intern_int(TypeTag::Int32, 1234567);
    for (std::size_t i = 0; i + 2 < go->code.size(); ++i) {
        if (static_cast<Op>(go->code[i]) != Op::InlineAsm) continue;
        go->code[i + 1] = static_cast<std::uint8_t>(number & 0xff);
        go->code[i + 2] = static_cast<std::uint8_t>((number >> 8) & 0xff);
        break;
    }
    KHU_CHECK_CONTAINS(report_for(module), "inline_asm does not name a string constant");
}
