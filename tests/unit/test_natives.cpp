// The native binding table: what a `native func` declaration in lib/*.khu is
// bound to, and what the runtime knows about the id it is bound to.
//
// PLAN.md's rule 2 is that a signature and its binding arrive together. The
// checker enforces half of that -- an unbound declaration is a compile error --
// and these tests enforce the other half: that the id a binding names is one
// both backends actually implement, that it sits in its namespace's reserved
// block, and that the stack effect the emitter reads is the stack effect the
// backends produce.
#include "test_harness.h"

#include <set>
#include <string>

#include "bytecode/native.h"
#include "compiler.h"
#include "sema/builtins.h"
#include "sema/symbol.h"

namespace {

using khu::bytecode::NativeId;

// Every id the build defines, in declaration order.
const std::vector<NativeId>& every_native() {
    static const std::vector<NativeId> ids = {
#define KHU_NATIVE_LIST(name, value, text, results) NativeId::name,
        KHU_NATIVES(KHU_NATIVE_LIST)
#undef KHU_NATIVE_LIST
    };
    return ids;
}

// The block a native's qualified name says it belongs to, as [first, last].
bool block_of(const std::string& qualified, std::uint32_t& first, std::uint32_t& last) {
    struct Block {
        const char* prefix;
        std::uint32_t first;
        std::uint32_t last;
    };
    static const Block blocks[] = {
        {"io.", khu::bytecode::kNativeBlockIo, khu::bytecode::kNativeBlockErr - 1},
        {"khu.", khu::bytecode::kNativeBlockIo, khu::bytecode::kNativeBlockErr - 1},
        {"khuStdErr.", khu::bytecode::kNativeBlockErr, khu::bytecode::kNativeBlockMath - 1},
        {"khuStdMath.", khu::bytecode::kNativeBlockMath, khu::bytecode::kNativeBlockConv - 1},
        {"khuStdConv.", khu::bytecode::kNativeBlockConv, khu::bytecode::kNativeBlockString - 1},
        {"khuStdString.", khu::bytecode::kNativeBlockString,
         khu::bytecode::kNativeBlockCollection - 1},
        {"khuStdCollection.", khu::bytecode::kNativeBlockCollection,
         khu::bytecode::kNativeBlockErrors - 1},
        {"khuErrors.", khu::bytecode::kNativeBlockErrors, khu::bytecode::kNativeBlockMem - 1},
        {"khuStdMem.", khu::bytecode::kNativeBlockMem, khu::bytecode::kNativeBlockRandom - 1},
        {"khuStdRandom.", khu::bytecode::kNativeBlockRandom, khu::bytecode::kNativeBlockTime - 1},
        {"khuStdTime.", khu::bytecode::kNativeBlockTime, khu::bytecode::kNativeBlockSystem - 1},
        {"khuStdSystem.", khu::bytecode::kNativeBlockSystem, khu::bytecode::kNativeBlockEnd - 1},
    };
    for (const Block& block : blocks) {
        if (qualified.rfind(block.prefix, 0) == 0) {
            first = block.first;
            last = block.last;
            return true;
        }
    }
    return false;
}

}  // namespace

KHU_TEST(natives, every_id_is_named_and_known) {
    for (NativeId id : every_native()) {
        std::string name = khu::bytecode::native_name(id);
        KHU_CHECK(khu::bytecode::native_is_known(id));
        KHU_CHECK_NE(name, std::string("<native>"));
        KHU_CHECK(name.find('.') != std::string::npos);
    }
}

KHU_TEST(natives, ids_do_not_collide) {
    std::set<std::uint32_t> seen;
    for (NativeId id : every_native()) {
        auto value = static_cast<std::uint32_t>(id);
        KHU_CHECK_NE(value, 0u);
        if (!seen.insert(value).second) {
            KHU_FAIL("native id " + std::to_string(value) + " (" +
                     khu::bytecode::native_name(id) + ") is used twice");
        }
    }
}

KHU_TEST(natives, every_id_sits_in_its_namespaces_block) {
    for (NativeId id : every_native()) {
        std::string name = khu::bytecode::native_name(id);
        std::uint32_t first = 0;
        std::uint32_t last = 0;
        if (!block_of(name, first, last)) {
            KHU_FAIL("no reserved id block for '" + name + "'");
            continue;
        }
        auto value = static_cast<std::uint32_t>(id);
        if (value < first || value > last) {
            KHU_FAIL(name + " has id " + std::to_string(value) + ", outside its block [" +
                     std::to_string(first) + ", " + std::to_string(last) + "]");
        }
    }
}

