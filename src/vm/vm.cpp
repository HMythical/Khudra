#include "vm/vm.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "bytecode/native.h"
#include "bytecode/verifier.h"
#include "vm/proc_host.h"

namespace khu::vm {

using bytecode::Op;

// The width rules -- wrapping overflow and 32-bit float rounding -- live in
// vm/value.h, shared with the natives so the two cannot drift.

Vm::Vm(const bytecode::Module& module) : module_(module) {}

bool Vm::prepare() {
    if (prepared_) return true;

    // The interpreter is written assuming its operands are in range, so the
    // image is checked once here rather than on every instruction.
    std::string report;
    if (!bytecode::verify(module_, report)) {
        return trap("this bytecode image is not valid:\n" + report);
    }

    std::string error;
    if (!classes_.load(module_, error)) return trap("cannot load the class table: " + error);
    collector_.set_root_source(this);
    install_proc_host(*this);
    prepared_ = true;
    return true;
}

void Vm::enumerate_roots(Collector& collector) {
    for (const Value& value : stack_) collector.mark_value(value);
    for (Frame* frame : frames_) {
        if (!frame) continue;
        collector.mark_value(frame->receiver);
        for (const Value& local : frame->locals) collector.mark_value(local);
    }
    for (const Value& value : native_roots_) collector.mark_value(value);
    for (const Value& value : staged_arguments_) collector.mark_value(value);
}

// Allocation is the only thing that triggers a cycle, and it happens with every
// root reachable: the operand stack, the frames, and native_roots_ for anything
// a half-finished materialization is holding.
Object* Vm::allocate(RuntimeClass& type, bool manual) {
    if (!manual && collector_.should_collect()) collector_.collect();
    Object* object = heap_.allocate(type, manual);
    if (!object && !manual) {
        // Out of memory is worth one more cycle before giving up.
        collector_.collect();
        object = heap_.allocate(type, manual);
    }
    return object;
}

// docs/memory-model.md, section 4.2: assigning a manual object into a managed
// slot increments its pin count; overwriting or dropping that slot decrements.
void Vm::write_barrier(Object* owner, const RuntimeClass& type, std::uint32_t slot,
                       const Value& incoming) {
    if (!owner->is_managed()) return;  // manual -> manual is plain ownership
    const bytecode::FieldEntry* field = type.field(slot);
    // A slot declared to hold a manual reference always pins; an erased slot
    // pins when what is being stored is one, which is what an array does too.
    if (!field) return;
    if (field->ref_kind != bytecode::kRefManual && field->ref_kind != bytecode::kRefDynamic) {
        return;
    }

    Value& current = owner->slots()[slot];
    if (current.tag == TypeTag::Ref && current.as_ref && current.as_ref->is_manual() &&
        current.as_ref->header.pin_count > 0) {
        --current.as_ref->header.pin_count;
    }
    if (incoming.tag == TypeTag::Ref && incoming.as_ref && incoming.as_ref->is_manual()) {
        ++incoming.as_ref->header.pin_count;
    }
}

Vm::~Vm() { uninstall_proc_host(*this); }

Value Vm::pop() {
    if (stack_.empty()) return Value::make_void();
    Value value = stack_.back();
    stack_.pop();
    return value;
}

Value& Vm::top() {
    static Value fallback;
    if (stack_.empty()) return fallback;
    return stack_.back();
}

std::uint8_t Vm::read_u8(Frame& frame) {
    if (frame.ip >= frame.method->code.size()) return 0;
    return frame.method->code[frame.ip++];
}

std::uint16_t Vm::read_u16(Frame& frame) {
    std::uint16_t low = read_u8(frame);
    std::uint16_t high = read_u8(frame);
    return static_cast<std::uint16_t>(low | (high << 8));
}

std::int32_t Vm::read_i32(Frame& frame) {
    std::uint32_t bits = 0;
    for (int i = 0; i < 4; ++i) bits |= static_cast<std::uint32_t>(read_u8(frame)) << (i * 8);
    return static_cast<std::int32_t>(bits);
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

std::string Vm::describe_location(const Frame& frame) const {
    if (!frame.method) return std::string(module_.string_at(module_.source_file));
    // An image is built from the program plus the standard library, so the file
    // a frame names is the method's, not the module's.
    std::string path(module_.string_at(frame.method->source_file != 0
                                           ? frame.method->source_file
                                           : module_.source_file));

    // `ip` already points past the instruction being executed, so back up by
    // one byte to land inside it.
    std::uint32_t offset = frame.ip > 0 ? frame.ip - 1 : 0;
    const bytecode::LineEntry* line = module_.line_for(*frame.method, offset);
    if (!line) return path;
    return path + ":" + std::to_string(line->line) + ":" + std::to_string(line->column);
}

bool Vm::trap(std::string message) {
    if (!error_.empty()) return false;  // keep the first failure

    std::string text;
    if (!frames_.empty()) {
        text = describe_location(*frames_.back()) + ": runtime error: " + message + "\n";
    } else {
        text = "khudra: runtime error: " + message + "\n";
    }

    // A runaway recursion produces hundreds of identical frames; showing the
    // top few and a count is more useful than all of them.
    constexpr std::size_t kMaxTraceFrames = 12;
    std::size_t shown = 0;
    for (std::size_t i = frames_.size(); i > 0; --i) {
        const Frame& frame = *frames_[i - 1];
        if (!frame.method) continue;
        if (shown == kMaxTraceFrames) {
            text += "    ... " + std::to_string(i) + " more frames\n";
            break;
        }
        ++shown;
        const bytecode::ClassEntry* owner =
            module_.class_at(static_cast<std::int32_t>(frame.method->owner_class));
        text += "    at ";
        if (owner) text += std::string(module_.string_at(owner->name)) + ".";
        text += std::string(module_.string_at(frame.method->name));
        text += " (" + describe_location(frame) + ")\n";
    }

    error_ = std::move(text);
    return false;
}

std::string Vm::render_value(const Value& value) const {
    if (value.tag == TypeTag::Ref && value.as_ref) {
        // An array has no class descriptor -- its element type is erased and
        // its class id is not an index into anything -- so it renders as what
        // it is rather than as class #0.
        if (value.as_ref->is_array()) {
            return "<array of " + std::to_string(value.as_ref->array_length()) + ">";
        }
        const bytecode::ClassEntry* owner =
            module_.class_at(static_cast<std::int32_t>(value.as_ref->header.class_id));
        std::string name = owner ? std::string(module_.string_at(owner->name)) : "object";
        return "<" + name + ">";
    }
    return to_display_string(value);
}

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------

bool Vm::arithmetic(Op op, TypeTag tag) {
    if (op == Op::Neg || op == Op::BitNot) {
        Value operand = pop();
        if (bytecode::is_float(tag)) {
            if (op == Op::BitNot) return trap("'~' is not defined for floating point values");
            push(normalize_float(tag, -operand.as_float));
            return true;
        }
        std::uint64_t bits = operand.as_uint;
        push(normalize_int(tag, op == Op::Neg ? (0ull - bits) : ~bits));
        return true;
    }

    Value right = pop();
    Value left = pop();

    if (bytecode::is_float(tag)) {
        double a = left.as_float;
        double b = right.as_float;
        switch (op) {
            case Op::Add: push(normalize_float(tag, a + b)); return true;
            case Op::Sub: push(normalize_float(tag, a - b)); return true;
            case Op::Mul: push(normalize_float(tag, a * b)); return true;
            case Op::Div: push(normalize_float(tag, a / b)); return true;
            case Op::Rem: push(normalize_float(tag, std::fmod(a, b))); return true;
            default:
                return trap(std::string("'") + bytecode::mnemonic(op) +
                            "' is not defined for floating point values");
        }
    }

    bool is_signed = bytecode::is_signed_integer(tag);
    std::uint32_t width = bytecode::type_width(tag);

    switch (op) {
        case Op::Add: push(normalize_int(tag, left.as_uint + right.as_uint)); return true;
        case Op::Sub: push(normalize_int(tag, left.as_uint - right.as_uint)); return true;
        case Op::Mul: push(normalize_int(tag, left.as_uint * right.as_uint)); return true;
        case Op::BitAnd: push(normalize_int(tag, left.as_uint & right.as_uint)); return true;
        case Op::BitOr: push(normalize_int(tag, left.as_uint | right.as_uint)); return true;
        case Op::BitXor: push(normalize_int(tag, left.as_uint ^ right.as_uint)); return true;

        case Op::Div:
        case Op::Rem: {
            if ((right.as_uint & mask_of(width)) == 0) {
                return trap(op == Op::Div ? "division by zero" : "remainder by zero");
            }
            if (is_signed) {
                std::int64_t a = left.as_int;
                std::int64_t b = right.as_int;
                // The one signed division that overflows; wrapping keeps it
                // consistent with every other arithmetic result.
                if (b == -1) {
                    push(normalize_int(tag, op == Op::Div
                                                ? (0ull - static_cast<std::uint64_t>(a))
                                                : 0ull));
                    return true;
                }
                push(normalize_int(tag, static_cast<std::uint64_t>(op == Op::Div ? a / b : a % b)));
                return true;
            }
            std::uint64_t a = left.as_uint & mask_of(width);
            std::uint64_t b = right.as_uint & mask_of(width);
            push(normalize_int(tag, op == Op::Div ? a / b : a % b));
            return true;
        }

        case Op::Shl: {
            std::uint64_t count = right.as_uint % (width ? width : 64);
            push(normalize_int(tag, left.as_uint << count));
            return true;
        }
        case Op::Shr: {
            std::uint64_t count = right.as_uint % (width ? width : 64);
            if (is_signed) {
                std::int64_t value = sign_extend(left.as_uint, width);
                push(normalize_int(tag, static_cast<std::uint64_t>(value >> count)));
            } else {
                push(normalize_int(tag, (left.as_uint & mask_of(width)) >> count));
            }
            return true;
        }

        default:
            return trap(std::string("unsupported arithmetic opcode '") + bytecode::mnemonic(op) +
                        "'");
    }
}

bool Vm::compare(Op op, TypeTag tag) {
    Value right = pop();
    Value left = pop();

    int order = 0;
    if (bytecode::is_float(tag)) {
        double a = left.as_float;
        double b = right.as_float;
        order = a < b ? -1 : (a > b ? 1 : 0);
    } else if (bytecode::is_signed_integer(tag)) {
        std::int64_t a = left.as_int;
        std::int64_t b = right.as_int;
        order = a < b ? -1 : (a > b ? 1 : 0);
    } else {
        std::uint64_t a = left.as_uint;
        std::uint64_t b = right.as_uint;
        order = a < b ? -1 : (a > b ? 1 : 0);
    }

    switch (op) {
        case Op::CmpEq: push(Value::make_bool(order == 0)); return true;
        case Op::CmpNe: push(Value::make_bool(order != 0)); return true;
        case Op::CmpLt: push(Value::make_bool(order < 0)); return true;
        case Op::CmpLe: push(Value::make_bool(order <= 0)); return true;
        case Op::CmpGt: push(Value::make_bool(order > 0)); return true;
        case Op::CmpGe: push(Value::make_bool(order >= 0)); return true;
        default: return trap("unsupported comparison opcode");
    }
}

bool Vm::convert(TypeTag from, TypeTag to) {
    Value value = pop();

    if (!bytecode::is_numeric(from) || !bytecode::is_numeric(to)) {
        return trap(std::string("cannot convert ") + bytecode::type_tag_name(from) + " to " +
                    bytecode::type_tag_name(to));
    }

    if (bytecode::is_float(to)) {
        double result;
        if (bytecode::is_float(from)) {
            result = value.as_float;
        } else if (bytecode::is_signed_integer(from)) {
            result = static_cast<double>(value.as_int);
        } else {
            result = static_cast<double>(value.as_uint);
        }
        push(normalize_float(to, result));
        return true;
    }

    std::uint64_t bits;
    if (bytecode::is_float(from)) {
        // Out-of-range float-to-integer conversions truncate toward zero and
        // then wrap, matching the language's wrapping overflow rule.
        double source = value.as_float;
        if (std::isnan(source)) {
            bits = 0;
        } else if (bytecode::is_signed_integer(to)) {
            bits = static_cast<std::uint64_t>(static_cast<std::int64_t>(source));
        } else {
            bits = source < 0 ? static_cast<std::uint64_t>(static_cast<std::int64_t>(source))
                              : static_cast<std::uint64_t>(source);
        }
    } else if (bytecode::is_signed_integer(from)) {
        bits = static_cast<std::uint64_t>(value.as_int);
    } else {
        bits = value.as_uint;
    }
    push(normalize_int(to, bits));
    return true;
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------

// --- the services vm/natives.h runs a native against -----------------------

void Vm::Services::write_output(const std::string& text) {
    if (vm_.sink_) {
        *vm_.sink_ += text;
    } else {
        std::fwrite(text.data(), 1, text.size(), stdout);
    }
}

void Vm::Services::write_error(const std::string& text) {
    if (vm_.err_sink_) {
        *vm_.err_sink_ += text;
    } else {
        std::fwrite(text.data(), 1, text.size(), stderr);
    }
}

const std::string* Vm::Services::make_string(std::string text) {
    return vm_.runtime_strings_.intern(std::move(text));
}

void Vm::Services::flush_output() {
    if (!vm_.sink_) std::fflush(stdout);
}

void Vm::Services::flush_error() {
    if (!vm_.err_sink_) std::fflush(stderr);
}

int Vm::Services::read_byte() {
    if (!vm_.input_.empty() || vm_.input_offset_ > 0) {
        if (vm_.input_offset_ >= vm_.input_.size()) return -1;
        return static_cast<unsigned char>(vm_.input_[vm_.input_offset_++]);
    }
    int byte = std::fgetc(stdin);
    return byte == EOF ? -1 : byte;
}

std::string Vm::Services::read_line() {
    // A program under test is fed a whole transcript up front; only a real run
    // goes to the terminal.
    if (!vm_.input_.empty() || vm_.input_offset_ > 0) {
        std::size_t end = vm_.input_.find('\n', vm_.input_offset_);
        if (end == std::string::npos) end = vm_.input_.size();
        std::string line = vm_.input_.substr(vm_.input_offset_, end - vm_.input_offset_);
        vm_.input_offset_ = end < vm_.input_.size() ? end + 1 : vm_.input_.size();
        return line;
    }
    std::string line;
    char buffer[4096];
    if (std::fgets(buffer, sizeof(buffer), stdin)) {
        line = buffer;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    }
    return line;
}

std::string Vm::Services::render(const Value& value) const { return vm_.render_value(value); }

// --- producing a Khudra object from a native -------------------------------
//
// These four are the interpreter's half of the capability EXPANSION-PLAN.md
// section 4 adds: a native that answers a `List<string>` runs exactly the
// pipeline `List()` in Khudra source would, so `khuStdSystem.argv()` cannot
// build something the language itself could not.

bool Vm::Services::materialize(std::string_view class_name, Value& out) {
    std::int32_t class_id = vm_.class_id_of(class_name);
    if (class_id < 0) return false;
    return vm_.materialize(static_cast<std::uint32_t>(class_id),
                           bytecode::StrategyByte::ClassDefault, out);
}

bool Vm::Services::invoke(const Value& receiver, std::string_view method_name, const Value* args,
                          std::uint32_t argc, Value& out) {
    if (receiver.tag != TypeTag::Ref || !receiver.as_ref) return false;
    auto* type = static_cast<RuntimeClass*>(receiver.as_ref->header.vtable);
    if (!type) return false;
    std::int32_t index = vm_.method_index_of(*type, method_name, argc);
    if (index < 0) return false;
    // call_method takes its arguments off the operand stack, the way every
    // other call does -- there is no second calling convention here.
    for (std::uint32_t i = 0; i < argc; ++i) vm_.push(args[i]);
    return vm_.call_method(index, receiver, out);
}

void Vm::Services::push_root(const Value& value) { vm_.native_roots_.push(value); }

void Vm::Services::pop_root() {
    if (!vm_.native_roots_.empty()) vm_.native_roots_.pop();
}

bool Vm::call_native(std::uint32_t native_id, std::uint8_t argc) {
    auto id = static_cast<bytecode::NativeId>(native_id);

    // Arguments were pushed left to right, so they come off in reverse; the
    // shared implementation wants them in declaration order.
    util::Array<Value> arguments;
    arguments.resize(argc);
    for (std::size_t i = argc; i > 0; --i) arguments[i - 1] = pop();

    NativeOutcome outcome = invoke_native(services_, id, arguments.data(), argc);
    if (!outcome.ok) return trap(std::move(outcome.trap));
    // `khuStdSystem.exit` leaves through the same door a trap does: the frames
    // unwind and run() answers false. What is different is that error_ stays
    // empty, so the driver reports the program's own code rather than a fault.
    if (services_.system.exit_requested) return false;
    if (bytecode::native_result_count(id) > 0) push(outcome.value);
    return true;
}

std::int32_t Vm::class_id_of(std::string_view name) const {
    for (std::size_t i = 0; i < classes_.size(); ++i) {
        const RuntimeClass* type = classes_.at(static_cast<std::uint32_t>(i));
        if (type && type->name == name) return static_cast<std::int32_t>(i);
    }
    return -1;
}

std::int32_t Vm::method_index_of(const RuntimeClass& type, std::string_view name,
                                 std::uint32_t argc) const {
    for (std::size_t slot = 0; slot < type.vtable.size(); ++slot) {
        const bytecode::MethodEntry* method = type.vtable[slot];
        if (!method || method->param_count != argc) continue;
        if (module_.string_at(method->name) != name) continue;
        return type.vtable_indices[slot];
    }
    return -1;
}

// The three checks every array instruction makes, so their trap text is
// written once.
bool Vm::array_operand(const Value& target, Object*& array) {
    if (target.is_null_reference()) {
        return trap("cannot use an array that is null: it is not instantiated yet");
    }
    if (target.tag != TypeTag::Ref || !target.as_ref || !target.as_ref->is_array()) {
        return trap("expected an array, found " + std::string(bytecode::type_tag_name(target.tag)));
    }
    array = target.as_ref;
    return true;
}

bool Vm::array_index_in_range(const Object& array, std::int64_t index) {
    std::uint32_t length = array.array_length();
    if (index < 0 || static_cast<std::uint64_t>(index) >= length) {
        return trap("array index " + std::to_string(index) + " is out of range for a length of " +
                    std::to_string(length));
    }
    return true;
}

void Vm::array_write_barrier(const Value& current, const Value& incoming) {
    if (current.tag == TypeTag::Ref && current.as_ref && current.as_ref->is_manual() &&
        current.as_ref->header.pin_count > 0) {
        --current.as_ref->header.pin_count;
    }
    if (incoming.tag == TypeTag::Ref && incoming.as_ref && incoming.as_ref->is_manual()) {
        ++incoming.as_ref->header.pin_count;
    }
}

// Reads or writes `pointer[index]` at `element`. Exactly one of `out` and
// `incoming` is non-null.
//
// Nothing bounds-checks this: a `*T` names an address and carries no length,
// which is the whole difference between it and an `Array<T>`. What is checked
// is that there is an address at all.
bool Vm::pointer_element(const Value& pointer, const Value& index, TypeTag element, Value* out,
                         const Value* incoming) {
    if (pointer.is_null_reference() || (pointer.tag == TypeTag::Ptr && !pointer.as_raw)) {
        return trap("cannot reach through a null pointer: it is not instantiated yet");
    }
    if (pointer.tag != TypeTag::Ptr) {
        return trap("expected a raw pointer, found " +
                    std::string(bytecode::type_tag_name(pointer.tag)));
    }
    std::uint32_t width = bytecode::type_width(element);
    if (width == 0) return trap("a raw pointer to a non-numeric type cannot be indexed");
    std::size_t stride = width / 8;
    auto* base = static_cast<unsigned char*>(pointer.as_raw);
    unsigned char* slot = base + static_cast<std::int64_t>(stride) * index.as_int;

    if (out) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, slot, stride);
        if (bytecode::is_float(element)) {
            if (element == TypeTag::Float32) {
                float narrow = 0.0f;
                std::memcpy(&narrow, slot, sizeof(narrow));
                *out = Value::make_float(element, static_cast<double>(narrow));
            } else {
                double wide = 0.0;
                std::memcpy(&wide, slot, sizeof(wide));
                *out = Value::make_float(element, wide);
            }
        } else {
            *out = normalize_int(element, bits);
        }
        return true;
    }

    if (bytecode::is_float(element)) {
        if (element == TypeTag::Float32) {
            auto narrow = static_cast<float>(incoming->as_float);
            std::memcpy(slot, &narrow, sizeof(narrow));
        } else {
            double wide = incoming->as_float;
            std::memcpy(slot, &wide, sizeof(wide));
        }
    } else {
        std::uint64_t bits = incoming->as_uint;
        std::memcpy(slot, &bits, stride);
    }
    return true;
}

bool Vm::call_method(std::int32_t method_index, Value receiver, Value& result) {
    const bytecode::MethodEntry* method = module_.method_at(method_index);
    if (!method) return trap("call to unknown method #" + std::to_string(method_index));

    if (depth_ >= kMaxCallDepth) {
        return trap("call depth limit of " + std::to_string(kMaxCallDepth) +
                    " exceeded (runaway recursion?)");
    }

    Frame frame;
    frame.method = method;
    frame.receiver = receiver;
    frame.locals.resize(method->frame_size);
    // Arguments were pushed left to right, so they come off in reverse.
    for (std::size_t i = method->param_count; i > 0; --i) {
        if (i - 1 < frame.locals.size()) frame.locals[i - 1] = pop();
        else pop();
    }

    ++depth_;
    frames_.push(&frame);
    bool ok = execute(frame, result);
    frames_.pop();
    --depth_;
    return ok;
}

bool Vm::invoke(std::int32_t method_index, Value receiver, Value& result) {
    return call_method(method_index, receiver, result);
}

// ---------------------------------------------------------------------------
// Interpreter
// ---------------------------------------------------------------------------

bool Vm::execute(Frame& frame, Value& result) {
    result = Value::make_void();

    while (true) {
        if (frame.ip >= frame.method->code.size()) return true;  // implicit return
        ++instructions_;

        auto op = static_cast<Op>(read_u8(frame));
        switch (op) {
            case Op::Nop:
                break;

            case Op::Pop:
                pop();
                break;

            case Op::Dup:
                push(top());
                break;

            case Op::DupX1: {
                Value value = pop();
                Value under = pop();
                push(value);
                push(under);
                push(value);
                break;
            }

            case Op::DupX2: {
                Value value = pop();
                Value middle = pop();
                Value under = pop();
                push(value);
                push(under);
                push(middle);
                push(value);
                break;
            }

            case Op::LoadConst: {
                std::uint16_t index = read_u16(frame);
                if (index >= module_.constants.size()) {
                    return trap("constant #" + std::to_string(index) + " is out of range");
                }
                const bytecode::Constant& entry = module_.constants[index];
                switch (entry.tag) {
                    case TypeTag::String:
                        push(Value::make_string(&entry.text));
                        break;
                    case TypeTag::Float32:
                    case TypeTag::Float64:
                        push(normalize_float(entry.tag, entry.as_float));
                        break;
                    case TypeTag::Bool:
                        push(Value::make_bool(entry.as_uint != 0));
                        break;
                    case TypeTag::Memory:
                        push(Value::make_strategy(static_cast<std::uint8_t>(entry.as_uint)));
                        break;
                    default:
                        push(normalize_int(entry.tag, entry.as_uint));
                        break;
                }
                break;
            }

            case Op::LoadNull:
                push(Value::make_null());
                break;
            case Op::LoadTrue:
                push(Value::make_bool(true));
                break;
            case Op::LoadFalse:
                push(Value::make_bool(false));
                break;

            case Op::LoadLocal: {
                std::uint16_t slot = read_u16(frame);
                if (slot >= frame.locals.size()) return trap("local slot out of range");
                push(frame.locals[slot]);
                break;
            }

            case Op::StoreLocal: {
                std::uint16_t slot = read_u16(frame);
                if (slot >= frame.locals.size()) return trap("local slot out of range");
                frame.locals[slot] = pop();
                break;
            }

            case Op::LoadThis:
                push(frame.receiver);
                break;

            case Op::GetField: {
                std::uint16_t slot = read_u16(frame);
                Value receiver = pop();
                if (receiver.is_null_reference()) {
                    return trap("cannot read a field of null: the object is not instantiated yet");
                }
                if (receiver.tag != TypeTag::Ref || !receiver.as_ref) {
                    return trap("getfield expects an object, found " +
                                std::string(bytecode::type_tag_name(receiver.tag)));
                }
                Object* object = receiver.as_ref;
                if (heap_.is_released(object)) {
                    return trap("use of an object after it was released (use-after-free)");
                }
                auto* type = static_cast<RuntimeClass*>(object->header.vtable);
                if (!type || slot >= type->slot_count) {
                    return trap("field slot " + std::to_string(slot) + " is out of range");
                }
                push(object->slots()[slot]);
                break;
            }

            case Op::PutField: {
                std::uint16_t slot = read_u16(frame);
                Value value = pop();
                Value receiver = pop();
                if (receiver.is_null_reference()) {
                    return trap("cannot assign a field of null: the object is not instantiated "
                                "yet");
                }
                if (receiver.tag != TypeTag::Ref || !receiver.as_ref) {
                    return trap("putfield expects an object, found " +
                                std::string(bytecode::type_tag_name(receiver.tag)));
                }
                Object* object = receiver.as_ref;
                if (heap_.is_released(object)) {
                    return trap("use of an object after it was released (use-after-free)");
                }
                auto* type = static_cast<RuntimeClass*>(object->header.vtable);
                if (!type || slot >= type->slot_count) {
                    return trap("field slot " + std::to_string(slot) + " is out of range");
                }
                write_barrier(object, *type, slot, value);
                object->slots()[slot] = value;
                break;
            }

            case Op::ArrayNew: {
                auto element = static_cast<TypeTag>(read_u8(frame));
                Value length = pop();
                if (length.as_int < 0) {
                    return trap("cannot create an array of " +
                                std::to_string(length.as_int) + " elements");
                }
                if (length.as_int > 0xffffff) {
                    return trap("array length " + std::to_string(length.as_int) +
                                " is larger than this runtime allocates");
                }
                // An allocation is a safepoint: collect first when the heap has
                // grown past its threshold, exactly as materialization does.
                if (collector_.should_collect()) collector_.collect();
                Object* array = heap_.allocate_array(
                    static_cast<std::uint32_t>(length.as_int), element);
                if (!array) return trap("out of memory creating an array");
                push(Value::make_ref(array));
                break;
            }

            case Op::ArrayLen: {
                Value target = pop();
                Object* array = nullptr;
                if (!array_operand(target, array)) return false;
                push(Value::make_int(TypeTag::Int32,
                                     static_cast<std::int64_t>(array->array_length())));
                break;
            }

            case Op::ArrayGet: {
                Value index = pop();
                Value target = pop();
                Object* array = nullptr;
                if (!array_operand(target, array)) return false;
                if (!array_index_in_range(*array, index.as_int)) return false;
                push(array->slots()[index.as_int]);
                break;
            }

            case Op::ArraySet: {
                Value value = pop();
                Value index = pop();
                Value target = pop();
                Object* array = nullptr;
                if (!array_operand(target, array)) return false;
                if (!array_index_in_range(*array, index.as_int)) return false;
                Value& slot = array->slots()[index.as_int];
                // The same barrier a field assignment applies: a manual object
                // stored in a collected slot is pinned, and overwriting the
                // slot drops that pin (docs/memory-model.md).
                array_write_barrier(slot, value);
                slot = value;
                break;
            }

            case Op::PtrGet: {
                auto element = static_cast<TypeTag>(read_u8(frame));
                Value index = pop();
                Value target = pop();
                Value loaded;
                if (!pointer_element(target, index, element, &loaded, nullptr)) return false;
                push(loaded);
                break;
            }

            case Op::PtrSet: {
                auto element = static_cast<TypeTag>(read_u8(frame));
                Value value = pop();
                Value index = pop();
                Value target = pop();
                if (!pointer_element(target, index, element, nullptr, &value)) return false;
                break;
            }

            case Op::Materialize: {
                std::uint16_t class_id = read_u16(frame);
                auto strategy = static_cast<bytecode::StrategyByte>(read_u8(frame));
                Value created;
                if (!materialize(class_id, strategy, created)) return false;
                push(created);
                break;
            }

            case Op::Alloc:
            case Op::ManualAlloc: {
                std::uint16_t class_id = read_u16(frame);
                RuntimeClass* type = classes_.at(class_id);
                if (!type) return trap("unknown class id " + std::to_string(class_id));
                Object* object = allocate(*type, op == Op::ManualAlloc);
                if (!object) return trap("out of memory allocating '" + std::string(type->name) +
                                         "'");
                push(Value::make_ref(object));
                break;
            }

            case Op::Free:
                if (!release_manual(pop())) return false;
                break;

            case Op::Pin:
            case Op::Unpin: {
                Value target = pop();
                if (target.is_null_reference()) break;  // nothing to pin
                if (target.tag != TypeTag::Ref || !target.as_ref) {
                    return trap(std::string(bytecode::mnemonic(op)) + " expects an object");
                }
                std::uint32_t& count = target.as_ref->header.pin_count;
                if (op == Op::Pin) {
                    ++count;
                } else if (count > 0) {
                    --count;
                }
                break;
            }

            case Op::Add:
            case Op::Sub:
            case Op::Mul:
            case Op::Div:
            case Op::Rem:
            case Op::Neg:
            case Op::BitAnd:
            case Op::BitOr:
            case Op::BitXor:
            case Op::BitNot:
            case Op::Shl:
            case Op::Shr:
                if (!arithmetic(op, static_cast<TypeTag>(read_u8(frame)))) return false;
                break;

            case Op::CmpEq:
            case Op::CmpNe:
            case Op::CmpLt:
            case Op::CmpLe:
            case Op::CmpGt:
            case Op::CmpGe:
                if (!compare(op, static_cast<TypeTag>(read_u8(frame)))) return false;
                break;

            case Op::RefEq:
            case Op::RefNe: {
                Value right = pop();
                Value left = pop();
                bool same;
                if (left.tag == TypeTag::String && right.tag == TypeTag::String) {
                    // String literals compare by contents; there is no interning
                    // guarantee across constant pools.
                    same = left.as_text && right.as_text && *left.as_text == *right.as_text;
                } else if (left.is_null_reference() || right.is_null_reference()) {
                    same = left.is_null_reference() && right.is_null_reference();
                } else {
                    same = left.as_ref == right.as_ref;
                }
                push(Value::make_bool(op == Op::RefEq ? same : !same));
                break;
            }

            case Op::LogicalNot:
                push(Value::make_bool(!pop().truthy()));
                break;

            case Op::Convert: {
                auto from = static_cast<TypeTag>(read_u8(frame));
                auto to = static_cast<TypeTag>(read_u8(frame));
                if (!convert(from, to)) return false;
                break;
            }

            case Op::Jump: {
                std::int32_t delta = read_i32(frame);
                frame.ip = static_cast<std::uint32_t>(static_cast<std::int64_t>(frame.ip) + delta);
                break;
            }

            case Op::JumpIfFalse: {
                std::int32_t delta = read_i32(frame);
                if (!pop().truthy()) {
                    frame.ip =
                        static_cast<std::uint32_t>(static_cast<std::int64_t>(frame.ip) + delta);
                }
                break;
            }

            case Op::JumpIfTrue: {
                std::int32_t delta = read_i32(frame);
                if (pop().truthy()) {
                    frame.ip =
                        static_cast<std::uint32_t>(static_cast<std::int64_t>(frame.ip) + delta);
                }
                break;
            }

            case Op::Return:
                result = Value::make_void();
                return true;

            case Op::ReturnValue:
                result = pop();
                return true;

            case Op::CallDirect: {
                std::uint16_t index = read_u16(frame);
                const bytecode::MethodEntry* callee =
                    module_.method_at(static_cast<std::int32_t>(index));
                if (!callee) return trap("call to unknown method #" + std::to_string(index));
                Value receiver = Value::make_null();
                // The receiver sits under the arguments; lift it out and put
                // the arguments back for the callee to consume.
                {
                    util::Array<Value> args;
                    for (std::uint8_t i = 0; i < callee->param_count; ++i) args.push(pop());
                    receiver = pop();
                    for (std::size_t i = args.size(); i > 0; --i) push(args[i - 1]);
                }
                Value returned;
                if (!call_method(static_cast<std::int32_t>(index), receiver, returned)) {
                    return false;
                }
                if (callee->return_type != TypeTag::Void) push(returned);
                break;
            }

            case Op::CallVirtual: {
                std::uint16_t slot = read_u16(frame);
                std::uint8_t argc = read_u8(frame);
                // The third operand -- whether the call leaves a value behind
                // -- is for the native emitter's abstract stack. The
                // interpreter resolves the method and then knows.
                read_u8(frame);

                // The receiver sits under the arguments; lift them off to reach
                // it, then dispatch on its class.
                util::Array<Value> args;
                for (std::uint8_t i = 0; i < argc; ++i) args.push(pop());
                Value receiver = pop();

                if (receiver.is_null_reference()) {
                    return trap("cannot call a method on null: the object is not instantiated "
                                "yet");
                }
                if (receiver.tag != TypeTag::Ref || !receiver.as_ref) {
                    return trap("a method call expects an object, found " +
                                std::string(bytecode::type_tag_name(receiver.tag)));
                }
                if (heap_.is_released(receiver.as_ref)) {
                    return trap("call of a method on an object after it was released "
                                "(use-after-free)");
                }

                // A derived override replaced the inherited entry when the
                // subclass's vtable was built, so the receiver's own table is
                // the whole of dynamic dispatch.
                auto* type = static_cast<RuntimeClass*>(receiver.as_ref->header.vtable);
                if (!type || slot >= type->vtable.size()) {
                    return trap("vtable slot " + std::to_string(slot) +
                                " is out of range for class '" +
                                (type ? std::string(type->name) : std::string("?")) + "'");
                }
                const bytecode::MethodEntry* callee = type->vtable[slot];

                for (std::size_t i = args.size(); i > 0; --i) push(args[i - 1]);
                Value returned;
                if (!call_method(type->vtable_indices[slot], receiver, returned)) return false;
                if (callee->return_type != TypeTag::Void) push(returned);
                break;
            }

            case Op::CallNative: {
                std::uint16_t native_id = read_u16(frame);
                std::uint8_t argc = read_u8(frame);
                if (!call_native(native_id, argc)) return false;
                break;
            }

            case Op::Halt:
                return true;

            case Op::Count:
            default:
                return trap("invalid opcode 0x" + std::to_string(static_cast<int>(op)));
        }
    }
}

// ---------------------------------------------------------------------------
// Materialization
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Procedure engine host
// ---------------------------------------------------------------------------
//
// The pipeline itself lives in utils/proc_engine.c. What follows is the set of
// steps it drives; each one does exactly one thing and reports failure by
// returning false after trapping.

namespace {

// The inheritance chain from the root down to `type`.
void collect_chain(RuntimeClass& type, util::Array<RuntimeClass*>& out) {
    if (type.base) collect_chain(*type.base, out);
    out.push(&type);
}

}  // namespace

std::uint32_t Vm::proc_chain_length(std::uint32_t class_id) {
    RuntimeClass* type = classes_.at(class_id);
    std::uint32_t length = 0;
    for (RuntimeClass* step = type; step; step = step->base) ++length;
    return length;
}

std::uint32_t Vm::proc_chain_at(std::uint32_t class_id, std::uint32_t index) {
    RuntimeClass* type = classes_.at(class_id);
    if (!type) return class_id;
    util::Array<RuntimeClass*> chain;
    collect_chain(*type, chain);
    if (index >= chain.size()) return class_id;
    return chain[index]->class_id;
}

Object* Vm::proc_allocate(std::uint32_t class_id, KhuStrategy strategy) {
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

bool Vm::proc_has_field_init(std::uint32_t class_id) {
    RuntimeClass* type = classes_.at(class_id);
    return type && type->field_init_index >= 0;
}

bool Vm::proc_run_field_init(Object* object, std::uint32_t class_id) {
    RuntimeClass* type = classes_.at(class_id);
    if (!type || type->field_init_index < 0) return true;
    Value ignored;
    return call_method(type->field_init_index, Value::make_ref(object), ignored);
}

bool Vm::proc_take_arguments(std::uint32_t argc) {
    if (stack_.size() < argc) {
        trap("allocation site supplied fewer arguments than the class binds");
        return false;
    }
    staged_marks_.push(staged_arguments_.size());
    // They come off the stack in reverse, so put them back in declaration order.
    std::size_t base = staged_arguments_.size();
    staged_arguments_.resize(base + argc);
    for (std::size_t i = argc; i > 0; --i) staged_arguments_[base + i - 1] = pop();
    return true;
}

void Vm::proc_push_arguments(std::uint32_t argc) {
    if (staged_marks_.empty()) return;
    std::size_t base = staged_marks_.back();
    for (std::uint32_t i = 0; i < argc && base + i < staged_arguments_.size(); ++i) {
        push(staged_arguments_[base + i]);
    }
}

void Vm::proc_drop_arguments() {
    if (staged_marks_.empty()) return;
    staged_arguments_.resize(staged_marks_.back());
    staged_marks_.pop();
}

bool Vm::proc_has_procedures(std::uint32_t class_id) {
    RuntimeClass* type = classes_.at(class_id);
    return type && type->procedures_index >= 0;
}

bool Vm::proc_run_procedures(Object* object, std::uint32_t class_id, std::uint32_t argc) {
    (void)argc;
    RuntimeClass* type = classes_.at(class_id);
    if (!type || type->procedures_index < 0) return true;
    Value ignored;
    return call_method(type->procedures_index, Value::make_ref(object), ignored);
}

bool Vm::proc_has_constructor(std::uint32_t class_id) {
    RuntimeClass* type = classes_.at(class_id);
    return type && type->constructor_index >= 0;
}

bool Vm::proc_run_constructor(Object* object, std::uint32_t class_id, std::uint32_t argc) {
    (void)argc;
    RuntimeClass* type = classes_.at(class_id);
    if (!type || type->constructor_index < 0) return true;
    Value ignored;
    return call_method(type->constructor_index, Value::make_ref(object), ignored);
}

void Vm::proc_register_live(Object* object) {
    // The object is already on the heap's live list; this is the point at which
    // it becomes a materialized instance rather than a half-built one.
    if (object) object->header.flags |= kObjectMaterialized;
}

void Vm::proc_discard(Object* object) {
    if (!object) return;
    // A manual object goes straight back to its arena -- unless something the
    // block ran pinned it, in which case releasing it would leave a dangling
    // managed slot. A managed object is simply never registered: nothing refers
    // to it, so the next cycle reclaims it.
    if (object->is_manual() && object->header.pin_count == 0) heap_.free_manual(object);
}

void Vm::proc_retain(Object* object) {
    if (object) native_roots_.push(Value::make_ref(object));
}

void Vm::proc_release(Object* object) {
    (void)object;
    if (!native_roots_.empty()) native_roots_.pop();
}

void Vm::proc_report_error(const char* message) { trap(message); }

bool Vm::materialize(std::uint32_t class_id, bytecode::StrategyByte strategy, Value& out) {
    RuntimeClass* type = classes_.at(class_id);
    if (!type) return trap("unknown class id " + std::to_string(class_id));

    KhuStrategy engine_strategy = strategy == bytecode::StrategyByte::Manual
                                      ? KHU_STRATEGY_MANUAL
                                      : (strategy == bytecode::StrategyByte::Gc
                                             ? KHU_STRATEGY_GC
                                             : KHU_STRATEGY_CLASS_DEFAULT);

    KhuObject* created = nullptr;
    KhuProcStatus status = khu_proc_materialize(class_id, engine_strategy,
                                                type->materialize_argc, &created);
    if (status != KHU_PROC_OK) {
        // A program that asked to exit part-way through a materialization is
        // not a failed materialization: it unwinds, and the exit code stands.
        if (exit_requested()) return false;
        // The engine reports the depth guard itself, and every step traps on
        // its own failure; only an unreported status needs a message here.
        if (!has_error()) {
            return trap("cannot materialize '" + std::string(type->name) + "': " +
                        khu_proc_status_name(status));
        }
        return false;
    }

    out = Value::make_ref(reinterpret_cast<Object*>(created));
    return true;
}

bool Vm::release_manual(Value target) {
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
    // A released chunk has been zeroed and returned to its arena, so its header
    // says nothing useful. The live list is the authority.
    if (!heap_.is_tracked_manual(object)) {
        return trap("cannot free this object: it was already released");
    }
    if (object->header.pin_count > 0) {
        // Name the slots that still hold it: a pin count alone does not tell
        // the programmer what to clear.
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
// Entry dispatch
// ---------------------------------------------------------------------------

bool Vm::run() {
    if (!prepare()) return false;

    if (module_.main_method >= 0) {
        const bytecode::MethodEntry* main = module_.method_at(module_.main_method);
        if (!main) return trap("the image names a missing entry point");

        // `main` is an ordinary member -- Khudra has no static context -- so
        // the class that declares it is materialized and main runs on that
        // instance.
        Value receiver;
        if (!materialize(main->owner_class, bytecode::StrategyByte::ClassDefault, receiver)) {
            return false;
        }
        Value result;
        return call_method(module_.main_method, receiver, result);
    }

    if (module_.root_class >= 0) {
        // The other entry form: materialize the first top-level class and let
        // its Procedures block fire.
        Value root;
        return materialize(static_cast<std::uint32_t>(module_.root_class),
                           bytecode::StrategyByte::ClassDefault, root);
    }

    return trap("this image has no entry point");
}

}  // namespace khu::vm
