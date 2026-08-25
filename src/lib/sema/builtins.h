// Built-in namespaces.
//
// `khuStdMath`, `io` and `khu` are classes that are never materialized: their
// members are static and are reached as `Namespace.member(...)`. Phase 7
// replaces these hand-registered declarations with real `.khu` sources under
// lib/ that bind to the same native ids.
#ifndef KHU_SEMA_BUILTINS_H
#define KHU_SEMA_BUILTINS_H

#include <cstdint>

#include "bytecode/native.h"
#include "sema/symbol.h"
#include "sema/type.h"
#include "util/arena.h"

namespace khu::sema {

// Compiler-lowered operations: these never become a call.
enum class Intrinsic : std::uint32_t {
    None = 0,
    ConvertTo,  // khuStdMath.convertTo(<type>, expr)
    // Width-specific arithmetic. These lower to a single bytecode instruction
    // rather than a call, which is why they are intrinsics and not natives.
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
};

// Runtime-provided operations. The ids are the VM's, not ours -- see
// bytecode/native.h -- so an image written here loads there.
using Native = bytecode::NativeId;

// Registers the built-in namespaces into `program`. Safe to call once per
// compilation.
void install_builtins(Program& program, TypeContext& types, util::Arena& arena);

// The name of the arithmetic namespace, used by the checker to spot intrinsics.
constexpr const char* kMathNamespace = "khuStdMath";
constexpr const char* kRuntimeNamespace = "khu";
constexpr const char* kIoNamespace = "io";

}  // namespace khu::sema

#endif  // KHU_SEMA_BUILTINS_H
