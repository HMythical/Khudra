// Mark-sweep collector.
//
// Precise, not conservative: every slot's reference kind comes from the class's
// reference map, so the collector never has to guess whether a word is a
// pointer (docs/memory-model.md, section 3).
//
// Roots are the VM's value stack, every live frame's receiver and locals, the
// values a native frame is holding, and **every live manual object**. That last
// one is how manual -> managed rooting works: manual memory is outside the
// collected heap, so anything a manual object still refers to has to be traced
// from it.
#ifndef KHU_VM_GC_COLLECTOR_H
#define KHU_VM_GC_COLLECTOR_H

#include <cstddef>
#include <cstdint>

#include "util/array.h"
#include "vm/class_table.h"
#include "vm/heap.h"
#include "vm/object.h"
#include "vm/value.h"

namespace khu::vm {

class Collector;

// Implemented by the VM: hands the collector everything the interpreter is
// holding onto.
class RootSource {
public:
    virtual ~RootSource() = default;
    virtual void enumerate_roots(Collector& collector) = 0;
};

class Collector {
public:
    Collector(Heap& heap, ClassTable& classes) : heap_(heap), classes_(classes) {}

    void set_root_source(RootSource* source) { roots_ = source; }

    // Called from RootSource::enumerate_roots.
    void mark_value(const Value& value);
    void mark_object(Object* object);

    // Runs one full cycle. Returns how many objects were reclaimed.
    std::size_t collect();

    // True when the managed heap has grown past the current threshold.
    bool should_collect() const;

    // Bytes of managed heap that trigger the next cycle. After a collection the
    // threshold grows with the live set, but never below this floor -- so
    // setting it low really does keep collections frequent.
    void set_threshold(std::size_t bytes) {
        threshold_ = bytes ? bytes : 1;
        minimum_threshold_ = threshold_;
    }
    std::size_t threshold() const { return threshold_; }
    void set_growth_factor(std::size_t factor) { growth_ = factor ? factor : 2; }

    std::size_t cycles() const { return cycles_; }
    std::size_t total_reclaimed() const { return total_reclaimed_; }

    // Names the managed slots that currently hold `target`, for the error a
    // `free` on a pinned object raises. Walks the heap, so it is only called on
    // the failure path.
    util::Array<std::string> find_holders(const Object* target) const;

private:
    void mark_from_roots();
    void trace_worklist();
    void sweep();
    // Drops the pins a dying managed object was holding on manual objects.
    void release_pins(Object* object);

    Heap& heap_;
    ClassTable& classes_;
    RootSource* roots_ = nullptr;
    util::Array<Object*> worklist_;

    std::size_t threshold_ = 512 * 1024;
    std::size_t minimum_threshold_ = 64 * 1024;
    std::size_t growth_ = 2;
    std::size_t cycles_ = 0;
    std::size_t total_reclaimed_ = 0;
};

}  // namespace khu::vm

#endif  // KHU_VM_GC_COLLECTOR_H
