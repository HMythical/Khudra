// The JIT wrapper: compile the emitted C to a shared object, load it with the
// dynamic loader, and run it against the in-process host.
//
// The loader is one of the three places the backend touches the operating
// system (EXPANSION-PLAN.md, section 6, D2). The shim below is the whole of
// the difference: `dlopen`/`dlsym`/`dlclose` against `LoadLibrary`/
// `GetProcAddress`/`FreeLibrary`, and a `.so` against a `.dll`. What is loaded
// is the same translation unit built with the same flags, because the emitted
// C is portable C11 and MinGW-w64 speaks the same GCC flag grammar.
//
// The symbol model is what makes this work at all, and it is not in this file:
// on POSIX the loaded object leaves every `khu_rt_*` dangling and the loader
// satisfies it from the executable's dynamic table (`-rdynamic`, which CMake
// spells ENABLE_EXPORTS); on Windows/MinGW `khudra.exe` is linked with
// `--export-all-symbols` so it *exports* them and the DLL imports them at
// LoadLibrary time. Either way `cemit.cpp` emits the same thing.
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

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
namespace {

// --- platform_dl -----------------------------------------------------------

#if defined(_WIN32)
using LibraryHandle = HMODULE;

LibraryHandle library_open(const std::string& path) {
    return ::LoadLibraryA(path.c_str());
}

void* library_symbol(LibraryHandle handle, const char* name) {
    return reinterpret_cast<void*>(::GetProcAddress(handle, name));
}

void library_close(LibraryHandle handle) { ::FreeLibrary(handle); }

std::string library_error() {
    DWORD code = ::GetLastError();
    char scratch[256];
    std::snprintf(scratch, sizeof(scratch), "LoadLibrary failed with error %lu",
                  static_cast<unsigned long>(code));
    return scratch;
}

// What the emitted translation unit is built into.
constexpr const char* kScratchObjectName = "khudra_native.dll";

// Where this process's own executable is.
//
// This is the other half of D3, and the half that has no POSIX counterpart. A
// POSIX shared object may be linked with `khu_rt_*` left dangling and have the
// loader fill them in; a Windows DLL may not -- every import has to be
// resolved at link time, against something. That something is `khudra.exe`
// itself, which is linked with `--export-all-symbols` so it has an export
// table for MinGW's ld to import from. So the exe goes on the DLL's link line.
std::string own_executable() {
    char scratch[MAX_PATH];
    DWORD length = ::GetModuleFileNameA(nullptr, scratch, sizeof(scratch));
    if (length == 0 || length >= sizeof(scratch)) return std::string();
    return std::string(scratch, length);
}

// What the DLL should be linked against to resolve `khu_rt_*`.
//
// CMake's ENABLE_EXPORTS makes MinGW write an import library beside the
// executable, and an import library is what a linker most wants to be handed.
// Linking straight against the .exe also works with GNU ld, so that is the
// fallback rather than the first choice -- and `KHUDRA_NATIVE_IMPORT_LIB` is
// the escape hatch for a layout neither guess finds, in the same spirit as
// KHUDRA_CC and KHUDRA_NATIVE_LIB_DIR.
std::string host_import_target() {
    if (const char* override_lib = std::getenv("KHUDRA_NATIVE_IMPORT_LIB")) return override_lib;

    std::string exe = own_executable();
    if (exe.empty()) return std::string();

    std::size_t slash = exe.find_last_of("/\\");
    std::string directory = slash == std::string::npos ? std::string(".") : exe.substr(0, slash);
    std::string stem = slash == std::string::npos ? exe : exe.substr(slash + 1);
    if (stem.size() >= 4 && stem.compare(stem.size() - 4, 4, ".exe") == 0) {
        stem.resize(stem.size() - 4);
    }

    for (const std::string& candidate : {directory + "/lib" + stem + ".dll.a",
                                         directory + "/lib" + stem + ".exe.a",
                                         directory + "/" + stem + ".lib"}) {
        if (std::FILE* probe = std::fopen(candidate.c_str(), "rb")) {
            std::fclose(probe);
            return candidate;
        }
    }
    return exe;
}
#else
using LibraryHandle = void*;

LibraryHandle library_open(const std::string& path) {
    // RTLD_LOCAL: the lowered program's symbols belong to it, and the host's
    // own symbols reach it through the executable's dynamic table.
    return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}

void* library_symbol(LibraryHandle handle, const char* name) { return ::dlsym(handle, name); }

void library_close(LibraryHandle handle) { ::dlclose(handle); }

std::string library_error() {
    const char* reason = ::dlerror();
    return reason ? reason : "unknown";
}

constexpr const char* kScratchObjectName = "khudra_native.so";
#endif

}  // namespace

NativeStatus run_jit(const bytecode::Module& module, int& exit_code, std::string& error,
                     JitCapture* capture, const std::vector<std::string>* program_args) {
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
    std::string object_path = scratch.file(kScratchObjectName);
    scratch.track(source_path);
    scratch.track(object_path);

    if (!util::write_file(source_path, emitted.source)) {
        error = "cannot write '" + source_path + "'";
        return NativeStatus::Failed;
    }

    std::vector<std::string> command{compiler, "-std=c11", "-O1", "-shared",
                                     "-Wall", "-Wextra", "-Wpedantic"};
#if !defined(_WIN32)
    // Position-independent code is required for a shared object on POSIX and
    // meaningless for a Windows DLL, where every image is relocatable already.
    command.push_back("-fPIC");
#endif
    append_flags(native_extra_flags(), command);
    command.push_back(source_path);
    command.push_back("-o");
    command.push_back(object_path);
#if defined(_WIN32)
    // What the khu_rt_* imports resolve against (see host_import_target). An
    // empty answer leaves them unresolved, which the linker then reports --
    // better than a silently different program.
    std::string host = host_import_target();
    if (!host.empty()) command.push_back(host);
#endif
    command.push_back("-lm");

    std::string output;
    int status = run_tool(command, output);
    if (status != 0) {
        error = "the host compiler could not build the lowered program\n" + output;
        return NativeStatus::Failed;
    }

    LibraryHandle handle = library_open(object_path);
    if (!handle) {
        error = "cannot load the lowered program: " + library_error();
        return NativeStatus::Failed;
    }

    auto* methods =
        reinterpret_cast<const KhuNativeMethod*>(library_symbol(handle, "khu_native_methods"));
    auto* count =
        reinterpret_cast<const std::uint32_t*>(library_symbol(handle, "khu_native_method_count"));
    if (!methods || !count) {
        library_close(handle);
        error = "the lowered program does not export a method table";
        return NativeStatus::Failed;
    }

    exit_code = 0;
    {
        // The host must be gone before the code it called is unmapped.
        NativeHost host(module, methods, *count);
        if (program_args) host.set_program_args(*program_args);
        if (capture) {
            host.set_output_sink(&capture->output);
            host.set_error_sink(&capture->error_output);
            if (!capture->input.empty()) host.set_input(capture->input);
        }
        if (!host.run()) {
            // `khuStdSystem.exit(code)` unwinds like a trap but is not one, so
            // the code it asked for is the program's status and there is
            // nothing to report on stderr.
            if (host.exit_requested()) {
                exit_code = host.exit_code();
            } else {
                if (capture) {
                    capture->runtime_error = host.error();
                } else {
                    std::fwrite(host.error().data(), 1, host.error().size(), stderr);
                }
                exit_code = 1;
            }
        }
    }
    library_close(handle);
    return NativeStatus::Ok;
}

}  // namespace khu::native
