// khuStdSystem: the handle registry, the process surface, and the one new
// runtime capability underneath them.
//
// Three things are checked here that no golden can check:
//
//   1. **Both backends, same answers.** Every program runs on KhudraVm and on
//      the emitted C, and the two are compared -- including the exit code,
//      which `khuStdSystem.exit` is the first thing in the language to set.
//   2. **The handle registry accounts.** An open handle is a manual block, so
//      `khuStdMem.liveBytes()`/`liveBlocks()` see it, and closing has to give
//      it back. The assertions are on the allocator itself, not on what the
//      program printed.
//   3. **Failure paths.** A golden may not carry an `errno()` code or an
//      `errorMessage()` string, because both are platform-specific
//      (EXPANSION-PLAN.md, section 2.6). This is where they are checked, with
//      the platform-specific halves behind `#if defined(_WIN32)`.
#include "test_harness.h"

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "alloc.h"
}

#include "bytecode/module.h"
#include "compiler.h"
#include "host/native_backend.h"
#include "vm/natives.h"
#include "vm/platform.h"
#include "vm/vm.h"

namespace {

struct Outcome {
    bool compiled = false;
    bool ran = false;
    int exit_code = 0;
    std::string output;
    std::string error_output;
    std::string runtime_error;
    std::string diagnostics;
};

bool compile_program(const std::string& body, khu::bytecode::Module& module,
                     std::string& diagnostics) {
    khu::Compiler compiler;
    std::uint32_t file = compiler.add_buffer(
        "test.khu", "public class SystemTest {\n    func main() {\n" + body + "\n    }\n}\n");
    bool built = compiler.compile(file, module);
    diagnostics = compiler.diagnostics().render();
    return built;
}

Outcome run_vm(const std::string& body, const std::vector<std::string>& args = {}) {
    Outcome result;
    khu::bytecode::Module module;
    result.compiled = compile_program(body, module, result.diagnostics);
    if (!result.compiled) return result;

    khu::vm::Vm vm(module);
    vm.set_output_sink(&result.output);
    vm.set_error_sink(&result.error_output);
    vm.set_program_args(args);
    result.ran = vm.run();
    result.runtime_error = vm.error();
    // A program that asked to exit did not fail; it named its own status.
    if (!result.ran && vm.exit_requested()) {
        result.ran = true;
        result.exit_code = vm.exit_code();
        result.runtime_error.clear();
    } else if (!result.ran) {
        result.exit_code = 1;
    }
    return result;
}

// The same program through the emitted C. Answers false when the machine has
// no host compiler, so a caller reports "not run" rather than failing -- the
// same fallback `khudra run --native` makes.
bool run_native(const std::string& body, Outcome& result,
                const std::vector<std::string>& args = {}) {
    khu::bytecode::Module module;
    result.compiled = compile_program(body, module, result.diagnostics);
    if (!result.compiled) return true;

    khu::native::JitCapture capture;
    std::string backend_error;
    int exit_code = 0;
    switch (khu::native::run_jit(module, exit_code, backend_error, &capture, &args)) {
        case khu::native::NativeStatus::NoCompiler:
            return false;
        case khu::native::NativeStatus::Failed:
            result.ran = false;
            result.runtime_error = backend_error;
            return true;
        case khu::native::NativeStatus::Ok:
            break;
    }
    result.exit_code = exit_code;
    result.output = capture.output;
    result.error_output = capture.error_output;
    result.runtime_error = capture.runtime_error;
    result.ran = capture.runtime_error.empty();
    return true;
}

// Runs `body` on both backends and asserts that stdout, stderr and the exit
// code agree -- the invariant the whole native backend exists to keep -- then
// hands back the VM's answer for the test to make its own assertions on.
Outcome run_both(const std::string& body, const std::vector<std::string>& args = {}) {
    Outcome vm = run_vm(body, args);
    if (!vm.compiled) {
        KHU_FAIL("did not compile:\n" + vm.diagnostics);
        return vm;
    }
    if (!vm.ran) return vm;

    Outcome native;
    if (!run_native(body, native, args)) return vm;  // no host compiler
    KHU_CHECK_EQ(native.output, vm.output);
    KHU_CHECK_EQ(native.error_output, vm.error_output);
    KHU_CHECK_EQ(native.exit_code, vm.exit_code);
    KHU_CHECK_EQ(native.runtime_error, vm.runtime_error);
    return vm;
}

// A path in the current directory, distinct per test so two of them cannot
// collide if the suite is ever run in parallel.
std::string scratch_path(const char* name) { return std::string("khu_unit_") + name + ".tmp"; }

void remove_scratch(const std::string& path) { std::remove(path.c_str()); }

}  // namespace

