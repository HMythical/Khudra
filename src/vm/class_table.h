// Runtime class descriptors.
//
// Loaded from the .kbc class table at VM start (KHU-PLAN.md, Class descriptor).
// Everything an object needs at run time is resolved once here -- base pointer,
// allocation size, vtable entries, reference map -- so the hot paths index
// arrays instead of chasing indices through the module.
#ifndef KHU_VM_CLASS_TABLE_H
#define KHU_VM_CLASS_TABLE_H

#include <cstdint>
#include <string>
#include <string_view>

#include "bytecode/module.h"
#include "util/array.h"

namespace khu::vm {

struct RuntimeClass {
    const bytecode::ClassEntry* entry = nullptr;
    std::uint32_t class_id = 0;
    std::string_view name;
    RuntimeClass* base = nullptr;

    // One tagged Value per field slot; inherited slots come first.
    std::uint32_t slot_count = 0;
    std::uint32_t allocation_size = 0;
    bool manual = false;

    // Resolved dispatch table: index is the vtable slot.
    util::Array<const bytecode::MethodEntry*> vtable;
    util::Array<std::int32_t> vtable_indices;

    const bytecode::MethodEntry* constructor = nullptr;
    const bytecode::MethodEntry* procedures = nullptr;
    const bytecode::MethodEntry* field_init = nullptr;
    std::int32_t constructor_index = -1;
    std::int32_t procedures_index = -1;
    std::int32_t field_init_index = -1;

    std::uint8_t materialize_argc = 0;

    const bytecode::FieldEntry* field(std::uint32_t slot) const {
        if (!entry || slot >= entry->fields.size()) return nullptr;
        return &entry->fields[slot];
    }

    bool derives_from(const RuntimeClass* other) const {
        for (const RuntimeClass* current = this; current; current = current->base) {
            if (current == other) return true;
        }
        return false;
    }
};

class ClassTable {
public:
    // Resolves every class in `module`. Returns false and fills `error` when
    // the image is inconsistent.
    bool load(const bytecode::Module& module, std::string& error);

    RuntimeClass* at(std::uint32_t class_id) {
        return class_id < classes_.size() ? &classes_[class_id] : nullptr;
    }
    const RuntimeClass* at(std::uint32_t class_id) const {
        return class_id < classes_.size() ? &classes_[class_id] : nullptr;
    }
    std::size_t size() const { return classes_.size(); }

private:
    util::Array<RuntimeClass> classes_;
};

}  // namespace khu::vm

#endif  // KHU_VM_CLASS_TABLE_H
