// Owns the text of every file the compiler has loaded and maps SourceLocations
// back to human-readable positions.
#ifndef KHU_DIAG_SOURCE_MANAGER_H
#define KHU_DIAG_SOURCE_MANAGER_H

#include <cstdint>
#include <string>
#include <string_view>

#include "diag/source_location.h"
#include "util/array.h"

namespace khu::diag {

class SourceManager {
public:
    // Registers an in-memory buffer under `path` and returns its file id.
    std::uint32_t add_buffer(std::string_view path, std::string contents);

    // Loads `path` from disk. Returns kInvalidFileId when the read fails and
    // fills `error` with a human-readable reason.
    std::uint32_t load_file(std::string_view path, std::string& error);

    std::string_view path(std::uint32_t file_id) const;
    std::string_view contents(std::uint32_t file_id) const;

    // The full text of the line containing `location`, without its newline.
    std::string_view line_text(SourceLocation location) const;

    // Formats "path:line:column".
    std::string format(SourceLocation location) const;

    std::size_t file_count() const { return files_.size(); }

private:
    struct File {
        std::string path;
        std::string contents;
    };

    khu::util::Array<File> files_;
};

}  // namespace khu::diag

#endif  // KHU_DIAG_SOURCE_MANAGER_H
