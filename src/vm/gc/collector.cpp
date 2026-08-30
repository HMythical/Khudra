#include "gc/collector.h"

#include <string>

namespace khu::vm {

void Collector::mark_object(Object* object) {
    // Manual objects are never collected: they are freed explicitly, and they
    // act as roots rather than as heap members.
    if (!object || object->is_manual()) return;
    if (object->header.flags & kObjectMarked) return;
    object->header.flags |= kObjectMarked;
    worklist_.push(object);
}

void Collector::mark_value(const Value& value) {
    if (value.tag != TypeTag::Ref) return;
    mark_object(value.as_ref);
}

void Collector::trace_worklist() {
    while (!worklist_.empty()) {
        Object* object = worklist_.back();
        worklist_.pop();

        // An array has no reference map -- its element type is erased -- so it
        // is traced by tag instead. That is still precise: a Value's tag says
        // exactly whether it holds a reference, which is the same question the
        // map answers for a field.
        if (object->is_array()) {
            Value* elements = object->slots();
            std::uint32_t length = object->array_length();
            for (std::uint32_t i = 0; i < length; ++i) mark_value(elements[i]);
            continue;
        }

        auto* type = static_cast<RuntimeClass*>(object->header.vtable);
        if (!type) continue;

        Value* slots = object->slots();
        for (std::uint32_t i = 0; i < type->slot_count; ++i) {
            const bytecode::FieldEntry* field = type->field(i);
            // Precise tracing: only managed references are followed. A manual
            // reference is pinned instead, and a raw slot is never a pointer.
            // A managed slot is followed; a dynamic one is followed only when
            // the value in it says it holds a reference, which mark_value
            // already checks.
            if (!field) continue;
            if (field->ref_kind != bytecode::kRefManaged &&
                field->ref_kind != bytecode::kRefDynamic) {
                continue;
            }
            mark_value(slots[i]);
        }
    }
}

void Collector::mark_from_roots() {
    if (roots_) roots_->enumerate_roots(*this);

    // Manual -> managed rooting: a manual object is outside the collected heap,
    // so anything it still points at is reachable by definition.
    for (Object* manual = heap_.manual_head(); manual;
         manual = manual->header.gc_link) {
        auto* type = static_cast<RuntimeClass*>(manual->header.vtable);
        if (!type) continue;
        Value* slots = manual->slots();
        for (std::uint32_t i = 0; i < type->slot_count; ++i) {
            const bytecode::FieldEntry* field = type->field(i);
            // A managed slot is followed; a dynamic one is followed only when
            // the value in it says it holds a reference, which mark_value
            // already checks.
            if (!field) continue;
            if (field->ref_kind != bytecode::kRefManaged &&
                field->ref_kind != bytecode::kRefDynamic) {
                continue;
            }
            mark_value(slots[i]);
        }
    }

    trace_worklist();
}

namespace {

// Drops one pin an object was holding on a manual object through `slot`.
void release_pin(Value& slot) {
    if (slot.tag != TypeTag::Ref || !slot.as_ref) return;
    if (!slot.as_ref->is_manual()) return;
    if (slot.as_ref->header.pin_count > 0) --slot.as_ref->header.pin_count;
}

}  // namespace

void Collector::release_pins(Object* object) {
    // Same story as tracing: an array's pins are found by tag, a class
    // instance's by its reference map.
    if (object->is_array()) {
        Value* elements = object->slots();
        std::uint32_t length = object->array_length();
        for (std::uint32_t i = 0; i < length; ++i) release_pin(elements[i]);
        return;
    }

    auto* type = static_cast<RuntimeClass*>(object->header.vtable);
    if (!type) return;

    Value* slots = object->slots();
    for (std::uint32_t i = 0; i < type->slot_count; ++i) {
        const bytecode::FieldEntry* field = type->field(i);
        if (!field) continue;
        if (field->ref_kind != bytecode::kRefManual &&
            field->ref_kind != bytecode::kRefDynamic) {
            continue;
        }
        release_pin(slots[i]);
    }
}

void Collector::sweep() {
    Object* survivor_head = nullptr;
    std::size_t survivors = 0;
    std::size_t survivor_bytes = 0;
    std::size_t reclaimed = 0;

    Object* current = heap_.managed_head();
    while (current) {
        Object* next = current->header.gc_link;
        if (current->header.flags & kObjectMarked) {
            current->header.flags &= ~static_cast<std::uint32_t>(kObjectMarked);
            current->header.gc_link = survivor_head;
            survivor_head = current;
            ++survivors;
            survivor_bytes += current->header.size;
        } else {
            // Reclaiming a managed object drops the pins it was holding on
            // manual objects (docs/memory-model.md, section 4.2).
            release_pins(current);
            heap_.destroy_managed(current);
            ++reclaimed;
        }
        current = next;
    }

    heap_.adopt_managed_list(survivor_head, survivors, survivor_bytes);
    total_reclaimed_ += reclaimed;

    // Let the heap grow with the live set so a program with a large working
    // set does not collect on every allocation.
    std::size_t next_threshold = survivor_bytes * growth_;
    threshold_ = next_threshold < minimum_threshold_ ? minimum_threshold_ : next_threshold;
}

std::size_t Collector::collect() {
    std::size_t before = heap_.managed_count();
    ++cycles_;
    mark_from_roots();
    sweep();
    std::size_t after = heap_.managed_count();
    return before > after ? before - after : 0;
}

bool Collector::should_collect() const { return heap_.managed_bytes() >= threshold_; }

util::Array<std::string> Collector::find_holders(const Object* target) const {
    util::Array<std::string> holders;
    if (!target) return holders;

    for (Object* owner = heap_.managed_head(); owner; owner = owner->header.gc_link) {
        auto* type = static_cast<RuntimeClass*>(owner->header.vtable);
        if (!type) continue;
        const Value* slots = owner->slots();
        for (std::uint32_t i = 0; i < type->slot_count; ++i) {
            const bytecode::FieldEntry* field = type->field(i);
            if (!field || field->ref_kind != bytecode::kRefManual) continue;
            if (slots[i].tag != TypeTag::Ref || slots[i].as_ref != target) continue;
            std::string_view field_name =
                i < type->field_names.size() ? type->field_names[i] : std::string_view("?");
            holders.push(std::string(type->name) + "." + std::string(field_name));
        }
    }
    return holders;
}

}  // namespace khu::vm
