#include "vm/format.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace khu::vm::format {

namespace {

// The most negative value of a width, as an unsigned magnitude, so an
// out-of-range check can be written without overflowing on the way to it.
std::uint64_t signed_min_magnitude(std::uint32_t width) {
    return width >= 64 ? (std::uint64_t{1} << 63) : (std::uint64_t{1} << (width - 1));
}

std::uint64_t signed_max(std::uint32_t width) { return signed_min_magnitude(width) - 1; }

std::uint64_t unsigned_max(std::uint32_t width) {
    return width >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << width) - 1);
}

// Reads `digit+` starting at `index`, accumulating into `value` and failing on
// overflow past `limit`. Returns how many digits it consumed.
std::size_t read_digits(std::string_view text, std::size_t index, std::uint64_t limit,
                        std::uint64_t& value, bool& overflowed) {
    std::size_t start = index;
    value = 0;
    overflowed = false;
    while (index < text.size() && is_digit_byte(static_cast<std::uint8_t>(text[index]))) {
        std::uint64_t digit = static_cast<std::uint64_t>(text[index] - '0');
        if (value > (limit - digit) / 10) overflowed = true;
        if (!overflowed) value = value * 10 + digit;
        ++index;
    }
    return index - start;
}

// Walks the float grammar without converting, so the conversion below only ever
// sees text it already agreed was a number.
bool scan_float(std::string_view text, std::size_t& end) {
    std::size_t index = 0;
    if (index < text.size() && (text[index] == '+' || text[index] == '-')) ++index;

    std::size_t integer_digits = 0;
    while (index < text.size() && is_digit_byte(static_cast<std::uint8_t>(text[index]))) {
        ++index;
        ++integer_digits;
    }

    std::size_t fraction_digits = 0;
    if (index < text.size() && text[index] == '.') {
        ++index;
        while (index < text.size() && is_digit_byte(static_cast<std::uint8_t>(text[index]))) {
            ++index;
            ++fraction_digits;
        }
    }
    if (integer_digits == 0 && fraction_digits == 0) return false;

    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        std::size_t exponent = index + 1;
        if (exponent < text.size() && (text[exponent] == '+' || text[exponent] == '-')) {
            ++exponent;
        }
        std::size_t exponent_digits = 0;
        while (exponent < text.size() &&
               is_digit_byte(static_cast<std::uint8_t>(text[exponent]))) {
            ++exponent;
            ++exponent_digits;
        }
        // A trailing `e` with no digits is not part of the number.
        if (exponent_digits > 0) index = exponent;
    }

    end = index;
    return true;
}

}  // namespace

std::string format_int(std::int64_t value) {
    char scratch[32];
    std::snprintf(scratch, sizeof(scratch), "%" PRId64, value);
    return scratch;
}

std::string format_uint(std::uint64_t value) {
    char scratch[32];
    std::snprintf(scratch, sizeof(scratch), "%" PRIu64, value);
    return scratch;
}

std::string format_float(double value, bool is32) {
    (void)is32;  // both widths render the same way; see the rule in format.h
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
    char scratch[64];
    std::snprintf(scratch, sizeof(scratch), "%g", value);
    return scratch;
}

std::string format_bool(bool value) { return value ? "true" : "false"; }

bool parse_int(std::string_view text, std::uint32_t width, std::int64_t& out) {
    if (text.empty()) return false;
    std::size_t index = 0;
    bool negative = false;
    if (text[index] == '+' || text[index] == '-') {
        negative = text[index] == '-';
        ++index;
    }
    std::uint64_t limit = negative ? signed_min_magnitude(width) : signed_max(width);
    std::uint64_t magnitude = 0;
    bool overflowed = false;
    std::size_t digits = read_digits(text, index, limit, magnitude, overflowed);
    if (digits == 0 || overflowed) return false;
    if (index + digits != text.size()) return false;
    if (magnitude > limit) return false;
    // The negation happens in unsigned space: `-INT64_MIN` is UB in signed
    // arithmetic, and the answer this function must give for "-9223372036854775808"
    // is INT64_MIN. `0 - magnitude` wraps by the unsigned rules, which is
    // defined behaviour at this width (UBSan's `-fsanitize=sign` agrees).
    out = negative ? static_cast<std::int64_t>(0 - magnitude)
                   : static_cast<std::int64_t>(magnitude);
    return true;
}

bool parse_uint(std::string_view text, std::uint32_t width, std::uint64_t& out) {
    if (text.empty()) return false;
    std::size_t index = 0;
    // A leading `+` is accepted for symmetry with the signed grammar; `-` is
    // not, because an unsigned type has no negative values to name.
    if (text[index] == '+') ++index;
    std::uint64_t limit = unsigned_max(width);
    std::uint64_t value = 0;
    bool overflowed = false;
    std::size_t digits = read_digits(text, index, limit, value, overflowed);
    if (digits == 0 || overflowed) return false;
    if (index + digits != text.size()) return false;
    if (value > limit) return false;
    out = value;
    return true;
}

bool parse_float(std::string_view text, bool is32, double& out) {
    std::size_t end = 0;
    if (!scan_float(text, end) || end != text.size()) return false;
    // The grammar already matched, so strtod only has to do the arithmetic. It
    // runs in the "C" locale -- Khudra never calls setlocale -- so the decimal
    // point is a `.` on every host.
    std::string buffer(text);
    const char* first = buffer.c_str();
    char* last = nullptr;
    double value = std::strtod(first, &last);
    if (last != first + buffer.size()) return false;
    out = is32 ? static_cast<double>(static_cast<float>(value)) : value;
    return true;
}

bool is_numeric(std::string_view text) {
    std::size_t end = 0;
    return scan_float(text, end) && end == text.size();
}

bool is_digit_byte(std::uint8_t byte) { return byte >= '0' && byte <= '9'; }

bool is_letter_byte(std::uint8_t byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

bool is_whitespace_byte(std::uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\f' ||
           byte == '\v';
}

std::uint8_t to_upper_byte(std::uint8_t byte) {
    return (byte >= 'a' && byte <= 'z') ? static_cast<std::uint8_t>(byte - 'a' + 'A') : byte;
}

std::uint8_t to_lower_byte(std::uint8_t byte) {
    return (byte >= 'A' && byte <= 'Z') ? static_cast<std::uint8_t>(byte - 'A' + 'a') : byte;
}

}  // namespace khu::vm::format
