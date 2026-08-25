// Source positions. Threaded through every token, AST node, symbol and
// diagnostic from the first commit -- retrofitting locations later is painful.
#ifndef KHU_DIAG_SOURCE_LOCATION_H
#define KHU_DIAG_SOURCE_LOCATION_H

#include <cstdint>

namespace khu::diag {

// Sentinel file id meaning "no known file".
constexpr std::uint32_t kInvalidFileId = 0xffffffffu;

struct SourceLocation {
    std::uint32_t file_id = kInvalidFileId;
    std::uint32_t line = 0;    // 1-based
    std::uint32_t column = 0;  // 1-based, counted in bytes
    std::uint32_t offset = 0;  // byte offset into the file buffer

    bool valid() const { return file_id != kInvalidFileId && line != 0; }
};

// Half-open byte range within a single file.
struct SourceRange {
    SourceLocation begin;
    std::uint32_t length = 0;
};

}  // namespace khu::diag

#endif  // KHU_DIAG_SOURCE_LOCATION_H
