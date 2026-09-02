#include "vm/platform.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
// winsock2.h has to come before windows.h, and ws2tcpip.h after it.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
// The environment block. POSIX declares it in <unistd.h> only under _GNU_SOURCE
// on some libcs, so it is named here where every platform branch can see it.
extern char** environ;
#endif

namespace khu::vm::platform {
namespace {

// The OS's own socket type. A POSIX `int fd` and a Winsock `SOCKET` (a 64-bit
// UINT_PTR) both fit in the int64 the handle registry stores, so every entry
// point below converts once, here, instead of at each call.
#if defined(_WIN32)
using RawSocket = SOCKET;
#else
using RawSocket = int;
#endif

RawSocket raw_of(Socket socket) { return static_cast<RawSocket>(socket); }

// What ::socket and ::accept answer on failure: -1 on POSIX, INVALID_SOCKET
// (an unsigned all-ones value) on Windows. One predicate so no caller has to
// remember which.
bool valid(Socket socket) {
#if defined(_WIN32)
    return static_cast<RawSocket>(socket) != INVALID_SOCKET;
#else
    return socket >= 0;
#endif
}

// The slot rule 2 in the header describes. A plain function-local static, not a
// thread-local: the whole library is single-threaded by design
// (EXPANSION-PLAN.md, section 9 -- threads are a non-goal), and a per-process
// slot is what makes `errno()` answer the same number under the VM and under
// the native backend.
int& error_slot() {
    static int slot = 0;
    return slot;
}

#if defined(_WIN32)
// Winsock's failure channel. Recorded into the same slot the file layer uses,
// so `errno()` is defined for a socket call too.
void capture_socket_error() { error_slot() = ::WSAGetLastError(); }

bool& winsock_started() {
    static bool started = false;
    return started;
}
#else
void capture_socket_error() { error_slot() = errno; }
#endif

// The three origins, spelled the same on both platforms but never assumed to
// be: a bad origin is refused rather than passed through.
bool origin_of(int origin, int& out) {
    switch (origin) {
        case 0: out = SEEK_SET; return true;
        case 1: out = SEEK_CUR; return true;
        case 2: out = SEEK_END; return true;
        default: return false;
    }
}

// --- socket address plumbing ----------------------------------------------
//
// Everything goes through `getaddrinfo`, so the same code covers IPv4 and IPv6
// and there is no address parsing to get wrong per platform.

void close_socket(Socket socket) {
#if defined(_WIN32)
    ::closesocket(raw_of(socket));
#else
    ::close(raw_of(socket));
#endif
}

// The numeric host and port behind a filled-in `sockaddr`.
bool describe(const sockaddr* address, socklen_t length, std::string& host, int& port) {
    char host_text[NI_MAXHOST];
    char port_text[NI_MAXSERV];
    if (::getnameinfo(address, length, host_text, sizeof(host_text), port_text,
                      sizeof(port_text), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        capture_socket_error();
        return false;
    }
    host = host_text;
    port = std::atoi(port_text);
    return true;
}

struct AddressList {
    addrinfo* head = nullptr;
    ~AddressList() {
        if (head) ::freeaddrinfo(head);
    }
};

bool lookup(const std::string& host, int port, bool passive, AddressList& out) {
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (passive) hints.ai_flags = AI_PASSIVE;

    char service[16];
    std::snprintf(service, sizeof(service), "%d", port);
    const char* node = host.empty() ? nullptr : host.c_str();

    int status = ::getaddrinfo(node, service, &hints, &out.head);
    if (status != 0) {
        // getaddrinfo has its own code space. Recording the platform's own
        // resolver error keeps `errno()` meaningful for a name that does not
        // resolve, which is the failure a program actually hits here.
#if defined(_WIN32)
        error_slot() = status;
#else
        error_slot() = EHOSTUNREACH;
#endif
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// The error slot
// ---------------------------------------------------------------------------

int last_error() { return error_slot(); }
void set_last_error(int code) { error_slot() = code; }
void capture_errno() { error_slot() = errno; }
void clear_last_error() { error_slot() = 0; }

std::string error_message(int code) {
    if (code == 0) return "no error";
#if defined(_WIN32)
    // One formatter covers both spaces: Winsock's codes live in the same
    // system message table Win32's do.
    char* text = nullptr;
    DWORD length = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<char*>(&text), 0, nullptr);
    if (length == 0 || !text) {
        if (text) ::LocalFree(text);
        // Fall back to the CRT's table: a code the system message table does
        // not know may still be a plain `errno` from the file layer.
        return std::strerror(code);
    }
    std::string message(text, length);
    ::LocalFree(text);
    // FormatMessage ends its lines; a message that is one line of text is
    // easier to concatenate into a program's own output.
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' ||
                                message.back() == ' ' || message.back() == '.')) {
        message.pop_back();
    }
    return message;
#else
    return std::strerror(code);
#endif
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

std::FILE* file_open(const std::string& path, const std::string& mode) {
    // Every mode is byte-oriented. On Windows a text-mode stream would have the
    // CRT translating CRLF underneath it, and the same file would read as
    // different bytes on the two platforms -- which is precisely what this
    // library promises cannot happen. On POSIX the "b" is a no-op, so the one
    // fixup is written once rather than branched on.
    std::string effective = mode;
    if (effective.find('b') == std::string::npos) effective += "b";
    std::FILE* stream = std::fopen(path.c_str(), effective.c_str());
    if (!stream) capture_errno();
    return stream;
}

bool file_seek(std::FILE* stream, std::int64_t offset, int origin) {
    int whence = 0;
    if (!stream || !origin_of(origin, whence)) {
        error_slot() = EINVAL;
        return false;
    }
#if defined(_WIN32)
    if (::_fseeki64(stream, offset, whence) != 0) {
#else
    if (::fseeko(stream, static_cast<off_t>(offset), whence) != 0) {
#endif
        capture_errno();
        return false;
    }
    return true;
}

std::int64_t file_tell(std::FILE* stream) {
    if (!stream) {
        error_slot() = EINVAL;
        return -1;
    }
#if defined(_WIN32)
    std::int64_t where = ::_ftelli64(stream);
#else
    auto where = static_cast<std::int64_t>(::ftello(stream));
#endif
    if (where < 0) capture_errno();
    return where;
}

std::int64_t file_length(std::FILE* stream) {
    // fseek(END) + tell, restored -- rather than fstat, which would drag in a
    // second (and differently spelled) descriptor model on Windows for no gain.
    std::int64_t here = file_tell(stream);
    if (here < 0) return -1;
    if (!file_seek(stream, 0, 2)) return -1;
    std::int64_t length = file_tell(stream);
    if (!file_seek(stream, here, 0)) return -1;
    return length;
}

// ---------------------------------------------------------------------------
// Files and directories by path
// ---------------------------------------------------------------------------

namespace {

#if defined(_WIN32)
using StatBuffer = struct _stat64;
int stat_path(const std::string& path, StatBuffer& out) { return ::_stat64(path.c_str(), &out); }
constexpr unsigned short kDirectoryBit = _S_IFDIR;
constexpr unsigned short kFileBit = _S_IFREG;
unsigned short mode_of(const StatBuffer& info) { return info.st_mode; }
#else
using StatBuffer = struct stat;
int stat_path(const std::string& path, StatBuffer& out) { return ::stat(path.c_str(), &out); }
constexpr mode_t kDirectoryBit = S_IFDIR;
constexpr mode_t kFileBit = S_IFREG;
mode_t mode_of(const StatBuffer& info) { return info.st_mode; }
#endif

bool stat_of(const std::string& path, StatBuffer& out) {
    if (stat_path(path, out) != 0) {
        capture_errno();
        return false;
    }
    return true;
}

}  // namespace

bool path_exists(const std::string& path) {
    StatBuffer info;
    return stat_of(path, info);
}

bool path_is_file(const std::string& path) {
    StatBuffer info;
    if (!stat_of(path, info)) return false;
    return (mode_of(info) & S_IFMT) == kFileBit;
}

bool path_is_directory(const std::string& path) {
    StatBuffer info;
    if (!stat_of(path, info)) return false;
    return (mode_of(info) & S_IFMT) == kDirectoryBit;
}

std::int64_t path_length(const std::string& path) {
    StatBuffer info;
    if (!stat_of(path, info)) return 0;
    return static_cast<std::int64_t>(info.st_size);
}

bool delete_file(const std::string& path) {
    if (std::remove(path.c_str()) != 0) {
        capture_errno();
        return false;
    }
    return true;
}

bool rename_path(const std::string& from, const std::string& to) {
    // Windows `rename` refuses to replace an existing target where POSIX
    // silently would. That difference is *reported* rather than papered over:
    // the call answers false and sets the error slot, and a program that wants
    // Java's semantics decides for itself (EXPANSION-PLAN.md, section 2.4).
    if (std::rename(from.c_str(), to.c_str()) != 0) {
        capture_errno();
        return false;
    }
    return true;
}

bool create_directory(const std::string& path) {
#if defined(_WIN32)
    if (::_mkdir(path.c_str()) != 0) {
#else
    if (::mkdir(path.c_str(), 0777) != 0) {
#endif
        capture_errno();
        return false;
    }
    return true;
}

bool remove_directory(const std::string& path) {
#if defined(_WIN32)
    if (::_rmdir(path.c_str()) != 0) {
#else
    if (::rmdir(path.c_str()) != 0) {
#endif
        capture_errno();
        return false;
    }
    return true;
}

std::string current_directory() {
    char scratch[4096];
#if defined(_WIN32)
    if (!::_getcwd(scratch, static_cast<int>(sizeof(scratch)))) {
#else
    if (!::getcwd(scratch, sizeof(scratch))) {
#endif
        capture_errno();
        return std::string();
    }
    return scratch;
}

bool change_directory(const std::string& path) {
#if defined(_WIN32)
    if (::_chdir(path.c_str()) != 0) {
#else
    if (::chdir(path.c_str()) != 0) {
#endif
        capture_errno();
        return false;
    }
    return true;
}

const char* path_separator() {
#if defined(_WIN32)
    return "\\";
#else
    return "/";
#endif
}

const char* path_list_separator() {
#if defined(_WIN32)
    return ";";
#else
    return ":";
#endif
}

// ---------------------------------------------------------------------------
// The environment
// ---------------------------------------------------------------------------

bool get_env(const std::string& name, std::string& out) {
    out.clear();
    const char* value = std::getenv(name.c_str());
    if (!value) return false;
    out = value;
    return true;
}

std::vector<std::string> env_keys() {
    std::vector<std::string> keys;
#if defined(_WIN32)
    char** entries = _environ;
#else
    char** entries = environ;
#endif
    for (char** entry = entries; entry && *entry; ++entry) {
        std::string_view text(*entry);
        std::size_t equals = text.find('=');
        keys.emplace_back(equals == std::string_view::npos ? text : text.substr(0, equals));
    }
    return keys;
}

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------

bool socket_startup() {
#if defined(_WIN32)
    if (winsock_started()) return true;
    WSADATA data;
    int status = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (status != 0) {
        error_slot() = status;
        return false;
    }
    winsock_started() = true;
    // Matched at process exit rather than at the end of a run. A run's end is
    // the wrong place: the unit-test binary builds many VMs in one process, and
    // tearing Winsock down after the first would break the rest. `atexit` pairs
    // the one startup with exactly one cleanup, whenever the program ends --
    // including through `khuStdSystem.exit`, which unwinds to `main` and
    // returns rather than calling `_exit`.
    std::atexit([] { socket_cleanup(); });
#endif
    return true;
}

void socket_cleanup() {
#if defined(_WIN32)
    if (!winsock_started()) return;
    ::WSACleanup();
    winsock_started() = false;
#endif
}

Socket socket_listen(const std::string& host, int port, int backlog) {
    if (!socket_startup()) return kInvalidSocket;
    AddressList candidates;
    if (!lookup(host, port, true, candidates)) return kInvalidSocket;

    for (addrinfo* candidate = candidates.head; candidate; candidate = candidate->ai_next) {
        auto raw = static_cast<Socket>(::socket(candidate->ai_family, candidate->ai_socktype,
                                                candidate->ai_protocol));
        if (!valid(raw)) {
            capture_socket_error();
            continue;
        }
        // A server that restarts must be able to take its port back rather
        // than waiting out TIME_WAIT. This is the one option worth setting
        // unconditionally; everything else is the caller's business.
        int on = 1;
        ::setsockopt(raw_of(raw), SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&on), sizeof(on));
        if (::bind(raw_of(raw), candidate->ai_addr,
                   static_cast<socklen_t>(candidate->ai_addrlen)) == 0 &&
            ::listen(raw_of(raw), backlog) == 0) {
            clear_last_error();
            return raw;
        }
        capture_socket_error();
        close_socket(raw);
    }
    return kInvalidSocket;
}

Socket socket_accept(Socket listener) {
    if (!socket_startup()) return kInvalidSocket;
    sockaddr_storage peer;
    auto length = static_cast<socklen_t>(sizeof(peer));
    auto raw = static_cast<Socket>(
        ::accept(raw_of(listener), reinterpret_cast<sockaddr*>(&peer), &length));
    if (!valid(raw)) {
        capture_socket_error();
        return kInvalidSocket;
    }
    clear_last_error();
    return raw;
}

Socket socket_connect(const std::string& host, int port) {
    if (!socket_startup()) return kInvalidSocket;
    AddressList candidates;
    if (!lookup(host, port, false, candidates)) return kInvalidSocket;

    for (addrinfo* candidate = candidates.head; candidate; candidate = candidate->ai_next) {
        auto raw = static_cast<Socket>(::socket(candidate->ai_family, candidate->ai_socktype,
                                                candidate->ai_protocol));
        if (!valid(raw)) {
            capture_socket_error();
            continue;
        }
        if (::connect(raw_of(raw), candidate->ai_addr,
                      static_cast<socklen_t>(candidate->ai_addrlen)) == 0) {
            clear_last_error();
            return raw;
        }
        capture_socket_error();
        close_socket(raw);
    }
    return kInvalidSocket;
}

std::int64_t socket_send(Socket socket, const void* data, std::size_t count) {
#if defined(_WIN32)
    auto sent = static_cast<std::int64_t>(::send(raw_of(socket), static_cast<const char*>(data),
                                                 static_cast<int>(count), 0));
#else
    // MSG_NOSIGNAL: a write to a peer that has gone away answers EPIPE rather
    // than killing the process with SIGPIPE, which a language runtime must
    // never let a library call do.
    auto sent = static_cast<std::int64_t>(::send(raw_of(socket), data, count, MSG_NOSIGNAL));
#endif
    if (sent < 0) {
        capture_socket_error();
        return -1;
    }
    return sent;
}

std::int64_t socket_recv(Socket socket, void* buffer, std::size_t count) {
#if defined(_WIN32)
    auto read = static_cast<std::int64_t>(::recv(raw_of(socket), static_cast<char*>(buffer),
                                                 static_cast<int>(count), 0));
#else
    auto read = static_cast<std::int64_t>(::recv(raw_of(socket), buffer, count, 0));
#endif
    if (read < 0) {
        capture_socket_error();
        return -1;
    }
    return read;
}

bool socket_close(Socket socket) {
#if defined(_WIN32)
    if (::closesocket(raw_of(socket)) != 0) {
#else
    if (::close(raw_of(socket)) != 0) {
#endif
        capture_socket_error();
        return false;
    }
    return true;
}

bool socket_shutdown(Socket socket, int how) {
    // SHUT_RD/SHUT_WR/SHUT_RDWR and SD_RECEIVE/SD_SEND/SD_BOTH are 0/1/2 on
    // both platforms, so the caller's number goes straight through.
    if (how < 0 || how > 2) {
        error_slot() = EINVAL;
        return false;
    }
    if (::shutdown(raw_of(socket), how) != 0) {
        capture_socket_error();
        return false;
    }
    return true;
}

bool socket_peer(Socket socket, std::string& address, int& port) {
    sockaddr_storage storage;
    auto length = static_cast<socklen_t>(sizeof(storage));
    if (::getpeername(raw_of(socket), reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
        capture_socket_error();
        return false;
    }
    return describe(reinterpret_cast<sockaddr*>(&storage), length, address, port);
}

bool socket_local(Socket socket, std::string& address, int& port) {
    sockaddr_storage storage;
    auto length = static_cast<socklen_t>(sizeof(storage));
    if (::getsockname(raw_of(socket), reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
        capture_socket_error();
        return false;
    }
    return describe(reinterpret_cast<sockaddr*>(&storage), length, address, port);
}

bool socket_set_read_timeout(Socket socket, int millis) {
    if (millis < 0) {
        error_slot() = EINVAL;
        return false;
    }
#if defined(_WIN32)
    // Winsock takes a plain millisecond count; POSIX takes a timeval.
    DWORD timeout = static_cast<DWORD>(millis);
    if (::setsockopt(raw_of(socket), SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0) {
#else
    timeval timeout;
    timeout.tv_sec = millis / 1000;
    timeout.tv_usec = (millis % 1000) * 1000;
    if (::setsockopt(raw_of(socket), SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0) {
#endif
        capture_socket_error();
        return false;
    }
    return true;
}

bool resolve_host(const std::string& name, std::string& address) {
    address.clear();
    if (!socket_startup()) return false;
    AddressList candidates;
    if (!lookup(name, 0, false, candidates)) return false;
    for (addrinfo* candidate = candidates.head; candidate; candidate = candidate->ai_next) {
        int port = 0;
        if (describe(candidate->ai_addr, static_cast<socklen_t>(candidate->ai_addrlen), address,
                     port)) {
            clear_last_error();
            return true;
        }
    }
    return false;
}

}  // namespace khu::vm::platform
