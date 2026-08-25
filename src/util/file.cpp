#include "util/file.h"

#include <cstdio>

namespace khu::util {

FileError read_file(std::string_view path, std::string& out) {
    std::string path_copy(path);
    std::FILE* handle = std::fopen(path_copy.c_str(), "rb");
    if (!handle) return FileError::NotFound;

    out.clear();
    char chunk[8192];
    while (true) {
        std::size_t got = std::fread(chunk, 1, sizeof(chunk), handle);
        out.append(chunk, got);
        if (got < sizeof(chunk)) break;
    }
    bool failed = std::ferror(handle) != 0;
    std::fclose(handle);
    if (failed) {
        out.clear();
        return FileError::ReadFailed;
    }
    return FileError::None;
}

bool write_file(std::string_view path, std::string_view bytes) {
    std::string path_copy(path);
    std::FILE* handle = std::fopen(path_copy.c_str(), "wb");
    if (!handle) return false;
    std::size_t written = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), handle);
    bool ok = written == bytes.size() && std::ferror(handle) == 0;
    std::fclose(handle);
    return ok;
}

const char* describe(FileError error) {
    switch (error) {
        case FileError::None: return "ok";
        case FileError::NotFound: return "no such file";
        case FileError::ReadFailed: return "read failed";
    }
    return "unknown error";
}

}  // namespace khu::util
