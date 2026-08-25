// Human-readable rendering of a bytecode image.
//
// Backs `khudra disasm` and gives the codegen tests something readable to
// assert against.
#ifndef KHU_BYTECODE_DISASSEMBLER_H
#define KHU_BYTECODE_DISASSEMBLER_H

#include <string>

#include "bytecode/module.h"

namespace khu::bytecode {

// The whole image: header, constant pool, class table, then every method.
std::string disassemble(const Module& module);

// One method's instructions, one per line, prefixed by their offsets.
std::string disassemble_method(const Module& module, const MethodEntry& method);

// One instruction at `offset`. Sets `next` to the following offset.
std::string disassemble_instruction(const Module& module, const MethodEntry& method,
                                    std::uint32_t offset, std::uint32_t& next);

// A constant rendered for display: 42, "text", 1.5, true.
std::string describe_constant(const Module& module, std::uint32_t index);

}  // namespace khu::bytecode

#endif  // KHU_BYTECODE_DISASSEMBLER_H
