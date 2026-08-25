#include "sema/type.h"

#include "sema/symbol.h"

namespace khu::sema {

const char* ref_kind_name(RefKind kind) {
    switch (kind) {
        case RefKind::Raw: return "raw";
        case RefKind::ManagedRef: return "managed-ref";
        case RefKind::ManualRef: return "manual-ref";
    }
    return "raw";
}

std::uint32_t Type::slot_size() const {
    switch (kind) {
        case TypeKind::Bool: return 1;
        case TypeKind::Int:
        case TypeKind::UInt:
        case TypeKind::Float: return width / 8;
        case TypeKind::Memory: return 4;  // a strategy tag
        case TypeKind::Void:
        case TypeKind::Error: return 0;
        default: return sizeof(void*);
    }
}

std::string Type::display() const {
    switch (kind) {
        case TypeKind::Error: return "<error>";
        case TypeKind::Void: return "void";
        case TypeKind::Bool: return "bool";
        case TypeKind::Int: return "int" + std::to_string(width);
        case TypeKind::UInt: return "uint" + std::to_string(width);
        case TypeKind::Float: return width == 32 ? "float" : "dfloat";
        case TypeKind::String: return "string";
        case TypeKind::Array: return "Array";
        case TypeKind::Pointer: return "*" + (pointee ? pointee->display() : std::string("void"));
        case TypeKind::Class: return class_symbol ? std::string(class_symbol->name) : "<class>";
        case TypeKind::Memory: return "MemoryAllocationTypeObject";
        case TypeKind::Null: return "null";
    }
    return "<type>";
}

namespace {

// Interning key. Class types key on the symbol pointer, so two distinct classes
// never collide.
std::string key_for(const Type& type) {
    std::string key;
    key += static_cast<char>('a' + static_cast<int>(type.kind));
    key += ':';
    key += std::to_string(type.width);
    key += ':';
    key += std::to_string(reinterpret_cast<std::uintptr_t>(type.pointee));
    key += ':';
    key += std::to_string(reinterpret_cast<std::uintptr_t>(type.class_symbol));
    return key;
}

int width_index(std::uint32_t width) {
    switch (width) {
        case 8: return 0;
        case 16: return 1;
        case 32: return 2;
        case 64: return 3;
        default: return -1;
    }
}

}  // namespace

const Type* TypeContext::intern(Type value) {
    std::string key = key_for(value);
    if (Type** found = interned_.find(key)) return *found;
    Type* fresh = new Type(value);
    owned_.push(fresh);
    interned_.insert(key, fresh);
    return fresh;
}

TypeContext::TypeContext() {
    Type scratch;

    scratch = Type{};
    scratch.kind = TypeKind::Error;
    error_ = intern(scratch);

    scratch = Type{};
    scratch.kind = TypeKind::Void;
    void_ = intern(scratch);

    scratch = Type{};
    scratch.kind = TypeKind::Bool;
    bool_ = intern(scratch);

    scratch = Type{};
    scratch.kind = TypeKind::Null;
    null_ = intern(scratch);

    scratch = Type{};
    scratch.kind = TypeKind::String;
    string_ = intern(scratch);

    scratch = Type{};
    scratch.kind = TypeKind::Array;
    array_ = intern(scratch);

    scratch = Type{};
    scratch.kind = TypeKind::Memory;
    memory_ = intern(scratch);

    const std::uint32_t widths[4] = {8, 16, 32, 64};
    for (int i = 0; i < 4; ++i) {
        scratch = Type{};
        scratch.kind = TypeKind::Int;
        scratch.width = widths[i];
        signed_[i] = intern(scratch);

        scratch = Type{};
        scratch.kind = TypeKind::UInt;
        scratch.width = widths[i];
        unsigned_[i] = intern(scratch);
    }

    for (int i = 0; i < 2; ++i) {
        scratch = Type{};
        scratch.kind = TypeKind::Float;
        scratch.width = i == 0 ? 32 : 64;
        floats_[i] = intern(scratch);
    }
}

TypeContext::~TypeContext() {
    for (Type* type : owned_) delete type;
}

const Type* TypeContext::signed_int(std::uint32_t width) const {
    int index = width_index(width);
    return index < 0 ? error_ : signed_[index];
}

const Type* TypeContext::unsigned_int(std::uint32_t width) const {
    int index = width_index(width);
    return index < 0 ? error_ : unsigned_[index];
}

const Type* TypeContext::float_type(std::uint32_t width) const {
    if (width == 32) return floats_[0];
    if (width == 64) return floats_[1];
    return error_;
}

const Type* TypeContext::pointer_to(const Type* pointee) {
    Type scratch;
    scratch.kind = TypeKind::Pointer;
    scratch.pointee = pointee;
    return intern(scratch);
}

const Type* TypeContext::class_type(const ClassSymbol* symbol) {
    Type scratch;
    scratch.kind = TypeKind::Class;
    scratch.class_symbol = symbol;
    return intern(scratch);
}

RefKind TypeContext::ref_kind_of(const Type* type) const {
    if (!type) return RefKind::Raw;
    switch (type->kind) {
        case TypeKind::String:
        case TypeKind::Array:
            // The built-in reference types are always collected.
            return RefKind::ManagedRef;
        case TypeKind::Class:
            if (!type->class_symbol) return RefKind::ManagedRef;
            return type->class_symbol->strategy == Strategy::Manual ? RefKind::ManualRef
                                                                    : RefKind::ManagedRef;
        default:
            // Scalars, raw pointers and the strategy descriptor are never traced.
            return RefKind::Raw;
    }
}

}  // namespace khu::sema
