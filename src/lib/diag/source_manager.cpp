#include "diag/source_manager.h"

#include "util/file.h"

namespace khu::diag {

std::uint32_t SourceManager::add_buffer(std::string_view path, std::string contents) {
    auto id = static_cast<std::uint32_t>(files_.size());
    files_.push(File{std::string(path), std::move(contents)});
    return id;
}

std::uint32_t SourceManager::load_file(std::string_view path, std::string& error) {
    std::string contents;
    khu::util::FileError status = khu::util::read_file(path, contents);
    if (status != khu::util::FileError::None) {
        error = khu::util::describe(status);
        return kInvalidFileId;
    }
    return add_buffer(path, std::move(contents));
}

std::string_view SourceManager::path(std::uint32_t file_id) const {
    if (file_id >= files_.size()) return "<unknown>";
    return files_[file_id].path;
}

std::string_view SourceManager::contents(std::uint32_t file_id) const {
    if (file_id >= files_.size()) return {};
    return files_[file_id].contents;
}

std::string_view SourceManager::line_text(SourceLocation location) const {
    std::string_view text = contents(location.file_id);
    if (text.empty() || location.line == 0) return {};

    std::size_t start = 0;
    std::uint32_t line = 1;
    while (line < location.line && start < text.size()) {
        std::size_t newline = text.find('\n', start);
        if (newline == std::string_view::npos) return {};
        start = newline + 1;
        ++line;
    }
    if (line != location.line || start > text.size()) return {};

    std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos) end = text.size();
    // Tolerate CRLF sources.
    if (end > start && text[end - 1] == '\r') --end;
    return text.substr(start, end - start);
}

std::string SourceManager::format(SourceLocation location) const {
    std::string result(path(location.file_id));
    if (!location.valid()) return result;
    result += ':';
    result += std::to_string(location.line);
    result += ':';
    result += std::to_string(location.column);
    return result;
}

}  // namespace khu::diag
