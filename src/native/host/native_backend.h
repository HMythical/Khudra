// The two wrappers around the C emitter.
//
// One lowering pass, two wrappers (docs/native.md, section 2). `run_jit` compiles the
// emitted C to a shared object and loads it into the running process;
// `build_executable` compiles it into a standalone binary. Everything below
// the C source -- object memory, the collector, the Procedure engine -- is the
// same NativeHost in both cases.
#ifndef KHU_NATIVE_HOST_NATIVE_BACKEND_H
#define KHU_NATIVE_HOST_NATIVE_BACKEND_H

#include <string>

#include "bytecode/module.h"

namespace khu::native {

enum class NativeStatus {
    Ok,
    // No host C compiler. `run --native` prints a notice and falls back to the
    // VM; `build` has nothing to fall back to and reports it.
    NoCompiler,
    Failed,
};

// Redirects a JIT run away from the process's own streams, so a test can run a
// program under the native backend and compare it with the same program under
// the VM without spawning anything.
struct JitCapture {
    // Receives io.* output instead of stdout.
    std::string output;
    // Receives khuStdErr.* output instead of stderr. Separate from
    // `runtime_error` because they are separate things: this is what the
    // program wrote, that is how it died.
    std::string error_output;
    // Supplies io.readLine instead of stdin.
    std::string input;
    // The trap message, instead of stderr.
    std::string runtime_error;
};

// Lowers, compiles, loads and runs `module` in this process. `exit_code` is
// what `khudra` should exit with -- 0, or 1 for a trap. Without a `capture` the
// trap message goes to stderr, the way the driver wants it.
NativeStatus run_jit(const bytecode::Module& module, int& exit_code, std::string& error,
                     JitCapture* capture = nullptr);

// Writes a standalone executable at `output`. `keep_c` leaves the intermediate
// translation unit beside it as <output>.c.
NativeStatus build_executable(const bytecode::Module& module, const std::string& output,
                              bool keep_c, std::string& error);

// Where the backend looks for the archives an ahead-of-time link needs, and the
// extra flags (sanitizers) it must build with to match them. Exposed for the
// tests and for the diagnostic when a link fails.
std::string native_library_dir();
std::string native_extra_flags();

}  // namespace khu::native

#endif  // KHU_NATIVE_HOST_NATIVE_BACKEND_H
