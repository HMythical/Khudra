// The Khudra virtual machine.
//
// A stack interpreter over the .kbc image: a shared value stack, one frame per
// active call, and a switch over the opcode. Object memory, the collector and
// the Procedure engine plug in behind this core.
#ifndef KHU_VM_VM_H
#define KHU_VM_VM_H

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include "proc_engine.h"
}

#include "bytecode/module.h"
#include "gc/collector.h"
#include "util/array.h"
#include "vm/class_table.h"
#include "vm/heap.h"
#include "vm/natives.h"
#include "vm/object.h"
#include "vm/runtime_strings.h"
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

    // `khuStdSystem.exit(code)` unwinds the way a trap does, so `run()` answers
    // false for both. These are what tell them apart: an exit is not an error,
    // and the code it names is the program's own.
    bool exit_requested() const { return services_.system.exit_requested; }
    int exit_code() const { return services_.system.exit_code; }

    // The program's own arguments -- everything after `khudra run FILE --`.
    // `khuStdSystem.argc()`/`argv()` read these.
    void set_program_args(std::vector<std::string> args) {
        services_.system.program_args = std::move(args);
    }

    // Sends io.* output to `sink` instead of stdout. Used by the tests and by
    // `khudra run` when it needs to capture a program's output.
    void set_output_sink(std::string* sink) { sink_ = sink; }
    // The same for the standard error stream (fd 2), which khuStdErr writes to.
    // The two are captured separately because the invariant compares them
    // separately: a program's stderr has to match across backends the way its
    // stdout does.
    void set_error_sink(std::string* sink) { err_sink_ = sink; }
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
    // What vm/natives.h asks of a backend. Nested so it reaches the sinks, the
    // string store and render_value without widening any of them.
    class Services final : public NativeServices {
    public:
        explicit Services(Vm& vm) : vm_(vm) {}
        void write_output(const std::string& text) override;
        void write_error(const std::string& text) override;
        const std::string* make_string(std::string text) override;
        void flush_output() override;
        void flush_error() override;
        std::string read_line() override;
        int read_byte() override;
        std::string render(const Value& value) const override;
        bool materialize(std::string_view class_name, Value& out) override;
        bool invoke(const Value& receiver, std::string_view method_name, const Value* args,
                    std::uint32_t argc, Value& out) override;
        void push_root(const Value& value) override;
        void pop_root() override;

    private:
        Vm& vm_;
    };

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
    // Name lookups the system natives need to build an object: a native names a
    // class and a method in text, because it has no image index to name them
    // with. Both are linear scans over tables a program has only a few hundred
    // entries in, and both happen once per `argv()`-shaped call, not in a loop.
    std::int32_t class_id_of(std::string_view name) const;
    std::int32_t method_index_of(const RuntimeClass& type, std::string_view name,
                                 std::uint32_t argc) const;
    // The array instructions' shared checks and their write barrier.
    bool array_operand(const Value& target, Object*& array);
    bool array_index_in_range(const Object& array, std::int64_t index);
    static void array_write_barrier(const Value& current, const Value& incoming);
    // `pointer[index]` at `element`: reads into `out`, or writes `incoming`.
    bool pointer_element(const Value& pointer, const Value& index, TypeTag element, Value* out,
                         const Value* incoming);
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
    std::string* err_sink_ = nullptr;
    std::string input_;
    std::size_t input_offset_ = 0;
    // Strings produced at run time rather than read from the constant pool;
    // Values point into these.
    RuntimeStringStore runtime_strings_;
    Services services_{*this};
    std::uint64_t instructions_ = 0;
    std::uint32_t depth_ = 0;
};

// Guard against runaway recursion, including a Procedures block that
// materializes its own class (docs/procedures.md, section 4).
constexpr std::uint32_t kMaxCallDepth = 512;

}  // namespace khu::vm

#endif  // KHU_VM_VM_H
