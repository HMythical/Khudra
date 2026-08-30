#include "host/native_host.h"

#include <cstdio>
#include <cstring>

#include "bytecode/native.h"
#include "bytecode/verifier.h"

namespace khu::native {
namespace {

using khu::bytecode::TypeTag;

NativeHost* g_host = nullptr;

// KhuValue and khu::vm::Value are the same 16 bytes -- tests/unit/test_native_abi
// pins every offset -- but they are unrelated C++ types, so a conversion moves
// the tag and the payload rather than reinterpreting the storage. Reading the
// payload back as `as_uint` is the same union pun the interpreter makes on
// every arithmetic instruction.
Value to_value(const KhuValue& value) {
    Value out;
    out.tag = static_cast<TypeTag>(value.tag);
    out.as_uint = value.v.as_uint;
    return out;
}

KhuValue from_value(const Value& value) {
    KhuValue out;
    out.v.as_uint = value.as_uint;
    out.tag = static_cast<std::uint8_t>(value.tag);
    return out;
}

// The inheritance chain from the root down to `type`.
void collect_chain(RuntimeClass& type, util::Array<RuntimeClass*>& out) {
    if (type.base) collect_chain(*type.base, out);
    out.push(&type);
}

}  // namespace

NativeHost* current_host() { return g_host; }

NativeHost::NativeHost(const bytecode::Module& module, const KhuNativeMethod* methods,
                       std::uint32_t method_count)
    : module_(module), methods_(methods), method_count_(method_count) {}

NativeHost::~NativeHost() {
    if (khu_proc_engine_host() == reinterpret_cast<KhuVmHost*>(this)) khu_proc_engine_shutdown();
    if (g_host == this) g_host = nullptr;
    for (std::string* text : runtime_strings_) delete text;
}

// ---------------------------------------------------------------------------
// The Procedure engine's callback table
// ---------------------------------------------------------------------------

namespace {

NativeHost& as_host(KhuVmHost* host) { return *reinterpret_cast<NativeHost*>(host); }
Object* as_object(KhuObject* object) { return reinterpret_cast<Object*>(object); }
KhuObject* as_handle(Object* object) { return reinterpret_cast<KhuObject*>(object); }

std::uint32_t nh_chain_length(KhuVmHost* h, std::uint32_t id) {
    return as_host(h).proc_chain_length(id);
}
std::uint32_t nh_chain_at(KhuVmHost* h, std::uint32_t id, std::uint32_t index) {
    return as_host(h).proc_chain_at(id, index);
}
KhuObject* nh_allocate(KhuVmHost* h, std::uint32_t id, KhuStrategy strategy) {
    return as_handle(as_host(h).proc_allocate(id, strategy));
}
int nh_has_field_init(KhuVmHost* h, std::uint32_t id) {
    return as_host(h).proc_has_field_init(id) ? 1 : 0;
}
int nh_run_field_init(KhuVmHost* h, KhuObject* o, std::uint32_t id) {
    return as_host(h).proc_run_field_init(as_object(o), id) ? 0 : -1;
}
int nh_take_arguments(KhuVmHost* h, std::uint32_t argc) {
    return as_host(h).proc_take_arguments(argc) ? 0 : -1;
}
void nh_push_arguments(KhuVmHost* h, std::uint32_t argc) { as_host(h).proc_push_arguments(argc); }
void nh_drop_arguments(KhuVmHost* h) { as_host(h).proc_drop_arguments(); }
int nh_has_procedures(KhuVmHost* h, std::uint32_t id) {
    return as_host(h).proc_has_procedures(id) ? 1 : 0;
}
int nh_run_procedures(KhuVmHost* h, KhuObject* o, std::uint32_t id, std::uint32_t argc) {
    return as_host(h).proc_run_procedures(as_object(o), id, argc) ? 0 : -1;
}
int nh_has_constructor(KhuVmHost* h, std::uint32_t id) {
    return as_host(h).proc_has_constructor(id) ? 1 : 0;
}
int nh_run_constructor(KhuVmHost* h, KhuObject* o, std::uint32_t id, std::uint32_t argc) {
    return as_host(h).proc_run_constructor(as_object(o), id, argc) ? 0 : -1;
}
void nh_register_live(KhuVmHost* h, KhuObject* o) { as_host(h).proc_register_live(as_object(o)); }
void nh_discard(KhuVmHost* h, KhuObject* o) { as_host(h).proc_discard(as_object(o)); }
void nh_retain(KhuVmHost* h, KhuObject* o) { as_host(h).proc_retain(as_object(o)); }
void nh_release(KhuVmHost* h, KhuObject* o) { as_host(h).proc_release(as_object(o)); }
void nh_report_error(KhuVmHost* h, const char* message) {
    as_host(h).proc_report_error(message ? message : "materialization failed");
}

const KhuProcHostApi& native_proc_host_api() {
    static const KhuProcHostApi api = {
        &nh_chain_length,   &nh_chain_at,       &nh_allocate,       &nh_has_field_init,
        &nh_run_field_init, &nh_take_arguments, &nh_push_arguments, &nh_drop_arguments,
        &nh_has_procedures, &nh_run_procedures, &nh_has_constructor, &nh_run_constructor,
        &nh_register_live,  &nh_discard,        &nh_retain,         &nh_release,
        &nh_report_error,
    };
    return api;
}

}  // namespace

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

bool NativeHost::prepare() {
    if (prepared_) return true;

    // The same check `khudra run` makes before the interpreter starts: the
    // emitter assumed a verified image when it lowered the operand stack to
    // fixed array indices.
    std::string report;
    if (!bytecode::verify(module_, report)) {
        return trap("this bytecode image is not valid:\n" + report);
    }

    std::string error;
    if (!classes_.load(module_, error)) return trap("cannot load the class table: " + error);

    // LoadConst is an array index in native code, so the pool is pre-tagged
    // here with exactly the switch the interpreter runs per instruction.
    constants_.resize(module_.constants.size());
    for (std::size_t i = 0; i < module_.constants.size(); ++i) {
        const bytecode::Constant& entry = module_.constants[i];
        KhuValue slot;
        std::memset(&slot, 0, sizeof(slot));
        switch (entry.tag) {
            case TypeTag::String:
                slot = from_value(Value::make_string(&entry.text));
                break;
            case TypeTag::Float32:
            case TypeTag::Float64:
                slot = khu_normalize_float(static_cast<std::uint8_t>(entry.tag), entry.as_float);
                break;
            case TypeTag::Bool:
                slot = khu_make_bool(entry.as_uint != 0);
                break;
            case TypeTag::Memory:
                slot = from_value(
                    Value::make_strategy(static_cast<std::uint8_t>(entry.as_uint)));
                break;
            default:
                slot = khu_normalize_int(static_cast<std::uint8_t>(entry.tag), entry.as_uint);
                break;
        }
        constants_[i] = slot;
    }

    // LoadConst indexes this directly from the emitted code.
    khu_rt_constants = constants_.data();

    collector_.set_root_source(this);
    g_host = this;
    khu_proc_engine_init(&native_proc_host_api(), reinterpret_cast<KhuVmHost*>(this));
    prepared_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// Roots and memory
// ---------------------------------------------------------------------------

void NativeHost::enumerate_roots(Collector& collector) {
    for (const Value& value : stack_) collector.mark_value(value);
    for (const Value& value : staged_arguments_) collector.mark_value(value);
    for (const Value& value : native_roots_) collector.mark_value(value);

    // The scanned-locals regions: locals, then the operand-stack entries the
    // frame published as live before it reached this safepoint.
    for (KhuFrame* frame = frames_; frame; frame = frame->parent) {
        collector.mark_value(to_value(frame->receiver));
        std::uint32_t live = frame->local_count + frame->height;
        for (std::uint32_t i = 0; i < live; ++i) {
            collector.mark_value(to_value(frame->slots[i]));
        }
    }
}

Object* NativeHost::allocate(RuntimeClass& type, bool manual) {
    if (!manual && collector_.should_collect()) collector_.collect();
    Object* object = heap_.allocate(type, manual);
    if (!object && !manual) {
        collector_.collect();
        object = heap_.allocate(type, manual);
    }
    return object;
}

void NativeHost::write_barrier(Object* owner, const RuntimeClass& type, std::uint32_t slot,
                               const Value& incoming) {
    if (!owner->is_managed()) return;
    const bytecode::FieldEntry* field = type.field(slot);
    if (!field || field->ref_kind != bytecode::kRefManual) return;

    Value& current = owner->slots()[slot];
    if (current.tag == TypeTag::Ref && current.as_ref && current.as_ref->is_manual() &&
        current.as_ref->header.pin_count > 0) {
        --current.as_ref->header.pin_count;
    }
    if (incoming.tag == TypeTag::Ref && incoming.as_ref && incoming.as_ref->is_manual()) {
        ++incoming.as_ref->header.pin_count;
    }
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

std::string NativeHost::describe_location(const KhuFrame& frame) const {
    std::string path(module_.string_at(module_.source_file));
    const bytecode::MethodEntry* method =
        module_.method_at(static_cast<std::int32_t>(frame.method_index));
    if (!method) return path;

    // The emitted code publishes `ip` past the instruction in flight, so the
    // same back-up-one-byte rule the interpreter uses lands inside it.
    std::uint32_t offset = frame.ip > 0 ? frame.ip - 1 : 0;
    const bytecode::LineEntry* line = module_.line_for(*method, offset);
    if (!line) return path;
    return path + ":" + std::to_string(line->line) + ":" + std::to_string(line->column);
}

bool NativeHost::trap(std::string message) {
    if (!error_.empty()) return false;  // keep the first failure

    std::string text;
    if (frames_) {
        text = describe_location(*frames_) + ": runtime error: " + message + "\n";
    } else {
        text = "khudra: runtime error: " + message + "\n";
    }

    constexpr std::size_t kMaxTraceFrames = 12;
    std::size_t remaining = 0;
    for (KhuFrame* frame = frames_; frame; frame = frame->parent) ++remaining;

    std::size_t shown = 0;
    for (KhuFrame* frame = frames_; frame; frame = frame->parent, --remaining) {
        const bytecode::MethodEntry* method =
            module_.method_at(static_cast<std::int32_t>(frame->method_index));
        if (!method) continue;
        if (shown == kMaxTraceFrames) {
            text += "    ... " + std::to_string(remaining) + " more frames\n";
            break;
        }
        ++shown;
        const bytecode::ClassEntry* owner =
            module_.class_at(static_cast<std::int32_t>(method->owner_class));
        text += "    at ";
        if (owner) text += std::string(module_.string_at(owner->name)) + ".";
        text += std::string(module_.string_at(method->name));
        text += " (" + describe_location(*frame) + ")\n";
    }

    error_ = std::move(text);
    return false;
}

std::string NativeHost::render_value(const Value& value) const {
    if (value.tag == TypeTag::Ref && value.as_ref) {
        const bytecode::ClassEntry* owner =
            module_.class_at(static_cast<std::int32_t>(value.as_ref->header.class_id));
        std::string name = owner ? std::string(module_.string_at(owner->name)) : "object";
        return "<" + name + ">";
    }
    return khu::vm::to_display_string(value);
}

int NativeHost::raise(KhuFrame* frame, const char* message) {
    (void)frame;  // the frame is already linked; trap() walks the chain
    trap(message ? message : "runtime error");
    return 1;
}

// ---------------------------------------------------------------------------
// Frames and calls
// ---------------------------------------------------------------------------

void NativeHost::frame_enter(KhuFrame* frame, KhuValue* slots, std::uint32_t method_index,
                             std::uint32_t local_count, std::uint32_t slot_count,
                             KhuValue receiver, const KhuValue* argv, std::uint32_t argc) {
    // Zeroed slots are Void, which the collector ignores -- so a region is safe
    // to scan from the first instruction, before anything has written to it.
    std::memset(slots, 0, static_cast<std::size_t>(slot_count) * sizeof(KhuValue));
    for (std::uint32_t i = 0; i < argc && i < local_count; ++i) slots[i] = argv[i];

    frame->slots = slots;
    frame->receiver = receiver;
    frame->method_index = method_index;
    frame->local_count = local_count;
    frame->height = 0;
    frame->ip = 0;
    frame->parent = frames_;
    frames_ = frame;
}

void NativeHost::frame_leave(KhuFrame* frame) {
    if (frames_ == frame) frames_ = frame->parent;
}

bool NativeHost::call_method(std::int32_t method_index, Value receiver, const KhuValue* argv,
                             std::uint32_t argc, Value& result) {
    const bytecode::MethodEntry* method = module_.method_at(method_index);
    if (!method) return trap("call to unknown method #" + std::to_string(method_index));

    if (depth_ >= kMaxCallDepth) {
        return trap("call depth limit of " + std::to_string(kMaxCallDepth) +
                    " exceeded (runaway recursion?)");
    }
    if (method_index < 0 || static_cast<std::uint32_t>(method_index) >= method_count_ ||
        !methods_[method_index]) {
        return trap("method #" + std::to_string(method_index) +
                    " was not lowered into this native image");
    }

    KhuValue out;
    std::memset(&out, 0, sizeof(out));

    // The lowered function reads exactly `param_count` arguments, because that
    // is what it was compiled to do. A call site that supplied a different
    // number -- an invokevirtual whose argc disagrees with the resolved method
    // -- gets a padded frame rather than a read past the caller's slots. The
    // interpreter has the same shape: call_method pops param_count values
    // whatever the site said.
    util::Array<KhuValue> padded;
    if (argc < method->param_count) {
        padded.resize(method->param_count);
        std::memset(padded.data(), 0, padded.size() * sizeof(KhuValue));
        for (std::uint32_t i = 0; i < argc; ++i) padded[i] = argv[i];
        argv = padded.data();
    }

    ++depth_;
    int status = methods_[method_index](from_value(receiver), argv, &out);
    --depth_;
    if (status != 0) return false;

    result = to_value(out);
    return true;
}

bool NativeHost::call_method_from_stack(std::int32_t method_index, Value receiver, Value& result) {
    const bytecode::MethodEntry* method = module_.method_at(method_index);
    if (!method) return trap("call to unknown method #" + std::to_string(method_index));

    // Arguments were pushed left to right, so they come off in reverse.
    util::Array<KhuValue> argv;
    argv.resize(method->param_count);
    for (std::size_t i = method->param_count; i > 0; --i) {
        Value value;
        if (!stack_.empty()) {
            value = stack_.back();
            stack_.pop();
        }
        argv[i - 1] = from_value(value);
    }
    return call_method(method_index, receiver, argv.data(), method->param_count, result);
}

int NativeHost::call_direct(KhuFrame* frame, std::uint16_t method_index, const KhuValue* receiver,
                            const KhuValue* argv, KhuValue* out) {
    (void)frame;
    const bytecode::MethodEntry* callee =
        module_.method_at(static_cast<std::int32_t>(method_index));
    if (!callee) {
        trap("call to unknown method #" + std::to_string(method_index));
        return 1;
    }
    // `receiver` aliases `out`, so it is read before anything is written back.
    Value self = to_value(*receiver);
    Value returned;
    if (!call_method(static_cast<std::int32_t>(method_index), self, argv, callee->param_count,
                     returned)) {
        return 1;
    }
    if (callee->return_type != TypeTag::Void) *out = from_value(returned);
    return 0;
}

int NativeHost::call_virtual(KhuFrame* frame, std::uint16_t slot, std::uint8_t argc,
                             const KhuValue* receiver, const KhuValue* argv, KhuValue* out,
                             bool expects_result) {
    (void)frame;
    Value self = to_value(*receiver);

    if (self.is_null_reference()) {
        trap("cannot call a method on null: the object is not instantiated yet");
        return 1;
    }
    if (self.tag != TypeTag::Ref || !self.as_ref) {
        trap("a method call expects an object, found " +
             std::string(bytecode::type_tag_name(self.tag)));
        return 1;
    }

    // A derived override replaced the inherited entry when the subclass's
    // vtable was built, so the receiver's own table is the whole of dispatch.
    auto* type = static_cast<RuntimeClass*>(self.as_ref->header.vtable);
    if (!type || slot >= type->vtable.size()) {
        trap("vtable slot " + std::to_string(slot) + " is out of range for class '" +
             (type ? std::string(type->name) : std::string("?")) + "'");
        return 1;
    }
    const bytecode::MethodEntry* callee = type->vtable[slot];
    bool has_result = callee && callee->return_type != TypeTag::Void;
    if (has_result != expects_result) {
        trap("vtable slot " + std::to_string(slot) + " of class '" + std::string(type->name) +
             "' returns a different kind than the native lowering assumed");
        return 1;
    }

    Value returned;
    if (!call_method(type->vtable_indices[slot], self, argv, argc, returned)) return 1;
    if (has_result) *out = from_value(returned);
    return 0;
}

int NativeHost::call_native(KhuFrame* frame, std::uint16_t native_id, std::uint8_t argc,
                            const KhuValue* argv, KhuValue* out, bool expects_result) {
    (void)frame;
    auto id = static_cast<bytecode::NativeId>(native_id);

    switch (id) {
        case bytecode::NativeId::Print:
        case bytecode::NativeId::PrintLine: {
            // The interpreter builds the text by popping and prepending, which
            // is the same order as walking the arguments as they were pushed.
            std::string text;
            for (std::uint8_t i = 0; i < argc; ++i) text += render_value(to_value(argv[i]));
            if (id == bytecode::NativeId::PrintLine) text += "\n";
            if (sink_) {
                *sink_ += text;
            } else {
                std::fwrite(text.data(), 1, text.size(), stdout);
            }
            return 0;
        }

        case bytecode::NativeId::ReadLine: {
            auto* line = new std::string();
            if (!input_.empty() || input_offset_ > 0) {
                std::size_t end = input_.find('\n', input_offset_);
                if (end == std::string::npos) end = input_.size();
                *line = input_.substr(input_offset_, end - input_offset_);
                input_offset_ = end < input_.size() ? end + 1 : input_.size();
            } else {
                char buffer[4096];
                if (std::fgets(buffer, sizeof(buffer), stdin)) {
                    *line = buffer;
                    while (!line->empty() && (line->back() == '\n' || line->back() == '\r')) {
                        line->pop_back();
                    }
                }
            }
            runtime_strings_.push(line);
            *out = from_value(Value::make_string(line));
            return 0;
        }

        case bytecode::NativeId::StdlibLoadObject:
            return 0;

        case bytecode::NativeId::GetType:
        case bytecode::NativeId::LoadRuntimeType:
            *out = khu_make_null();
            return 0;

        case bytecode::NativeId::None:
            break;
    }

    (void)expects_result;
    trap("unknown native function id " + std::to_string(native_id));
    return 1;
}

// ---------------------------------------------------------------------------
// Fields, allocation, memory opcodes
// ---------------------------------------------------------------------------

int NativeHost::getfield(KhuFrame* frame, const KhuValue* receiver, std::uint16_t slot,
                         KhuValue* out) {
    (void)frame;
    Value self = to_value(*receiver);
    if (self.is_null_reference()) {
        trap("cannot read a field of null: the object is not instantiated yet");
        return 1;
    }
    if (self.tag != TypeTag::Ref || !self.as_ref) {
        trap("getfield expects an object, found " +
             std::string(bytecode::type_tag_name(self.tag)));
        return 1;
    }
    Object* object = self.as_ref;
    auto* type = static_cast<RuntimeClass*>(object->header.vtable);
    if (!type || slot >= type->slot_count) {
        trap("field slot " + std::to_string(slot) + " is out of range");
        return 1;
    }
    *out = from_value(object->slots()[slot]);
    return 0;
}

int NativeHost::putfield(KhuFrame* frame, const KhuValue* receiver, std::uint16_t slot,
                         const KhuValue* value) {
    (void)frame;
    Value self = to_value(*receiver);
    Value incoming = to_value(*value);
    if (self.is_null_reference()) {
        trap("cannot assign a field of null: the object is not instantiated yet");
        return 1;
    }
    if (self.tag != TypeTag::Ref || !self.as_ref) {
        trap("putfield expects an object, found " +
             std::string(bytecode::type_tag_name(self.tag)));
        return 1;
    }
    Object* object = self.as_ref;
    auto* type = static_cast<RuntimeClass*>(object->header.vtable);
    if (!type || slot >= type->slot_count) {
        trap("field slot " + std::to_string(slot) + " is out of range");
        return 1;
    }
    write_barrier(object, *type, slot, incoming);
    object->slots()[slot] = incoming;
    return 0;
}

int NativeHost::alloc(KhuFrame* frame, std::uint16_t class_id, bool manual, KhuValue* out) {
    (void)frame;
    RuntimeClass* type = classes_.at(class_id);
    if (!type) {
        trap("unknown class id " + std::to_string(class_id));
        return 1;
    }
    Object* object = allocate(*type, manual);
    if (!object) {
        trap("out of memory allocating '" + std::string(type->name) + "'");
        return 1;
    }
    *out = from_value(Value::make_ref(object));
    return 0;
}

int NativeHost::free_object(KhuFrame* frame, const KhuValue* target) {
    (void)frame;
    return release_manual(to_value(*target)) ? 0 : 1;
}

int NativeHost::pin(KhuFrame* frame, const KhuValue* target, bool pin_it) {
    (void)frame;
    Value value = to_value(*target);
    if (value.is_null_reference()) return 0;  // nothing to pin
    if (value.tag != TypeTag::Ref || !value.as_ref) {
        trap(std::string(pin_it ? "pin" : "unpin") + " expects an object");
        return 1;
    }
    std::uint32_t& count = value.as_ref->header.pin_count;
    if (pin_it) {
        ++count;
    } else if (count > 0) {
        --count;
    }
    return 0;
}

int NativeHost::ref_same(const KhuValue* left_in, const KhuValue* right_in) {
    Value left = to_value(*left_in);
    Value right = to_value(*right_in);
    if (left.tag == TypeTag::String && right.tag == TypeTag::String) {
        // String literals compare by contents; there is no interning guarantee
        // across constant pools.
        return left.as_text && right.as_text && *left.as_text == *right.as_text ? 1 : 0;
    }
    if (left.is_null_reference() || right.is_null_reference()) {
        return left.is_null_reference() && right.is_null_reference() ? 1 : 0;
    }
    return left.as_ref == right.as_ref ? 1 : 0;
}

int NativeHost::materialize_at(KhuFrame* frame, std::uint16_t class_id, std::uint8_t strategy,
                               const KhuValue* args, std::uint32_t argc, KhuValue* out) {
    (void)frame;
    // The engine takes its arguments off the host's handshake stack, exactly
    // where the interpreter leaves them on its operand stack.
    for (std::uint32_t i = 0; i < argc; ++i) stack_.push(to_value(args[i]));

    Value created;
    if (!materialize(class_id, static_cast<bytecode::StrategyByte>(strategy), created)) return 1;
    *out = from_value(created);
    return 0;
}

bool NativeHost::release_manual(Value target) {
    if (target.is_null_reference()) {
        return trap("cannot free null: the object is not instantiated yet");
    }
    if (target.tag != TypeTag::Ref || !target.as_ref) {
        return trap("free expects a manually allocated object, found " +
                    std::string(bytecode::type_tag_name(target.tag)));
    }

    Object* object = target.as_ref;
    auto* type = static_cast<RuntimeClass*>(object->header.vtable);
    std::string name = type ? std::string(type->name) : std::string("object");

    if (object->is_managed()) {
        return trap("cannot free '" + name + "': it is garbage collected");
    }
    if (!heap_.is_tracked_manual(object)) {
        return trap("cannot free this object: it was already released");
    }
    if (object->header.pin_count > 0) {
        util::Array<std::string> holders = collector_.find_holders(object);
        std::string message = "cannot free '" + name + "': it is still held by ";
        if (holders.empty()) {
            message += std::to_string(object->header.pin_count) + " managed slot" +
                       (object->header.pin_count == 1 ? "" : "s");
        } else {
            for (std::size_t i = 0; i < holders.size(); ++i) {
                if (i != 0) message += ", ";
                message += "'" + holders[i] + "'";
            }
        }
        message += "; clear the reference before releasing the object";
        return trap(message);
    }
    if (!heap_.free_manual(object)) {
        return trap("cannot free '" + name + "': it was already released");
    }
    return true;
}

// ---------------------------------------------------------------------------
// Materialization -- the engine drives, the host only supplies the steps
// ---------------------------------------------------------------------------

std::uint32_t NativeHost::proc_chain_length(std::uint32_t class_id) {
    RuntimeClass* type = classes_.at(class_id);
    std::uint32_t length = 0;
    for (RuntimeClass* step = type; step; step = step->base) ++length;
    return length;
}

std::uint32_t NativeHost::proc_chain_at(std::uint32_t class_id, std::uint32_t index) {
    RuntimeClass* type = classes_.at(class_id);
    if (!type) return class_id;
    util::Array<RuntimeClass*> chain;
    collect_chain(*type, chain);
    if (index >= chain.size()) return class_id;
    return chain[index]->class_id;
}

Object* NativeHost::proc_allocate(std::uint32_t class_id, KhuStrategy strategy) {
    RuntimeClass* type = classes_.at(class_id);
    if (!type) {
        trap("unknown class id " + std::to_string(class_id));
        return nullptr;
    }
    bool manual = strategy == KHU_STRATEGY_MANUAL ||
                  (strategy == KHU_STRATEGY_CLASS_DEFAULT && type->manual);
    Object* object = allocate(*type, manual);
    if (!object) trap("out of memory materializing '" + std::string(type->name) + "'");
    return object;
}

bool NativeHost::proc_has_field_init(std::uint32_t class_id) {
    RuntimeClass* type = classes_.at(class_id);
    return type && type->field_init_index >= 0;
}

bool NativeHost::proc_run_field_init(Object* object, std::uint32_t class_id) {
    RuntimeClass* type = classes_.at(class_id);
    if (!type || type->field_init_index < 0) return true;
    Value ignored;
    return call_method_from_stack(type->field_init_index, Value::make_ref(object), ignored);
}

bool NativeHost::proc_take_arguments(std::uint32_t argc) {
    if (stack_.size() < argc) {
        trap("allocation site supplied fewer arguments than the class binds");
        return false;
    }
    staged_marks_.push(staged_arguments_.size());
    std::size_t base = staged_arguments_.size();
    staged_arguments_.resize(base + argc);
    for (std::size_t i = argc; i > 0; --i) {
        staged_arguments_[base + i - 1] = stack_.back();
        stack_.pop();
    }
    return true;
}

void NativeHost::proc_push_arguments(std::uint32_t argc) {
    if (staged_marks_.empty()) return;
    std::size_t base = staged_marks_.back();
    for (std::uint32_t i = 0; i < argc && base + i < staged_arguments_.size(); ++i) {
        stack_.push(staged_arguments_[base + i]);
    }
}

void NativeHost::proc_drop_arguments() {
    if (staged_marks_.empty()) return;
    staged_arguments_.resize(staged_marks_.back());
    staged_marks_.pop();
}

bool NativeHost::proc_has_procedures(std::uint32_t class_id) {
    RuntimeClass* type = classes_.at(class_id);
    return type && type->procedures_index >= 0;
}

bool NativeHost::proc_run_procedures(Object* object, std::uint32_t class_id, std::uint32_t argc) {
    (void)argc;
    RuntimeClass* type = classes_.at(class_id);
    if (!type || type->procedures_index < 0) return true;
    Value ignored;
    return call_method_from_stack(type->procedures_index, Value::make_ref(object), ignored);
}

bool NativeHost::proc_has_constructor(std::uint32_t class_id) {
    RuntimeClass* type = classes_.at(class_id);
    return type && type->constructor_index >= 0;
}

bool NativeHost::proc_run_constructor(Object* object, std::uint32_t class_id, std::uint32_t argc) {
    (void)argc;
    RuntimeClass* type = classes_.at(class_id);
    if (!type || type->constructor_index < 0) return true;
    Value ignored;
    return call_method_from_stack(type->constructor_index, Value::make_ref(object), ignored);
}

void NativeHost::proc_register_live(Object* object) {
    if (object) object->header.flags |= khu::vm::kObjectMaterialized;
}

void NativeHost::proc_discard(Object* object) {
    if (!object) return;
    if (object->is_manual() && object->header.pin_count == 0) heap_.free_manual(object);
}

void NativeHost::proc_retain(Object* object) {
    if (object) native_roots_.push(Value::make_ref(object));
}

void NativeHost::proc_release(Object* object) {
    (void)object;
    if (!native_roots_.empty()) native_roots_.pop();
}

void NativeHost::proc_report_error(const char* message) { trap(message); }

bool NativeHost::materialize(std::uint32_t class_id, bytecode::StrategyByte strategy, Value& out) {
    RuntimeClass* type = classes_.at(class_id);
    if (!type) return trap("unknown class id " + std::to_string(class_id));

    KhuStrategy engine_strategy = strategy == bytecode::StrategyByte::Manual
                                      ? KHU_STRATEGY_MANUAL
                                      : (strategy == bytecode::StrategyByte::Gc
                                             ? KHU_STRATEGY_GC
                                             : KHU_STRATEGY_CLASS_DEFAULT);

    KhuObject* created = nullptr;
    KhuProcStatus status =
        khu_proc_materialize(class_id, engine_strategy, type->materialize_argc, &created);
    if (status != KHU_PROC_OK) {
        if (!has_error()) {
            return trap("cannot materialize '" + std::string(type->name) + "': " +
                        khu_proc_status_name(status));
        }
        return false;
    }

    out = Value::make_ref(reinterpret_cast<Object*>(created));
    return true;
}

// ---------------------------------------------------------------------------
// Entry dispatch
// ---------------------------------------------------------------------------

bool NativeHost::run() {
    if (!prepare()) return false;

    if (module_.main_method >= 0) {
        const bytecode::MethodEntry* main = module_.method_at(module_.main_method);
        if (!main) return trap("the image names a missing entry point");

        Value receiver;
        if (!materialize(main->owner_class, bytecode::StrategyByte::ClassDefault, receiver)) {
            return false;
        }
        Value result;
        return call_method_from_stack(module_.main_method, receiver, result);
    }

    if (module_.root_class >= 0) {
        Value root;
        return materialize(static_cast<std::uint32_t>(module_.root_class),
                           bytecode::StrategyByte::ClassDefault, root);
    }

    return trap("this image has no entry point");
}

}  // namespace khu::native

// ---------------------------------------------------------------------------
// The C ABI the emitted translation unit links against
// ---------------------------------------------------------------------------

extern "C" {

const KhuValue* khu_rt_constants = nullptr;

void khu_rt_frame_enter(KhuFrame* frame, KhuValue* slots, uint32_t method_index,
                        uint32_t local_count, uint32_t slot_count, KhuValue receiver,
                        const KhuValue* argv, uint32_t argc) {
    khu::native::current_host()->frame_enter(frame, slots, method_index, local_count, slot_count,
                                             receiver, argv, argc);
}

void khu_rt_frame_leave(KhuFrame* frame) { khu::native::current_host()->frame_leave(frame); }

int khu_rt_getfield(KhuFrame* frame, const KhuValue* receiver, uint16_t slot, KhuValue* out) {
    return khu::native::current_host()->getfield(frame, receiver, slot, out);
}

int khu_rt_putfield(KhuFrame* frame, const KhuValue* receiver, uint16_t slot,
                    const KhuValue* value) {
    return khu::native::current_host()->putfield(frame, receiver, slot, value);
}

int khu_rt_materialize(KhuFrame* frame, uint16_t class_id, uint8_t strategy, const KhuValue* args,
                       uint32_t argc, KhuValue* out) {
    return khu::native::current_host()->materialize_at(frame, class_id, strategy, args, argc, out);
}

int khu_rt_alloc(KhuFrame* frame, uint16_t class_id, int manual, KhuValue* out) {
    return khu::native::current_host()->alloc(frame, class_id, manual != 0, out);
}

int khu_rt_free(KhuFrame* frame, const KhuValue* target) {
    return khu::native::current_host()->free_object(frame, target);
}

int khu_rt_pin(KhuFrame* frame, const KhuValue* target, int pin) {
    return khu::native::current_host()->pin(frame, target, pin != 0);
}

int khu_rt_call_direct(KhuFrame* frame, uint16_t method_index, const KhuValue* receiver,
                       const KhuValue* argv, KhuValue* out) {
    return khu::native::current_host()->call_direct(frame, method_index, receiver, argv, out);
}

int khu_rt_call_virtual(KhuFrame* frame, uint16_t slot, uint8_t argc, const KhuValue* receiver,
                        const KhuValue* argv, KhuValue* out, int expects_result) {
    return khu::native::current_host()->call_virtual(frame, slot, argc, receiver, argv, out,
                                                     expects_result != 0);
}

int khu_rt_call_native(KhuFrame* frame, uint16_t native_id, uint8_t argc, const KhuValue* argv,
                       KhuValue* out, int expects_result) {
    return khu::native::current_host()->call_native(frame, native_id, argc, argv, out,
                                                    expects_result != 0);
}

int khu_rt_ref_same(const KhuValue* left, const KhuValue* right) {
    return khu::native::current_host()->ref_same(left, right);
}

int khu_rt_trap(KhuFrame* frame, const char* message) {
    return khu::native::current_host()->raise(frame, message);
}

}  // extern "C"
