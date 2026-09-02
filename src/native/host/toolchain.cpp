#include "host/toolchain.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>

#include <direct.h>
#include <io.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

#include "host/native_backend.h"
#include "util/file.h"

namespace khu::native {
namespace {

// --- what a platform spells differently ------------------------------------
//
// The backend's shape does not change on Windows: find a compiler, run it,
// clean up a scratch directory. Only these four spellings do, so they are
// named once here and the rest of the file reads the same on both platforms
// (EXPANSION-PLAN.md, section 6, D1).

#if defined(_WIN32)
// PATH entries are separated by ';', and an executable is named with its
// extension. `_access(path, 0)` is existence: Windows has no execute bit, so
// "is it there" is as much as can be asked.
constexpr char kPathSeparator = ';';
constexpr bool kPathNeedsExeSuffix = true;
bool is_executable(const std::string& path) { return ::_access(path.c_str(), 0) == 0; }
bool is_path_like(const std::string& name) {
    return name.find('/') != std::string::npos || name.find('\\') != std::string::npos;
}
const char* default_path() { return "C:\\Windows\\System32"; }
#else
constexpr char kPathSeparator = ':';
constexpr bool kPathNeedsExeSuffix = false;
bool is_executable(const std::string& path) { return ::access(path.c_str(), X_OK) == 0; }
bool is_path_like(const std::string& name) { return name.find('/') != std::string::npos; }
const char* default_path() { return "/usr/bin:/bin"; }
#endif

// A bare name is looked up on PATH the way the shell would, so the caller can
// ask for "cc" without knowing where it lives.
std::string which(const std::string& name) {
    if (is_path_like(name)) {
        if (is_executable(name)) return name;
        if (kPathNeedsExeSuffix && is_executable(name + ".exe")) return name + ".exe";
        return std::string();
    }

    const char* path = std::getenv("PATH");
    if (!path) path = default_path();
    std::string search(path);
    std::size_t start = 0;
    while (start <= search.size()) {
        std::size_t end = search.find(kPathSeparator, start);
        if (end == std::string::npos) end = search.size();
        std::string directory = search.substr(start, end - start);
        if (!directory.empty()) {
            std::string candidate = directory + "/" + name;
            if (is_executable(candidate)) return candidate;
            if (kPathNeedsExeSuffix && is_executable(candidate + ".exe")) {
                return candidate + ".exe";
            }
        }
        start = end + 1;
    }
    return std::string();
}

#if defined(_WIN32)
// CreateProcess takes one command line rather than an argument vector, so the
// vector has to be quoted back into a string using the exact rules the CRT
// uses to take it apart again. Paths with spaces are the common case, and a
// backslash before a quote is the corner the rules exist for.
std::string quote_argument(const std::string& argument) {
    if (!argument.empty() &&
        argument.find_first_of(" \t\n\v\"") == std::string::npos) {
        return argument;
    }
    std::string quoted = "\"";
    for (std::size_t i = 0; i < argument.size(); ++i) {
        std::size_t slashes = 0;
        while (i < argument.size() && argument[i] == '\\') {
            ++slashes;
            ++i;
        }
        if (i == argument.size()) {
            // Trailing backslashes precede the closing quote, so they double.
            quoted.append(slashes * 2, '\\');
            break;
        }
        if (argument[i] == '"') {
            quoted.append(slashes * 2 + 1, '\\');
        } else {
            quoted.append(slashes, '\\');
        }
        quoted.push_back(argument[i]);
    }
    quoted.push_back('"');
    return quoted;
}
#endif

}  // namespace

std::string find_c_compiler() {
    if (const char* override_cc = std::getenv("KHUDRA_CC")) {
        std::string resolved = which(override_cc);
        if (!resolved.empty()) return resolved;
        return std::string();
    }
    // The same three names on both platforms. `cl` is deliberately absent even
    // on Windows: MinGW-w64 is what this backend supports, MSVC speaks a
    // different flag grammar and a different symbol model, and finding a
    // compiler that then cannot build anything is worse than finding none
    // (EXPANSION-PLAN.md, D3b -- MSVC is a named deferred sub-phase).
    for (const char* name : {"cc", "clang", "gcc"}) {
        std::string resolved = which(name);
        if (!resolved.empty()) return resolved;
    }
    return std::string();
}

std::string native_library_dir() {
    if (const char* override_dir = std::getenv("KHUDRA_NATIVE_LIB_DIR")) return override_dir;
#ifdef KHU_NATIVE_LIB_DIR
    return KHU_NATIVE_LIB_DIR;
#else
    return std::string();
#endif
}

std::string native_extra_flags() {
    if (const char* override_flags = std::getenv("KHUDRA_NATIVE_FLAGS")) return override_flags;
#ifdef KHU_NATIVE_EXTRA_FLAGS
    return KHU_NATIVE_EXTRA_FLAGS;
#else
    return std::string();
#endif
}

void append_flags(const std::string& flags, std::vector<std::string>& out) {
    std::size_t start = 0;
    while (start < flags.size()) {
        std::size_t end = flags.find(' ', start);
        if (end == std::string::npos) end = flags.size();
        if (end > start) out.push_back(flags.substr(start, end - start));
        start = end + 1;
    }
}

std::string find_cxx_compiler() {
    if (const char* override_cxx = std::getenv("KHUDRA_CXX")) {
        std::string resolved = which(override_cxx);
        return resolved;
    }
    for (const char* name : {"c++", "clang++", "g++"}) {
        std::string resolved = which(name);
        if (!resolved.empty()) return resolved;
    }
    return std::string();
}

int run_tool(const std::vector<std::string>& argv, std::string& output) {
    output.clear();
    if (argv.empty()) return -1;

#if defined(_WIN32)
    // CreateProcess with both streams redirected into one pipe, waited on to
    // completion -- the same three steps posix_spawn + pipe + waitpid make
    // below, and the same bytes end up in `output`.
    SECURITY_ATTRIBUTES inheritable;
    inheritable.nLength = sizeof(inheritable);
    inheritable.lpSecurityDescriptor = nullptr;
    inheritable.bInheritHandle = TRUE;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!::CreatePipe(&read_end, &write_end, &inheritable, 0)) return -1;
    // Only the child inherits the writing end; this process must not, or the
    // read below would never see end of file.
    ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    std::string command;
    for (const std::string& argument : argv) {
        if (!command.empty()) command.push_back(' ');
        command += quote_argument(argument);
    }
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');

