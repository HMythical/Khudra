// khudra command-line surface.
//
// Parsing lives apart from the driver so the unit tests can exercise it
// directly: an option table is exactly the kind of thing that rots quietly, and
// `main` is not reachable from a test binary.
#ifndef KHU_MAIN_CLI_OPTIONS_H
#define KHU_MAIN_CLI_OPTIONS_H

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace khu::cli {

extern const char* const kVersion;

enum class Command {
    None,
    Compile,
    Run,
    Build,
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
    // `run --native`: execute through the native backend instead of the VM.
    bool native = false;
    // `build --keep-c`: leave the intermediate translation unit on disk.
    bool keep_c = false;
    // The kernel tier is opt-in at the toolchain boundary, because a program
    // that talks to the kernel directly is not portable and nothing in the
    // runtime checks what it asks for (PLAN.md, section 9.4).
    bool allow_kernel = false;
    // Everything after a `--`: the *program's* arguments, not khudra's.
    // `khuStdSystem.argc()`/`argv()` answer with these, and the separator is
    // what keeps a program's `--native` from being read as khudra's.
    std::vector<std::string> program_args;
};

Command parse_command(std::string_view name);
void print_usage(std::FILE* stream);

// Returns false when the arguments are malformed; `error` explains why.
bool parse_arguments(int argc, char** argv, Options& options, std::string& error);

// Rejects options that do not apply to the chosen command. Separate from
// parsing so a flag's spelling and a flag's applicability fail differently.
bool validate_options(const Options& options, std::string& error);

bool command_needs_input(Command command);

// `khudra compile hello.khu` writes hello.kbc next to the source.
std::string default_output_path(const std::string& input);
// `khudra build hello.khu` writes ./hello next to the source.
std::string default_binary_path(const std::string& input);

}  // namespace khu::cli

#endif  // KHU_MAIN_CLI_OPTIONS_H
