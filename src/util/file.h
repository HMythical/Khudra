// Whole-file reader. The compiler always loads sources in one shot so that
// SourceManager can own a stable buffer for diagnostics to point into.
#ifndef KHU_UTIL_FILE_H
#define KHU_UTIL_FILE_H

#include <string>
#include <string_view>

namespace khu::util {

enum class FileError {
    None,
    NotFound,
    ReadFailed,
};

// Reads `path` into `out`. Returns FileError::None on success.
FileError read_file(std::string_view path, std::string& out);

// Writes `bytes` to `path`, truncating any existing file.
bool write_file(std::string_view path, std::string_view bytes);

const char* describe(FileError error);

}  // namespace khu::util

#endif  // KHU_UTIL_FILE_H
