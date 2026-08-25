// khudra -- command line driver.
//
//   khudra compile <file.khu>   compile to a .kbc bytecode image
//   khudra run     <file.khu>   compile in memory and execute
//   khudra check   <file.khu>   parse + typecheck only, no output
//   khudra emit-bc <file.khu>   write bytecode (alias of compile with -o)
//   khudra disasm  <file.kbc>   disassemble a bytecode image
//
// The subcommands are wired to a shared front end as the phases land; until a
// stage exists the driver still loads the source and reports a real diagnostic
// with a file:line so error plumbing is exercised from day one.
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "compiler.h"
#include "diag/diagnostic.h"
#include "diag/source_manager.h"
#include "lexer/token.h"
#include "parser/pretty_printer.h"
#include "util/array.h"

namespace {

constexpr const char* kVersion = "0.1.0-dev";

enum class Command {
    None,
    Compile,
    Run,
    Check,
    EmitBc,
    Disasm,
    Help,
    Version,
};

struct Options {
    Command command = Command::None;
    std::string command_name;
    std::string input;
    std::string output;
    bool dump_tokens = false;
    bool dump_ast = false;
};

Command parse_command(std::string_view name) {
    if (name == "compile") return Command::Compile;
    if (name == "run") return Command::Run;
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
                 "  run     <file.khu>   compile in memory and execute\n"
                 "  check   <file.khu>   parse and typecheck only\n"
                 "  emit-bc <file.khu>   write a .kbc bytecode image\n"
                 "  disasm  <file.kbc>   disassemble a bytecode image\n"
                 "  version              print the toolchain version\n"
                 "  help                 print this message\n"
                 "\n"
                 "options:\n"
                 "  -o <path>            output path\n"
                 "  --dump-tokens        print the token stream\n"
                 "  --dump-ast           print the parsed AST\n",
                 kVersion);
}

// Returns false when the arguments are malformed; `error` explains why.
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

bool command_needs_input(Command command) {
    switch (command) {
        case Command::Compile:
        case Command::Run:
        case Command::Check:
        case Command::EmitBc:
        case Command::Disasm:
            return true;
        default:
            return false;
    }
}

void dump_tokens(const khu::Compiler& compiler, const khu::util::Array<khu::lexer::Token>& tokens) {
    for (const khu::lexer::Token& token : tokens) {
        std::printf("%-24s %-16s %s\n", compiler.sources().format(token.loc).c_str(),
                    khu::lexer::token_name(token.kind), std::string(token.text).c_str());
    }
}

int run_stage(const Options& options) {
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

    compiler.analyze(file_id);
    compiler.diagnostics().print(stderr);
    if (compiler.diagnostics().has_errors()) return 1;

    // `check` stops after semantic analysis by design.
    if (options.command == Command::Check) return 0;

    // The stages behind the front end arrive in later phases. The report still
    // carries a real file:line:col.
    khu::diag::SourceLocation start{file_id, 1, 1, 0};
    khu::diag::DiagnosticEngine& diagnostics = compiler.diagnostics();
    diagnostics.error(start, "'" + options.command_name + "' is not implemented yet")
        .note(start, "the source passed semantic analysis; bytecode and the VM land in Phase 3 "
                     "(see KHU-PLAN.md)");

    diagnostics.print(stderr);
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!parse_arguments(argc, argv, options, error)) {
        std::fprintf(stderr, "khudra: %s\n\n", error.c_str());
        print_usage(stderr);
        return 2;
    }

    switch (options.command) {
        case Command::Help:
            print_usage(stdout);
            return 0;
        case Command::Version:
            std::printf("khudra %s\n", kVersion);
            return 0;
        default:
            break;
    }

    if (command_needs_input(options.command) && options.input.empty()) {
        std::fprintf(stderr, "khudra: '%s' requires an input file\n\n",
                     options.command_name.c_str());
        print_usage(stderr);
        return 2;
    }

    return run_stage(options);
}
