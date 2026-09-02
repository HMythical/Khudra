// The one place a platform difference is allowed to be written down.
//
// `khuStdSystem` is a *portable* library: every member behaves the same way on
// Linux and on Windows (EXPANSION-PLAN.md, section 2.6). That is only possible
// if the natives themselves are written once, so every `#if defined(_WIN32)`
// the library needs lives here and nowhere else -- the same shape the shared
// natives already use for the clock (`natives.cpp`, `gmtime_s` vs `gmtime_r`).
//
// Three rules give the shim its shape:
//
//   1. **The file layer is C `stdio`, not POSIX I/O.** `fopen`/`fread`/`fseek`
//      behave identically on both platforms and are already the house style
//      (`src/util/file.cpp`). Positioning goes through the 64-bit variants
//      (`fseeko`/`ftello` vs `_fseeki64`/`_ftelli64`) so a seek is not 2 GB
//      capped, and the length of an open stream is `fseek(END)` + `tell`,
//      restored -- not `fstat`.
//   2. **There is one "last OS error" slot, and it is not `errno`.** Winsock
//      reports failure through `WSAGetLastError` and never touches `errno`, so
//      every entry point here records its own failure in a slot the
//      `khuStdSystem.errno()` native reads back. The number is
//      platform-specific by definition; `error_message` is its readable form.
//   3. **Strings are byte strings.** The narrow CRT surface (`fopen`, `_mkdir`,
//      `getenv`) is what v1 uses on both platforms. Wide/UTF-8 paths are an
//      explicit non-goal (EXPANSION-PLAN.md, section 9).
#ifndef KHU_VM_PLATFORM_H
#define KHU_VM_PLATFORM_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace khu::vm::platform {

// --- the last-OS-error slot ------------------------------------------------

// The code `khuStdSystem.errno()` answers. Per process, like `errno` itself.
int last_error();
void set_last_error(int code);
// Records the C library's `errno`. The file and path layers call this; the
// socket layer records `WSAGetLastError` instead, which is the whole reason
// the slot exists.
void capture_errno();
void clear_last_error();
// `strerror(3)`, or the Win32/Winsock formatted message for a socket code.
std::string error_message(int code);

// --- files, over C stdio ---------------------------------------------------

// `fopen(3)`. Answers null and records the error when it fails. Modes are
// C's -- "r", "w", "a", "r+" -- and every one of them is byte-oriented, so a
// program reads the same bytes on both platforms.
std::FILE* file_open(const std::string& path, const std::string& mode);
// `fseeko`/`_fseeki64`. `origin` is 0 = SET, 1 = CUR, 2 = END, which is
// numerically what SEEK_SET/SEEK_CUR/SEEK_END are on both platforms.
bool file_seek(std::FILE* stream, std::int64_t offset, int origin);
// `ftello`/`_ftelli64`, or -1.
std::int64_t file_tell(std::FILE* stream);
// The stream's current length, by seeking to the end and back. -1 on failure.
std::int64_t file_length(std::FILE* stream);

// --- files and directories by path ----------------------------------------

bool path_exists(const std::string& path);
bool path_is_file(const std::string& path);
bool path_is_directory(const std::string& path);
// The file's length in bytes, or 0 with the error slot set when it cannot be
// read -- the sentinel convention the rest of the namespace uses.
std::int64_t path_length(const std::string& path);
bool delete_file(const std::string& path);
bool rename_path(const std::string& from, const std::string& to);
// No mode argument: `_mkdir` has none and `File.mkdir()` has none, so the
// signature is the least common denominator (EXPANSION-PLAN.md, section 2.4).
bool create_directory(const std::string& path);
bool remove_directory(const std::string& path);
std::string current_directory();
bool change_directory(const std::string& path);

// `java.io.File.separator` and `File.pathSeparator`: "/" and ":" on POSIX,
// "\\" and ";" on Windows. Building a path portably needs both.
const char* path_separator();
const char* path_list_separator();

// --- the environment -------------------------------------------------------

// True when `name` is set, whatever its value; `out` receives the value. The
// bool is what tells "set to empty" apart from "not set at all".
bool get_env(const std::string& name, std::string& out);
// Every name in the environment, in the order the C library holds them.
std::vector<std::string> env_keys();

// --- sockets ---------------------------------------------------------------

// A descriptor wide enough for a POSIX `int fd` and for a Winsock `SOCKET`
// (a 64-bit `UINT_PTR`), which is why the handle registry stores an int64.
using Socket = std::int64_t;
constexpr Socket kInvalidSocket = -1;

// `WSAStartup` on the first call and nothing at all on POSIX. Every socket
// entry point below calls it, so a program never sees it.
bool socket_startup();
// `WSACleanup`, once. Called when the program exits.
void socket_cleanup();

// A bound, listening socket. `host` empty means "every interface"; a `port` of
// 0 asks the OS to choose one, which `socket_local_port` then reports.
Socket socket_listen(const std::string& host, int port, int backlog);
Socket socket_accept(Socket listener);
Socket socket_connect(const std::string& host, int port);
// Bytes moved, or -1. `socket_recv` answers 0 at end of stream.
std::int64_t socket_send(Socket socket, const void* data, std::size_t count);
std::int64_t socket_recv(Socket socket, void* buffer, std::size_t count);
bool socket_close(Socket socket);
// `how`: 0 = further reads, 1 = further writes, 2 = both. The numbers are
// SHUT_RD/SHUT_WR/SHUT_RDWR on POSIX and SD_RECEIVE/SD_SEND/SD_BOTH on
// Windows, which happen to agree, so the native takes them straight through.
bool socket_shutdown(Socket socket, int how);
bool socket_peer(Socket socket, std::string& address, int& port);
bool socket_local(Socket socket, std::string& address, int& port);
// SO_RCVTIMEO. The one timeout in an otherwise entirely blocking surface.
bool socket_set_read_timeout(Socket socket, int millis);
// `getaddrinfo(3)`: the first address `name` resolves to, or "" on failure.
bool resolve_host(const std::string& name, std::string& address);

}  // namespace khu::vm::platform

#endif  // KHU_VM_PLATFORM_H
