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

ManualArena& Heap::arena_for(const RuntimeClass& type) {
    if (arenas_.size() <= type.class_id) arenas_.resize(type.class_id + 1, nullptr);
    ManualArena*& arena = arenas_[type.class_id];
    if (!arena) arena = new ManualArena(type.allocation_size);
    return *arena;
}

Object* Heap::allocate(RuntimeClass& type, bool manual) {
    std::size_t bytes = type.allocation_size;

    // Manual objects come from their class's arena, which in turn takes blocks
    // from the C runtime -- so its live-byte accounting still covers every
    // manual byte in the process.
    void* storage = manual ? arena_for(type).allocate() : std::calloc(1, bytes);
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

bool Heap::is_tracked_manual(const Object* object) const {
    for (const Object* current = manual_head_; current; current = current->header.gc_link) {
        if (current == object) return true;
    }
    return false;
}

bool Heap::free_manual(Object* object) {
    if (!object || !object->is_manual()) return false;
    auto* type = static_cast<RuntimeClass*>(object->header.vtable);
    if (!type) return false;
    // Returning an already-released chunk to the arena would corrupt its free
    // list, so a pointer that is not on the live list is refused.
    if (!is_tracked_manual(object)) return false;

    unlink(manual_head_, object);
    --manual_count_;
    arena_for(*type).release(object);
    return true;
}

void Heap::destroy_managed(Object* object) {
    if (!object) return;
    if (managed_bytes_ >= object->header.size) managed_bytes_ -= object->header.size;
    if (managed_count_ > 0) --managed_count_;
    std::free(object);
}

void Heap::adopt_managed_list(Object* head, std::size_t count, std::size_t bytes) {
    managed_head_ = head;
    managed_count_ = count;
    managed_bytes_ = bytes;
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

    // Manual chunks belong to their arenas; dropping the arenas releases every
    // block back to the C runtime in one go.
    manual_head_ = nullptr;
    manual_count_ = 0;
    for (ManualArena* arena : arenas_) delete arena;
    arenas_.clear();
}

}  // namespace khu::vm