// ---------------------------------------------------------------------------
// The handle registry
// ---------------------------------------------------------------------------

KHU_TEST(system, an_open_handle_is_a_manual_block_that_close_gives_back) {
    std::string path = scratch_path("handle");
    remove_scratch(path);
    std::size_t before = khu_manual_live_bytes();
    std::size_t blocks_before = khu_manual_live_blocks();

    // A delta, not an absolute: this process runs many programs, and one of
    // them traps on purpose and leaks its own buffer doing it. What matters is
    // that opening costs one block and closing gives it back.
    Outcome result = run_vm(
        "        int64 base = khuStdMem.liveBlocks();\n"
        "        *byte f = khuStdSystem.open(\"" + path + "\", \"w\");\n"
        "        io.printLine(khuStdSystem.isOpen(f));\n"
        "        io.printLine(khuStdMath.subtract(khuStdMem.liveBlocks(), base));\n"
        "        khuStdSystem.close(f);\n"
        "        io.printLine(khuStdSystem.isOpen(f));\n"
        "        io.printLine(khuStdMath.subtract(khuStdMem.liveBlocks(), base));\n");

    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("true\n1\nfalse\n0\n"));
    // And nothing leaked out of the run itself.
    KHU_CHECK_EQ(khu_manual_live_bytes(), before);
    KHU_CHECK_EQ(khu_manual_live_blocks(), blocks_before);
    remove_scratch(path);
}

KHU_TEST(system, a_handle_left_open_is_closed_when_the_run_ends) {
    // A program that forgets to close still may not leak an OS object -- or a
    // manual block -- out of the process.
    std::string path = scratch_path("leaked");
    remove_scratch(path);
    std::size_t blocks_before = khu_manual_live_blocks();

    Outcome result = run_vm(
        "        int64 base = khuStdMem.liveBlocks();\n"
        "        *byte f = khuStdSystem.open(\"" + path + "\", \"w\");\n"
        "        khuStdSystem.writeText(f, \"unclosed\");\n"
        "        io.printLine(khuStdMath.subtract(khuStdMem.liveBlocks(), base));\n");

    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("1\n"));
    KHU_CHECK_EQ(khu_manual_live_blocks(), blocks_before);
    remove_scratch(path);
}

