// The host system compiler, and the scratch space the backend hands it.
//
// Both native modes end in the same place: a C translation unit and `cc`. This
// is the part that finds the compiler, runs it, and cleans up after itself --
// separated out because "is there a compiler on this machine" is a question
// `run --native` answers by falling back to the VM and `build` answers by
// failing, and neither should have to know how the answer was reached.
#ifndef KHU_NATIVE_HOST_TOOLCHAIN_H
#define KHU_NATIVE_HOST_TOOLCHAIN_H

#include <string>
#include <vector>

namespace khu::native {

// The C compiler to drive: $KHUDRA_CC when set, otherwise the first of cc,
// clang, gcc found on PATH. Empty when the machine has none.
std::string find_c_compiler();

// Splits a space-separated flag string onto the end of a command line. The
// values come from the build system or from an environment variable a
// developer set, never from program input.
void append_flags(const std::string& flags, std::vector<std::string>& out);

// The C++ compiler that links a built binary: the archives it links are C++,
// so the C++ driver has to do the link. $KHUDRA_CXX, otherwise c++, clang++,
// g++.
std::string find_cxx_compiler();

// Runs `argv` to completion. Returns the exit status, or -1 when the program
// could not be started; `output` collects stdout and stderr together.
int run_tool(const std::vector<std::string>& argv, std::string& output);

// A scratch directory for the intermediate C and the artifact built from it.
class TempDir {
public:
    TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir();

    bool ok() const { return !path_.empty(); }
    const std::string& path() const { return path_; }
    std::string file(const std::string& name) const { return path_ + "/" + name; }

    // Records a file to delete on the way out. The directory only ever holds
    // what the backend put there, so nothing else is ever removed.
    void track(const std::string& path) { files_.push_back(path); }
    // Leaves everything on disk -- `build --keep-c`.
    void keep() { keep_ = true; }

private:
    std::string path_;
    std::vector<std::string> files_;
    bool keep_ = false;
};

}  // namespace khu::native

#endif  // KHU_NATIVE_HOST_TOOLCHAIN_H
