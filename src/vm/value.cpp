#include "vm/value.h"

#include <cinttypes>
#include <cstdio>

namespace khu::vm {

std::string to_display_string(const Value& value) {
    char scratch[64];
    switch (value.tag) {
        case TypeTag::Bool:
            return value.as_uint != 0 ? "true" : "false";
        case TypeTag::String:
            return value.as_text ? *value.as_text : std::string("null");
        case TypeTag::Null:
            return "null";
        case TypeTag::Float32:
        case TypeTag::Float64:
            std::snprintf(scratch, sizeof(scratch), "%g", value.as_float);
            return scratch;
        case TypeTag::Void:
            return "void";
        case TypeTag::Memory:
            return value.as_uint == 2 ? "manual" : "standard";
        case TypeTag::Ref:
            // The VM prints the class name; without the module all we know is
            // whether anything is there.
            return value.as_ref ? "<object>" : "null";
        default:
            if (bytecode::is_signed_integer(value.tag)) {
                std::snprintf(scratch, sizeof(scratch), "%" PRId64, value.as_int);
            } else if (bytecode::is_integer(value.tag)) {
                std::snprintf(scratch, sizeof(scratch), "%" PRIu64, value.as_uint);
            } else {
                return bytecode::type_tag_name(value.tag);
            }
            return scratch;
    }
}

}  // namespace khu::vm
