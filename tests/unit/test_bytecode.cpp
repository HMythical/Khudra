// Phase 3: the .kbc container and the disassembler.
#include "test_harness.h"

#include <string>

#include "bytecode/disassembler.h"
#include "bytecode/module.h"
#include "bytecode/opcode.h"
#include "compiler.h"

using namespace khu::bytecode;

KHU_TEST(opcodes, sizes_match_their_operand_formats) {
    KHU_CHECK_EQ(instruction_size(Op::Nop), static_cast<std::uint32_t>(1));
    KHU_CHECK_EQ(instruction_size(Op::LoadConst), static_cast<std::uint32_t>(3));
    KHU_CHECK_EQ(instruction_size(Op::Add), static_cast<std::uint32_t>(2));
    KHU_CHECK_EQ(instruction_size(Op::Convert), static_cast<std::uint32_t>(3));
    KHU_CHECK_EQ(instruction_size(Op::Jump), static_cast<std::uint32_t>(5));
    KHU_CHECK_EQ(instruction_size(Op::Materialize), static_cast<std::uint32_t>(4));
    KHU_CHECK_EQ(instruction_size(Op::InlineC), static_cast<std::uint32_t>(3));
    KHU_CHECK_EQ(instruction_size(Op::InlineAsm), static_cast<std::uint32_t>(3));

    // Every opcode has a mnemonic; nothing falls through the table.
    for (std::size_t i = 0; i < static_cast<std::size_t>(Op::Count); ++i) {
        std::string name = mnemonic(static_cast<Op>(i));
        KHU_CHECK(!name.empty());
        KHU_CHECK_NE(name, std::string("<bad-opcode>"));
    }
}

KHU_TEST(opcodes, type_tag_predicates) {
    KHU_CHECK(is_signed_integer(TypeTag::Int8));
    KHU_CHECK(is_signed_integer(TypeTag::Int64));
    KHU_CHECK(!is_signed_integer(TypeTag::UInt8));
    KHU_CHECK(is_integer(TypeTag::UInt64));
    KHU_CHECK(!is_integer(TypeTag::Float32));
    KHU_CHECK(is_float(TypeTag::Float64));
    KHU_CHECK(is_numeric(TypeTag::Float32));
    KHU_CHECK(!is_numeric(TypeTag::Bool));
    KHU_CHECK(is_reference(TypeTag::Ref));
    KHU_CHECK(is_reference(TypeTag::Null));
    KHU_CHECK_EQ(type_width(TypeTag::Int16), static_cast<std::uint32_t>(16));
    KHU_CHECK_EQ(type_width(TypeTag::Float64), static_cast<std::uint32_t>(64));
    KHU_CHECK_EQ(type_width(TypeTag::Bool), static_cast<std::uint32_t>(0));
}

KHU_TEST(constant_pool, interns_by_value_and_type) {
    Module module;
    std::uint32_t a = module.intern_int(TypeTag::Int32, 7);
    std::uint32_t b = module.intern_int(TypeTag::Int32, 7);
    std::uint32_t c = module.intern_int(TypeTag::Int64, 7);
    std::uint32_t s1 = module.intern_string("hello");
    std::uint32_t s2 = module.intern_string("hello");

    KHU_CHECK_EQ(a, b);
    // Same numeric value, different width: distinct constants, because the VM
    // must never guess a width.
    KHU_CHECK_NE(a, c);
    KHU_CHECK_EQ(s1, s2);
    KHU_CHECK_EQ(module.constants.size(), static_cast<std::size_t>(3));
    KHU_CHECK_EQ(std::string(module.string_at(s1)), std::string("hello"));
}

namespace {

Module build_sample_module() {
    Module module;
    module.source_file = module.intern_string("sample.khu");

    ClassEntry klass;
    klass.name = module.intern_string("Sample");
    klass.flags = kClassManual;
    klass.object_size = 12;
    FieldEntry field;
    field.name = module.intern_string("value");
    field.type = TypeTag::Int32;
    field.offset = 0;
    field.ref_kind = kRefRaw;
    field.is_public = 1;
    klass.fields.push(field);
    FieldEntry reference;
    reference.name = module.intern_string("next");
    reference.type = TypeTag::Ref;
    reference.offset = 4;
    reference.class_ref = 0;
    reference.ref_kind = kRefManual;
    klass.fields.push(reference);
    klass.vtable.push(0);
    klass.constructor = -1;
    klass.procedures = -1;
    module.classes.push(std::move(klass));

    MethodEntry method;
    method.name = module.intern_string("run");
    method.owner_class = 0;
    method.param_count = 1;
    method.frame_size = 3;
    method.return_type = TypeTag::Int32;
    method.code.push(static_cast<std::uint8_t>(Op::LoadConst));
    method.code.push(0);
    method.code.push(0);
    method.code.push(static_cast<std::uint8_t>(Op::Add));
    method.code.push(static_cast<std::uint8_t>(TypeTag::Int32));
    method.code.push(static_cast<std::uint8_t>(Op::ReturnValue));
    method.lines.push(LineEntry{0, 4, 9});
    module.methods.push(std::move(method));

    module.main_method = 0;
    module.root_class = 0;
    return module;
}

}  // namespace

