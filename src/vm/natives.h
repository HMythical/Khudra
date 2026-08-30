// The natives themselves, written once.
//
// `docs/native.md`, section 5 asks that a program's stdout, stderr and exit
// code not depend on which backend ran it. For arithmetic that is achieved by
// sharing the *rules* (`khu_native_abi.h` carries the interpreter's exact
// wrapping and rounding). For natives it is achieved more bluntly: there is one
// implementation, and `Vm::call_native` and `NativeHost::call_native` are the
// two callers of it.
//
// A native needs a handful of things a backend owns -- somewhere to put output,
// somewhere to put a string it produced, a line of input, the module the class
// names come from. Those arrive through `NativeServices`; everything else about
// a native is here, where neither backend can quietly grow its own version.
#ifndef KHU_VM_NATIVES_H
#define KHU_VM_NATIVES_H

#include <cstdint>
#include <string>

#include "bytecode/native.h"
#include "vm/value.h"

namespace khu::vm {

// What a native may ask of the backend running it. Implemented once by the
// interpreter and once by the native host; nothing else is backend-specific.
class NativeServices {
public:
    virtual ~NativeServices() = default;

    // Standard output (fd 1) and standard error (fd 2). A backend may point
    // either at a capture buffer instead of the real stream.
    virtual void write_output(const std::string& text) = 0;
    virtual void write_error(const std::string& text) = 0;

    // Takes ownership of a string the native produced and returns a pointer
    // good for the rest of the run (see vm/runtime_strings.h).
    virtual const std::string* make_string(std::string text) = 0;

    // Pushes whatever is buffered out to the stream. A capture buffer has
    // nothing to push.
    virtual void flush_output() = 0;
    virtual void flush_error() = 0;

    // One line of standard input without its newline, or an empty string at
    // end of input.
    virtual std::string read_line() = 0;

    // One byte of standard input, or -1 at end of input. Reads from the same
    // position `read_line` does: the two are two views of one stream.
    virtual int read_byte() = 0;

    // Renders a value the way `io.print` renders it. Class references print
    // their class name, which needs the module, which is the backend's.
    virtual std::string render(const Value& value) const = 0;

    // The deterministic PRNG's state (khuStdRandom). It lives here rather than
    // in either backend because it is per-run state a native needs and nothing
    // else does -- and because starting both backends from the same constant
    // is what makes an unseeded program reproducible under either of them.
    std::uint64_t random_state = 0x853c49e6748fea9bull;
    std::uint64_t random_increment = 0xda3e39cb94b95bdbull;
};

// The outcome of one native call: a value when the native produces one, or a
// trap message when it fails. A trap is the same fatal runtime error a division
// by zero raises -- Khudra has no exceptions, and recoverable failure is
// `khuErrors.Result<T>`, not a second return channel here.
struct NativeOutcome {
    bool ok = true;
    Value value;
    std::string trap;

    static NativeOutcome nothing() { return NativeOutcome{}; }
    static NativeOutcome produced(Value value) {
        NativeOutcome outcome;
        outcome.value = value;
        return outcome;
    }
    static NativeOutcome failed(std::string message) {
        NativeOutcome outcome;
        outcome.ok = false;
        outcome.trap = std::move(message);
        return outcome;
    }
};

// Runs one native. `argv` holds the arguments in declaration order -- the order
// they were pushed, which is the order the emitted C leaves them in too.
NativeOutcome invoke_native(NativeServices& services, bytecode::NativeId id, const Value* argv,
                            std::uint32_t argc);

}  // namespace khu::vm

#endif  // KHU_VM_NATIVES_H
