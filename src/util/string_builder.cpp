#include "util/string_builder.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>

namespace khu::util {

StringBuilder& StringBuilder::append(char c) {
    buffer_.push_back(c);
    return *this;
}

StringBuilder& StringBuilder::append(std::string_view text) {
    buffer_.append(text.data(), text.size());
    return *this;
}

StringBuilder& StringBuilder::append_int(std::int64_t value) {
    char scratch[24];
    std::snprintf(scratch, sizeof(scratch), "%" PRId64, value);
    return append(scratch);
}

StringBuilder& StringBuilder::append_uint(std::uint64_t value) {
    char scratch[24];
    std::snprintf(scratch, sizeof(scratch), "%" PRIu64, value);
    return append(scratch);
}

StringBuilder& StringBuilder::append_double(double value) {
    // Shortest representation that still reads back as the same double, so the
    // pretty printer round-trips float literals exactly.
    char scratch[48];
    for (int digits = 15; digits <= 17; ++digits) {
        std::snprintf(scratch, sizeof(scratch), "%.*g", digits, value);
        if (std::strtod(scratch, nullptr) == value) break;
    }

    // Keep it lexing as a float: "2" would come back as an integer literal.
    bool looks_like_float = false;
    for (const char* c = scratch; *c; ++c) {
        if (*c == '.' || *c == 'e' || *c == 'E' || *c == 'n' || *c == 'i') {
            looks_like_float = true;
            break;
        }
    }
    append(scratch);
    if (!looks_like_float) append(".0");
    return *this;
}

StringBuilder& StringBuilder::append_line(std::string_view text) {
    append(text);
    return append('\n');
}

StringBuilder& StringBuilder::indent(int count) {
    for (int i = 0; i < count; ++i) append(indent_unit_);
    return *this;
}

}  // namespace khu::util
