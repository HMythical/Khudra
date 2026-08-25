// The Khudra virtual machine.
//
// A stack interpreter over the .kbc image: a shared value stack, one frame per
// active call, and a switch over the opcode. Object memory, the collector and
// the Procedure engine plug in behind this core.
#ifndef KHU_VM_VM_H
#define KHU_VM_VM_H

#include <cstdint>
#include <string>

#include "bytecode/module.h"
#include "util/array.h"
#include "vm/class_table.h"
#include "vm/heap.h"
#include "vm/object.h"
#include "vm/value.h"

namespace khu::vm {

class Vm {
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
    bool run_field_initializers(RuntimeClass& type, Object* object);
    bool release_manual(Value target);

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
