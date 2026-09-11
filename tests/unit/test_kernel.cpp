// khuAdvKernel: the shared namespace, the gate, and the buffer/address bridge.
//
// Three things are checked here that no golden can check:
//
//   1. **Both backends, same answers.** Every program runs on KhudraVm and on
//      the emitted C, and the two are compared -- which is what proves the
//      kernel natives did not split the backends.
//   2. **The gate, both halves.** The positive half is this file: compiling
//      with `set_allow_kernel(true)` must be exactly what lets a program past.
//      The negative half lives in tests/integration/errors/kernel_not_allowed,
//      driven by `khudra check` with no flags.
//   3. **The validation layer.** An address carries no extent (PLAN.md,
//      section 5.3), so every entry point that turns a pointer into a machine
//      word or back again is argued for here: length assertions, the wrap trap,
//      and the `dropAddress` that makes a region uncheckable again.
//
// The OS-specific natives do not exist yet -- this is Phase 1 of PLAN.md -- so
// the wrong-OS trap tests arrive with them. The registry itself is asserted
// directly, the way the handle-accounting tests assert on the allocator.
#include "test_harness.h"

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "alloc.h"
}

#include "bytecode/module.h"
#include "compiler.h"
#include "host/native_backend.h"
#include "vm/kernel/kernel.h"
#include "vm/natives.h"
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
                     std::string& diagnostics, bool allow_kernel = false) {
    khu::Compiler compiler;
    std::uint32_t file = compiler.add_buffer(
        "test.khu", "public class KernelTest {\n    func main() {\n" + body + "\n    }\n}\n");
    compiler.set_allow_kernel(allow_kernel);
    bool built = compiler.compile(file, module);
    diagnostics = compiler.diagnostics().render();
    return built;
}

Outcome run_vm(const std::string& body, bool allow_kernel = true,
               const std::vector<std::string>& args = {}) {
    Outcome result;
    khu::bytecode::Module module;
    result.compiled = compile_program(body, module, result.diagnostics, allow_kernel);
    if (!result.compiled) return result;

    khu::vm::Vm vm(module);
    vm.set_output_sink(&result.output);
    vm.set_error_sink(&result.error_output);
    vm.set_program_args(args);
    result.ran = vm.run();
    result.runtime_error = vm.error();
    if (!result.ran) result.exit_code = 1;
    return result;
}

