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

#include <cstddef>
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
    Neg,        // khuStdMath.neg(x) -- the one unary arithmetic intrinsic
    ArrayCreate,  // khuStdCollection.arrayCreate(<type>, count)
    ArraySize,    // khuStdCollection.arraySize(array)
};

// How many arguments an intrinsic takes. `ConvertTo` takes two, the first of
// which is a type, and is handled before this is consulted.
std::size_t intrinsic_arity(Intrinsic which);

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

// The namespaces the compiler treats specially. There is one binder function
// per namespace, assembled by resolve_native_binding: a namespace is a
// self-contained block of the table, so adding one is adding a function rather
// than another branch in a chain that grows without bound.
constexpr const char* kMathNamespace = "khuStdMath";
constexpr const char* kConvNamespace = "khuStdConv";
constexpr const char* kStringNamespace = "khuStdString";
constexpr const char* kCollectionNamespace = "khuStdCollection";
constexpr const char* kMemNamespace = "khuStdMem";
constexpr const char* kErrorsNamespace = "khuErrors";
constexpr const char* kRuntimeNamespace = "khu";
constexpr const char* kIoNamespace = "io";
// The standard error *stream* (fd 2). Deliberately not the same thing as
// kErrorsNamespace, which holds error *values*: PLAN.md keeps the two apart,
// and so does the naming.
constexpr const char* kStdErrNamespace = "khuStdErr";
constexpr const char* kRandomNamespace = "khuStdRandom";
constexpr const char* kTimeNamespace = "khuStdTime";
// The operating system: open streams, the process, the file namespace and
// sockets. Distinct from kMemNamespace the way khuStdErr is distinct from
// khuErrors -- a handle is an OS object, a buffer is bytes.
constexpr const char* kSystemNamespace = "khuStdSystem";
// The kernel tier: what a program reaches for when it wants a syscall, the
// inverse of khuStdSystem. The shared namespace carries the platform branch,
// the error slot and the buffer/address bridge; the per-OS namespaces carry
// the syscalls and trap when called from the wrong host. `is_kernel_namespace`
// recognizes all four, which is what lets the checker gate every one of them.
constexpr const char* kKernelNamespace = "khuAdvKernel";
constexpr const char* kKernelLinuxNamespace = "khuAdvKernelLinux";
constexpr const char* kKernelWindowsNamespace = "khuAdvKernelWindows";
constexpr const char* kKernelMacNamespace = "khuAdvKernelMac";

// The one member whose first argument is a type, so it cannot be declared in
// Khudra and stays a compiler intrinsic.
constexpr const char* kConvertTo = "convertTo";

// The per-namespace binders. Exposed so tests/unit/test_stdlib can walk a
// namespace's table without going through a name lookup first.
NativeBinding bind_math(std::string_view member_name, std::size_t arity);
NativeBinding bind_conv(std::string_view member_name, std::size_t arity);
NativeBinding bind_string(std::string_view member_name, std::size_t arity);
NativeBinding bind_collection(std::string_view member_name, std::size_t arity);
NativeBinding bind_errors(std::string_view member_name, std::size_t arity);
NativeBinding bind_mem(std::string_view member_name, std::size_t arity);
NativeBinding bind_io(std::string_view member_name, std::size_t arity);
NativeBinding bind_std_err(std::string_view member_name, std::size_t arity);
NativeBinding bind_random(std::string_view member_name, std::size_t arity);
NativeBinding bind_time(std::string_view member_name, std::size_t arity);
NativeBinding bind_system(std::string_view member_name, std::size_t arity);
NativeBinding bind_runtime(std::string_view member_name, std::size_t arity);
// The shared khuAdvKernel namespace. The per-OS tiers bind in later phases,
// one function per namespace, exactly the way the rest of the table grows.
NativeBinding bind_kernel(std::string_view member_name, std::size_t arity);
// Whether `namespace_name` is one of the kernel tier's namespaces. The checker
// uses it as the compile-time half of the trust model: a namespace the program
// is allowed to reach is `allow_kernel`, every kernel namespace is gated on it.
bool is_kernel_namespace(std::string_view namespace_name);

}  // namespace khu::sema

#endif  // KHU_SEMA_BUILTINS_H
