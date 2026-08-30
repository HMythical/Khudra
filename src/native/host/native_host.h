// The native host.
//
// Native code still needs everything the interpreter needed apart from the
// dispatcher: a heap, a collector, the Procedure engine, io. This is that
// half. It owns the same `Heap` and `Collector` the VM owns, implements the
// same `RootSource` and the same `KhuProcHostApi`, and reproduces the
// interpreter's trap text down to the source position -- so a program's
// stdout, stderr and exit code do not depend on which backend ran it.
//
// Roots (docs/native.md, section 4): a native frame is a `KhuFrame` the emitted
// function put on its own C stack, holding a pointer to its scanned-locals
// region. Collections happen only at safepoints -- allocation sites and call
// boundaries -- and the emitted code publishes its live stack height and its
// bytecode position immediately before each one, so `enumerate_roots` walks
// exactly the live references rather than guessing at machine registers.
#ifndef KHU_NATIVE_HOST_NATIVE_HOST_H
#define KHU_NATIVE_HOST_NATIVE_HOST_H

#include <cstdint>
#include <string>

extern "C" {
#include "proc_engine.h"
}

#include "bytecode/module.h"
#include "gc/collector.h"
#include "host/khu_native_abi.h"
#include "util/array.h"
#include "vm/class_table.h"
#include "vm/heap.h"
#include "vm/object.h"
#include "vm/value.h"

namespace khu::native {

using khu::vm::Collector;
using khu::vm::Heap;
using khu::vm::Object;
using khu::vm::RuntimeClass;
using khu::vm::Value;

// The same guard KhudraVm applies, including to a Procedures block that
// materializes its own class. Declared here rather than taken from vm.h: the
// native host must not depend on the interpreter's header, and
// tests/unit/test_native_abi asserts the two stay equal.
constexpr std::uint32_t kMaxCallDepth = 512;

class NativeHost : public khu::vm::RootSource {
public:
    NativeHost(const bytecode::Module& module, const KhuNativeMethod* methods,
               std::uint32_t method_count);
    NativeHost(const NativeHost&) = delete;
    NativeHost& operator=(const NativeHost&) = delete;
    ~NativeHost() override;

    // Verifies the image, loads the class table, builds the constant pool and
    // installs itself as the Procedure engine's host.
    bool prepare();

    // The image's entry point: `main` when it has one, otherwise root-class
    // materialization -- the same rule KhudraVm::run applies.
    bool run();

    bool has_error() const { return !error_.empty(); }
    const std::string& error() const { return error_; }

    void set_output_sink(std::string* sink) { sink_ = sink; }
    void set_input(std::string input) { input_ = std::move(input); }

    const Heap& heap() const { return heap_; }
    Collector& collector() { return collector_; }

    void enumerate_roots(Collector& collector) override;

    // --- the runtime the emitted C calls (see khu_native_abi.h) ---
    void frame_enter(KhuFrame* frame, KhuValue* slots, std::uint32_t method_index,
                     std::uint32_t local_count, std::uint32_t slot_count, KhuValue receiver,
                     const KhuValue* argv, std::uint32_t argc);
    void frame_leave(KhuFrame* frame);
    int getfield(KhuFrame* frame, const KhuValue* receiver, std::uint16_t slot, KhuValue* out);
    int putfield(KhuFrame* frame, const KhuValue* receiver, std::uint16_t slot,
                 const KhuValue* value);
    int materialize_at(KhuFrame* frame, std::uint16_t class_id, std::uint8_t strategy,
                       const KhuValue* args, std::uint32_t argc, KhuValue* out);
    int alloc(KhuFrame* frame, std::uint16_t class_id, bool manual, KhuValue* out);
    int free_object(KhuFrame* frame, const KhuValue* target);
    int pin(KhuFrame* frame, const KhuValue* target, bool pin_it);
    int call_direct(KhuFrame* frame, std::uint16_t method_index, const KhuValue* receiver,
                    const KhuValue* argv, KhuValue* out);
    int call_virtual(KhuFrame* frame, std::uint16_t slot, std::uint8_t argc,
                     const KhuValue* receiver, const KhuValue* argv, KhuValue* out,
                     bool expects_result);
    int call_native(KhuFrame* frame, std::uint16_t native_id, std::uint8_t argc,
                    const KhuValue* argv, KhuValue* out, bool expects_result);
    int ref_same(const KhuValue* left, const KhuValue* right);
    int raise(KhuFrame* frame, const char* message);

    const KhuValue* constants() const { return constants_.data(); }

    // --- Procedure engine host (utils/proc_engine.h) ---
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
    // Invokes one lowered method. `argv` is the arguments in declaration order.
    bool call_method(std::int32_t method_index, Value receiver, const KhuValue* argv,
                     std::uint32_t argc, Value& result);
    // The same, taking the arguments off the handshake stack the way
    // KhudraVm::call_method takes them off the interpreter's.
    bool call_method_from_stack(std::int32_t method_index, Value receiver, Value& result);

    bool materialize(std::uint32_t class_id, bytecode::StrategyByte strategy, Value& out);
    bool release_manual(Value target);
    Object* allocate(RuntimeClass& type, bool manual);
    void write_barrier(Object* owner, const RuntimeClass& type, std::uint32_t slot,
                       const Value& incoming);

    bool trap(std::string message);
    std::string describe_location(const KhuFrame& frame) const;
    std::string render_value(const Value& value) const;

    const bytecode::Module& module_;
    const KhuNativeMethod* methods_;
    std::uint32_t method_count_;

    khu::vm::ClassTable classes_;
    Heap heap_;
    Collector collector_{heap_, classes_};

    // The constant pool, pre-tagged exactly as LoadConst would push it.
    util::Array<KhuValue> constants_;

    // The Procedure engine handshake: allocation-site arguments live here
    // between the materialization site and the block that binds them, the same
    // way they live on the interpreter's operand stack.
    util::Array<Value> stack_;
    util::Array<Value> staged_arguments_;
    util::Array<std::size_t> staged_marks_;
    util::Array<Value> native_roots_;

    KhuFrame* frames_ = nullptr;
    std::uint32_t depth_ = 0;
    bool prepared_ = false;

    std::string error_;
    std::string* sink_ = nullptr;
    std::string input_;
    std::size_t input_offset_ = 0;
    util::Array<std::string*> runtime_strings_;
};

// The host the `khu_rt_*` shims route to. One at a time, like the Procedure
// engine's host.
NativeHost* current_host();

}  // namespace khu::native

#endif  // KHU_NATIVE_HOST_NATIVE_HOST_H
