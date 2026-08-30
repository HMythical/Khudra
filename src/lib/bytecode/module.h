// The in-memory form of a .kbc image, and its serialization.
//
// A module is self-contained: constants, classes (with layout, reference maps
// and vtables), and one code blob per method or Procedures block. The VM loads
// it without consulting the front end.
#ifndef KHU_BYTECODE_MODULE_H
#define KHU_BYTECODE_MODULE_H

#include <cstdint>
#include <string>
#include <string_view>

#include "bytecode/opcode.h"
#include "util/array.h"
#include "util/hashmap.h"

namespace khu::bytecode {

constexpr char kMagic[4] = {'K', 'H', 'U', 'B'};
constexpr std::uint16_t kVersionMajor = 1;
constexpr std::uint16_t kVersionMinor = 2;

// One typed entry in the constant pool. Integer constants keep their width so
// the VM never has to guess one.
struct Constant {
    TypeTag tag = TypeTag::Int32;
    std::int64_t as_int = 0;
    std::uint64_t as_uint = 0;
    double as_float = 0.0;
    std::string text;  // String entries
};

// Bits in ClassEntry::flags.
enum ClassFlags : std::uint32_t {
    kClassGc = 1u << 0,
    kClassManual = 1u << 1,
    kClassNamespace = 1u << 2,
};

// Mirrors sema::RefKind; the collector reads it straight out of the image.
enum RefKindByte : std::uint8_t {
    kRefRaw = 0,
    kRefManaged = 1,
    kRefManual = 2,
    // An erased slot -- the `T` of a generic class. What is in it is only known
    // at run time, so it is traced and pinned by the value's own tag, the way
    // an array's elements are (docs/memory-model.md, 3.1).
    kRefDynamic = 3,
};

struct FieldEntry {
    std::uint32_t name = 0;  // constant-pool index of a String
    TypeTag type = TypeTag::Int32;
    std::uint32_t offset = 0;
    std::uint32_t class_ref = 0xffffffffu;  // target class id when type == Ref
    std::uint8_t ref_kind = kRefRaw;
    std::uint8_t is_public = 0;
};

struct ClassEntry {
    std::uint32_t name = 0;
    std::int32_t base = -1;
    std::uint32_t flags = kClassGc;
    std::uint32_t object_size = 0;
    // The full layout: inherited fields first, then this class's own.
    util::Array<FieldEntry> fields;
    // Full dispatch table; entries are method indices.
    util::Array<std::uint32_t> vtable;
    std::int32_t constructor = -1;
    std::int32_t procedures = -1;
    // Synthetic method that evaluates this class's field initializers. It runs
    // during object linking, base class first, before the Procedures block.
    std::int32_t field_init = -1;
    // How many allocation-site arguments this class binds. Both the Procedures
    // block and the constructor receive the same ones, so one count covers both
    // (docs/procedures.md).
    std::uint8_t materialize_argc = 0;
};

// Bits in MethodEntry::flags.
enum MethodFlags : std::uint32_t {
    kMethodStatic = 1u << 0,
    kMethodNative = 1u << 1,
    kMethodConstructor = 1u << 2,
    kMethodProcedures = 1u << 3,
    kMethodFieldInit = 1u << 4,
};

// Maps a code offset back to a source position, for VM stack traces.
struct LineEntry {
    std::uint32_t offset = 0;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
};

struct MethodEntry {
    std::uint32_t name = 0;
    std::uint32_t owner_class = 0;
    std::uint32_t flags = 0;
    std::uint8_t param_count = 0;
    std::uint16_t frame_size = 0;  // parameters plus locals
    TypeTag return_type = TypeTag::Void;
    std::uint32_t native_id = 0;
    // Constant-pool index of the file this method was written in. An image is
    // built from more than one file -- the program plus the standard library --
    // so a stack trace has to name the right one per frame rather than assuming
    // the module's own path.
    std::uint32_t source_file = 0;
    util::Array<std::uint8_t> code;
    util::Array<LineEntry> lines;

    bool is_static() const { return (flags & kMethodStatic) != 0; }
    bool is_native() const { return (flags & kMethodNative) != 0; }
};

class Module {
public:
    util::Array<Constant> constants;
    util::Array<ClassEntry> classes;
    util::Array<MethodEntry> methods;
    std::int32_t main_method = -1;
    std::int32_t root_class = -1;
    std::uint32_t source_file = 0;  // constant-pool index of the source path

    // Interning: repeated names and literals share one entry.
    std::uint32_t intern_string(std::string_view text);
    std::uint32_t intern_int(TypeTag tag, std::int64_t value);
    std::uint32_t intern_uint(TypeTag tag, std::uint64_t value);
    std::uint32_t intern_float(TypeTag tag, double value);
    std::uint32_t intern_bool(bool value);

    std::string_view string_at(std::uint32_t index) const;
    const MethodEntry* method_at(std::int32_t index) const;
    const ClassEntry* class_at(std::int32_t index) const;

    // Line lookup for stack traces; returns the last entry at or before
    // `offset`.
    const LineEntry* line_for(const MethodEntry& method, std::uint32_t offset) const;

private:
    util::StringMap<std::uint32_t> constant_index_;
};

// Serialization. Both directions are explicit little-endian so an image written
// on one machine loads on another.
std::string serialize(const Module& module);
// Returns false and fills `error` when the bytes are not a valid image.
bool deserialize(std::string_view bytes, Module& out, std::string& error);

}  // namespace khu::bytecode

#endif  // KHU_BYTECODE_MODULE_H
