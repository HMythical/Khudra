// The Khudra virtual machine.
//
// A stack interpreter over the .kbc image: a shared value stack, one frame per
// active call, and a switch over the opcode. Object memory, the collector and
// the Procedure engine plug in behind this core.
#ifndef KHU_VM_VM_H
#define KHU_VM_VM_H

#include <cstdint>
#include <string>

extern "C" {
#include "proc_engine.h"
}

#include "bytecode/module.h"
#include "gc/collector.h"
#include "util/array.h"
#include "vm/class_table.h"
#include "vm/heap.h"
#include "vm/object.h"
#include "vm/value.h"

namespace khu::vm {

class Vm : public RootSource {
public:
    explicit Vm(const bytecode::Module& module);
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;
    ~Vm();

    // Loads the class table. run() does this itself; call it directly when
    // invoking individual methods.
    bool prepare();

    // Runs the image's entry point: `main` when it has one, otherwise
    // root-class materialization. Returns false and sets error() on a trap.
    bool run();

    // Invokes one method by index. Arguments must already be pushed.
    bool invoke(std::int32_t method_index, Value receiver, Value& result);

    bool has_error() const { return !error_.empty(); }
    const std::string& error() const { return error_; }

    // Sends io.* output to `sink` instead of stdout. Used by the tests and by
    // `khudra run` when it needs to capture a program's output.
    void set_output_sink(std::string* sink) { sink_ = sink; }
    // Supplies io.readLine input instead of stdin.
    void set_input(std::string input) { input_ = std::move(input); }

    std::uint64_t instructions_executed() const { return instructions_; }
    const Heap& heap() const { return heap_; }
    const ClassTable& classes() const { return classes_; }
    Collector& collector() { return collector_; }
    const Collector& collector() const { return collector_; }

    // Hands the collector the value stack, every live frame and any value a
    // native frame is holding. Manual objects are roots too, and the collector
    // walks those itself.
    void enumerate_roots(Collector& collector) override;

    // --- Procedure engine host (utils/proc_engine.h) ---
    //
    // The engine in utils/proc_engine.c owns the order of the materialization
    // pipeline; these are the steps it drives. They are public because the C
    // callback table in proc_host.cpp calls them.
    std::uint32_t proc_chain_length(std::uint32_t class_id);
    std::uint32_t proc_chain_at(std::uint32_t class_id, std::uint32_t index);
    Object* proc_allocate(std::uint32_t class_id, KhuStrategy strategy);
    bool proc_has_field_init(std::uint32_t class_id);
    bool proc_run_field_init(Object* object, std::uint32_t class_id);
    bool proc_take_arguments(std::uint32_t argc);
    void proc_push_arguments(std::uint32_t argc);
    void proc_drop_arguments();
    bool proc_has_procedures(std::uint32_t class_id);
    bool proc_run_procedures(Object* object, std::uint32_t class_id, std::uint32_t argc);
    bool proc_has_constructor(std::uint32_t class_id);
    bool proc_run_constructor(Object* object, std::uint32_t class_id, std::uint32_t argc);
    void proc_register_live(Object* object);
    void proc_discard(Object* object);
    void proc_retain(Object* object);
    void proc_release(Object* object);
    void proc_report_error(const char* message);

private:
    struct Frame {
        const bytecode::MethodEntry* method = nullptr;
        std::uint32_t ip = 0;
        Value receiver;
        util::Array<Value> locals;
    };

    // --- stack ---
    void push(Value value) { stack_.push(value); }
    Value pop();
    Value& top();

    // --- decoding ---
    std::uint8_t read_u8(Frame& frame);
    std::uint16_t read_u16(Frame& frame);
    std::int32_t read_i32(Frame& frame);

    bool execute(Frame& frame, Value& result);
    bool call_method(std::int32_t method_index, Value receiver, Value& result);
    bool call_native(std::uint32_t native_id, std::uint8_t argc);

    // The materialization pipeline (docs/procedures.md). Phase 6 moves the
    // driving loop into the C engine; the steps themselves live here.
    bool materialize(std::uint32_t class_id, bytecode::StrategyByte strategy, Value& out);
    bool release_manual(Value target);
    // Allocates, collecting first when the heap has grown past its threshold.
    Object* allocate(RuntimeClass& type, bool manual);
    // The managed-slot write barrier: assigning a manual reference into a
    // collected object pins it, overwriting one drops the pin.
    void write_barrier(Object* owner, const RuntimeClass& type, std::uint32_t slot,
                       const Value& incoming);

    // Raises a runtime error carrying the current source position and a stack
    // trace, and unwinds.
    bool trap(std::string message);
    std::string describe_location(const Frame& frame) const;
    std::string render_value(const Value& value) const;

    // --- arithmetic helpers ---
    bool arithmetic(bytecode::Op op, TypeTag tag);
    bool compare(bytecode::Op op, TypeTag tag);
    bool convert(TypeTag from, TypeTag to);

    const bytecode::Module& module_;
    ClassTable classes_;
    Heap heap_;
    Collector collector_{heap_, classes_};
    // Values held only by a native frame -- a half-materialized object, its
    // allocation arguments -- which the interpreter's stack cannot see.
    util::Array<Value> native_roots_;
    // Allocation-site arguments the Procedure engine has lifted off the stack,
    // as a stack of frames because nested materializations run depth-first.
    util::Array<Value> staged_arguments_;
    util::Array<std::size_t> staged_marks_;
    bool prepared_ = false;
    util::Array<Value> stack_;
    util::Array<Frame*> frames_;
    std::string error_;
    std::string* sink_ = nullptr;
    std::string input_;
    std::size_t input_offset_ = 0;
    // Strings produced at run time (io.readLine); Values point into these.
    util::Array<std::string*> runtime_strings_;
    std::uint64_t instructions_ = 0;
    std::uint32_t depth_ = 0;
};

// Guard against runaway recursion, including a Procedures block that
// materializes its own class (docs/procedures.md, section 4).
constexpr std::uint32_t kMaxCallDepth = 512;

}  // namespace khu::vm

#endif  // KHU_VM_VM_H
