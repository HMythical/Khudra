#include "bytecode/module.h"

#include <cstring>

namespace khu::bytecode {
namespace {

// ---- little-endian primitives ----

void put_u8(std::string& out, std::uint8_t value) { out.push_back(static_cast<char>(value)); }

void put_u16(std::string& out, std::uint16_t value) {
    put_u8(out, static_cast<std::uint8_t>(value & 0xff));
    put_u8(out, static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void put_u32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) put_u8(out, static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
}

void put_u64(std::string& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) put_u8(out, static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
}

void put_i32(std::string& out, std::int32_t value) {
    put_u32(out, static_cast<std::uint32_t>(value));
}

void put_double(std::string& out, double value) {
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u64(out, bits);
}

void put_string(std::string& out, std::string_view text) {
    put_u32(out, static_cast<std::uint32_t>(text.size()));
    out.append(text.data(), text.size());
}

class Reader {
public:
    Reader(std::string_view bytes) : bytes_(bytes) {}

    bool ok() const { return ok_; }
    std::size_t offset() const { return offset_; }

    std::uint8_t u8() {
        if (offset_ + 1 > bytes_.size()) return fail<std::uint8_t>();
        return static_cast<std::uint8_t>(bytes_[offset_++]);
    }
    std::uint16_t u16() {
        std::uint16_t low = u8();
        std::uint16_t high = u8();
        return static_cast<std::uint16_t>(low | (high << 8));
    }
    std::uint32_t u32() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(u8()) << (i * 8);
        return value;
    }
    std::uint64_t u64() {
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(u8()) << (i * 8);
        return value;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    double float64() {
        std::uint64_t bits = u64();
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    std::string text() {
        std::uint32_t length = u32();
        if (!ok_ || offset_ + length > bytes_.size()) {
            fail<std::uint8_t>();
            return {};
        }
        std::string result(bytes_.substr(offset_, length));
        offset_ += length;
        return result;
    }
    bool expect_magic() {
        if (bytes_.size() < 4) return false;
        bool match = std::memcmp(bytes_.data(), kMagic, 4) == 0;
        offset_ = 4;
        return match;
    }

private:
    template <typename T>
    T fail() {
        ok_ = false;
        return T{};
    }

    std::string_view bytes_;
    std::size_t offset_ = 0;
    bool ok_ = true;
};

// A pool key that keeps entries of different tags apart.
std::string constant_key(TypeTag tag, std::string_view payload) {
    std::string key;
    key.push_back(static_cast<char>(static_cast<std::uint8_t>(tag)));
    key.append(payload.data(), payload.size());
    return key;
}

}  // namespace

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

std::uint32_t Module::intern_string(std::string_view text) {
    std::string key = constant_key(TypeTag::String, text);
    if (std::uint32_t* found = constant_index_.find(key)) return *found;

    Constant entry;
    entry.tag = TypeTag::String;
    entry.text = std::string(text);
    auto index = static_cast<std::uint32_t>(constants.size());
    constants.push(std::move(entry));
    constant_index_.insert(key, index);
    return index;
}

std::uint32_t Module::intern_int(TypeTag tag, std::int64_t value) {
    std::string key = constant_key(tag, std::to_string(value));
    if (std::uint32_t* found = constant_index_.find(key)) return *found;

    Constant entry;
    entry.tag = tag;
    entry.as_int = value;
    entry.as_uint = static_cast<std::uint64_t>(value);
    auto index = static_cast<std::uint32_t>(constants.size());
    constants.push(std::move(entry));
    constant_index_.insert(key, index);
    return index;
}

std::uint32_t Module::intern_uint(TypeTag tag, std::uint64_t value) {
    std::string key = constant_key(tag, "u" + std::to_string(value));
    if (std::uint32_t* found = constant_index_.find(key)) return *found;

    Constant entry;
    entry.tag = tag;
    entry.as_uint = value;
    entry.as_int = static_cast<std::int64_t>(value);
    auto index = static_cast<std::uint32_t>(constants.size());
    constants.push(std::move(entry));
    constant_index_.insert(key, index);
    return index;
}

std::uint32_t Module::intern_float(TypeTag tag, double value) {
    // Key on the bit pattern so -0.0 and 0.0 stay distinct.
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    std::string key = constant_key(tag, "f" + std::to_string(bits));
    if (std::uint32_t* found = constant_index_.find(key)) return *found;

    Constant entry;
    entry.tag = tag;
    entry.as_float = value;
    auto index = static_cast<std::uint32_t>(constants.size());
    constants.push(std::move(entry));
    constant_index_.insert(key, index);
    return index;
}

std::uint32_t Module::intern_bool(bool value) {
    std::string key = constant_key(TypeTag::Bool, value ? "1" : "0");
    if (std::uint32_t* found = constant_index_.find(key)) return *found;

    Constant entry;
    entry.tag = TypeTag::Bool;
    entry.as_int = value ? 1 : 0;
    entry.as_uint = value ? 1 : 0;
    auto index = static_cast<std::uint32_t>(constants.size());
    constants.push(std::move(entry));
    constant_index_.insert(key, index);
    return index;
}

std::string_view Module::string_at(std::uint32_t index) const {
    if (index >= constants.size()) return "<bad-index>";
    return constants[index].text;
}

const MethodEntry* Module::method_at(std::int32_t index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= methods.size()) return nullptr;
    return &methods[static_cast<std::size_t>(index)];
}

const ClassEntry* Module::class_at(std::int32_t index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= classes.size()) return nullptr;
    return &classes[static_cast<std::size_t>(index)];
}

const LineEntry* Module::line_for(const MethodEntry& method, std::uint32_t offset) const {
    const LineEntry* best = nullptr;
    for (const LineEntry& entry : method.lines) {
        if (entry.offset > offset) break;
        best = &entry;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::string serialize(const Module& module) {
    std::string out;
    out.append(kMagic, 4);
    put_u16(out, kVersionMajor);
    put_u16(out, kVersionMinor);
    put_u32(out, module.source_file);
    put_i32(out, module.main_method);
    put_i32(out, module.root_class);

    put_u32(out, static_cast<std::uint32_t>(module.constants.size()));
    for (const Constant& entry : module.constants) {
        put_u8(out, static_cast<std::uint8_t>(entry.tag));
        switch (entry.tag) {
            case TypeTag::String:
                put_string(out, entry.text);
                break;
            case TypeTag::Float32:
            case TypeTag::Float64:
                put_double(out, entry.as_float);
                break;
            default:
                put_u64(out, entry.as_uint);
                break;
        }
    }

    put_u32(out, static_cast<std::uint32_t>(module.classes.size()));
    for (const ClassEntry& entry : module.classes) {
        put_u32(out, entry.name);
        put_i32(out, entry.base);
        put_u32(out, entry.flags);
        put_u32(out, entry.object_size);
        put_i32(out, entry.constructor);
        put_i32(out, entry.procedures);
        put_i32(out, entry.field_init);
        put_u8(out, entry.materialize_argc);

        put_u32(out, static_cast<std::uint32_t>(entry.fields.size()));
        for (const FieldEntry& field : entry.fields) {
            put_u32(out, field.name);
            put_u8(out, static_cast<std::uint8_t>(field.type));
            put_u32(out, field.offset);
            put_u32(out, field.class_ref);
            put_u8(out, field.ref_kind);
            put_u8(out, field.is_public);
        }

        put_u32(out, static_cast<std::uint32_t>(entry.vtable.size()));
        for (std::uint32_t slot : entry.vtable) put_u32(out, slot);
    }

    put_u32(out, static_cast<std::uint32_t>(module.methods.size()));
    for (const MethodEntry& entry : module.methods) {
        put_u32(out, entry.name);
        put_u32(out, entry.owner_class);
        put_u32(out, entry.flags);
        put_u8(out, entry.param_count);
        put_u16(out, entry.frame_size);
        put_u8(out, static_cast<std::uint8_t>(entry.return_type));
        put_u32(out, entry.native_id);

        put_u32(out, static_cast<std::uint32_t>(entry.code.size()));
        for (std::uint8_t byte : entry.code) put_u8(out, byte);

        put_u32(out, static_cast<std::uint32_t>(entry.lines.size()));
        for (const LineEntry& line : entry.lines) {
            put_u32(out, line.offset);
            put_u32(out, line.line);
            put_u32(out, line.column);
        }
    }

    return out;
}

bool deserialize(std::string_view bytes, Module& out, std::string& error) {
    Reader reader(bytes);
    if (!reader.expect_magic()) {
        error = "not a Khudra bytecode image (bad magic)";
        return false;
    }

    std::uint16_t major = reader.u16();
    std::uint16_t minor = reader.u16();
    if (major != kVersionMajor) {
        error = "bytecode version " + std::to_string(major) + "." + std::to_string(minor) +
                " is not supported by this toolchain (expected " +
                std::to_string(kVersionMajor) + ".x)";
        return false;
    }

    out.source_file = reader.u32();
    out.main_method = reader.i32();
    out.root_class = reader.i32();

    std::uint32_t constant_count = reader.u32();
    for (std::uint32_t i = 0; i < constant_count && reader.ok(); ++i) {
        Constant entry;
        entry.tag = static_cast<TypeTag>(reader.u8());
        switch (entry.tag) {
            case TypeTag::String:
                entry.text = reader.text();
                break;
            case TypeTag::Float32:
            case TypeTag::Float64:
                entry.as_float = reader.float64();
                break;
            default:
                entry.as_uint = reader.u64();
                entry.as_int = static_cast<std::int64_t>(entry.as_uint);
                break;
        }
        out.constants.push(std::move(entry));
    }

    std::uint32_t class_count = reader.u32();
    for (std::uint32_t i = 0; i < class_count && reader.ok(); ++i) {
        ClassEntry entry;
        entry.name = reader.u32();
        entry.base = reader.i32();
        entry.flags = reader.u32();
        entry.object_size = reader.u32();
        entry.constructor = reader.i32();
        entry.procedures = reader.i32();
        entry.field_init = reader.i32();
        entry.materialize_argc = reader.u8();

        std::uint32_t field_count = reader.u32();
        for (std::uint32_t f = 0; f < field_count && reader.ok(); ++f) {
            FieldEntry field;
            field.name = reader.u32();
            field.type = static_cast<TypeTag>(reader.u8());
            field.offset = reader.u32();
            field.class_ref = reader.u32();
            field.ref_kind = reader.u8();
            field.is_public = reader.u8();
            entry.fields.push(field);
        }

        std::uint32_t vtable_count = reader.u32();
        for (std::uint32_t v = 0; v < vtable_count && reader.ok(); ++v) {
            entry.vtable.push(reader.u32());
        }
        out.classes.push(std::move(entry));
    }

    std::uint32_t method_count = reader.u32();
    for (std::uint32_t i = 0; i < method_count && reader.ok(); ++i) {
        MethodEntry entry;
        entry.name = reader.u32();
        entry.owner_class = reader.u32();
        entry.flags = reader.u32();
        entry.param_count = reader.u8();
        entry.frame_size = reader.u16();
        entry.return_type = static_cast<TypeTag>(reader.u8());
        entry.native_id = reader.u32();

        std::uint32_t code_length = reader.u32();
        for (std::uint32_t c = 0; c < code_length && reader.ok(); ++c) entry.code.push(reader.u8());

        std::uint32_t line_count = reader.u32();
        for (std::uint32_t l = 0; l < line_count && reader.ok(); ++l) {
            LineEntry line;
            line.offset = reader.u32();
            line.line = reader.u32();
            line.column = reader.u32();
            entry.lines.push(line);
        }
        out.methods.push(std::move(entry));
    }

    if (!reader.ok()) {
        error = "bytecode image is truncated";
        return false;
    }
    return true;
}

}  // namespace khu::bytecode
