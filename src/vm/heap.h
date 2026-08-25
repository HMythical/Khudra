// Object allocation.
//
// Phase 4 gives objects a real home: a header-stamped block per instance, kept
// on an intrusive list through the header's `gc_link`. Managed and manual
// objects are tracked separately because the collector only ever walks one of
// them and manual memory comes from the C runtime (utils/alloc.c).
//
// Phase 5 replaces the managed side's "free everything at shutdown" with
// mark-sweep and the manual side with per-class arenas; the interface here is
// what both plug into.
#ifndef KHU_VM_HEAP_H
#define KHU_VM_HEAP_H

#include <cstddef>
#include <cstdint>

#include "vm/class_table.h"
#include "vm/object.h"
#include "vm/value.h"

namespace khu::vm {

// The value a freshly linked slot holds. `null` means "not instantiated yet".
Value default_value_for(TypeTag tag);

class Heap {
public:
    Heap() = default;
    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;
    ~Heap();

    // Allocates and zero-initializes storage for one instance of `type` under
    // the given strategy. Returns nullptr when memory runs out.
    Object* allocate(RuntimeClass& type, bool manual);

    // Releases a manual object. Returns false when `object` is not manual or is
    // not tracked here.
    bool free_manual(Object* object);

    // Frees everything. Called at VM shutdown.
    void destroy_all();

    std::size_t managed_count() const { return managed_count_; }
    std::size_t manual_count() const { return manual_count_; }
    std::size_t managed_bytes() const { return managed_bytes_; }
    std::size_t total_allocations() const { return total_allocations_; }

    Object* managed_head() const { return managed_head_; }
    Object* manual_head() const { return manual_head_; }

private:
    void unlink(Object*& head, Object* object);

    Object* managed_head_ = nullptr;
    Object* manual_head_ = nullptr;
    std::size_t managed_count_ = 0;
    std::size_t manual_count_ = 0;
    std::size_t managed_bytes_ = 0;
    std::size_t total_allocations_ = 0;
};

}  // namespace khu::vm

#endif  // KHU_VM_HEAP_H
