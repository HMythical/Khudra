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

            case Op::GetField:
            case Op::PutField:
            case Op::Materialize:
            case Op::Alloc:
            case Op::ManualAlloc:
            case Op::Free:
            case Op::Pin:
            case Op::Unpin:
                // Object memory arrives in Phase 4 (objects and dispatch) and
                // Phase 5 (the collector and manual arenas).
                return trap(std::string("'") + bytecode::mnemonic(op) +
                            "' needs runtime objects, which arrive in Phase 4 (see KHU-PLAN.md)");

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
                // The receiver is under the arguments, and the number of
                // arguments depends on which method the slot resolves to, so
                // resolve the class first.
                std::uint32_t class_id = frame.method->owner_class;
                // Peek for the receiver once the argument count is known: try
                // the caller's own class table entry, which has the same shape
                // for every subclass.
                const bytecode::ClassEntry* owner =
                    module_.class_at(static_cast<std::int32_t>(class_id));
                if (!owner || slot >= owner->vtable.size()) {
                    return trap("vtable slot " + std::to_string(slot) + " is out of range");
                }
                const bytecode::MethodEntry* shape =
                    module_.method_at(static_cast<std::int32_t>(owner->vtable[slot]));
                if (!shape) return trap("vtable slot " + std::to_string(slot) + " is empty");

                util::Array<Value> args;
                for (std::uint8_t i = 0; i < shape->param_count; ++i) args.push(pop());
                Value receiver = pop();

                // With a real object the vtable comes from its class; the
                // caller's own class is the fallback for a self-call before
                // objects exist (Phase 4 makes a null receiver an error).
                if (receiver.tag == TypeTag::Ref && receiver.as_ref) {
                    class_id = receiver.as_ref->header.class_id;
                    owner = module_.class_at(static_cast<std::int32_t>(class_id));
                    if (!owner || slot >= owner->vtable.size()) {
                        return trap("vtable slot " + std::to_string(slot) +
                                    " is out of range for the receiver's class");
                    }
                }

                auto target = static_cast<std::int32_t>(owner->vtable[slot]);
                const bytecode::MethodEntry* callee = module_.method_at(target);
                if (!callee) return trap("vtable slot " + std::to_string(slot) + " is empty");

                for (std::size_t i = args.size(); i > 0; --i) push(args[i - 1]);
                Value returned;
                if (!call_method(target, receiver, returned)) return false;
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
// Entry dispatch
// ---------------------------------------------------------------------------

bool Vm::run() {
    if (module_.main_method >= 0) {
        Value result;
        // `main` runs without a receiver: the two entry forms stay distinct, so
        // an explicit main does not also materialize its own class.
        return call_method(module_.main_method, Value::make_null(), result);
    }

    if (module_.root_class >= 0) {
        return trap("root-class materialization needs the Procedure engine, which arrives in "
                    "Phase 6 (see KHU-PLAN.md)");
    }

    return trap("this image has no entry point");
}

}  // namespace khu::vm
