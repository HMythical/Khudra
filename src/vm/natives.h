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
#include <string_view>
#include <vector>

#include "bytecode/native.h"
#include "vm/value.h"

namespace khu::vm {

// --- khuStdSystem: the state one run of a program keeps ---------------------
//
// The system library needs a handful of things that outlive a single native
// call and that both backends must see identically: which OS objects are open,
// what the last OS call went wrong with, what the program was invoked with, and
// whether it has asked to exit. They live here, alongside the PRNG state
// below, for exactly the reason that does: a run's state belongs to the shared
// `NativeServices` rather than to either backend, so `khudra run` and
// `khudra run --native` cannot drift.

// What sort of OS object a handle names. `close` is one native for files and
// sockets alike (EXPANSION-PLAN.md, section 3), so it has to be able to ask.
enum class HandleKind : std::uint8_t {
    None = 0,
    File,
    // A connected socket, and a listening one. Kept apart so `accept` can
    // refuse a connection and `recv` can refuse a listener, rather than
    // failing later with an OS error nobody can read.
    Socket,
    Listener,
};

// One open OS object.
//
// `cookie` is the `*byte` khuStdMem's allocator handed out, and it is a **key,
// never a pointer this code follows**. `close` frees the block so the existing
// leak accounting (`liveBytes`/`liveBlocks`) returns to zero, and the address
// stays behind only to be compared -- which is what lets `isOpen` answer
// `false` for a handle that has already been closed instead of reading freed
// memory.
struct HandleSlot {
    const void* cookie = nullptr;
    HandleKind kind = HandleKind::None;
    // A `FILE*` for a file, a `platform::Socket` for a socket. One int64 covers
    // both, so files and sockets share one registry (EXPANSION-PLAN.md, 2.1).
    std::int64_t descriptor = 0;
};

struct SystemServices {
    // The open handles. A program holds a handful at a time, so a flat array
    // scanned linearly is both the simplest and the fastest thing here.
    std::vector<HandleSlot> handles;

    // The arguments after `khudra run FILE --`, or a built binary's own argv
    // past its name. `argc()`/`argv()` read this.
    std::vector<std::string> program_args;

    // `exit(code)`. The native sets these and unwinds; each backend's execution
    // loop turns that into the process's exit status, so `exit` leaves through
    // the same door a trap does (docs/roadmap.md).
    bool exit_requested = false;
    int exit_code = 0;

    ~SystemServices();

    // Registers `descriptor` under a freshly allocated cookie and answers it,
    // or null when the allocator refuses.
    void* open_handle(HandleKind kind, std::int64_t descriptor);
    // The slot `cookie` names, or null when it names no open handle.
    HandleSlot* find(const void* cookie);
    // Forgets the slot and frees the cookie. The caller has already closed the
    // OS object; this is the bookkeeping half.
    void forget(const void* cookie);
    // Closes every handle still open. Called when the run ends, so a program
    // that leaks a handle still does not leak one out of the process.
    void close_all();

    // Requests that the program end with `code`.
    void request_exit(int code) {
        exit_requested = true;
        exit_code = code;
    }
};

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

    // --- producing a Khudra object from a native -------------------------
    //
    // `NativeOutcome` could always carry a primitive, a string or a `*T`; it
    // could not carry an *instance*, which is what `khuStdSystem.argv()` has to
    // answer (EXPANSION-PLAN.md, section 4). These three run the backend's own
    // machinery, so an object a native builds is materialized and called
    // exactly the way one built by `new` in Khudra source is.

    // Materializes `class_name` through the normal pipeline -- allocate,
    // Procedures, constructor -- and leaves a `Ref` in `out`. False when the
    // image has no such class or the pipeline trapped.
    virtual bool materialize(std::string_view class_name, Value& out) = 0;

    // Invokes `method_name` on `receiver` with `args` in declaration order.
    // False when the class does not declare it or the call trapped.
    virtual bool invoke(const Value& receiver, std::string_view method_name, const Value* args,
                        std::uint32_t argc, Value& out) = 0;

    // Keeps `value` reachable for as long as it is rooted. A collection can
    // happen inside `materialize` and inside `invoke`, so anything a native is
    // still holding across one of those has to be a root first.
    virtual void push_root(const Value& value) = 0;
    virtual void pop_root() = 0;

    // Per-run state for khuStdSystem: open handles, the program's arguments
    // and the pending exit request. Shared rather than mirrored, so the two
    // backends cannot answer differently.
    SystemServices system;

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

// Roots a value for a scope, so an object a native is still building survives a
// collection triggered further down the same call.
class RootScope {
public:
    RootScope(NativeServices& services, const Value& value) : services_(services) {
        services_.push_root(value);
    }
    RootScope(const RootScope&) = delete;
    RootScope& operator=(const RootScope&) = delete;
    ~RootScope() { services_.pop_root(); }

private:
    NativeServices& services_;
};

// Runs one native. `argv` holds the arguments in declaration order -- the order
// they were pushed, which is the order the emitted C leaves them in too.
NativeOutcome invoke_native(NativeServices& services, bytecode::NativeId id, const Value* argv,
                            std::uint32_t argc);

}  // namespace khu::vm

#endif  // KHU_VM_NATIVES_H
