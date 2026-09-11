// khudra -- command line driver.
//
//   khudra compile <file.khu>   compile to a .kbc bytecode image
//   khudra run     <file>       compile in memory and execute
//   khudra build   <file>       compile to a standalone native executable
//   khudra check   <file.khu>   parse + typecheck only, no output
//   khudra emit-bc <file.khu>   write bytecode (alias of compile with -o)
//   khudra disasm  <file.kbc>   disassemble a bytecode image
//
// Two execution paths reach the same image: the bytecode VM (`run`) and the
// native backend (`run --native`, `build`). Argument parsing lives in
// cli_options.cpp so the unit tests can drive it.
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "bytecode/disassembler.h"
#include "bytecode/module.h"
#include "bytecode/verifier.h"
#include "cli_options.h"
#include "compiler.h"
#include "diag/diagnostic.h"
#include "diag/source_manager.h"
#include "host/native_backend.h"
#include "lexer/token.h"
#include "parser/pretty_printer.h"
#include "util/array.h"
#include "util/file.h"
#include "vm/vm.h"

namespace {

using khu::cli::Command;
using khu::cli::Options;

void dump_tokens(const khu::Compiler& compiler, const khu::util::Array<khu::lexer::Token>& tokens) {
    for (const khu::lexer::Token& token : tokens) {
        std::printf("%-24s %-16s %s\n", compiler.sources().format(token.loc).c_str(),
                    khu::lexer::token_name(token.kind), std::string(token.text).c_str());
    }
}

// Executes a loaded image on the bytecode VM.
int execute(const khu::bytecode::Module& module, const Options& options) {
    khu::vm::Vm vm(module);
    vm.set_program_args(options.program_args);
    if (!vm.run()) {
        // Two ways to stop early, and only one of them is a fault:
        // `khuStdSystem.exit(code)` unwinds exactly the way a trap does, so the
        // exit code is what tells them apart, not the return value.
        if (vm.exit_requested()) return vm.exit_code();
        std::fwrite(vm.error().data(), 1, vm.error().size(), stderr);
        return 1;
    }
    return 0;
}

// Executes a loaded image through the native backend.
//
// A machine with no host compiler falls back to the VM with a notice rather
// than failing: the toolchain stays usable, and the fallback is printed rather
// than silent (docs/native.md, section 1).
int execute_native(const khu::bytecode::Module& module, const Options& options) {
    int exit_code = 0;
    std::string error;
    switch (khu::native::run_jit(module, exit_code, error, nullptr, &options.program_args)) {
        case khu::native::NativeStatus::Ok:
            return exit_code;
        case khu::native::NativeStatus::NoCompiler:
            if (module.has_inline()) {
                // There is nothing to fall back to: an inline block is raw C
                // or asm and only the native backend can run it
                // (docs/spec.md, section 5.1).
                std::fprintf(stderr,
                             "khudra: %s: this image contains inline blocks "
                             "(inline_c / inline_asm), which are native-only, "
                             "but no host C compiler (cc, clang or gcc) was "
                             "found\n",
                             options.input.c_str());
                return 1;
            }
            std::fprintf(stderr,
                         "khudra: %s: no host C compiler (cc, clang or gcc) was found, so "
                         "--native falls back to the bytecode VM\n",
                         options.input.c_str());
            return execute(module, options);
        case khu::native::NativeStatus::Failed:
            break;
    }
    std::fprintf(stderr, "khudra: %s: %s\n", options.input.c_str(), error.c_str());
    return 1;
}

// Writes a standalone native executable.
int build_native(const khu::bytecode::Module& module, const Options& options) {
    std::string path =
        options.output.empty() ? khu::cli::default_binary_path(options.input) : options.output;
    std::string error;
    switch (khu::native::build_executable(module, path, options.keep_c, error)) {
        case khu::native::NativeStatus::Ok:
            return 0;
        case khu::native::NativeStatus::NoCompiler:
            // `build` has no interpreter to fall back to: producing a native
            // binary is the whole request.
            std::fprintf(stderr,
                         "khudra: %s: 'build' needs a host C and C++ compiler (cc/clang and "
                         "c++/clang++); none was found\n",
                         options.input.c_str());
            return 1;
        case khu::native::NativeStatus::Failed:
            break;
    }
    std::fprintf(stderr, "khudra: %s: %s\n", options.input.c_str(), error.c_str());
    return 1;
}

int run_stage(const Options& options) {
    // A .kbc image is already compiled: `run`, `build` and `disasm` take it
    // directly rather than looking for a source file that may not be there.
    if (options.command == Command::Disasm || options.command == Command::Run ||
        options.command == Command::Build) {
        std::string bytes;
        if (khu::util::read_file(options.input, bytes) == khu::util::FileError::None &&
            bytes.size() >= 4 && std::memcmp(bytes.data(), khu::bytecode::kMagic, 4) == 0) {
            khu::bytecode::Module module;
            std::string error;
            if (!khu::bytecode::deserialize(bytes, module, error)) {
                std::fprintf(stderr, "khudra: %s: %s\n", options.input.c_str(), error.c_str());
                return 1;
            }
            if (options.command == Command::Build) return build_native(module, options);
            if (options.command == Command::Run) {
                return options.native ? execute_native(module, options) : execute(module, options);
            }
            std::string text = khu::bytecode::disassemble(module);
            std::fwrite(text.data(), 1, text.size(), stdout);
            return 0;
        }
    }

    khu::Compiler compiler;

    std::string load_error;
    std::uint32_t file_id = compiler.add_file(options.input, load_error);
    if (file_id == khu::diag::kInvalidFileId) {
        std::fprintf(stderr, "khudra: cannot open '%s': %s\n", options.input.c_str(),
                     load_error.c_str());
        return 1;
    }

    if (options.dump_tokens) {
        khu::util::Array<khu::lexer::Token> tokens = compiler.tokenize(file_id);
        compiler.diagnostics().print(stderr);
        if (compiler.diagnostics().has_errors()) return 1;
        dump_tokens(compiler, tokens);
        if (!options.dump_ast) return 0;
    }

    if (options.dump_ast) {
        khu::ast::CompilationUnit* unit = compiler.parse(file_id);
        compiler.diagnostics().print(stderr);
        if (compiler.diagnostics().has_errors()) return 1;
        khu::parser::PrettyPrinter printer;
        std::string text = printer.print(*unit);
        std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }

    if (options.command == Command::Check) {
        compiler.analyze(file_id);
        compiler.diagnostics().print(stderr);
        return compiler.diagnostics().has_errors() ? 1 : 0;
    }

    khu::bytecode::Module module;
    bool built = compiler.compile(file_id, module);
    compiler.diagnostics().print(stderr);
    if (!built) return 1;

    switch (options.command) {
        case Command::Disasm: {
            // A listing is a debugging aid, so a bad image is still worth
            // printing -- with the reason it would be refused at the top.
            std::string report;
            if (!khu::bytecode::verify(module, report)) {
                std::fprintf(stderr, "khudra: this image would be refused at run time:\n%s",
                             report.c_str());
            }
            std::string text = khu::bytecode::disassemble(module);
            std::fwrite(text.data(), 1, text.size(), stdout);
            return 0;
        }

        case Command::Compile:
        case Command::EmitBc: {
            std::string path = options.output.empty()
                                   ? khu::cli::default_output_path(options.input)
                                   : options.output;
            std::string bytes = khu::bytecode::serialize(module);
            if (!khu::util::write_file(path, bytes)) {
                std::fprintf(stderr, "khudra: cannot write '%s'\n", path.c_str());
                return 1;
            }
            return 0;
        }

        case Command::Build:
            return build_native(module, options);

        case Command::Run:
            return options.native ? execute_native(module, options) : execute(module, options);

        default:
            break;
    }

    // Every command that reaches here is handled above; this is a guard, not a
    // stage that is still missing.
    std::fprintf(stderr, "khudra: internal error: no handler for '%s'\n",
                 options.command_name.c_str());
    return 70;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!khu::cli::parse_arguments(argc, argv, options, error) ||
        !khu::cli::validate_options(options, error)) {
        std::fprintf(stderr, "khudra: %s\n\n", error.c_str());
        khu::cli::print_usage(stderr);
        return 2;
    }

    switch (options.command) {
        case Command::Help:
            khu::cli::print_usage(stdout);
            return 0;
        case Command::Version:
            std::printf("khudra %s\n", khu::cli::kVersion);
            return 0;
        default:
            break;
    }

    if (khu::cli::command_needs_input(options.command) && options.input.empty()) {
        std::fprintf(stderr, "khudra: '%s' requires an input file\n\n",
                     options.command_name.c_str());
        khu::cli::print_usage(stderr);
        return 2;
    }

    return run_stage(options);
}
