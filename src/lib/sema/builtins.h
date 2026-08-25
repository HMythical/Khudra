// Bindings for `native` declarations.
//
// The standard library declares its signatures in `lib/*.khu`; this table says
// what each declaration is bound to. A binding is either an **intrinsic**, which
// codegen lowers to a single instruction, or a **native**, which becomes a
// `callnative` the VM answers.
//
// Keeping the signatures in Khudra and the bindings here means the checker sees
// real declarations with real types -- overload resolution, arity checking and
// error messages all work the same way they do for user code.
#ifndef KHU_SEMA_BUILTINS_H
#define KHU_SEMA_BUILTINS_H

#include <cstdint>
#include <string_view>

#include "bytecode/native.h"

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

using Native = bytecode::NativeId;

struct NativeBinding {
    Intrinsic intrinsic = Intrinsic::None;
    Native native = Native::None;

    bool valid() const { return intrinsic != Intrinsic::None || native != Native::None; }
};

// Looks up the binding for `namespace_name.member_name` with `arity`
// parameters. Returns an invalid binding when there is none.
NativeBinding resolve_native_binding(std::string_view namespace_name,
                                     std::string_view member_name, std::size_t arity);

// The namespaces the compiler treats specially.
constexpr const char* kMathNamespace = "khuStdMath";
constexpr const char* kRuntimeNamespace = "khu";
constexpr const char* kIoNamespace = "io";
// The one member whose first argument is a type, so it cannot be declared in
// Khudra and stays a compiler intrinsic.
constexpr const char* kConvertTo = "convertTo";

}  // namespace khu::sema

#endif  // KHU_SEMA_BUILTINS_H