// The same program through the emitted C. Answers false when the machine has
// no host compiler, so a caller reports "not run" rather than failing -- the
// same fallback `khudra run --native` makes.
bool run_native(const std::string& body, Outcome& result,
                const std::vector<std::string>& args = {}) {
    khu::bytecode::Module module;
    result.compiled = compile_program(body, module, result.diagnostics, /*allow_kernel=*/true);
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
// code agree, then hands back the VM's answer for the test's own assertions.
Outcome run_both(const std::string& body, const std::vector<std::string>& args = {}) {
    Outcome vm = run_vm(body, /*allow_kernel=*/true, args);
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

}  // namespace

// ---------------------------------------------------------------------------
// The gate, both halves
// ---------------------------------------------------------------------------

// The positive half: with the grant on, the shared namespace is callable and
// agrees with the host it runs on. The answer is asserted here, behind #if,
// and never in an .expected file (PLAN.md, section 11.1).
KHU_TEST(kernel, platform_agrees_with_the_host) {
#if defined(_WIN32)
    std::string expected = "2\nwindows\n";
#elif defined(__linux__)
    std::string expected = "1\nlinux\n";
#elif defined(__APPLE__)
    std::string expected = "3\nmac\n";
#else
    std::string expected = "0\nunknown\n";
#endif
    Outcome result = run_both("        io.printLine(khuAdvKernel.platform());\n"
                              "        io.printLine(khuAdvKernel.platformName());\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, expected);
}

// The negative half, at the same level of the stack as the positive one: the
// check itself, phrased the way run_error.cmake phrases it. This is the same
// diagnostic the versions/errors/kernel_not_allowed golden pins by driving
// `khudra check` end to end.
KHU_TEST(kernel, a_kernel_call_without_the_grant_is_rejected) {
    khu::bytecode::Module module;
    std::string diagnostics;
    bool built = compile_program("        io.printLine(khuAdvKernel.platform());\n", module,
                                 diagnostics, /*allow_kernel=*/false);
    KHU_CHECK(!built);
    KHU_CHECK(diagnostics.find("'khuAdvKernel.platform' is a direct kernel call, which this "
                               "build did not allow") != std::string::npos);
    KHU_CHECK(diagnostics.find("compile or run it with --allow-kernel; a program that talks to "
                               "the kernel directly is not portable and nothing in the runtime "
                               "checks what it asks for") != std::string::npos);
}

// The shared namespace's five no-argument answers are all portable and all
// syscall-free; the platform branch above covers three of them, and the flag
// values are pinned here so a rename cannot drift behind the docs.
KHU_TEST(kernel, platform_constants) {
    Outcome result = run_vm("        io.printLine(khuAdvKernel.linux());\n"
                            "        io.printLine(khuAdvKernel.windows());\n"
                            "        io.printLine(khuAdvKernel.mac());\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("1\n2\n3\n"));
}

// The error slot is one per process and shared with khuStdSystem, so errno()
// reads what a failed call just left behind.
KHU_TEST(kernel, error_slot_is_shared_with_khuStdSystem) {
    Outcome result = run_both("        *byte f = khuStdSystem.open(\"\", \"w\");\n"
                              "        io.printLine(khuAdvKernel.errno() != 0);\n"
                              "        io.printLine(khuAdvKernel.errorMessage(khuAdvKernel.errno())"
                              " != \"\");\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("true\ntrue\n"));
}

// ---------------------------------------------------------------------------
// The buffer/address bridge
// ---------------------------------------------------------------------------

// toAddress / fromAddress round-trips a real manual block, and the two ends
// agree on the machine word, on both backends.
KHU_TEST(kernel, to_and_from_address_roundtrip_a_manual_block) {
    Outcome result =
        run_both("        *byte block = khuStdMem.alloc(16);\n"
                 "        int64 a = khuAdvKernel.toAddress(block);\n"
                 "        io.printLine(a > 0);\n"
                 "        *byte back = khuAdvKernel.fromAddress(a, 16);\n"
                 "        io.printLine(khuAdvKernel.toAddress(back) == a);\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("true\ntrue\n"));
}

// fromAddress asserts a length, and the assertion is enforced by the *whole*
// runtime: khuStdMem.copy succeeds inside the region and traps past it with the
// existing message. This is the layer that most needs proving.
KHU_TEST(kernel, a_kernel_region_checks_like_a_buffer) {
    Outcome result =
        run_vm("        *byte block = khuStdMem.alloc(16);\n"
               "        *byte region = khuAdvKernel.fromAddress(khuAdvKernel.toAddress(block), 16);\n"
               "        *byte dest = khuStdMem.alloc(16);\n"
               "        khuStdMem.copy(dest, region, 16);\n"
               "        io.printLine(\"copied\");\n"
               "        khuStdMem.copy(dest, region, 17);\n"
               "        io.printLine(\"unreachable\");\n");
    // The inside step ran and printed; the past-the-end step trapped, which is
    // the assertion. A trap ends the run, so `ran` is false.
    KHU_CHECK(!result.ran);
    KHU_CHECK_EQ(result.output, std::string("copied\n"));
    KHU_CHECK(result.runtime_error.find("khuStdMem.copy of 17 bytes runs past the end of a "
                                        "16-byte buffer") != std::string::npos);
}

// An existing native that knew nothing about kernel regions gains them for
// free through the pointer_of fallback: a fromAddress'd buffer satisfies the
// same check_range a manual block does.
KHU_TEST(kernel, an_existing_native_gains_kernel_regions_for_free) {
    Outcome result =
        run_vm("        *byte block = khuStdMem.alloc(16);\n"
               "        *byte region = khuAdvKernel.fromAddress(khuAdvKernel.toAddress(block), 16);\n"
               "        khuStdRandom.nextBytes(region, 16);\n"
               "        io.printLine(\"filled\");\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("filled\n"));
}

// A dropped region is an unregistered foreign pointer, and the manual
// allocator still refuses to release it -- the regression guard proving the
// fallback weakened nothing.
KHU_TEST(kernel, a_dropped_region_is_still_not_releasable) {
    // A deliberately foreign address: never handed out by the manual
    // allocator, so the only knowledge a runtime could ever have of it is the
    // region a fromAddress asserted. After the drop there is no such knowledge
    // left, and release must refuse the way it always refused foreign memory.
    Outcome result =
        run_vm("        *byte region = khuAdvKernel.fromAddress(98765432, 16);\n"
               "        khuAdvKernel.dropAddress(region);\n"
               "        khuStdMem.release(region);\n"
               "        io.printLine(\"unreachable\");\n");
    KHU_CHECK(!result.ran);
    KHU_CHECK_EQ(result.output, std::string(""));
    KHU_CHECK(result.runtime_error.find(
                  "khuStdMem.release of a buffer this runtime did not allocate, or of one that "
                  "has already been released") != std::string::npos);
}

// Address 0 is null, consistent with khuStdMem.isNull.
KHU_TEST(kernel, null_address_is_zero_and_back) {
    Outcome result = run_both("        io.printLine(khuAdvKernel.toAddress(null) == 0);\n"
                              "        io.printLine(khuStdMem.isNull(khuAdvKernel.fromAddress(0, 4)"
                              "));\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("true\ntrue\n"));
}

// The length assertion: a non-positive length means there is no buffer to talk
// about, so the raw bridge refuses to invent one.
KHU_TEST(kernel, from_address_rejects_a_nonpositive_length) {
    Outcome zero = run_vm("        *byte block = khuStdMem.alloc(4);\n"
                              "        khuAdvKernel.fromAddress(khuAdvKernel.toAddress(block), 0);\n"
                              "        io.printLine(\"unreachable\");\n");
    KHU_CHECK(!zero.ran);
    KHU_CHECK(zero.runtime_error.find("khuAdvKernel.fromAddress requires a positive length") !=
              std::string::npos);

    Outcome negative = run_vm("        *byte block = khuStdMem.alloc(4);\n"
                              "        khuAdvKernel.fromAddress(khuAdvKernel.toAddress(block), -1);\n"
                              "        io.printLine(\"unreachable\");\n");
    KHU_CHECK(!negative.ran);
    KHU_CHECK(negative.runtime_error.find("khuAdvKernel.fromAddress requires a positive length") !=
              std::string::npos);
}

// An address near the top of the machine word plus a length overflows it; a
// buffer that cannot exist must not be invented.
KHU_TEST(kernel, from_address_rejects_an_address_plus_length_that_wraps) {
    Outcome result = run_vm("        khuAdvKernel.fromAddress(khuStdMath.neg(1), 5);\n"
                            "        io.printLine(\"unreachable\");\n");
    KHU_CHECK(!result.ran);
    KHU_CHECK(result.runtime_error.find(
                  "khuAdvKernel.fromAddress was given an address + length that wraps around") !=
              std::string::npos);
}

// Registering a region and dropping it are the two directions of the same
// assertion, so the round trip has to bring the pointer back to "length
// unknown" for a check that matters.
KHU_TEST(kernel, drop_address_makes_a_region_uncheckable_again) {
    // sizeOf reads only the runtime's knowledge of the block -- no
    // dereference -- so it is the safe way to watch a region's length come and
    // go: 16 while the assertion stands, 0 once dropAddress forgets it.
    Outcome result =
        run_vm("        *byte region = khuAdvKernel.fromAddress(98765432, 16);\n"
               "        io.printLine(khuStdMem.sizeOf(region) == 16);\n"
               "        khuAdvKernel.dropAddress(region);\n"
               "        io.printLine(khuStdMem.sizeOf(region) == 0);\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("true\ntrue\n"));
}

// The registry holds no OS resource and takes no part in the manual
// accounting: a kernel region is not a manual block, so liveBytes/liveBlocks
// say nothing about it (PLAN.md, section 9.2).
KHU_TEST(kernel, kernel_regions_do_not_touch_manual_accounting) {
    // Three checkpoints: the manual baseline, the full block cost, and back to
    // the baseline. The region appears twice around the full cost, and both
    // times the accounting does not move an inch (PLAN.md, section 9.2).
    Outcome result =
        run_vm("        int64 base = khuStdMem.liveBytes();\n"
               "        *byte block = khuStdMem.alloc(16);\n"
               "        int64 with_block = khuStdMem.liveBytes();\n"
               "        io.printLine(with_block - base >= 16);\n"
               "        *byte region = khuAdvKernel.fromAddress(khuAdvKernel.toAddress(block), 16);\n"
               "        io.printLine(khuStdMem.liveBytes() == with_block);\n"
               "        khuAdvKernel.dropAddress(region);\n"
               "        io.printLine(khuStdMem.liveBytes() == with_block);\n"
               "        khuStdMem.release(block);\n"
               "        io.printLine(khuStdMem.liveBytes() == base);\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("true\ntrue\ntrue\ntrue\n"));
}

// ---------------------------------------------------------------------------
// The registry, asserted directly
// ---------------------------------------------------------------------------

// An interior pointer into a region reports the remaining length, so pointer
// arithmetic still checks: from base+40 of a 100-byte region, 60 bytes remain.
// This is the `extent_of` contract the pointer_of fallback leans on.
KHU_TEST(kernel, an_interior_pointer_reports_the_remaining_length) {
    khu::vm::kernel::KernelServices services;
    char storage[100];
    services.record(storage, 100, false);
    KHU_CHECK_EQ(services.extent_of(
                     static_cast<const void*>(static_cast<unsigned char*>(reinterpret_cast<void*>(
                         storage)) + 40)),
                 60u);
    KHU_CHECK_EQ(services.extent_of(storage + 99), 1u);
    KHU_CHECK_EQ(services.extent_of(storage + 100), 0u);
}

// A region outside range answers nothing, and find_base answers the whole
// region rather than panicking on an interior address.
KHU_TEST(kernel, a_region_answers_exactly_what_it_knows) {
    khu::vm::kernel::KernelServices services;
    char storage[64];
    services.record(storage, 64, false);
    const auto* region = services.find_base(storage);
    KHU_CHECK(region != nullptr);
    KHU_CHECK_EQ(region->base,
                 reinterpret_cast<std::uint64_t>(storage));
    KHU_CHECK_EQ(region->length, 64u);
    KHU_CHECK(services.find_base(static_cast<void*>(&storage[32])) == nullptr);
    KHU_CHECK(services.extent_of(static_cast<void*>(&storage[65])) == 0u);
    services.forget(storage);
    KHU_CHECK(services.find_base(storage) == nullptr);
}