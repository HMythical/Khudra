// Bytecode image -> C translation unit.
//
// The whole native backend hangs off this one pass: `khudra run --native` and
// `khudra build` differ only in what they do with the C it produces.
//
// How the lowering maps the bytecode model
// ----------------------------------------
// Every non-native method becomes one C function. Its frame is a single array
// of tagged values -- `frame_size` locals first, then the operand stack, whose
// height is known statically at every instruction because the emitter walks the
// method with an abstract stack before it writes anything. So the operand stack
// is not a runtime structure at all: `ldc 4` becomes an assignment to a fixed
// array element, and the dispatcher disappears.
//
// Control flow becomes C control flow: a jump is a `goto` to a label the
// emitter places on the target offset, which the verifier already guarantees is
// an instruction boundary.
//
// Arithmetic, comparison and conversion are lowered to the `static inline`
// helpers in khu_native_abi.h -- they carry the interpreter's exact wrapping,
// rounding and division rules and need nothing from the host. Everything with
// state behind it -- fields, allocation, materialization, calls, traps -- is a
// `khu_rt_*` call, so object memory, the collector and the Procedure engine
// stay shared with the VM rather than reimplemented here.
#ifndef KHU_NATIVE_CEMIT_H
#define KHU_NATIVE_CEMIT_H

#include <string>

#include "bytecode/module.h"

namespace khu::native {

struct EmitResult {
    std::string source;
    // Empty when the lowering succeeded. An emitter error is a bug or an image
    // the verifier should have refused, so it is reported, never worked around.
    std::string error;

    bool ok() const { return error.empty(); }
};

struct EmitOptions {
    // The .kbc bytes to embed. The produced translation unit reloads them at
    // startup for the class table, the constant pool and the line tables, so a
    // standalone binary needs nothing beside it on disk. Empty embeds nothing,
    // which is what the snapshot tests want.
    std::string image;
};

// Lowers `module` to one C11 translation unit.
EmitResult emit_c(const bytecode::Module& module, const EmitOptions& options);

// The text of khu_native_abi.h, embedded into the toolchain at build time. The
// emitted C carries a copy so a translation unit is self-contained: `khudra
// build` must work without the Khudra source tree beside it.
const char* native_abi_source();

}  // namespace khu::native

#endif  // KHU_NATIVE_CEMIT_H
