// The kernel tier's shared implementation: what khuAdvKernel on every host
// has in common. The per-OS syscalls live one file apart -- kernel_linux.cpp,
// kernel_windows.cpp, kernel_mac.cpp -- each carrying its own `#else` stub for
// the wrong-OS trap.
//
// ## A deliberate departure from platform.h's rule
//
// `src/vm/platform.h` calls itself "the one place a platform difference is
// allowed to be written down", and that is true for khuStdSystem, whose job is
// to be *portable*. khuAdvKernel's job is the opposite: to expose
// *non-portability*, and it is built on the fact that Linux, Windows and
// macOS syscalls are not interchangeable -- a fact surfaced in the namespace
// the programmer types rather than papered over. So the kernel tier
// deliberately keeps its `#if`s out of platform.cpp and in this directory,
// one translation unit per OS. That is a decision, and it is stated here so
// the departure reads as one rather than a lapse.
//
// Nothing in this header branches. The per-OS units are the only place a
// platform difference is written down for this library.
#ifndef KHU_VM_KERNEL_KERNEL_H
#define KHU_VM_KERNEL_KERNEL_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vm/value.h"

namespace khu::vm {

class NativeServices;
struct NativeOutcome;

namespace kernel {

// One span of memory whose length this runtime knows. `owned_mapping` is true
// for something mmap handed us and false for a length the program asserted
// through `fromAddress`; the distinction is what lets `munmap` refuse a region
// it never mapped (PLAN.md, section 9.2).
struct KernelRegion {
    std::uint64_t base = 0;
    std::uint64_t length = 0;
    bool owned_mapping = false;
};

// The per-run registry of known-extent regions.
//
// This is NOT an fd registry, and the difference is the whole justification:
// an fd has no length, so registering one would manufacture leak accounting
// with nothing to check; a mapping *has* a length, which is precisely the fact
// that makes checking possible. The registry therefore holds no OS resource.
// It is discarded at end of run without unmapping anything, because process
// exit already does that, and a surviving fd stays deliberate (`fork` / `exec`
// inheritance). Consequently `khuStdMem.liveBytes()` / `liveBlocks()`
// returning to zero stays true but stops being complete: kernel regions are
// not manual blocks and never appear in that accounting (PLAN.md, section 9.2).
struct KernelServices {
    // A program holds a handful of mappings at a time, so a flat array scanned
    // linearly is both the simplest and the fastest thing here -- the same
    // reasoning SystemServices::handles records.
    std::vector<KernelRegion> regions;

    void record(void* base, std::uint64_t length, bool owned);
    void forget(const void* base);
    // The extent from `address` to the end of the region holding it, or 0 when
    // no region does. An interior address answers the remaining length, so
    // pointer arithmetic still checks.
    std::uint64_t extent_of(const void* address) const;
    const KernelRegion* find_base(const void* base) const;
};

// The host this build runs on, as khuAdvKernel's constants: 1 = linux,
// 2 = windows, 3 = mac, 0 = a host no tier knows. Compile-time -- there is
// one answer per build, which is the expressible truth here.
std::int32_t platform();

// "linux", "windows" or "mac" -- the readable form of `platform()`.
std::string platform_name();

// The buffer/address bridge (khuAdvKernel.toAddress / fromAddress /
// dropAddress). The validations live here because they are exactly the
// boundary doctrine: an address carries no extent, so every entry point that
// turns a pointer into a machine word or back again says so on its own terms
// (PLAN.md, section 5.3).
NativeOutcome to_address(const Value& buffer);
NativeOutcome from_address(const Value& address, const Value& length,
                           NativeServices& services);
NativeOutcome drop_address(const Value& region, NativeServices& services);

// Builds the fatal-trap message a wrong-OS call produces: "khuAdvKernelWindows
// .writeFile is a Windows call, but this program is running on Linux; guard it
// with khuAdvKernel.platform() == khuAdvKernel.windows()". `expected` is the
// host the member was written for, `where` is the full member name.
std::string wrong_os_message(std::string_view where, std::string_view expected);

}  // namespace khu::vm::kernel

}  // namespace khu::vm

#endif  // KHU_VM_KERNEL_KERNEL_H