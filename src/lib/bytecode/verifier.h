// Bytecode verification.
//
// A .kbc image is an input like any other: it can arrive from disk, from
// another toolchain version, or corrupted. The interpreter is written assuming
// its operands are in range, so the check happens once, here, before anything
// executes -- rather than as a bounds test on every instruction.
//
// This is a structural verifier: it proves an image decodes, that every index
// it names exists, and that every jump lands on an instruction boundary. It
// does not re-typecheck the program.
#ifndef KHU_BYTECODE_VERIFIER_H
#define KHU_BYTECODE_VERIFIER_H

#include <string>

#include "bytecode/module.h"
#include "util/array.h"

namespace khu::bytecode {

struct VerificationError {
    // -1 when the problem is not inside a method.
    std::int32_t method = -1;
    std::uint32_t offset = 0;
    std::string message;
};

// Fills `errors` with everything wrong with `module`. Returns true when it is
// safe to execute.
bool verify(const Module& module, util::Array<VerificationError>& errors);

// The same check, rendered as one message per line.
bool verify(const Module& module, std::string& report);

}  // namespace khu::bytecode

#endif  // KHU_BYTECODE_VERIFIER_H
