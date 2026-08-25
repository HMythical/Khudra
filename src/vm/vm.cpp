#include "vm/vm.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "bytecode/native.h"

namespace khu::vm {

using bytecode::Op;

namespace {

std::uint64_t mask_of(std::uint32_t width) {
    return width >= 64 ? ~0ull : ((1ull << width) - 1);
}

std::int64_t sign_extend(std::uint64_t bits, std::uint32_t width) {
    if (width >= 64) return static_cast<std::int64_t>(bits);
    std::uint64_t sign = 1ull << (width - 1);
    bits &= mask_of(width);
    return static_cast<std::int64_t>((bits ^ sign) - sign);
}

// Overflow wraps (KHU-PLAN.md, Type rules), so every integer result is
// truncated back to its declared width.
Value normalize_int(TypeTag tag, std::uint64_t bits) {
    std::uint32_t width = bytecode::type_width(tag);
    if (bytecode::is_signed_integer(tag)) return Value::make_int(tag, sign_extend(bits, width));
    return Value::make_uint(tag, bits & mask_of(width));
}

Value normalize_float(TypeTag tag, double value) {
    // A 32-bit float must round through `float` so its precision is real.
    if (tag == TypeTag::Float32) return Value::make_float(tag, static_cast<float>(value));
    return Value::make_float(tag, value);
}

}  // namespace

Vm::Vm(const bytecode::Module& module) : module_(module) {}

bool Vm::prepare() {
    if (prepared_) return true;
    std::string error;
    if (!classes_.load(module_, error)) return trap("cannot load the class table: " + error);
    collector_.set_root_source(this);
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

Vm::~Vm() {
    for (std::string* text : runtime_strings_) delete text;
}

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
    std::string path(module_.string_at(module_.source_file));
    if (!frame.method) return path;

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

    for (std::size_t i = frames_.size(); i > 0; --i) {
        const Frame& frame = *frames_[i - 1];
        if (!frame.method) continue;
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

bool Vm::call_native(std::uint32_t native_id, std::uint8_t argc) {
    auto id = static_cast<bytecode::NativeId>(native_id);

    switch (id) {
        case bytecode::NativeId::Print:
        case bytecode::NativeId::PrintLine: {
            std::string text;
            for (std::uint8_t i = 0; i < argc; ++i) text = render_value(pop()) + text;
            if (id == bytecode::NativeId::PrintLine) text += "\n";
            if (sink_) {
                *sink_ += text;
            } else {
                std::fwrite(text.data(), 1, text.size(), stdout);
            }
            return true;
        }

        case bytecode::NativeId::ReadLine: {
            for (std::uint8_t i = 0; i < argc; ++i) pop();
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
            push(Value::make_string(line));
            return true;
        }

        case bytecode::NativeId::None:
            break;
    }

    for (std::uint8_t i = 0; i < argc; ++i) pop();
    return trap("unknown native function id " + std::to_string(native_id));
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
                auto* type = static_cast<RuntimeClass*>(object->header.vtable);
                if (!type || slot >= type->slot_count) {
                    return trap("field slot " + std::to_string(slot) + " is out of range");
                }
                write_barrier(object, *type, slot, value);
                object->slots()[slot] = value;
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

// The inheritance chain from the root down to `type`. Every stage of
// materialization walks it in this order, so a derived class never observes
// uninitialized inherited state.
static void collect_chain(RuntimeClass& type, util::Array<RuntimeClass*>& out) {
    if (type.base) collect_chain(*type.base, out);
    out.push(&type);
}

bool Vm::run_field_initializers(RuntimeClass& type, Object* object) {
    if (type.base && !run_field_initializers(*type.base, object)) return false;
    if (type.field_init_index < 0) return true;
    Value ignored;
    return call_method(type.field_init_index, Value::make_ref(object), ignored);
}

bool Vm::materialize(std::uint32_t class_id, bytecode::StrategyByte strategy, Value& out) {
    RuntimeClass* type = classes_.at(class_id);
    if (!type) return trap("unknown class id " + std::to_string(class_id));

    bool manual = strategy == bytecode::StrategyByte::Manual ||
                  (strategy == bytecode::StrategyByte::ClassDefault && type->manual);

    // The allocation-site arguments are on the stack; both the Procedures block
    // and the constructor receive the same ones. They come off the stack here,
    // so they have to be roots for as long as this materialization runs.
    std::size_t root_mark = native_roots_.size();
    util::Array<Value> args;
    for (std::uint8_t i = 0; i < type->materialize_argc; ++i) {
        args.push(pop());
        native_roots_.push(args.back());
    }

    // 1. allocate per strategy, 2. link the object (header + defaults)
    Object* object = allocate(*type, manual);
    if (!object) {
        native_roots_.resize(root_mark);
        return trap("out of memory materializing '" + std::string(type->name) + "'");
    }
    Value reference = Value::make_ref(object);
    // Nothing on the interpreter stack refers to the new object yet, so a
    // collection triggered by a nested allocation would reclaim it.
    native_roots_.push(reference);

    struct RootScope {
        util::Array<Value>& roots;
        std::size_t mark;
        ~RootScope() { roots.resize(mark); }
    } scope{native_roots_, root_mark};

    util::Array<RuntimeClass*> chain;
    collect_chain(*type, chain);

    // 3. field initializers, part of linking
    if (!run_field_initializers(*type, object)) return false;

    // 4. Procedures blocks, base class first. Every block in the chain runs
    //    before any constructor body does, which is what "Procedures runs
    //    before the constructor" means for an inherited class.
    //    Phase 6 moves this loop into the C engine.
    for (RuntimeClass* step : chain) {
        if (step->procedures_index < 0) continue;
        for (std::size_t i = args.size(); i > 0; --i) push(args[i - 1]);
        Value ignored;
        if (!call_method(step->procedures_index, reference, ignored)) return false;
    }

    // 5. constructor bodies, base class first: the base sets up its own fields
    //    and the derived class may then overwrite them.
    for (RuntimeClass* step : chain) {
        if (step->constructor_index < 0) continue;
        for (std::size_t i = args.size(); i > 0; --i) push(args[i - 1]);
        Value ignored;
        if (!call_method(step->constructor_index, reference, ignored)) return false;
    }

    // 6. register the object as live and hand the reference back
    object->header.flags |= kObjectMaterialized;
    out = reference;
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
