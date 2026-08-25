// The standard library, embedded in the toolchain.
//
// `lib/*.khu` is the source of truth; scripts/embed_stdlib.cmake turns it into
// a generated translation unit at build time. Embedding rather than reading
// from disk means `khudra` works wherever it is copied to, and a program cannot
// silently compile against a different library than the one it was built with.
#ifndef KHU_SEMA_STDLIB_SOURCES_H
#define KHU_SEMA_STDLIB_SOURCES_H

#include <cstddef>

namespace khu::sema {

struct StdlibFile {
    const char* path;     // e.g. "lib/io.khu", used in diagnostics
    const char* contents;
};

const StdlibFile* stdlib_files();
std::size_t stdlib_file_count();

}  // namespace khu::sema

#endif  // KHU_SEMA_STDLIB_SOURCES_H
