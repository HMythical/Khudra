// Semantic types.
//
// Distinct from ast::TypeNode: that records what was written, this records what
// it means. Aliases canonicalize here (i32 -> int32, u8/byte -> uint8,
// int -> int32), and types are interned so identity comparison is exact --
// which is what "no implicit widening or narrowing" needs.
#ifndef KHU_SEMA_TYPE_H
#define KHU_SEMA_TYPE_H

#include <cstdint>
#include <string>
#include <string_view>

#include "util/array.h"
#include "util/hashmap.h"

namespace khu::sema {

struct ClassSymbol;

enum class TypeKind : std::uint8_t {
    Error,   // a type that failed to resolve; suppresses cascading diagnostics
    Void,
    Bool,
    Int,     // signed, `width` bits
    UInt,    // unsigned, `width` bits
    Float,   // `width` is 32 (float) or 64 (dfloat)
    String,
    Array,
    Pointer, // *byte and friends
    Class,
    Memory,  // MemoryAllocationTypeObject
    Null,    // the type of the `null` literal: "not instantiated yet"
};

// How the collector must treat a slot of this type (docs/memory-model.md).
enum class RefKind : std::uint8_t {
    Raw,         // scalars and raw pointers -- never traced
    ManagedRef,  // a GC object -- traced
    ManualRef,   // a manual object -- pinned, never traced
};

const char* ref_kind_name(RefKind kind);

class Type {
public:
    TypeKind kind = TypeKind::Error;
    std::uint32_t width = 0;              // bits, for Int/UInt/Float
    const Type* pointee = nullptr;        // Pointer
    const ClassSymbol* class_symbol = nullptr;  // Class

    bool is_error() const { return kind == TypeKind::Error; }
    bool is_void() const { return kind == TypeKind::Void; }
    bool is_bool() const { return kind == TypeKind::Bool; }
    bool is_null() const { return kind == TypeKind::Null; }
    bool is_signed() const { return kind == TypeKind::Int; }
    bool is_unsigned() const { return kind == TypeKind::UInt; }
    bool is_integer() const { return kind == TypeKind::Int || kind == TypeKind::UInt; }
    bool is_float() const { return kind == TypeKind::Float; }
    bool is_numeric() const { return is_integer() || is_float(); }
    bool is_class() const { return kind == TypeKind::Class; }
    bool is_memory() const { return kind == TypeKind::Memory; }

    // Types that can hold `null` -- everything that is a reference at runtime.
    bool is_reference() const {
        switch (kind) {
            case TypeKind::String:
            case TypeKind::Array:
            case TypeKind::Pointer:
            case TypeKind::Class:
            case TypeKind::Null:
                return true;
            default:
                return false;
        }
    }

    // Size of one slot holding this type, in bytes.
    std::uint32_t slot_size() const;

    std::string display() const;
};

// Owns and interns every Type. One context per compilation.
class TypeContext {
public:
    TypeContext();
    TypeContext(const TypeContext&) = delete;
    TypeContext& operator=(const TypeContext&) = delete;
    ~TypeContext();

    const Type* error() const { return error_; }
    const Type* void_type() const { return void_; }
    const Type* bool_type() const { return bool_; }
    const Type* null_type() const { return null_; }
    const Type* string_type() const { return string_; }
    const Type* array_type() const { return array_; }
    const Type* memory_type() const { return memory_; }

    const Type* signed_int(std::uint32_t width) const;
    const Type* unsigned_int(std::uint32_t width) const;
    const Type* float_type(std::uint32_t width) const;

    const Type* int32() const { return signed_int(32); }
    const Type* pointer_to(const Type* pointee);
    const Type* class_type(const ClassSymbol* symbol);

    // Reference kind for a slot of `type`, used to build class reference maps.
    RefKind ref_kind_of(const Type* type) const;

private:
    const Type* intern(Type value);

    util::Array<Type*> owned_;
    util::StringMap<Type*> interned_;

    const Type* error_ = nullptr;
    const Type* void_ = nullptr;
    const Type* bool_ = nullptr;
    const Type* null_ = nullptr;
    const Type* string_ = nullptr;
    const Type* array_ = nullptr;
    const Type* memory_ = nullptr;
    const Type* signed_[4] = {};    // 8, 16, 32, 64
    const Type* unsigned_[4] = {};
    const Type* floats_[2] = {};    // 32, 64
};

}  // namespace khu::sema

#endif  // KHU_SEMA_TYPE_H
