#include "vm/value.h"

#include "vm/format.h"

namespace khu::vm {

std::uint64_t mask_of(std::uint32_t width) {
    return width >= 64 ? ~0ull : ((1ull << width) - 1);
}

std::int64_t sign_extend(std::uint64_t bits, std::uint32_t width) {
    if (width >= 64) return static_cast<std::int64_t>(bits);
    std::uint64_t sign = 1ull << (width - 1);
    bits &= mask_of(width);
    return static_cast<std::int64_t>((bits ^ sign) - sign);
}

Value normalize_int(TypeTag tag, std::uint64_t bits) {
    std::uint32_t width = bytecode::type_width(tag);
    if (bytecode::is_signed_integer(tag)) return Value::make_int(tag, sign_extend(bits, width));
    return Value::make_uint(tag, bits & mask_of(width));
}

Value normalize_float(TypeTag tag, double value) {
    if (tag == TypeTag::Float32) return Value::make_float(tag, static_cast<float>(value));
    return Value::make_float(tag, value);
}

std::string to_display_string(const Value& value) {
    switch (value.tag) {
        case TypeTag::Bool:
            return format::format_bool(value.as_uint != 0);
        case TypeTag::String:
            return value.as_text ? *value.as_text : std::string("null");
        case TypeTag::Null:
            return "null";
        case TypeTag::Float32:
        case TypeTag::Float64:
            return format::format_float(value.as_float, value.tag == TypeTag::Float32);
        case TypeTag::Void:
            return "void";
        case TypeTag::Memory:
            return value.as_uint == 2 ? "manual" : "standard";
        case TypeTag::Ref:
            // The VM prints the class name; without the module all we know is
            // whether anything is there.
            return value.as_ref ? "<object>" : "null";
        default:
            if (bytecode::is_signed_integer(value.tag)) return format::format_int(value.as_int);
            if (bytecode::is_integer(value.tag)) return format::format_uint(value.as_uint);
            return bytecode::type_tag_name(value.tag);
    }
}

}  // namespace khu::vm
