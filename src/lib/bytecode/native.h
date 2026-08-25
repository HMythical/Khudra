// Native function ids.
//
// Shared by the front end (which resolves `io.print` to an id) and the VM
// (which binds the id to an implementation), so the numbering lives in one
// place and .kbc images stay stable across both.
#ifndef KHU_BYTECODE_NATIVE_H
#define KHU_BYTECODE_NATIVE_H

#include <cstdint>

namespace khu::bytecode {

enum class NativeId : std::uint32_t {
    None = 0,
    Print = 1,
    PrintLine = 2,
    ReadLine = 3,
};

const char* native_name(NativeId id);

}  // namespace khu::bytecode

#endif  // KHU_BYTECODE_NATIVE_H