KHU_TEST(kbc, round_trips_through_serialization) {
    Module original = build_sample_module();
    std::string bytes = serialize(original);

    KHU_CHECK(bytes.size() > 16);
    KHU_CHECK_EQ(bytes.compare(0, 4, kMagic, 4), 0);

    Module reloaded;
    std::string error;
    KHU_CHECK(deserialize(bytes, reloaded, error));
    KHU_CHECK_EQ(error, std::string(""));

    KHU_CHECK_EQ(reloaded.constants.size(), original.constants.size());
    KHU_CHECK_EQ(reloaded.classes.size(), original.classes.size());
    KHU_CHECK_EQ(reloaded.methods.size(), original.methods.size());
    KHU_CHECK_EQ(reloaded.main_method, original.main_method);
    KHU_CHECK_EQ(reloaded.root_class, original.root_class);

    const ClassEntry& klass = reloaded.classes[0];
    KHU_CHECK_EQ(std::string(reloaded.string_at(klass.name)), std::string("Sample"));
    KHU_CHECK_EQ(klass.flags, static_cast<std::uint32_t>(kClassManual));
    KHU_CHECK_EQ(klass.object_size, static_cast<std::uint32_t>(12));
    KHU_CHECK_EQ(klass.fields.size(), static_cast<std::size_t>(2));
    KHU_CHECK_EQ(klass.fields[1].ref_kind, static_cast<std::uint8_t>(kRefManual));
    KHU_CHECK_EQ(klass.fields[1].class_ref, static_cast<std::uint32_t>(0));

    const MethodEntry& method = reloaded.methods[0];
    KHU_CHECK_EQ(method.param_count, static_cast<std::uint8_t>(1));
    KHU_CHECK_EQ(method.frame_size, static_cast<std::uint16_t>(3));
    KHU_CHECK_EQ(method.code.size(), static_cast<std::size_t>(6));
    KHU_CHECK_EQ(method.lines.size(), static_cast<std::size_t>(1));
    KHU_CHECK_EQ(method.lines[0].line, static_cast<std::uint32_t>(4));

    // Re-serializing the reload produces identical bytes.
    KHU_CHECK_EQ(serialize(reloaded), bytes);
}

KHU_TEST(kbc, rejects_bad_images) {
    Module module;
    std::string error;
    KHU_CHECK(!deserialize("not a khudra image", module, error));
    KHU_CHECK_CONTAINS(error, "bad magic");

    std::string truncated = serialize(build_sample_module());
    truncated.resize(truncated.size() - 8);
    Module partial;
    error.clear();
    KHU_CHECK(!deserialize(truncated, partial, error));
    KHU_CHECK_CONTAINS(error, "truncated");

    // A future major version is refused rather than misread.
    std::string wrong_version = serialize(build_sample_module());
    wrong_version[4] = static_cast<char>(kVersionMajor + 1);
    Module newer;
    error.clear();
    KHU_CHECK(!deserialize(wrong_version, newer, error));
    KHU_CHECK_CONTAINS(error, "is not supported by this toolchain");
}

KHU_TEST(disassembler, renders_a_readable_listing) {
    Module module = build_sample_module();
    std::string text = disassemble(module);

    KHU_CHECK_CONTAINS(text, "; Khudra bytecode v1.3  source=sample.khu");
    KHU_CHECK_CONTAINS(text, "class #0  Sample  strategy=manual  size=12");
    KHU_CHECK_CONTAINS(text, "public  int32 value  @0  raw");
    KHU_CHECK_CONTAINS(text, "private ref next  @4  manual-ref");
    KHU_CHECK_CONTAINS(text, "method #0  Sample.run  params=1  frame=3  returns=int32");
    KHU_CHECK_CONTAINS(text, "ldc");
    KHU_CHECK_CONTAINS(text, "add             int32");
    KHU_CHECK_CONTAINS(text, "retval");
}

