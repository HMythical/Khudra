// Command-line parsing.
//
// The option table is the toolchain's contract with its users, and `main` is
// not reachable from a test binary, so the parser is exercised through
// khu::cli directly.
#include "test_harness.h"

#include <string>
#include <vector>

#include "cli_options.h"

namespace {

using khu::cli::Command;
using khu::cli::Options;

// Builds an argv the parser can walk. The strings outlive the call.
struct Args {
    std::vector<std::string> storage;
    std::vector<char*> pointers;

    explicit Args(std::initializer_list<const char*> words) {
        for (const char* word : words) storage.emplace_back(word);
        for (std::string& word : storage) pointers.push_back(word.data());
    }

    int count() const { return static_cast<int>(pointers.size()); }
    char** argv() { return pointers.data(); }
};

// Parse and validate together, the way main() does.
bool parse(std::initializer_list<const char*> words, Options& options, std::string& error) {
    Args args(words);
    if (!khu::cli::parse_arguments(args.count(), args.argv(), options, error)) return false;
    return khu::cli::validate_options(options, error);
}

}  // namespace

KHU_TEST(cli, parses_run) {
    Options options;
    std::string error;
    KHU_CHECK(parse({"khudra", "run", "a.khu"}, options, error));
    KHU_CHECK(options.command == Command::Run);
    KHU_CHECK_EQ(options.input, std::string("a.khu"));
    KHU_CHECK_EQ(options.native, false);
}

KHU_TEST(cli, parses_native_flag) {
    Options options;
    std::string error;
    KHU_CHECK(parse({"khudra", "run", "--native", "a.khu"}, options, error));
    KHU_CHECK(options.command == Command::Run);
    KHU_CHECK_EQ(options.native, true);
    KHU_CHECK_EQ(options.input, std::string("a.khu"));
}

KHU_TEST(cli, native_flag_may_follow_the_file) {
    Options options;
    std::string error;
    KHU_CHECK(parse({"khudra", "run", "a.khu", "--native"}, options, error));
    KHU_CHECK_EQ(options.native, true);
}

KHU_TEST(cli, parses_build) {
    Options options;
    std::string error;
    KHU_CHECK(parse({"khudra", "build", "a.khu", "-o", "out/a"}, options, error));
    KHU_CHECK(options.command == Command::Build);
    KHU_CHECK_EQ(options.input, std::string("a.khu"));
    KHU_CHECK_EQ(options.output, std::string("out/a"));
}

KHU_TEST(cli, build_accepts_an_image) {
    Options options;
    std::string error;
    KHU_CHECK(parse({"khudra", "build", "a.kbc"}, options, error));
    KHU_CHECK(options.command == Command::Build);
    KHU_CHECK_EQ(options.input, std::string("a.kbc"));
}

KHU_TEST(cli, build_keeps_the_c_when_asked) {
    Options options;
    std::string error;
    KHU_CHECK(parse({"khudra", "build", "--keep-c", "a.khu"}, options, error));
    KHU_CHECK_EQ(options.keep_c, true);
}

KHU_TEST(cli, native_is_rejected_where_it_means_nothing) {
    for (const char* command : {"compile", "check", "emit-bc", "disasm", "build"}) {
        Options options;
        std::string error;
        KHU_CHECK(!parse({"khudra", command, "--native", "a.khu"}, options, error));
        KHU_CHECK_CONTAINS(error, "--native applies to 'run'");
        KHU_CHECK_CONTAINS(error, command);
    }
}

KHU_TEST(cli, keep_c_is_rejected_outside_build) {
    Options options;
    std::string error;
    KHU_CHECK(!parse({"khudra", "run", "--keep-c", "a.khu"}, options, error));
    KHU_CHECK_CONTAINS(error, "--keep-c applies to 'build'");
}

KHU_TEST(cli, native_does_not_mix_with_the_dumps) {
    Options options;
    std::string error;
    KHU_CHECK(!parse({"khudra", "run", "--native", "--dump-ast", "a.khu"}, options, error));
    KHU_CHECK_CONTAINS(error, "--dump-ast");
}

KHU_TEST(cli, rejects_unknown_options_and_commands) {
    Options options;
    std::string error;
    KHU_CHECK(!parse({"khudra", "run", "--nativ", "a.khu"}, options, error));
    KHU_CHECK_CONTAINS(error, "unknown option '--nativ'");

    Options other;
    std::string other_error;
    KHU_CHECK(!parse({"khudra", "jit", "a.khu"}, other, other_error));
    KHU_CHECK_CONTAINS(other_error, "unknown command 'jit'");
}

KHU_TEST(cli, rejects_a_dangling_output_flag) {
    Options options;
    std::string error;
    KHU_CHECK(!parse({"khudra", "build", "a.khu", "-o"}, options, error));
    KHU_CHECK_CONTAINS(error, "-o requires a path");
}

KHU_TEST(cli, rejects_extra_arguments) {
    Options options;
    std::string error;
    KHU_CHECK(!parse({"khudra", "run", "a.khu", "b.khu"}, options, error));
    KHU_CHECK_CONTAINS(error, "unexpected extra argument 'b.khu'");
}

KHU_TEST(cli, build_needs_an_input_file) {
    KHU_CHECK(khu::cli::command_needs_input(Command::Build));
    KHU_CHECK(!khu::cli::command_needs_input(Command::Help));
    KHU_CHECK(!khu::cli::command_needs_input(Command::Version));
}

KHU_TEST(cli, derives_default_output_paths) {
    KHU_CHECK_EQ(khu::cli::default_output_path("a/b.khu"), std::string("a/b.kbc"));
    KHU_CHECK_EQ(khu::cli::default_output_path("plain"), std::string("plain.kbc"));
    // A dot in a directory name is not an extension.
    KHU_CHECK_EQ(khu::cli::default_output_path("a.d/b"), std::string("a.d/b.kbc"));

    KHU_CHECK_EQ(khu::cli::default_binary_path("a/b.khu"), std::string("a/b"));
    KHU_CHECK_EQ(khu::cli::default_binary_path("a/b.kbc"), std::string("a/b"));
    KHU_CHECK_EQ(khu::cli::default_binary_path("a.d/b"), std::string("a.d/b"));
}

KHU_TEST(cli, help_and_version_parse_as_commands) {
    KHU_CHECK(khu::cli::parse_command("help") == Command::Help);
    KHU_CHECK(khu::cli::parse_command("--help") == Command::Help);
    KHU_CHECK(khu::cli::parse_command("-h") == Command::Help);
    KHU_CHECK(khu::cli::parse_command("version") == Command::Version);
    KHU_CHECK(khu::cli::parse_command("-V") == Command::Version);
    KHU_CHECK(khu::cli::parse_command("build") == Command::Build);
}
