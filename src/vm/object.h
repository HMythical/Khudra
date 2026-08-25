// Runtime object layout.
//
// Every object -- collected or manual -- starts with the same header, and
// `gc_link` lives in it rather than in a side table so the layout is GC-aware
// from day one (KHU-PLAN.md, Object model).
//
//   class_id   uint32   index into the class table
//   flags      uint32   bit0 = GC, bit1 = MANUAL
//   pin_count  uint32   managed -> manual pins
//   size       uint32   total allocation size
//   vtable     void*    method table
//   gc_link    void*    intrusive collector list
//   ---------------------------------------------
//   slot 0 ... fields at fixed offsets
#ifndef KHU_VM_OBJECT_H
#define KHU_VM_OBJECT_H

#include <cstdint>

#include "vm/value.h"

namespace khu::vm {

enum ObjectFlags : std::uint32_t {
    kObjectGc = 1u << 0,
    kObjectManual = 1u << 1,
    // Set once the Procedures block and the constructor have both completed
    // and the object has been registered as live.
    kObjectMaterialized = 1u << 2,
    kObjectMarked = 1u << 3,
};

struct ObjectHeader {
    std::uint32_t class_id = 0;
    std::uint32_t flags = 0;
    std::uint32_t pin_count = 0;
    std::uint32_t size = 0;
    void* vtable = nullptr;
    Object* gc_link = nullptr;
};

struct Object {
    ObjectHeader header;
    // Field storage follows: one tagged Value per slot, indexed by the slot
    // number that getfield/putfield carry. Keeping fields as Values means the
    // collector can trace a slot without a second lookup. FieldEntry::offset
    // records the packed byte offset a future compact layout would use.
    Value* slots() { return reinterpret_cast<Value*>(this + 1); }
    const Value* slots() const { return reinterpret_cast<const Value*>(this + 1); }

    bool is_manual() const { return (header.flags & kObjectManual) != 0; }
    bool is_managed() const { return (header.flags & kObjectGc) != 0; }
};

}  // namespace khu::vm

#endif  // KHU_VM_OBJECT_H
