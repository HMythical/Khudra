#include "util/string_builder.h"

#include <cinttypes>
#include <cstdio>

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
    // %g keeps round-tripped literals short while staying lossless enough for
    // the pretty printer to reparse to the same value.
    char scratch[40];
    std::snprintf(scratch, sizeof(scratch), "%.17g", value);
    return append(scratch);
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