KHU_TEST(natives, a_result_count_is_zero_or_one) {
    for (NativeId id : every_native()) {
        int results = khu::bytecode::native_result_count(id);
        KHU_CHECK(results == 0 || results == 1);
    }
    // The unbound id is not a native at all, so it has no stack effect.
    KHU_CHECK_EQ(khu::bytecode::native_result_count(NativeId::None), 0);
    KHU_CHECK(!khu::bytecode::native_is_known(NativeId::None));
}

// PLAN.md, rule 3: an id that has shipped is frozen, because a .kbc image
// records the number. These six are the ones that shipped first.
KHU_TEST(natives, the_original_ids_are_stable) {
    KHU_CHECK_EQ(static_cast<std::uint32_t>(NativeId::Print), 1u);
    KHU_CHECK_EQ(static_cast<std::uint32_t>(NativeId::PrintLine), 2u);
    KHU_CHECK_EQ(static_cast<std::uint32_t>(NativeId::ReadLine), 3u);
    KHU_CHECK_EQ(static_cast<std::uint32_t>(NativeId::StdlibLoadObject), 4u);
    KHU_CHECK_EQ(static_cast<std::uint32_t>(NativeId::GetType), 5u);
    KHU_CHECK_EQ(static_cast<std::uint32_t>(NativeId::LoadRuntimeType), 6u);
}

KHU_TEST(natives, every_stdlib_declaration_binds_to_something_the_runtime_knows) {
    khu::Compiler compiler;
    std::uint32_t file = compiler.add_buffer("empty.khu", "bring khu::stdlib;\n");
    khu::sema::Program* program = compiler.analyze(file);
    if (!program) {
        KHU_FAIL("the standard library did not analyze:\n" + compiler.diagnostics().render());
        return;
    }
    KHU_CHECK_EQ(compiler.diagnostics().render(), std::string(""));

    std::size_t bound = 0;
    for (khu::sema::ClassSymbol* space : program->namespaces) {
        for (khu::sema::MethodSymbol* method : space->methods) {
            std::string where = std::string(space->name) + "." + std::string(method->name);
            if (method->is_intrinsic) {
                KHU_CHECK_NE(method->intrinsic_id,
                             static_cast<std::uint32_t>(khu::sema::Intrinsic::None));
                ++bound;
                continue;
            }
            if (!method->is_native) {
                KHU_FAIL(where + " is neither an intrinsic nor a native");
                continue;
            }
            auto id = static_cast<NativeId>(method->native_id);
            if (!khu::bytecode::native_is_known(id)) {
                KHU_FAIL(where + " binds to native id " + std::to_string(method->native_id) +
                         ", which no backend implements");
                continue;
            }
            // A native that returns a value has to leave one behind, and a void
            // one must not: the emitter's abstract stack is built on this.
            bool returns_value = method->return_type && !method->return_type->is_void();
            KHU_CHECK_EQ(khu::bytecode::native_result_count(id) == 1, returns_value);
            ++bound;
        }
    }
    KHU_CHECK(bound > 0);
}

KHU_TEST(natives, a_namespace_binder_answers_only_for_what_it_declares) {
    using namespace khu::sema;
    KHU_CHECK(bind_math("add", 2).valid());
    KHU_CHECK(!bind_math("add", 1).valid());
    KHU_CHECK(!bind_math("nonesuch", 2).valid());

    KHU_CHECK(bind_io("print", 1).valid());
    KHU_CHECK(!bind_io("print", 0).valid());
    KHU_CHECK(bind_runtime("getType", 0).valid());
    KHU_CHECK(!bind_runtime("getType", 1).valid());

    // A namespace the compiler does not know has no bindings at all.
    KHU_CHECK(!resolve_native_binding("notANamespace", "print", 1).valid());
}

// The error-value namespace and the stderr stream are different things with
// deliberately different names (PLAN.md, locked decisions).
KHU_TEST(natives, the_error_namespaces_are_kept_apart) {
    KHU_CHECK_NE(std::string(khu::sema::kErrorsNamespace),
                 std::string(khu::sema::kStdErrNamespace));
    KHU_CHECK_EQ(std::string(khu::sema::kErrorsNamespace), std::string("khuErrors"));
    KHU_CHECK_EQ(std::string(khu::sema::kStdErrNamespace), std::string("khuStdErr"));
}
