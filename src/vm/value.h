// VM values.
//
// One tagged union covering the 8 integer widths, 2 floats, bool, and object
// references (KHU-PLAN.md, Tagged values). The tag is the bytecode TypeTag, so
// a constant-pool entry, a field slot and a stack value all speak one language.
#ifndef KHU_VM_VALUE_H
#define KHU_VM_VALUE_H

#include <cstdint>
#include <string>

#include "bytecode/opcode.h"

namespace khu::vm {

using bytecode::TypeTag;

struct Object;

struct Value {
    TypeTag tag = TypeTag::Void;
    union {
        std::int64_t as_int;
        std::uint64_t as_uint;
        double as_float;
        Object* as_ref;
        // String literals point straight at the constant pool: they are
        // immutable and outlive every frame that can see them.
        const std::string* as_text;
        void* as_raw;
    };

    Value() : as_uint(0) {}

    static Value make_void() { return Value(); }

    static Value make_int(TypeTag tag, std::int64_t value) {
        Value result;
        result.tag = tag;
        result.as_int = value;
        return result;
    }

    static Value make_uint(TypeTag tag, std::uint64_t value) {
        Value result;
        result.tag = tag;
        result.as_uint = value;
        return result;
    }

    static Value make_float(TypeTag tag, double value) {
        Value result;
        result.tag = tag;
        result.as_float = value;
        return result;
    }

    static Value make_bool(bool value) {
        Value result;
        result.tag = TypeTag::Bool;
        result.as_uint = value ? 1 : 0;
        return result;
    }

    static Value make_null() {
        Value result;
        result.tag = TypeTag::Null;
        result.as_ref = nullptr;
        return result;
    }

    static Value make_ref(Object* object) {
        Value result;
        result.tag = TypeTag::Ref;
        result.as_ref = object;
        return result;
    }

    static Value make_string(const std::string* text) {
        Value result;
        result.tag = TypeTag::String;
        result.as_text = text;
        return result;
    }

    static Value make_strategy(std::uint8_t strategy) {
        Value result;
        result.tag = TypeTag::Memory;
        result.as_uint = strategy;
        return result;
    }

    bool truthy() const { return as_uint != 0; }
    // `null` and a Ref with a null pointer both mean "not instantiated yet".
    bool is_null_reference() const {
        return tag == TypeTag::Null || (tag == TypeTag::Ref && as_ref == nullptr);
    }
};

// --- width normalization ---------------------------------------------------
//
// Overflow wraps (docs/spec.md, section 2), so every integer result is truncated back
// to its declared width, and a 32-bit float rounds through `float` so its
// precision is real rather than nominal. The interpreter, the shared natives
// and `khu_native_abi.h`'s inline C all apply the same two rules; these are the
// C++ side of that, shared so the interpreter and a native cannot diverge.

std::uint64_t mask_of(std::uint32_t width);
std::int64_t sign_extend(std::uint64_t bits, std::uint32_t width);
Value normalize_int(TypeTag tag, std::uint64_t bits);
Value normalize_float(TypeTag tag, double value);

// Rendering for io.print and for runtime error messages.
std::string to_display_string(const Value& value);

}  // namespace khu::vm

#endif  // KHU_VM_VALUE_H
