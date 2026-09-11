// khuAdvKernelLinux -- the Linux half of the kernel tier.
//
// On Linux this file provides the raw `syscall(2)` gate and the name-to-number
// lookup. On every other host it provides a wrong-OS trap for every member --
// the only correct answer when a Linux-only namespace is called from a program
// running on a different OS (PLAN.md, section 7).
//
// The `#if defined(__linux__)` / `#else` shape is per-file, not centralized:
// each translation unit carries its own stub, so there is exactly one
// definition of every symbol on every platform with no ODR or link-order
// question (PLAN.md, section 9.3).
//
// `number()` is a hand-maintained chain of `SYS_*` macros. The macros cannot
// be enumerated at compile time, so the map is necessarily hand-written. Say
// so rather than implying completeness.
#include "vm/kernel/kernel.h"

#include "bytecode/native.h"
#include "vm/natives.h"
#include "vm/value.h"

// The Linux-only headers live at global scope and only on Linux: <sys/syscall.h>
// and <unistd.h> do not exist on the other hosts, and a standard header must
// never be parsed inside a namespace.
#if defined(__linux__)
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace khu::vm::kernel {

#if defined(__linux__)

// The Linux syscall ABI takes at most six arguments beyond the number.  The
// seven overloads (arities 1-7) cover 0..6 trailing arguments; the raw gate
// passes them straight through with no validation (PLAN.md, section 5.1).
static std::int64_t linux_invoke(std::int64_t number, std::int64_t a0 = 0,
                                 std::int64_t a1 = 0, std::int64_t a2 = 0,
                                 std::int64_t a3 = 0, std::int64_t a4 = 0,
                                 std::int64_t a5 = 0) {
    errno = 0;
    return static_cast<std::int64_t>(
        ::syscall(static_cast<long>(number), a0, a1, a2, a3, a4, a5));
}

// A hand-maintained name-to-number map. The SYS_* macros are compiler-defined
// constants, so there is no way to enumerate them; this table is necessarily
// hand-written and intentionally incomplete. An unrecognised name answers -1,
// using the sentinel convention because the name is input, not a program error
// (PLAN.md, section 6.2). Each row is also guarded against the host
// architecture: a syscall that does not exist here (SYS_open is absent on
// aarch64, where openat is the only open) answers -1 exactly like an unknown
// name, so one table compiles everywhere.
static std::int64_t syscall_number(std::string_view name) {
    // file I/O
#ifdef SYS_read
    if (name == "read") return SYS_read;
#endif
#ifdef SYS_write
    if (name == "write") return SYS_write;
#endif
#ifdef SYS_open
    if (name == "open") return SYS_open;
#endif
#ifdef SYS_openat
    if (name == "openat") return SYS_openat;
#endif
#ifdef SYS_close
    if (name == "close") return SYS_close;
#endif
#ifdef SYS_lseek
    if (name == "lseek") return SYS_lseek;
#endif
#ifdef SYS_ioctl
    if (name == "ioctl") return SYS_ioctl;
#endif
    // memory
#ifdef SYS_mmap
    if (name == "mmap") return SYS_mmap;
#endif
#ifdef SYS_munmap
    if (name == "munmap") return SYS_munmap;
#endif
#ifdef SYS_mprotect
    if (name == "mprotect") return SYS_mprotect;
#endif
    // process
#ifdef SYS_exit
    if (name == "exit") return SYS_exit;
#endif
#ifdef SYS_exit_group
    if (name == "exit_group") return SYS_exit_group;
#endif
#ifdef SYS_getpid
    if (name == "getpid") return SYS_getpid;
#endif
#ifdef SYS_gettid
    if (name == "gettid") return SYS_gettid;
#endif
    // pipe / fork / exec
#ifdef SYS_pipe
    if (name == "pipe") return SYS_pipe;
#endif
#ifdef SYS_pipe2
    if (name == "pipe2") return SYS_pipe2;
#endif
#ifdef SYS_fork
    if (name == "fork") return SYS_fork;
#endif
#ifdef SYS_execve
    if (name == "execve") return SYS_execve;
#endif
    // signals and cwd
#ifdef SYS_kill
    if (name == "kill") return SYS_kill;
#endif
#ifdef SYS_getcwd
    if (name == "getcwd") return SYS_getcwd;
#endif
#ifdef SYS_chdir
    if (name == "chdir") return SYS_chdir;
#endif
    // stat
#ifdef SYS_stat
    if (name == "stat") return SYS_stat;
#endif
#ifdef SYS_fstat
    if (name == "fstat") return SYS_fstat;
#endif
#ifdef SYS_lstat
    if (name == "lstat") return SYS_lstat;
#endif
    // directory
#ifdef SYS_getdents64
    if (name == "getdents64") return SYS_getdents64;
#endif
    // socket (subset)
#ifdef SYS_socket
    if (name == "socket") return SYS_socket;
#endif
#ifdef SYS_bind
    if (name == "bind") return SYS_bind;
#endif
#ifdef SYS_listen
    if (name == "listen") return SYS_listen;
#endif
#ifdef SYS_accept
    if (name == "accept") return SYS_accept;
#endif
#ifdef SYS_connect
    if (name == "connect") return SYS_connect;
#endif
#ifdef SYS_sendto
    if (name == "sendto") return SYS_sendto;
#endif
#ifdef SYS_recvfrom
    if (name == "recvfrom") return SYS_recvfrom;
#endif
#ifdef SYS_close_range
    if (name == "close_range") return SYS_close_range;
#endif
    return -1;
}

NativeOutcome linux_invoke_1(NativeServices& services, const Value* argv,
                                    std::uint32_t argc) {
    (void)services;
    (void)argc;
    return NativeOutcome::produced(
        Value::make_int(TypeTag::Int64, linux_invoke(argv[0].as_int)));
}

NativeOutcome linux_invoke_2(NativeServices& services, const Value* argv,
                                    std::uint32_t argc) {
    (void)services;
    (void)argc;
    return NativeOutcome::produced(
        Value::make_int(TypeTag::Int64,
                        linux_invoke(argv[0].as_int, argv[1].as_int)));
}

NativeOutcome linux_invoke_3(NativeServices& services, const Value* argv,
                                    std::uint32_t argc) {
    (void)services;
    (void)argc;
    return NativeOutcome::produced(
        Value::make_int(TypeTag::Int64,
                        linux_invoke(argv[0].as_int, argv[1].as_int,
                                     argv[2].as_int)));
}

NativeOutcome linux_invoke_4(NativeServices& services, const Value* argv,
                                    std::uint32_t argc) {
    (void)services;
    (void)argc;
    return NativeOutcome::produced(
        Value::make_int(TypeTag::Int64,
                        linux_invoke(argv[0].as_int, argv[1].as_int,
                                     argv[2].as_int, argv[3].as_int)));
}

NativeOutcome linux_invoke_5(NativeServices& services, const Value* argv,
                                    std::uint32_t argc) {
    (void)services;
    (void)argc;
    return NativeOutcome::produced(
        Value::make_int(TypeTag::Int64,
                        linux_invoke(argv[0].as_int, argv[1].as_int,
                                     argv[2].as_int, argv[3].as_int,
                                     argv[4].as_int)));
}

NativeOutcome linux_invoke_6(NativeServices& services, const Value* argv,
                                    std::uint32_t argc) {
    (void)services;
    (void)argc;
    return NativeOutcome::produced(
        Value::make_int(TypeTag::Int64,
                        linux_invoke(argv[0].as_int, argv[1].as_int,
                                     argv[2].as_int, argv[3].as_int,
                                     argv[4].as_int, argv[5].as_int)));
}

NativeOutcome linux_invoke_7(NativeServices& services, const Value* argv,
                                    std::uint32_t argc) {
    (void)services;
    (void)argc;
    return NativeOutcome::produced(
        Value::make_int(TypeTag::Int64,
                        linux_invoke(argv[0].as_int, argv[1].as_int,
                                     argv[2].as_int, argv[3].as_int,
                                     argv[4].as_int, argv[5].as_int,
                                     argv[6].as_int)));
}

NativeOutcome linux_number(NativeServices& services,
                                       const std::string* name) {
    std::int64_t n = syscall_number(*name);
    return NativeOutcome::produced(Value::make_int(TypeTag::Int64, n));
}

#else  // not Linux -- wrong-OS trap for every member

// Every Linux-only member traps here when called on a non-Linux host. The
// message tells the programmer which platform guard is missing (PLAN.md,
// section 7).
static NativeOutcome wrong_os(int id) {
    const char* member = "?";
    switch (id) {
        case 0: member = "invoke"; break;
        case 1: member = "number"; break;
    }
    return NativeOutcome::failed(
        wrong_os_message(std::string("khuAdvKernelLinux.") + member, "Linux"));
}

NativeOutcome linux_invoke_1(NativeServices& s, const Value* a,
                                    std::uint32_t c) {
    (void)a; (void)c; return wrong_os(0);
}
NativeOutcome linux_invoke_2(NativeServices& s, const Value* a,
                                    std::uint32_t c) {
    (void)a; (void)c; return wrong_os(0);
}
NativeOutcome linux_invoke_3(NativeServices& s, const Value* a,
                                    std::uint32_t c) {
    (void)a; (void)c; return wrong_os(0);
}
NativeOutcome linux_invoke_4(NativeServices& s, const Value* a,
                                    std::uint32_t c) {
    (void)a; (void)c; return wrong_os(0);
}
NativeOutcome linux_invoke_5(NativeServices& s, const Value* a,
                                    std::uint32_t c) {
    (void)a; (void)c; return wrong_os(0);
}
NativeOutcome linux_invoke_6(NativeServices& s, const Value* a,
                                    std::uint32_t c) {
    (void)a; (void)c; return wrong_os(0);
}
NativeOutcome linux_invoke_7(NativeServices& s, const Value* a,
                                    std::uint32_t c) {
    (void)a; (void)c; return wrong_os(0);
}
NativeOutcome linux_number(NativeServices& s,
                                       const std::string* name) {
    (void)name; return wrong_os(1);
}

#endif  // defined(__linux__)

}  // namespace khu::vm::kernel
