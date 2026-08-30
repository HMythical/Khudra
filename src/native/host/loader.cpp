// The JIT wrapper: compile the emitted C to a shared object, load it with the
// dynamic loader, and run it against the in-process host.
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "cemit/cemit.h"
#include "host/khu_native_abi.h"
#include "host/native_backend.h"
#include "host/native_host.h"
#include "host/toolchain.h"
#include "util/file.h"

namespace khu::native {

NativeStatus run_jit(const bytecode::Module& module, int& exit_code, std::string& error,
                     JitCapture* capture) {
    std::string compiler = find_c_compiler();
    if (compiler.empty()) return NativeStatus::NoCompiler;

    // The JIT host already holds the Module, so the translation unit does not
    // need a copy of the image embedded in it; only `build` does.
    EmitResult emitted = emit_c(module, EmitOptions{});
    if (!emitted.ok()) {
        error = emitted.error;
        return NativeStatus::Failed;
    }

    TempDir scratch;
    if (!scratch.ok()) {
        error = "cannot create a scratch directory for the native backend";
        return NativeStatus::Failed;
    }
    std::string source_path = scratch.file("khudra_native.c");
    std::string object_path = scratch.file("khudra_native.so");
    scratch.track(source_path);
    scratch.track(object_path);

    if (!util::write_file(source_path, emitted.source)) {
        error = "cannot write '" + source_path + "'";
        return NativeStatus::Failed;
    }

    std::vector<std::string> command{compiler, "-std=c11", "-O1",  "-fPIC", "-shared",
                                     "-Wall",  "-Wextra",  "-Wpedantic"};
    append_flags(native_extra_flags(), command);
    command.push_back(source_path);
    command.push_back("-o");
    command.push_back(object_path);
    command.push_back("-lm");

    std::string output;
    int status = run_tool(command, output);
    if (status != 0) {
        error = "the host compiler could not build the lowered program\n" + output;
        return NativeStatus::Failed;
    }

    // RTLD_LOCAL: the lowered program's symbols belong to it, and the host's
    // own symbols reach it through the executable's dynamic table.
    void* handle = ::dlopen(object_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* reason = ::dlerror();
        error = std::string("cannot load the lowered program: ") + (reason ? reason : "unknown");
        return NativeStatus::Failed;
    }

    auto* methods = reinterpret_cast<const KhuNativeMethod*>(::dlsym(handle, "khu_native_methods"));
    auto* count = reinterpret_cast<const std::uint32_t*>(::dlsym(handle, "khu_native_method_count"));
    if (!methods || !count) {
        ::dlclose(handle);
        error = "the lowered program does not export a method table";
        return NativeStatus::Failed;
    }

    exit_code = 0;
    {
        // The host must be gone before the code it called is unmapped.
        NativeHost host(module, methods, *count);
        if (capture) {
            host.set_output_sink(&capture->output);
            if (!capture->input.empty()) host.set_input(capture->input);
        }
        if (!host.run()) {
            if (capture) {
                capture->runtime_error = host.error();
            } else {
                std::fwrite(host.error().data(), 1, host.error().size(), stderr);
            }
            exit_code = 1;
        }
    }
    ::dlclose(handle);
    return NativeStatus::Ok;
}

}  // namespace khu::native