KHU_TEST(disassembler, annotates_jumps_and_materialize) {
    Module module;
    module.source_file = module.intern_string("j.khu");
    ClassEntry klass;
    klass.name = module.intern_string("J");
    module.classes.push(std::move(klass));

    MethodEntry method;
    method.name = module.intern_string("go");
    method.code.push(static_cast<std::uint8_t>(Op::Jump));
    for (int i = 0; i < 4; ++i) method.code.push(0);  // offset 0 -> next instruction
    method.code.push(static_cast<std::uint8_t>(Op::Materialize));
    method.code.push(0);
    method.code.push(0);
    method.code.push(static_cast<std::uint8_t>(StrategyByte::Manual));
    module.methods.push(std::move(method));

    std::string text = disassemble_method(module, module.methods[0]);
    KHU_CHECK_CONTAINS(text, "jmp");
    KHU_CHECK_CONTAINS(text, "-> 5");
    KHU_CHECK_CONTAINS(text, "materialize     0, manual    ; J");
}

KHU_TEST(kbc, round_trips_inline_flags_and_locals_table) {
    khu::Compiler compiler;
    std::uint32_t file = compiler.add_buffer(
        "i.khu",
        "public class I {\n"
        "    func go(int32 seed) {\n"
        "        int32 x = 0;\n"
        "        inline_c {\n"
        "            x = seed->v.as_int;\n"
        "        }\n"
        "        io.printLine(x);\n"
        "    }\n"
        "}\n");
    Module original;
    KHU_CHECK(compiler.compile(file, original));
    KHU_CHECK(original.has_inline());

    // The method that holds the block carries its inline flag and a name chain
    // for every named frame slot (parameters first, then locals).
    bool found = false;
    std::uint32_t go_locals_0 = 0xffffffffu;
    std::uint32_t go_locals_1 = 0xffffffffu;
    for (const MethodEntry& method : original.methods) {
        if (original.string_at(method.name) != "go") continue;
        found = true;
        KHU_CHECK(method.has_inline());
        KHU_CHECK(method.locals.size() >= static_cast<std::size_t>(2));
        KHU_CHECK_NE(method.locals[0], static_cast<std::uint32_t>(0xffffffffu));  // seed
        KHU_CHECK_NE(method.locals[1], static_cast<std::uint32_t>(0xffffffffu));  // x
        go_locals_0 = method.locals[0];
        go_locals_1 = method.locals[1];
    }
    KHU_CHECK(found);

    // Serialization carries both: the module flags word and each method's
    // locals table survive a round trip.
    std::string bytes = serialize(original);
    Module reloaded;
    std::string error;
    KHU_CHECK(deserialize(bytes, reloaded, error));
    KHU_CHECK_EQ(error, std::string(""));
    KHU_CHECK(reloaded.has_inline());
    found = false;
    for (const MethodEntry& method : reloaded.methods) {
        if (reloaded.string_at(method.name) != "go") continue;
        found = true;
        KHU_CHECK(method.has_inline());
        KHU_CHECK(method.locals.size() >= static_cast<std::size_t>(2));
        KHU_CHECK_EQ(method.locals[0], go_locals_0);
        KHU_CHECK_EQ(method.locals[1], go_locals_1);
    }
    KHU_CHECK(found);
    KHU_CHECK_EQ(serialize(reloaded), bytes);
}

namespace {

void put_u16_le(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
}

void put_u32_le(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
    }
}

void put_i32_le(std::string& out, std::int32_t value) {
    put_u32_le(out, static_cast<std::uint32_t>(value));
}

// A small, well-formed bytecode image in the 1.2 layout: no module flags word
// and no per-method locals table, exactly what a release before 1.3 emitted.
std::string sample_1_2_image() {
    std::string bytes;
    bytes.append(kMagic, 4);
    put_u16_le(bytes, kVersionMajor);
    put_u16_le(bytes, 2);
    put_u32_le(bytes, 0);   // source_file
    put_i32_le(bytes, -1);  // entry: none
    put_i32_le(bytes, 0);   // root_class
    put_u32_le(bytes, 0);   // no constants
    put_u32_le(bytes, 0);   // no classes
    put_u32_le(bytes, 0);   // no methods
    return bytes;
}

}  // namespace

KHU_TEST(kbc, a_1_2_image_loads_without_the_new_fields) {
    std::string bytes = sample_1_2_image();

    Module reloaded;
    std::string error;
    KHU_CHECK(deserialize(bytes, reloaded, error));
    KHU_CHECK_EQ(error, std::string(""));
    // The 1.3-only fields simply read as absent for an older image.
    KHU_CHECK(!reloaded.has_inline());
    KHU_CHECK_EQ(reloaded.methods.size(), static_cast<std::size_t>(0));
}
