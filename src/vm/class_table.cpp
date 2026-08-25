#include "vm/class_table.h"

#include "vm/object.h"

namespace khu::vm {

bool ClassTable::load(const bytecode::Module& module, std::string& error) {
    classes_.resize(module.classes.size());

    // First pass: everything that does not depend on another class.
    for (std::size_t i = 0; i < module.classes.size(); ++i) {
        const bytecode::ClassEntry& entry = module.classes[i];
        RuntimeClass& runtime = classes_[i];

        runtime.entry = &entry;
        runtime.class_id = static_cast<std::uint32_t>(i);
        runtime.name = module.string_at(entry.name);
        runtime.manual = (entry.flags & bytecode::kClassManual) != 0;
        runtime.slot_count = static_cast<std::uint32_t>(entry.fields.size());
        runtime.allocation_size =
            static_cast<std::uint32_t>(sizeof(Object) + runtime.slot_count * sizeof(Value));
        runtime.materialize_argc = entry.materialize_argc;

        for (const bytecode::FieldEntry& field : entry.fields) {
            runtime.field_names.push(module.string_at(field.name));
        }

        runtime.constructor_index = entry.constructor;
        runtime.procedures_index = entry.procedures;
        runtime.field_init_index = entry.field_init;
        runtime.constructor = module.method_at(entry.constructor);
        runtime.procedures = module.method_at(entry.procedures);
        runtime.field_init = module.method_at(entry.field_init);

        if (entry.constructor >= 0 && !runtime.constructor) {
            error = "class '" + std::string(runtime.name) + "' names a missing constructor";
            return false;
        }
        if (entry.procedures >= 0 && !runtime.procedures) {
            error = "class '" + std::string(runtime.name) + "' names a missing Procedures block";
            return false;
        }

        for (std::uint32_t slot : entry.vtable) {
            const bytecode::MethodEntry* method =
                module.method_at(static_cast<std::int32_t>(slot));
            if (!method) {
                error = "class '" + std::string(runtime.name) + "' has an empty vtable slot";
                return false;
            }
            runtime.vtable.push(method);
            runtime.vtable_indices.push(static_cast<std::int32_t>(slot));
        }
    }

    // Second pass: link base classes, now that every descriptor exists.
    for (std::size_t i = 0; i < classes_.size(); ++i) {
        std::int32_t base = module.classes[i].base;
        if (base < 0) continue;
        if (static_cast<std::size_t>(base) >= classes_.size()) {
            error = "class '" + std::string(classes_[i].name) + "' names an unknown base class";
            return false;
        }
        classes_[i].base = &classes_[static_cast<std::size_t>(base)];
    }

    // A cycle here would hang every walk of the chain, so refuse the image.
    for (RuntimeClass& runtime : classes_) {
        RuntimeClass* slow = &runtime;
        RuntimeClass* fast = &runtime;
        while (fast && fast->base) {
            slow = slow->base;
            fast = fast->base->base;
            if (slow && slow == fast) {
                error = "inheritance cycle at class '" + std::string(runtime.name) + "'";
                return false;
            }
        }
    }

    return true;
}

}  // namespace khu::vm