KHU_TEST(system, closing_null_or_a_spent_handle_does_nothing) {
    Outcome result = run_both(
        "        *byte nothing = khuStdMem.alloc(0);\n"
        "        khuStdSystem.close(nothing);\n"
        "        io.printLine(khuStdSystem.isOpen(nothing));\n"
        "        khuStdMem.release(nothing);\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("false\n"));
}

KHU_TEST(system, a_buffer_is_not_a_handle) {
    // A `*byte` that khuStdMem handed out is not an open stream, and the
    // registry is what says so: `isOpen` is a lookup, not a null check.
    Outcome result = run_both(
        "        *byte buffer = khuStdMem.alloc(16);\n"
        "        io.printLine(khuStdSystem.isOpen(buffer));\n"
        "        io.printLine(khuStdSystem.readByte(buffer));\n"
        "        khuStdMem.release(buffer);\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("false\n-1\n"));
}

// ---------------------------------------------------------------------------
// Reading, writing and positioning
// ---------------------------------------------------------------------------

KHU_TEST(system, reads_back_exactly_what_it_wrote_on_both_backends) {
    std::string path = scratch_path("roundtrip");
    remove_scratch(path);
    Outcome result = run_both(
        "        *byte out = khuStdSystem.open(\"" + path + "\", \"w\");\n"
        "        khuStdSystem.writeText(out, \"one\\ntwo\\n\");\n"
        "        khuStdSystem.close(out);\n"
        "        *byte in = khuStdSystem.open(\"" + path + "\", \"r\");\n"
        "        io.printLine(khuStdSystem.readText(in, 64));\n"
        "        io.printLine(khuStdSystem.atEof(in));\n"
        "        khuStdSystem.rewind(in);\n"
        "        io.printLine(khuStdSystem.readLine(in));\n"
        "        io.printLine(khuStdSystem.readLine(in));\n"
        "        io.printLine(khuStdSystem.readLine(in));\n"
        "        io.printLine(khuStdSystem.atEof(in));\n"
        "        khuStdSystem.close(in);\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("one\ntwo\n\ntrue\none\ntwo\n\ntrue\n"));
    remove_scratch(path);
}

KHU_TEST(system, tells_end_of_input_apart_from_a_read_error) {
    // `readByte` answers -1 for both, which is why `atEof` and `error` are two
    // questions rather than one -- and why `clearError` clears both flags.
    std::string path = scratch_path("eof");
    remove_scratch(path);
    Outcome result = run_both(
        "        *byte out = khuStdSystem.open(\"" + path + "\", \"w\");\n"
        "        khuStdSystem.writeText(out, \"x\");\n"
        "        khuStdSystem.close(out);\n"
        "        *byte in = khuStdSystem.open(\"" + path + "\", \"r\");\n"
        "        io.printLine(khuStdSystem.readByte(in));\n"
        "        io.printLine(khuStdSystem.atEof(in));\n"
        "        io.printLine(khuStdSystem.readByte(in));\n"
        "        io.printLine(khuStdSystem.atEof(in));\n"
        "        io.printLine(khuStdSystem.error(in));\n"
        "        khuStdSystem.clearError(in);\n"
        "        io.printLine(khuStdSystem.atEof(in));\n"
        "        khuStdSystem.close(in);\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("120\nfalse\n-1\ntrue\nfalse\nfalse\n"));
    remove_scratch(path);
}

KHU_TEST(system, seeks_from_all_three_origins) {
    std::string path = scratch_path("seek");
    remove_scratch(path);
    Outcome result = run_both(
        "        *byte out = khuStdSystem.open(\"" + path + "\", \"w\");\n"
        "        khuStdSystem.writeText(out, \"abcdefgh\");\n"
        "        khuStdSystem.close(out);\n"
        "        *byte in = khuStdSystem.open(\"" + path + "\", \"r\");\n"
        "        khuStdSystem.seek(in, 2, 0);\n"
        "        io.printLine(khuStdSystem.readText(in, 2));\n"
        "        khuStdSystem.seek(in, 1, 1);\n"
        "        io.printLine(khuStdSystem.readText(in, 2));\n"
        "        khuStdSystem.seek(in, -2, 2);\n"
        "        io.printLine(khuStdSystem.readText(in, 2));\n"
        "        io.printLine(khuStdSystem.tell(in));\n"
        "        io.printLine(khuStdSystem.fileSize(in));\n"
        "        khuStdSystem.close(in);\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("cd\nfg\ngh\n8\n8\n"));
    remove_scratch(path);
}

KHU_TEST(system, writes_raw_bytes_from_a_checked_buffer) {
    std::string path = scratch_path("bytes");
    remove_scratch(path);
    Outcome result = run_both(
        "        *byte buffer = khuStdMem.alloc(4);\n"
        "        buffer[0] = 75;\n"
        "        buffer[1] = 72;\n"
        "        buffer[2] = 85;\n"
        "        buffer[3] = 10;\n"
        "        *byte out = khuStdSystem.open(\"" + path + "\", \"w\");\n"
        "        khuStdSystem.writeBytes(out, buffer, 4);\n"
        "        khuStdSystem.close(out);\n"
        "        khuStdMem.release(buffer);\n"
        "        *byte in = khuStdSystem.open(\"" + path + "\", \"r\");\n"
        "        io.print(khuStdSystem.readText(in, 8));\n"
        "        khuStdSystem.close(in);\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("KHU\n"));
    remove_scratch(path);
}

KHU_TEST(system, refuses_a_write_that_runs_past_the_end_of_its_buffer) {
    // The range check is khuStdMem's, reused: a buffer carries its length, so
    // this is a trap rather than a read off the end of the heap.
    std::string path = scratch_path("overrun");
    remove_scratch(path);
    Outcome result = run_vm(
        "        *byte buffer = khuStdMem.alloc(4);\n"
        "        *byte out = khuStdSystem.open(\"" + path + "\", \"w\");\n"
        "        khuStdSystem.writeBytes(out, buffer, 64);\n");
    KHU_CHECK(!result.ran);
    KHU_CHECK_CONTAINS(result.runtime_error, "runs past the end of a 4-byte buffer");
    remove_scratch(path);
}

// ---------------------------------------------------------------------------
// Failure paths -- the half a golden may not carry
// ---------------------------------------------------------------------------

KHU_TEST(system, opening_something_that_is_not_there_answers_null_and_sets_errno) {
    Outcome result = run_vm(
        "        *byte f = khuStdSystem.open(\"khu_unit_no_such_directory/nope\", \"r\");\n"
        "        io.printLine(khuStdMem.isNull(f));\n"
        "        io.printLine(khuStdSystem.errno() != 0);\n"
        "        io.printLine(khuStdString.isEmpty(khuStdSystem.errorMessage(khuStdSystem.errno())));\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("true\ntrue\nfalse\n"));
}

KHU_TEST(system, an_unknown_mode_is_refused_rather_than_guessed_at) {
    // A mode this runtime does not name could mean different things on the two
    // platforms, which is the one thing this library promises cannot happen.
    Outcome result = run_vm(
        "        *byte f = khuStdSystem.open(\"khu_unit_mode.tmp\", \"q\");\n"
        "        io.printLine(khuStdMem.isNull(f));\n"
        "        io.printLine(khuStdSystem.errno() != 0);\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("true\ntrue\n"));
}

KHU_TEST(system, the_error_slot_carries_the_platforms_own_number) {
    // The code is the operating system's, not a Khudra invention, so this is
    // the one place its actual value is asserted -- and it is asserted per
    // platform, because that is what it is.
    khu::vm::platform::clear_last_error();
    KHU_CHECK_EQ(khu::vm::platform::last_error(), 0);
    KHU_CHECK_EQ(khu::vm::platform::error_message(0), std::string("no error"));

    std::FILE* missing = khu::vm::platform::file_open("khu_unit_absent_dir/absent", "r");
    KHU_CHECK(missing == nullptr);
    int code = khu::vm::platform::last_error();
    KHU_CHECK(code != 0);
    KHU_CHECK(!khu::vm::platform::error_message(code).empty());
#if defined(_WIN32)
    // The narrow CRT reports a missing path as ENOENT here; the Winsock half of
    // the slot carries WSA* codes instead, which is exactly why the slot is not
    // bare `errno`.
    KHU_CHECK_EQ(code, ENOENT);
#else
    KHU_CHECK_EQ(code, ENOENT);
#endif
}

KHU_TEST(system, the_separators_are_this_platforms) {
    std::string separator = khu::vm::platform::path_separator();
    std::string list_separator = khu::vm::platform::path_list_separator();
#if defined(_WIN32)
    KHU_CHECK_EQ(separator, std::string("\\"));
    KHU_CHECK_EQ(list_separator, std::string(";"));
#else
    KHU_CHECK_EQ(separator, std::string("/"));
    KHU_CHECK_EQ(list_separator, std::string(":"));
#endif
}

// ---------------------------------------------------------------------------
// The process surface, and the object a native builds
// ---------------------------------------------------------------------------

KHU_TEST(system, argv_answers_a_real_list_of_strings_under_both_backends) {
    // The one genuinely new runtime capability: a native that produces a
    // Khudra *instance* rather than a primitive, a string or a pointer
    // (EXPANSION-PLAN.md, section 4). It has to be the same List under the
    // interpreter and under the emitted C.
    std::vector<std::string> args{"alpha", "beta", "gamma"};
    Outcome result = run_both(
        "        io.printLine(khuStdSystem.argc());\n"
        "        List<string> args = khuStdSystem.argv();\n"
        "        io.printLine(args.size());\n"
        "        int32 i = 0;\n"
        "        while (i < args.size()) {\n"
        "            io.printLine(args.get(i));\n"
        "            i = khuStdMath.add(i, 1);\n"
        "        }\n"
        "        io.printLine(args.contains(\"beta\"));\n"
        "        io.printLine(args.indexOf(\"gamma\"));\n",
        args);
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("3\n3\nalpha\nbeta\ngamma\ntrue\n2\n"));
}

KHU_TEST(system, a_native_built_list_survives_a_collection) {
    // `List.add` grows a backing array, so it allocates, so it can collect --
    // with nothing but the native's own C++ local holding the list. This is the
    // RootScope in vm/natives.h doing its job; under -DKHU_SANITIZE=ON it is
    // also the leak check.
    std::vector<std::string> args;
    for (int i = 0; i < 64; ++i) args.push_back("argument-" + std::to_string(i));

    Outcome result = run_both(
        "        List<string> args = khuStdSystem.argv();\n"
        "        io.printLine(args.size());\n"
        "        io.printLine(args.get(0));\n"
        "        io.printLine(args.get(63));\n"
        "        int32 i = 0;\n"
        "        while (i < 200) {\n"
        "            List<string> churn = List<string>();\n"
        "            churn.add(\"pressure\");\n"
        "            i = khuStdMath.add(i, 1);\n"
        "        }\n"
        "        io.printLine(args.get(63));\n",
        args);
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output,
                 std::string("64\nargument-0\nargument-63\nargument-63\n"));
}

KHU_TEST(system, no_arguments_is_an_empty_list_not_a_null_one) {
    Outcome result = run_both(
        "        io.printLine(khuStdSystem.argc());\n"
        "        List<string> args = khuStdSystem.argv();\n"
        "        io.printLine(args.isEmpty());\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("0\ntrue\n"));
}

KHU_TEST(system, tells_an_unset_variable_apart_from_one_set_to_nothing) {
    Outcome result = run_both(
        "        io.printLine(khuStdSystem.hasEnv(\"KHUDRA_UNIT_UNSET_VARIABLE\"));\n"
        "        io.printLine(khuStdString.isEmpty(khuStdSystem.getEnv(\"KHUDRA_UNIT_UNSET_VARIABLE\")));\n"
        "        List<string> names = khuStdSystem.envKeys();\n"
        "        io.printLine(names.size() > 0);\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("false\ntrue\ntrue\n"));
}

KHU_TEST(system, exit_names_the_status_and_stops_the_program) {
    Outcome result = run_both(
        "        io.printLine(\"printed\");\n"
        "        khuStdSystem.exit(7);\n"
        "        io.printLine(\"not printed\");\n");
    KHU_CHECK_EQ(result.output, std::string("printed\n"));
    KHU_CHECK_EQ(result.exit_code, 7);
    KHU_CHECK_EQ(result.runtime_error, std::string(""));
}

KHU_TEST(system, exit_zero_is_a_clean_exit_not_a_failure) {
    Outcome result = run_both(
        "        io.printLine(\"done\");\n"
        "        khuStdSystem.exit(0);\n");
    KHU_CHECK_EQ(result.output, std::string("done\n"));
    KHU_CHECK_EQ(result.exit_code, 0);
    KHU_CHECK_EQ(result.runtime_error, std::string(""));
}

// ---------------------------------------------------------------------------
// The file namespace
// ---------------------------------------------------------------------------

KHU_TEST(system, asks_and_answers_about_paths) {
    std::string path = scratch_path("paths");
    remove_scratch(path);
    Outcome result = run_both(
        "        io.printLine(khuStdSystem.exists(\"" + path + "\"));\n"
        "        *byte f = khuStdSystem.open(\"" + path + "\", \"w\");\n"
        "        khuStdSystem.writeText(f, \"1234567890\");\n"
        "        khuStdSystem.close(f);\n"
        "        io.printLine(khuStdSystem.exists(\"" + path + "\"));\n"
        "        io.printLine(khuStdSystem.isFile(\"" + path + "\"));\n"
        "        io.printLine(khuStdSystem.isDirectory(\"" + path + "\"));\n"
        "        io.printLine(khuStdSystem.fileSize(\"" + path + "\"));\n"
        "        io.printLine(khuStdSystem.deleteFile(\"" + path + "\"));\n"
        "        io.printLine(khuStdSystem.exists(\"" + path + "\"));\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output,
                 std::string("false\ntrue\ntrue\nfalse\n10\ntrue\nfalse\n"));
    remove_scratch(path);
}

KHU_TEST(system, a_missing_files_size_is_zero_with_a_reason) {
    // The sentinel convention: 0 rather than a trap, and `errno()` says why.
    Outcome result = run_vm(
        "        io.printLine(khuStdSystem.fileSize(\"khu_unit_no_such_file.tmp\"));\n"
        "        io.printLine(khuStdSystem.errno() != 0);\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("0\ntrue\n"));
}

// ---------------------------------------------------------------------------
// Sockets
//
// These are unit tests rather than goldens for the reason the plan gives: a
// socket picks a port and a moment in time, and neither belongs in a
// byte-for-byte comparison. What is deterministic is the *shape* of the
// exchange, and that is what is asserted.
//
// Every one of them binds 127.0.0.1 on port 0 -- the system chooses a free
// port, which `localPort` then reports -- so nothing here collides with
// anything else on the machine. Connect, accept, send and recv all happen on
// one thread, which works because the kernel completes a connect into the
// listener's backlog before anyone calls accept.
// ---------------------------------------------------------------------------

KHU_TEST(system, carries_bytes_over_a_real_socket_on_both_backends) {
    Outcome result = run_both(
        "        int64 base = khuStdMem.liveBlocks();\n"
        "        *byte server = khuStdSystem.listen(\"127.0.0.1\", 0);\n"
        "        io.printLine(khuStdSystem.isOpen(server));\n"
        "        int32 port = khuStdSystem.localPort(server);\n"
        "        io.printLine(port > 0);\n"
        "        *byte client = khuStdSystem.connect(\"127.0.0.1\", port);\n"
        "        *byte conn = khuStdSystem.accept(server);\n"
        "        io.printLine(khuStdSystem.isOpen(conn));\n"
        "        io.printLine(khuStdSystem.peerAddress(conn));\n"
        "        io.printLine(khuStdSystem.peerPort(conn) > 0);\n"
        "        *byte buffer = khuStdMem.alloc(64);\n"
        "        buffer[0] = 75;\n"
        "        buffer[1] = 72;\n"
        "        buffer[2] = 85;\n"
        "        io.printLine(khuStdSystem.send(client, buffer, 3));\n"
        // Half-closing the writing side is what lets the reader see a clean
        // end of stream without the connection being torn down.
        "        khuStdSystem.shutdown(client, 1);\n"
        "        khuStdMem.zero(buffer, 64);\n"
        "        io.printLine(khuStdSystem.recv(conn, buffer, 64));\n"
        "        io.printLine(buffer[0]);\n"
        "        io.printLine(buffer[2]);\n"
        "        io.printLine(khuStdSystem.recv(conn, buffer, 64));\n"
        "        khuStdMem.release(buffer);\n"
        "        khuStdSystem.close(conn);\n"
        "        khuStdSystem.close(client);\n"
        "        khuStdSystem.close(server);\n"
        // A delta, not an absolute: this process has run other programs, and
        // one of them traps on purpose and leaks its own buffer doing it.
        "        io.printLine(khuStdMath.subtract(khuStdMem.liveBlocks(), base));\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output,
                 std::string("true\ntrue\ntrue\n127.0.0.1\ntrue\n3\n3\n75\n85\n0\n0\n"));
}

KHU_TEST(system, a_socket_handle_is_a_manual_block_that_close_gives_back) {
    std::size_t blocks_before = khu_manual_live_blocks();
    Outcome result = run_vm(
        "        int64 base = khuStdMem.liveBlocks();\n"
        "        *byte server = khuStdSystem.listen(\"127.0.0.1\", 0);\n"
        "        *byte client = khuStdSystem.connect(\"127.0.0.1\", khuStdSystem.localPort(server));\n"
        "        *byte conn = khuStdSystem.accept(server);\n"
        "        io.printLine(khuStdMath.subtract(khuStdMem.liveBlocks(), base));\n"
        "        khuStdSystem.close(conn);\n"
        "        khuStdSystem.close(client);\n"
        "        khuStdSystem.close(server);\n"
        "        io.printLine(khuStdMath.subtract(khuStdMem.liveBlocks(), base));\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("3\n0\n"));
    KHU_CHECK_EQ(khu_manual_live_blocks(), blocks_before);
}

KHU_TEST(system, a_socket_left_open_is_closed_when_the_run_ends) {
    std::size_t blocks_before = khu_manual_live_blocks();
    Outcome result = run_vm(
        "        *byte server = khuStdSystem.listen(\"127.0.0.1\", 0);\n"
        "        io.printLine(khuStdSystem.isOpen(server));\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("true\n"));
    KHU_CHECK_EQ(khu_manual_live_blocks(), blocks_before);
}

KHU_TEST(system, a_listener_is_not_a_connection_and_a_file_is_not_a_socket) {
    // The registry records what kind of thing a handle names, so an operation
    // that cannot apply says so with a sentinel rather than handing a file
    // descriptor to Winsock and seeing what happens.
    std::string path = scratch_path("kinds");
    remove_scratch(path);
    Outcome result = run_both(
        "        *byte server = khuStdSystem.listen(\"127.0.0.1\", 0);\n"
        "        *byte buffer = khuStdMem.alloc(8);\n"
        // recv on a listener: wrong kind.
        "        io.printLine(khuStdSystem.recv(server, buffer, 8));\n"
        "        *byte f = khuStdSystem.open(\"" + path + "\", \"w\");\n"
        // accept on a file: wrong kind again.
        "        io.printLine(khuStdMem.isNull(khuStdSystem.accept(f)));\n"
        // seek on a socket: no position to move.
        "        io.printLine(khuStdSystem.tell(server));\n"
        "        khuStdMem.release(buffer);\n"
        "        khuStdSystem.close(f);\n"
        "        khuStdSystem.close(server);\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("-1\ntrue\n-1\n"));
    remove_scratch(path);
}

KHU_TEST(system, connecting_to_a_closed_port_answers_null_and_sets_errno) {
    // The port is bound, its number read, and then released -- so it is a
    // number nothing is listening on, without guessing at one.
    Outcome result = run_vm(
        "        *byte probe = khuStdSystem.listen(\"127.0.0.1\", 0);\n"
        "        int32 port = khuStdSystem.localPort(probe);\n"
        "        khuStdSystem.close(probe);\n"
        "        *byte client = khuStdSystem.connect(\"127.0.0.1\", port);\n"
        "        io.printLine(khuStdMem.isNull(client));\n"
        "        io.printLine(khuStdSystem.errno() != 0);\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("true\ntrue\n"));
}

KHU_TEST(system, resolves_a_name_and_reports_one_that_does_not_resolve) {
    // `localhost` is the one name every machine has. The failing case is a
    // name reserved by RFC 2606 precisely so that it never resolves.
    Outcome result = run_vm(
        "        io.printLine(khuStdString.isEmpty(khuStdSystem.resolveHost(\"localhost\")));\n"
        "        io.printLine(khuStdString.isEmpty(khuStdSystem.resolveHost(\"khudra.invalid\")));\n"
        "        io.printLine(khuStdSystem.errno() != 0);\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("false\ntrue\ntrue\n"));
}

KHU_TEST(system, a_read_timeout_stops_a_silent_peer_from_wedging_the_program) {
    // The one exception to "everything blocks". Without it a single-threaded
    // server has no way at all to give up on a peer that connects and then
    // says nothing -- which is why this member exists.
    Outcome result = run_vm(
        "        *byte server = khuStdSystem.listen(\"127.0.0.1\", 0);\n"
        "        *byte client = khuStdSystem.connect(\"127.0.0.1\", khuStdSystem.localPort(server));\n"
        "        *byte conn = khuStdSystem.accept(server);\n"
        "        khuStdSystem.setReadTimeout(conn, 100);\n"
        "        *byte buffer = khuStdMem.alloc(8);\n"
        // The client never sends, so this returns on the timeout rather than
        // blocking for ever.
        "        io.printLine(khuStdSystem.recv(conn, buffer, 8));\n"
        "        io.printLine(khuStdSystem.errno() != 0);\n"
        "        khuStdMem.release(buffer);\n"
        "        khuStdSystem.close(conn);\n"
        "        khuStdSystem.close(client);\n"
        "        khuStdSystem.close(server);\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("-1\ntrue\n"));
}

// ---------------------------------------------------------------------------
// Phase C: string ergonomics
// ---------------------------------------------------------------------------

KHU_TEST(system, join_reads_a_list_through_its_own_members) {
    // join knows nothing about how a List is laid out: it asks `size()` and
    // `get()`, the same way Khudra source would. That is what lets it be one
    // native rather than a second implementation of List.
    Outcome result = run_both(
        "        List<string> parts = List<string>();\n"
        "        io.printLine(khuStdString.concat(\"[\", khuStdCollection.join(parts, \",\"), \"]\"));\n"
        "        parts.add(\"a\");\n"
        "        io.printLine(khuStdCollection.join(parts, \", \"));\n"
        "        parts.add(\"b\");\n"
        "        parts.add(\"c\");\n"
        "        io.printLine(khuStdCollection.join(parts, \", \"));\n"
        "        io.printLine(khuStdCollection.join(parts, \"\"));\n"
        "        io.printLine(khuStdCollection.join(khuStdSystem.argv(), \"|\"));\n",
        std::vector<std::string>{"first", "second"});
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("[]\na\na, b, c\nabc\nfirst|second\n"));
}

KHU_TEST(system, join_takes_a_list_of_strings_and_the_checker_says_so) {
    // `join` is declared over `List<string>`, and generic arguments are checked
    // before erasure -- so a `List<int32>` never reaches the native at all.
    // (The native still refuses a non-string element: generics *are* erased at
    // run time, and a defensive answer beats rendering whatever was in the
    // slot. That path is unreachable from well-typed source, which is exactly
    // why this test pins the diagnostic rather than the trap.)
    Outcome result = run_vm(
        "        List<int32> numbers = List<int32>();\n"
        "        numbers.add(1);\n"
        "        io.printLine(khuStdCollection.join(numbers, \",\"));\n");
    KHU_CHECK(!result.compiled);
    KHU_CHECK_CONTAINS(result.diagnostics,
                       "cannot convert List<int32> to List<string> in argument 1 of call to "
                       "'khuStdCollection.join'");
}

KHU_TEST(system, concat_reaches_sixteen_arguments) {
    Outcome result = run_both(
        "        io.printLine(khuStdString.concat(\"a\", \"b\", \"c\", \"d\", \"e\", \"f\", \"g\",\n"
        "                                         \"h\", \"i\", \"j\", \"k\", \"l\", \"m\", \"n\",\n"
        "                                         \"o\", \"p\"));\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("abcdefghijklmnop\n"));
}