    STARTUPINFOA startup;
    std::memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process;
    std::memset(&process, 0, sizeof(process));
    BOOL started = ::CreateProcessA(argv[0].c_str(), mutable_command.data(), nullptr, nullptr,
                                    TRUE, 0, nullptr, nullptr, &startup, &process);
    ::CloseHandle(write_end);
    if (!started) {
        ::CloseHandle(read_end);
        return -1;
    }

    char buffer[4096];
    DWORD got = 0;
    while (::ReadFile(read_end, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        output.append(buffer, got);
    }
    ::CloseHandle(read_end);

    ::WaitForSingleObject(process.hProcess, INFINITE);
    DWORD status = 1;
    if (!::GetExitCodeProcess(process.hProcess, &status)) status = static_cast<DWORD>(-1);
    ::CloseHandle(process.hProcess);
    ::CloseHandle(process.hThread);
    return static_cast<int>(status);
#else
    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) return -1;

    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const std::string& argument : argv) {
        raw.push_back(const_cast<char*>(argument.c_str()));
    }
    raw.push_back(nullptr);

    // Both streams go to the same pipe: a compiler diagnostic is one message
    // whether it arrived on stdout or stderr.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);

    pid_t child = 0;
    int spawned = posix_spawn(&child, raw[0], &actions, nullptr, raw.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_fds[1]);

    if (spawned != 0) {
        ::close(pipe_fds[0]);
        return -1;
    }

    char buffer[4096];
    ssize_t got = 0;
    while ((got = ::read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        output.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(pipe_fds[0]);

    int status = 0;
    if (::waitpid(child, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
#endif
}

TempDir::TempDir() {
#if defined(_WIN32)
    // No mkdtemp on Windows. %TEMP% (or %TMP%) plus a name nothing else will
    // pick: CreateDirectory fails rather than succeeding on a name that is
    // already taken, so the loop is the exclusivity mkdtemp would have given.
    const char* base = std::getenv("TEMP");
    if (!base || !*base) base = std::getenv("TMP");
    if (!base || !*base) base = ".";
    for (int attempt = 0; attempt < 64; ++attempt) {
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), "khudra-native-%lu-%d",
                      static_cast<unsigned long>(::GetCurrentProcessId()),
                      attempt == 0 ? 0 : static_cast<int>(::GetTickCount() % 100000) + attempt);
        std::string candidate = std::string(base) + "\\" + suffix;
        if (::CreateDirectoryA(candidate.c_str(), nullptr)) {
            path_ = candidate;
            return;
        }
        if (::GetLastError() != ERROR_ALREADY_EXISTS) return;
    }
#else
    const char* base = std::getenv("TMPDIR");
    std::string pattern = (base && *base ? std::string(base) : std::string("/tmp")) +
                          "/khudra-native-XXXXXX";
    std::vector<char> scratch(pattern.begin(), pattern.end());
    scratch.push_back('\0');
    if (::mkdtemp(scratch.data())) path_ = scratch.data();
#endif
}

TempDir::~TempDir() {
    if (path_.empty() || keep_) return;
#if defined(_WIN32)
    for (const std::string& file : files_) ::_unlink(file.c_str());
    ::_rmdir(path_.c_str());
#else
    for (const std::string& file : files_) ::unlink(file.c_str());
    ::rmdir(path_.c_str());
#endif
}

}  // namespace khu::native
