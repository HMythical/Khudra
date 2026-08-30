// The ahead-of-time wrapper: the same emitted C, compiled and linked with the
// native host into a standalone executable.
//
// The link line is the interesting part. It carries the memory systems
// (khu_vm_core), the image reader (khu_frontend) and the C runtime -- and no
// interpreter: khu_vm is deliberately absent, which is what makes a built
// binary a native program rather than a VM with a program stapled to it.
#include <unistd.h>

#include <string>
#include <vector>

#include "cemit/cemit.h"
#include "host/native_backend.h"
#include "host/toolchain.h"
#include "util/file.h"

namespace khu::native {
namespace {

// In link order: each archive resolves what the ones before it left open.
const char* const kArchives[] = {
    "libkhu_native_aot.a", "libkhu_native_rt.a", "libkhu_vm_core.a",
    "libkhu_frontend.a",   "libkhu_util.a",      "libkhu_runtime_c.a",
};

}  // namespace

NativeStatus build_executable(const bytecode::Module& module, const std::string& output,
                              bool keep_c, std::string& error) {
    std::string compiler = find_c_compiler();
    std::string linker = find_cxx_compiler();
    if (compiler.empty() || linker.empty()) return NativeStatus::NoCompiler;

    std::string library_dir = native_library_dir();
    if (library_dir.empty()) {
        error = "this toolchain does not know where its native runtime archives are; set "
                "KHUDRA_NATIVE_LIB_DIR";
        return NativeStatus::Failed;
    }
    std::vector<std::string> archives;
    for (const char* name : kArchives) {
        std::string path = library_dir + "/" + name;
        if (::access(path.c_str(), R_OK) != 0) {
            error = "the native runtime archive '" + path +
                    "' is missing; set KHUDRA_NATIVE_LIB_DIR to where the toolchain was built";
            return NativeStatus::Failed;
        }
        archives.push_back(path);
    }

    // The binary has to be able to read its own program back, so the image
    // travels inside the translation unit.
    EmitOptions options;
    options.image = bytecode::serialize(module);
    EmitResult emitted = emit_c(module, options);
    if (!emitted.ok()) {
        error = emitted.error;
        return NativeStatus::Failed;
    }

    TempDir scratch;
    if (!scratch.ok()) {
        error = "cannot create a scratch directory for the native backend";
        return NativeStatus::Failed;
    }
    // `--keep-c` puts the translation unit beside the binary, where someone
    // reading it can find it; otherwise it lives and dies in the scratch dir.
    std::string source_path = keep_c ? output + ".c" : scratch.file("khudra_native.c");
    std::string object_path = scratch.file("khudra_native.o");
    if (!keep_c) scratch.track(source_path);
    scratch.track(object_path);

    if (!util::write_file(source_path, emitted.source)) {
        error = "cannot write '" + source_path + "'";
        return NativeStatus::Failed;
    }

    std::vector<std::string> compile{compiler, "-std=c11", "-O2",       "-Wall",
                                     "-Wextra", "-Wpedantic", "-c"};
    append_flags(native_extra_flags(), compile);
    compile.push_back(source_path);
    compile.push_back("-o");
    compile.push_back(object_path);

    std::string report;
    if (run_tool(compile, report) != 0) {
        error = "the host compiler could not build the lowered program\n" + report;
        return NativeStatus::Failed;
    }

    // The archives are C++, so the C++ driver does the link and brings its own
    // runtime with it.
    std::vector<std::string> link{linker};
    append_flags(native_extra_flags(), link);
    link.push_back(object_path);
    for (const std::string& archive : archives) link.push_back(archive);
    link.push_back("-lm");
    link.push_back("-o");
    link.push_back(output);

    if (run_tool(link, report) != 0) {
        error = "the host linker could not build '" + output + "'\n" + report;
        return NativeStatus::Failed;
    }
    return NativeStatus::Ok;
}

}  // namespace khu::native
