// Append-only string builder used by the pretty printer and the disassembler.
#ifndef KHU_UTIL_STRING_BUILDER_H
#define KHU_UTIL_STRING_BUILDER_H

#include <cstdint>
#include <string>
#include <string_view>

namespace khu::util {

class StringBuilder {
public:
    StringBuilder& append(char c);
    StringBuilder& append(std::string_view text);
    StringBuilder& append_int(std::int64_t value);
    StringBuilder& append_uint(std::uint64_t value);
    StringBuilder& append_double(double value);
    StringBuilder& append_line(std::string_view text = {});

    // Emits `count` levels of the configured indent unit.
    StringBuilder& indent(int count);
    void set_indent_unit(std::string_view unit) { indent_unit_ = std::string(unit); }

    const std::string& str() const { return buffer_; }
    std::string take() { return std::move(buffer_); }
    std::size_t size() const { return buffer_.size(); }
    bool empty() const { return buffer_.empty(); }
    void clear() { buffer_.clear(); }

private:
    std::string buffer_;
    std::string indent_unit_ = "    ";
};

}  // namespace khu::util

#endif  // KHU_UTIL_STRING_BUILDER_H
