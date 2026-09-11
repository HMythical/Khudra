#include "cli_options.h"

namespace khu::cli {

const char* const kVersion = "0.1.0-dev";

Command parse_command(std::string_view name) {
    if (name == "compile") return Command::Compile;
    if (name == "run") return Command::Run;
    if (name == "build") return Command::Build;
    if (name == "check") return Command::Check;
    if (name == "emit-bc") return Command::EmitBc;
    if (name == "disasm") return Command::Disasm;
    if (name == "help" || name == "--help" || name == "-h") return Command::Help;
    if (name == "version" || name == "--version" || name == "-V") return Command::Version;
    return Command::None;
}

void print_usage(std::FILE* stream) {
    std::fprintf(stream,
                 "khudra %s -- the Khudra language toolchain\n"
                 "\n"
                 "usage: khudra <command> [options] <file>\n"
                 "\n"
                 "commands:\n"
                 "  compile <file.khu>   compile a source file to bytecode\n"
                 "  run     <file>       execute a .khu source or a .kbc image\n"
                 "  build   <file>       compile a .khu source or a .kbc image to a\n"
                 "                       standalone native executable\n"
                 "  check   <file.khu>   parse and typecheck only\n"
                 "  emit-bc <file.khu>   write a .kbc bytecode image\n"
                 "  disasm  <file>       disassemble a .khu source or a .kbc image\n"
                 "  version              print the toolchain version\n"
                 "  help                 print this message\n"
                 "\n"
                 "options:\n"
                 "  -o <path>            output path\n"
                 "  --native             (run) execute through the native backend --\n"
                 "                       raw memory, no interpreter -- instead of the VM\n"
                 "  --keep-c             (build) keep the intermediate C translation unit\n"
                 "  --allow-kernel       enable the khuAdvKernel* direct syscall libraries;\n"
                 "                       off by default because portable programs never need them\n"
                 "  --dump-tokens        print the token stream\n"
                 "  --dump-ast           print the parsed AST\n"
                 "  -- <args...>         everything after this goes to the program,\n"
                 "                       where khuStdSystem.argv() reads it\n"
                 "\n"
                 "native execution:\n"
                 "  khudra run --native <file>            compile to C, load, run in process\n"
                 "  khudra build <file.khu|file.kbc> [-o <path>]\n"
                 "                                        write a standalone executable\n"
                 "  Both need a host C compiler (cc or clang). `run --native` falls back to\n"
                 "  the VM with a notice when none is installed; `build` reports an error.\n",
                 kVersion);
}

bool parse_arguments(int argc, char** argv, Options& options, std::string& error) {
    if (argc < 2) {
        error = "no command given";
        return false;
    }

    options.command_name = argv[1];
    options.command = parse_command(options.command_name);
    if (options.command == Command::None) {
        error = "unknown command '" + options.command_name + "'";
        return false;
    }

    for (int i = 2; i < argc; ++i) {
        std::string_view argument = argv[i];
        if (argument == "--") {
            // Everything past this point belongs to the program being run, not
            // to khudra. Without the separator a program could never be given
            // an argument that khudra also spells -- `--native` above all.
            for (int j = i + 1; j < argc; ++j) options.program_args.emplace_back(argv[j]);
            break;
        }
        if (argument == "-o") {
            if (i + 1 >= argc) {
                error = "-o requires a path";
                return false;
            }
            options.output = argv[++i];
        } else if (argument == "--dump-tokens") {
            options.dump_tokens = true;
        } else if (argument == "--dump-ast") {
            options.dump_ast = true;
        } else if (argument == "--native") {
            options.native = true;
        } else if (argument == "--keep-c") {
            options.keep_c = true;
        } else if (argument == "--allow-kernel") {
            options.allow_kernel = true;
        } else if (!argument.empty() && argument[0] == '-' && argument != "-") {
            error = "unknown option '" + std::string(argument) + "'";
            return false;
        } else if (options.input.empty()) {
            options.input = std::string(argument);
        } else {
            error = "unexpected extra argument '" + std::string(argument) + "'";
            return false;
        }
    }

    return true;
}

bool validate_options(const Options& options, std::string& error) {
    if (options.native && options.command != Command::Run) {
        error = "--native applies to 'run'; '" + options.command_name +
                "' has no interpreter to replace";
        return false;
    }
    if (options.keep_c && options.command != Command::Build) {
        error = "--keep-c applies to 'build'";
        return false;
    }
    if (options.allow_kernel && !command_needs_input(options.command)) {
        error = "--allow-kernel needs a source or a bytecode image to gate";
        return false;
    }
    if (options.native && (options.dump_tokens || options.dump_ast)) {
        error = "--native cannot be combined with --dump-tokens or --dump-ast";
        return false;
    }
    if (!options.program_args.empty() && options.command != Command::Run) {
        error = "arguments after '--' are for the program being run; '" + options.command_name +
                "' does not run one";
        return false;
    }
    return true;
}

bool command_needs_input(Command command) {
    switch (command) {
        case Command::Compile:
        case Command::Run:
        case Command::Build:
        case Command::Check:
        case Command::EmitBc:
        case Command::Disasm:
            return true;
        default:
            return false;
    }
}

namespace {

// The extension, when the last dot belongs to the file name rather than to a
// directory along the way.
std::size_t extension_dot(const std::string& input) {
    std::size_t dot = input.find_last_of('.');
    std::size_t slash = input.find_last_of('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) return dot;
    return std::string::npos;
}

}  // namespace

std::string default_output_path(const std::string& input) {
    std::size_t dot = extension_dot(input);
    if (dot != std::string::npos) return input.substr(0, dot) + ".kbc";
    return input + ".kbc";
}

std::string default_binary_path(const std::string& input) {
    std::size_t dot = extension_dot(input);
    std::string stem = dot != std::string::npos ? input.substr(0, dot) : input;
    // An executable with no extension next to a source of the same stem would
    // be indistinguishable from the source directory listing, so keep the
    // whole path but drop the extension -- `khudra build a/b.khu` -> `a/b`.
    if (stem.empty()) stem = "a.out";
    return stem;
}

}  // namespace khu::cli
