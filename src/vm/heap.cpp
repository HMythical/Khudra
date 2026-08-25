#include "vm/heap.h"

#include <cstdlib>
#include <cstring>

extern "C" {
#include "alloc.h"
}

namespace khu::vm {

Value default_value_for(TypeTag tag) {
    switch (tag) {
        case TypeTag::Bool:
            return Value::make_bool(false);
        case TypeTag::Float32:
        case TypeTag::Float64:
            return Value::make_float(tag, 0.0);
        case TypeTag::Memory:
            return Value::make_strategy(static_cast<std::uint8_t>(bytecode::StrategyByte::Gc));
        default:
            if (bytecode::is_signed_integer(tag)) return Value::make_int(tag, 0);
            if (bytecode::is_integer(tag)) return Value::make_uint(tag, 0);
            // Every reference slot starts as `null`: not instantiated yet.
            return Value::make_null();
    }
}

Heap::~Heap() { destroy_all(); }

Object* Heap::allocate(RuntimeClass& type, bool manual) {
    std::size_t bytes = type.allocation_size;

    // Manual memory comes from the C runtime so its live-byte accounting covers
    // every manual object in the process.
    void* storage = manual ? khu_manual_alloc(bytes) : std::calloc(1, bytes);
    if (!storage) return nullptr;

    auto* object = static_cast<Object*>(storage);
    object->header.class_id = type.class_id;
    object->header.flags = manual ? kObjectManual : kObjectGc;
    object->header.pin_count = 0;
    object->header.size = static_cast<std::uint32_t>(bytes);
    object->header.vtable = &type;

    // Slots start at their type's default; the field initializers run next.
    Value* slots = object->slots();
    for (std::uint32_t i = 0; i < type.slot_count; ++i) {
        const bytecode::FieldEntry* field = type.field(i);
        slots[i] = default_value_for(field ? field->type : TypeTag::Null);
    }

    Object*& head = manual ? manual_head_ : managed_head_;
    object->header.gc_link = head;
    head = object;

    if (manual) {
        ++manual_count_;
    } else {
        ++managed_count_;
        managed_bytes_ += bytes;
    }
    ++total_allocations_;
    return object;
}

void Heap::unlink(Object*& head, Object* object) {
    Object* previous = nullptr;
    for (Object* current = head; current; current = current->header.gc_link) {
        if (current == object) {
            if (previous) {
                previous->header.gc_link = current->header.gc_link;
            } else {
                head = current->header.gc_link;
            }
            return;
        }
        previous = current;
    }
}

bool Heap::free_manual(Object* object) {
    if (!object || !object->is_manual()) return false;
    unlink(manual_head_, object);
    --manual_count_;
    khu_manual_free(object);
    return true;
}

void Heap::destroy_all() {
    Object* current = managed_head_;
    while (current) {
        Object* next = current->header.gc_link;
        std::free(current);
        current = next;
    }
    managed_head_ = nullptr;
    managed_count_ = 0;
    managed_bytes_ = 0;

    current = manual_head_;
    while (current) {
        Object* next = current->header.gc_link;
        khu_manual_free(current);
        current = next;
    }
    manual_head_ = nullptr;
    manual_count_ = 0;
}

}  // namespace khu::vm
