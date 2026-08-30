#include "host/toolchain.h"

#include <cstdlib>
#include <cstring>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "host/native_backend.h"
#include "util/file.h"

extern char** environ;

namespace khu::native {
namespace {

bool is_executable(const std::string& path) { return ::access(path.c_str(), X_OK) == 0; }

// A bare name is looked up on PATH the way the shell would, so the caller can
// ask for "cc" without knowing where it lives.
std::string which(const std::string& name) {
    if (name.find('/') != std::string::npos) return is_executable(name) ? name : std::string();

    const char* path = std::getenv("PATH");
    if (!path) path = "/usr/bin:/bin";
    std::string search(path);
    std::size_t start = 0;
    while (start <= search.size()) {
        std::size_t end = search.find(':', start);
        if (end == std::string::npos) end = search.size();
        std::string directory = search.substr(start, end - start);
        if (!directory.empty()) {
            std::string candidate = directory + "/" + name;
            if (is_executable(candidate)) return candidate;
        }
        start = end + 1;
    }
    return std::string();
}

}  // namespace

std::string find_c_compiler() {
    if (const char* override_cc = std::getenv("KHUDRA_CC")) {
        std::string resolved = which(override_cc);
        if (!resolved.empty()) return resolved;
        return std::string();
    }
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
}

TempDir::TempDir() {
    const char* base = std::getenv("TMPDIR");
    std::string pattern = (base && *base ? std::string(base) : std::string("/tmp")) +
                          "/khudra-native-XXXXXX";
    std::vector<char> scratch(pattern.begin(), pattern.end());
    scratch.push_back('\0');
    if (::mkdtemp(scratch.data())) path_ = scratch.data();
}

TempDir::~TempDir() {
    if (path_.empty() || keep_) return;
    for (const std::string& file : files_) ::unlink(file.c_str());
    ::rmdir(path_.c_str());
}

}  // namespace khu::native
