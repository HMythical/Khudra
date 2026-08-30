#include "cemit/cemit.h"

#include <cinttypes>
#include <cstdio>
#include <string>
#include <vector>

#include "bytecode/native.h"
#include "bytecode/opcode.h"

namespace khu::native {
namespace {

using bytecode::Op;
using bytecode::OperandFormat;
using bytecode::TypeTag;

// One decoded instruction. `a` and `b` carry whatever the operand format holds;
// naming them after their meaning per opcode would need one struct per opcode.
struct Insn {
    std::uint32_t offset = 0;
    Op op = Op::Nop;
    std::uint32_t size = 1;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
    std::int32_t delta = 0;
};

// The depth an instruction is reached with, or kUnreached for code no path
// arrives at. Verified images have none, but an emitter that silently produced
// wrong array indices for one would be worse than an emitter that says so.
constexpr int kUnreached = -1;

// A slot whose class is not statically determined.
constexpr int kUnknownClass = -1;

// The abstract state at an instruction boundary: how deep the operand stack is,
// and which class each live slot holds where that is knowable.
//
// The classes are not decoration. A vtable slot number is only meaningful
// inside one inheritance chain -- slot 0 is `area` in the Shape chain and
// `main` in another class in the same image -- so the emitter cannot ask "does
// slot 0 return a value" globally. It asks the receiver's class, which is what
// the interpreter does at run time when it dispatches through that object's
// own table.
struct State {
    bool reached = false;
    int depth = 0;
    // Locals first, then the operand stack -- the layout the emitted frame uses.
    std::vector<int> classes;

    bool merge_from(const State& other, bool& changed) {
        if (depth != other.depth) return false;
        for (std::size_t i = 0; i < classes.size() && i < other.classes.size(); ++i) {
            if (classes[i] != other.classes[i] && classes[i] != kUnknownClass) {
                classes[i] = kUnknownClass;
                changed = true;
            }
        }
        return true;
    }
};

std::string decimal(std::int64_t value) { return std::to_string(value); }

// A C string literal. Trap messages are the only strings the emitter writes,
// and they come from the interpreter's own wording.
std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7f) {
                    char scratch[8];
                    std::snprintf(scratch, sizeof(scratch), "\\%03o",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += scratch;
                } else {
                    out += c;
                }
        }
    }
    out += "\"";
    return out;
}

// How many values a native call leaves behind. The answer comes from the
// shared table in bytecode/native.h rather than a second list here, so the
// emitter's abstract stack cannot disagree with what the two backends do. An
// id no build knows pops its arguments and traps, so it pushes nothing.
int native_result_count(std::uint32_t native_id) {
    return bytecode::native_result_count(static_cast<bytecode::NativeId>(native_id));
}

const char* arith_name(Op op) {
    switch (op) {
        case Op::Add: return "KHU_A_ADD";
        case Op::Sub: return "KHU_A_SUB";
        case Op::Mul: return "KHU_A_MUL";
        case Op::Div: return "KHU_A_DIV";
        case Op::Rem: return "KHU_A_REM";
        case Op::Neg: return "KHU_A_NEG";
        case Op::BitAnd: return "KHU_A_AND";
        case Op::BitOr: return "KHU_A_OR";
        case Op::BitXor: return "KHU_A_XOR";
        case Op::BitNot: return "KHU_A_NOT";
        case Op::Shl: return "KHU_A_SHL";
        default: return "KHU_A_SHR";
    }
}

const char* compare_name(Op op) {
    switch (op) {
        case Op::CmpEq: return "KHU_C_EQ";
        case Op::CmpNe: return "KHU_C_NE";
        case Op::CmpLt: return "KHU_C_LT";
        case Op::CmpLe: return "KHU_C_LE";
        case Op::CmpGt: return "KHU_C_GT";
        default: return "KHU_C_GE";
    }
}

// The single message a given (op, tag) pair can trap with. There is exactly one
// per site -- integer division traps only on zero, a float traps only on an
// operator it has no definition for -- so the emitted code carries the text and
// the helper only has to say whether it fired. The last case is the
// interpreter's own default branch: unreachable for the opcodes the emitter
// lowers, and worth carrying the right words anyway.
std::string arith_trap_message(Op op, TypeTag tag) {
    if (bytecode::is_float(tag)) {
        if (op == Op::BitNot) return "'~' is not defined for floating point values";
        return std::string("'") + bytecode::mnemonic(op) +
               "' is not defined for floating point values";
    }
    if (op == Op::Div) return "division by zero";
    if (op == Op::Rem) return "remainder by zero";
    return std::string("unsupported arithmetic opcode '") + bytecode::mnemonic(op) + "'";
}

class Emitter {
public:
    Emitter(const bytecode::Module& module, const EmitOptions& options)
        : module_(module), options_(options) {}

    EmitResult run();

private:
    bool decode(const bytecode::MethodEntry& method, std::vector<Insn>& out);
    bool analyze(const bytecode::MethodEntry& method, const std::vector<Insn>& code,
                 std::vector<State>& entry, std::vector<bool>& is_target, int& max_stack,
                 std::vector<char>& virtual_expects);
    bool emit_method(std::int32_t index, const bytecode::MethodEntry& method);

    // Applies one instruction to the abstract state, and reports whether
    // control falls through it and whether it branches.
    bool step(const bytecode::MethodEntry& method, const Insn& insn, State& state,
              bool& falls_through, bool& jumps, char& virtual_expects);

    // Does a call through vtable slot `slot` on a receiver of class
    // `receiver_class` leave a value behind? An unknown receiver falls back to
    // asking every class that has the slot, which is only ambiguous in an image
    // whose chains disagree -- and that one is reported rather than guessed at.
    bool virtual_result(std::uint16_t slot, int receiver_class, bool declared,
                        bool& expects_result);

    void fail(const std::string& message) {
        if (result_.error.empty()) result_.error = message;
    }

    // Names for the emitted frame array. `V` holds locals then operand stack.
    std::string local(std::uint32_t slot) const { return "V[" + decimal(slot) + "]"; }
    std::string stack(int depth) const { return "V[" + decimal(base_ + depth) + "]"; }
    std::string stack_addr(int depth) const { return "&V[" + decimal(base_ + depth) + "]"; }

    void line(const std::string& text) { out_ += "    " + text + "\n"; }

    const bytecode::Module& module_;
    const EmitOptions& options_;
    EmitResult result_;
    std::string out_;
    // Frame size of the method being emitted: where the operand stack starts.
    int base_ = 0;
};

bool Emitter::decode(const bytecode::MethodEntry& method, std::vector<Insn>& out) {
    const util::Array<std::uint8_t>& code = method.code;
    std::uint32_t offset = 0;
    while (offset < code.size()) {
        Insn insn;
        insn.offset = offset;
        insn.op = static_cast<Op>(code[offset]);
        if (insn.op >= Op::Count) {
            fail("method #" + decimal(static_cast<std::int64_t>(method.owner_class)) +
                 ": invalid opcode " + decimal(code[offset]) + " at offset " + decimal(offset));
            return false;
        }
        insn.size = bytecode::instruction_size(insn.op);
        if (offset + insn.size > code.size()) {
            fail("truncated instruction at offset " + decimal(offset));
            return false;
        }
        const std::uint8_t* operands = &code[offset + 1];
        switch (bytecode::operand_format(insn.op)) {
            case OperandFormat::None:
                break;
            case OperandFormat::U8:
            case OperandFormat::Type:
                insn.a = operands[0];
                break;
            case OperandFormat::TypeType:
                insn.a = operands[0];
                insn.b = operands[1];
                break;
            case OperandFormat::U16:
                insn.a = static_cast<std::uint32_t>(operands[0]) |
                         (static_cast<std::uint32_t>(operands[1]) << 8);
                break;
            case OperandFormat::U16U8:
                insn.a = static_cast<std::uint32_t>(operands[0]) |
                         (static_cast<std::uint32_t>(operands[1]) << 8);
                insn.b = operands[2];
                break;
            case OperandFormat::U16U8U8:
                insn.a = static_cast<std::uint32_t>(operands[0]) |
                         (static_cast<std::uint32_t>(operands[1]) << 8);
                insn.b = operands[2];
                insn.c = operands[3];
                break;
            case OperandFormat::I32: {
                std::uint32_t bits = 0;
                for (int i = 0; i < 4; ++i) {
                    bits |= static_cast<std::uint32_t>(operands[i]) << (i * 8);
                }
                insn.delta = static_cast<std::int32_t>(bits);
                break;
            }
        }
        out.push_back(insn);
        offset += insn.size;
    }
    return true;
}

// Whether an `invokevirtual` leaves a value behind.
//
// The instruction says so: the front end knew at the call site, and a slot
// number on its own does not -- slot 2 is a getter in one inheritance chain and
// a void method in an unrelated one, and the emitter walks the code with an
// abstract stack before any receiver exists to ask.
//
// The receiver's own table is still consulted when the class is statically
// known, as a cross-check: an override keeps its signature, so the two answers
// have to agree, and a disagreement means the image is not what it claims.
bool Emitter::virtual_result(std::uint16_t slot, int receiver_class, bool declared,
                             bool& expects_result) {
    expects_result = declared;
    if (receiver_class == kUnknownClass) return true;

    const bytecode::ClassEntry* entry =
        module_.class_at(static_cast<std::int32_t>(receiver_class));
    if (!entry || slot >= entry->vtable.size()) return true;
    const bytecode::MethodEntry* callee =
        module_.method_at(static_cast<std::int32_t>(entry->vtable[slot]));
    if (!callee) return true;

    bool actual = callee->return_type != TypeTag::Void;
    if (actual != declared) {
        fail("invokevirtual through slot " + decimal(slot) + " says it " +
             (declared ? "leaves a value behind" : "leaves nothing behind") +
             ", but the receiver's method disagrees");
        return false;
    }
    return true;
}

bool Emitter::step(const bytecode::MethodEntry& method, const Insn& insn, State& state,
                   bool& falls_through, bool& jumps, char& virtual_expects) {
    falls_through = true;
    jumps = false;
    virtual_expects = 0;

    bool overflowed = false;
    auto need = [&](int count) -> bool {
        if (state.depth < count) {
            fail("the operand stack is " + decimal(state.depth) + " deep at offset " +
                 decimal(insn.offset) + ", but " + bytecode::mnemonic(insn.op) + " needs " +
                 decimal(count));
            overflowed = true;
            return false;
        }
        return true;
    };
    auto push = [&](int class_id) {
        state.classes.push_back(class_id);
        ++state.depth;
    };
    auto pop = [&]() -> int {
        if (state.classes.empty()) return kUnknownClass;
        int class_id = state.classes.back();
        state.classes.pop_back();
        --state.depth;
        return class_id;
    };
    auto peek = [&](int from_top) -> int {
        std::size_t size = state.classes.size();
        if (static_cast<std::size_t>(from_top) >= size) return kUnknownClass;
        return state.classes[size - 1 - static_cast<std::size_t>(from_top)];
    };
    // Whatever a value operation produces, it is not an object reference.
    auto replace = [&](int operands) {
        for (int i = 0; i < operands; ++i) pop();
        push(kUnknownClass);
    };

    switch (insn.op) {
        case Op::Nop:
            break;

        case Op::Pop:
            if (!need(1)) return false;
            pop();
            break;

        case Op::Dup:
            if (!need(1)) return false;
            push(peek(0));
            break;

        case Op::DupX1: {
            if (!need(2)) return false;
            int value = pop();
            int under = pop();
            push(value);
            push(under);
            push(value);
            break;
        }

        case Op::DupX2: {
            if (!need(3)) return false;
            int value = pop();
            int middle = pop();
            int under = pop();
            push(value);
            push(under);
            push(middle);
            push(value);
            break;
        }

        case Op::LoadConst:
        case Op::LoadNull:
        case Op::LoadTrue:
        case Op::LoadFalse:
            push(kUnknownClass);
            break;

        case Op::LoadLocal:
            if (insn.a >= method.frame_size) {
                fail("local slot " + decimal(insn.a) + " is out of range");
                return false;
            }
            push(state.classes[insn.a]);
            break;

        case Op::StoreLocal: {
            if (insn.a >= method.frame_size) {
                fail("local slot " + decimal(insn.a) + " is out of range");
                return false;
            }
            if (!need(1)) return false;
            state.classes[insn.a] = pop();
            break;
        }

        case Op::LoadThis:
            push(static_cast<int>(method.owner_class));
            break;

        case Op::ArrayNew:
            if (!need(1)) return false;
            pop();
            push(kUnknownClass);
            break;

        case Op::ArrayLen:
            if (!need(1)) return false;
            pop();
            push(kUnknownClass);
            break;

        case Op::ArrayGet:
            if (!need(2)) return false;
            pop();
            pop();
            // The element type is erased, so nothing is known about the class
            // of a reference read out of an array. A virtual call on one is
            // dispatched by the host, which re-resolves it anyway.
            push(kUnknownClass);
            break;

        case Op::ArraySet:
            if (!need(3)) return false;
            pop();
            pop();
            pop();
            break;

        case Op::PtrGet:
            if (!need(2)) return false;
            pop();
            pop();
            push(kUnknownClass);
            break;

        case Op::PtrSet:
            if (!need(3)) return false;
            pop();
            pop();
            pop();
            break;

        case Op::GetField: {
            if (!need(1)) return false;
            int receiver = pop();
            // A field's declared class is in the image, so a reference read out
            // of a known object keeps its class through the load.
            int result = kUnknownClass;
            const bytecode::ClassEntry* owner =
                receiver == kUnknownClass
                    ? nullptr
                    : module_.class_at(static_cast<std::int32_t>(receiver));
            if (owner && insn.a < owner->fields.size()) {
                const bytecode::FieldEntry& field = owner->fields[insn.a];
                if (field.type == TypeTag::Ref && field.class_ref != 0xffffffffu) {
                    result = static_cast<int>(field.class_ref);
                }
            }
            push(result);
            break;
        }

        case Op::PutField:
            if (!need(2)) return false;
            pop();
            pop();
            break;

        case Op::Neg:
        case Op::BitNot:
        case Op::LogicalNot:
        case Op::Convert:
            if (!need(1)) return false;
            replace(1);
            break;

        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Rem:
        case Op::BitAnd:
        case Op::BitOr:
        case Op::BitXor:
        case Op::Shl:
        case Op::Shr:
        case Op::CmpEq:
        case Op::CmpNe:
        case Op::CmpLt:
        case Op::CmpLe:
        case Op::CmpGt:
        case Op::CmpGe:
        case Op::RefEq:
        case Op::RefNe:
            if (!need(2)) return false;
            replace(2);
            break;

        case Op::Jump:
            jumps = true;
            falls_through = false;
            break;

        case Op::JumpIfFalse:
        case Op::JumpIfTrue:
            if (!need(1)) return false;
            pop();
            jumps = true;
            break;

        case Op::Return:
        case Op::Halt:
            falls_through = false;
            break;

        case Op::ReturnValue:
            if (!need(1)) return false;
            pop();
            falls_through = false;
            break;

        case Op::CallDirect: {
            const bytecode::MethodEntry* callee =
                module_.method_at(static_cast<std::int32_t>(insn.a));
            if (!callee) {
                fail("call to unknown method #" + decimal(insn.a));
                return false;
            }
            if (!need(static_cast<int>(callee->param_count) + 1)) return false;
            for (std::uint8_t i = 0; i < callee->param_count; ++i) pop();
            pop();  // the receiver sits under the arguments
            if (callee->return_type != TypeTag::Void) push(kUnknownClass);
            break;
        }

        case Op::CallVirtual: {
            if (!need(static_cast<int>(insn.b) + 1)) return false;
            int receiver = peek(static_cast<int>(insn.b));
            bool expects = false;
            if (!virtual_result(static_cast<std::uint16_t>(insn.a), receiver, insn.c != 0,
                                expects)) {
                return false;
            }
            virtual_expects = expects ? 1 : 0;
            for (std::uint8_t i = 0; i < insn.b; ++i) pop();
            pop();
            if (expects) push(kUnknownClass);
            break;
        }

        case Op::CallNative: {
            if (!need(static_cast<int>(insn.b))) return false;
            for (std::uint8_t i = 0; i < insn.b; ++i) pop();
            if (native_result_count(insn.a) > 0) push(kUnknownClass);
            break;
        }

        case Op::Materialize: {
            const bytecode::ClassEntry* type =
                module_.class_at(static_cast<std::int32_t>(insn.a));
            if (!type) {
                fail("materialize names unknown class #" + decimal(insn.a));
                return false;
            }
            if (!need(static_cast<int>(type->materialize_argc))) return false;
            for (std::uint8_t i = 0; i < type->materialize_argc; ++i) pop();
            push(static_cast<int>(insn.a));
            break;
        }

        case Op::Alloc:
        case Op::ManualAlloc:
            if (!module_.class_at(static_cast<std::int32_t>(insn.a))) {
                fail("allocation names unknown class #" + decimal(insn.a));
                return false;
            }
            push(static_cast<int>(insn.a));
            break;

        case Op::Free:
        case Op::Pin:
        case Op::Unpin:
            if (!need(1)) return false;
            pop();
            break;

        case Op::Count:
            fail("invalid opcode");
            return false;
    }

    return !overflowed;
}

bool Emitter::analyze(const bytecode::MethodEntry& method, const std::vector<Insn>& code,
                      std::vector<State>& entry, std::vector<bool>& is_target, int& max_stack,
                      std::vector<char>& virtual_expects) {
    // Indexed by bytecode offset so a jump target maps straight through.
    std::vector<int> index_of(method.code.size() + 1, -1);
    for (std::size_t i = 0; i < code.size(); ++i) index_of[code[i].offset] = static_cast<int>(i);

    entry.assign(code.size() + 1, State{});
    is_target.assign(code.size() + 1, false);
    virtual_expects.assign(code.size(), 0);
    max_stack = 0;

    std::vector<int> worklist;
    if (!code.empty()) {
        entry[0].reached = true;
        entry[0].depth = 0;
        entry[0].classes.assign(method.frame_size, kUnknownClass);
        worklist.push_back(0);
    }

    auto reach = [&](std::uint32_t target_offset, const State& state, bool as_label) -> bool {
        if (target_offset > method.code.size()) {
            fail("jump target " + decimal(target_offset) + " is outside the method");
            return false;
        }
        int slot = target_offset == method.code.size() ? static_cast<int>(code.size())
                                                       : index_of[target_offset];
        if (slot < 0) {
            fail("jump target " + decimal(target_offset) + " is not an instruction boundary");
            return false;
        }
        if (as_label) is_target[slot] = true;
        if (!entry[slot].reached) {
            entry[slot] = state;
            entry[slot].reached = true;
            worklist.push_back(slot);
            return true;
        }
        if (entry[slot].depth != state.depth) {
            fail("the operand stack is " + decimal(state.depth) +
                 " deep on one path into offset " + decimal(target_offset) + " and " +
                 decimal(entry[slot].depth) + " on another");
            return false;
        }
        bool changed = false;
        if (!entry[slot].merge_from(state, changed)) return false;
        if (changed) worklist.push_back(slot);
        return true;
    };

    // A merge only ever widens a slot to "unknown", so the fixpoint is reached
    // in bounded time; the counter is a guard against a bug here, not a real
    // limit on program shape.
    std::size_t budget = (code.size() + 1) * (method.frame_size + 8) + 1024;
    while (!worklist.empty()) {
        if (budget-- == 0) {
            fail("the operand-stack analysis did not settle; this is an emitter bug");
            return false;
        }
        int slot = worklist.back();
        worklist.pop_back();
        if (slot >= static_cast<int>(code.size())) continue;

        const Insn& insn = code[slot];
        State state = entry[slot];
        int before = state.depth;

        bool falls_through = true;
        bool jumps = false;
        char expects = 0;
        if (!step(method, insn, state, falls_through, jumps, expects)) return false;
        virtual_expects[slot] = expects;

        // The deepest point the instruction touches: its operands are still
        // live while its result is written.
        int used = before > state.depth ? before : state.depth;
        if (used > max_stack) max_stack = used;

        if (falls_through && !reach(insn.offset + insn.size, state, false)) return false;
        if (jumps) {
            std::int64_t target = static_cast<std::int64_t>(insn.offset) + insn.size + insn.delta;
            if (target < 0) {
                fail("jump target " + decimal(target) + " is outside the method");
                return false;
            }
            if (!reach(static_cast<std::uint32_t>(target), state, true)) return false;
        }
    }
    return true;
}

bool Emitter::emit_method(std::int32_t index, const bytecode::MethodEntry& method) {
    std::vector<Insn> code;
    if (!decode(method, code)) return false;

    std::vector<State> entry;
    std::vector<bool> is_target;
    std::vector<char> virtual_expects;
    int max_stack = 0;
    if (!analyze(method, code, entry, is_target, max_stack, virtual_expects)) return false;

    base_ = method.frame_size;
    int slot_count = base_ + max_stack;
    if (slot_count <= 0) slot_count = 1;  // C has no zero-length array

    const bytecode::ClassEntry* owner =
        module_.class_at(static_cast<std::int32_t>(method.owner_class));
    std::string title = owner ? std::string(module_.string_at(owner->name)) + "." : std::string();
    title += std::string(module_.string_at(method.name));

    out_ += "\n/* " + title + " -- params=" + decimal(method.param_count) +
            " frame=" + decimal(method.frame_size) + " stack=" + decimal(max_stack) + " */\n";
    out_ += "static int khu_m" + decimal(index) +
            "(KhuValue self, const KhuValue* argv, KhuValue* out) {\n";
    line("KhuValue V[" + decimal(slot_count) + "];");
    line("KhuFrame F;");
    line("*out = khu_make_void();");
    line("khu_rt_frame_enter(&F, V, " + decimal(index) + "u, " + decimal(method.frame_size) +
         "u, " + decimal(slot_count) + "u, self, argv, " + decimal(method.param_count) + "u);");

    for (std::size_t i = 0; i < code.size(); ++i) {
        const Insn& insn = code[i];
        int depth = entry[i].depth;
        if (!entry[i].reached) {
            out_ += "    /* " + decimal(insn.offset) + " " + bytecode::mnemonic(insn.op) +
                    " -- unreachable */\n";
            continue;
        }
        if (is_target[i]) out_ += "  L" + decimal(insn.offset) + ":\n";

        // The location and live height the host needs if this instruction
        // traps or reaches a safepoint. `ip` lands past the instruction, where
        // the interpreter's would be, so the line lookup agrees.
        auto publish = [&]() {
            line("F.ip = " + decimal(insn.offset + insn.size) + "u; F.height = " +
                 decimal(depth) + "u;");
        };
        auto target_label = [&]() {
            return "L" + decimal(static_cast<std::int64_t>(insn.offset) + insn.size + insn.delta);
        };

        switch (insn.op) {
            case Op::Nop:
            case Op::Pop:
                break;

            case Op::Dup:
                line(stack(depth) + " = " + stack(depth - 1) + ";");
                break;

            case Op::DupX1:
                line("{ KhuValue t = " + stack(depth - 1) + "; " + stack(depth - 1) + " = " +
                     stack(depth - 2) + "; " + stack(depth - 2) + " = t; " + stack(depth) +
                     " = t; }");
                break;

            case Op::DupX2:
                line("{ KhuValue t = " + stack(depth - 1) + "; " + stack(depth - 1) + " = " +
                     stack(depth - 2) + "; " + stack(depth - 2) + " = " + stack(depth - 3) +
                     "; " + stack(depth - 3) + " = t; " + stack(depth) + " = t; }");
                break;

            case Op::LoadConst:
                if (insn.a >= module_.constants.size()) {
                    fail("constant #" + decimal(insn.a) + " is out of range");
                    return false;
                }
                line(stack(depth) + " = khu_rt_constants[" + decimal(insn.a) + "];");
                break;

            case Op::LoadNull:
                line(stack(depth) + " = khu_make_null();");
                break;
            case Op::LoadTrue:
                line(stack(depth) + " = khu_make_bool(1);");
                break;
            case Op::LoadFalse:
                line(stack(depth) + " = khu_make_bool(0);");
                break;

            case Op::LoadLocal:
                if (insn.a >= method.frame_size) {
                    fail("local slot " + decimal(insn.a) + " is out of range");
                    return false;
                }
                line(stack(depth) + " = " + local(insn.a) + ";");
                break;

            case Op::StoreLocal:
                if (insn.a >= method.frame_size) {
                    fail("local slot " + decimal(insn.a) + " is out of range");
                    return false;
                }
                line(local(insn.a) + " = " + stack(depth - 1) + ";");
                break;

            case Op::LoadThis:
                line(stack(depth) + " = F.receiver;");
                break;

            case Op::ArrayNew:
                publish();
                line("if (khu_rt_array_new(&F, " + decimal(insn.a) + "u, " +
                     stack_addr(depth - 1) + ", " + stack_addr(depth - 1) + ")) KHU_UNWIND(&F);");
                break;

            case Op::ArrayLen:
                publish();
                line("if (khu_rt_array_len(&F, " + stack_addr(depth - 1) + ", " +
                     stack_addr(depth - 1) + ")) KHU_UNWIND(&F);");
                break;

            case Op::ArrayGet:
                publish();
                line("if (khu_rt_array_get(&F, " + stack_addr(depth - 2) + ", " +
                     stack_addr(depth - 1) + ", " + stack_addr(depth - 2) + ")) KHU_UNWIND(&F);");
                break;

            case Op::ArraySet:
                publish();
                line("if (khu_rt_array_set(&F, " + stack_addr(depth - 3) + ", " +
                     stack_addr(depth - 2) + ", " + stack_addr(depth - 1) + ")) KHU_UNWIND(&F);");
                break;

            case Op::PtrGet:
                publish();
                line("if (khu_rt_ptr_get(&F, " + stack_addr(depth - 2) + ", " +
                     stack_addr(depth - 1) + ", " + decimal(insn.a) + "u, " +
                     stack_addr(depth - 2) + ")) KHU_UNWIND(&F);");
                break;

            case Op::PtrSet:
                publish();
                line("if (khu_rt_ptr_set(&F, " + stack_addr(depth - 3) + ", " +
                     stack_addr(depth - 2) + ", " + decimal(insn.a) + "u, " +
                     stack_addr(depth - 1) + ")) KHU_UNWIND(&F);");
                break;

            case Op::GetField:
                publish();
                line("if (khu_rt_getfield(&F, " + stack_addr(depth - 1) + ", " + decimal(insn.a) +
                     "u, " + stack_addr(depth - 1) + ")) KHU_UNWIND(&F);");
                break;

            case Op::PutField:
                publish();
                line("if (khu_rt_putfield(&F, " + stack_addr(depth - 2) + ", " + decimal(insn.a) +
                     "u, " + stack_addr(depth - 1) + ")) KHU_UNWIND(&F);");
                break;

            case Op::Neg:
            case Op::BitNot:
                publish();
                line("if (khu_val_unary(" + std::string(arith_name(insn.op)) + ", " +
                     decimal(insn.a) + "u, " + stack_addr(depth - 1) + ", " +
                     stack_addr(depth - 1) + ")) KHU_TRAP(&F, " +
                     quoted(arith_trap_message(insn.op, static_cast<TypeTag>(insn.a))) + ");");
                break;

            case Op::Add:
            case Op::Sub:
            case Op::Mul:
            case Op::Div:
            case Op::Rem:
            case Op::BitAnd:
            case Op::BitOr:
            case Op::BitXor:
            case Op::Shl:
            case Op::Shr:
                publish();
                line("if (khu_val_arith(" + std::string(arith_name(insn.op)) + ", " +
                     decimal(insn.a) + "u, " + stack_addr(depth - 2) + ", " +
                     stack_addr(depth - 1) + ", " + stack_addr(depth - 2) + ")) KHU_TRAP(&F, " +
                     quoted(arith_trap_message(insn.op, static_cast<TypeTag>(insn.a))) + ");");
                break;

            case Op::CmpEq:
            case Op::CmpNe:
            case Op::CmpLt:
            case Op::CmpLe:
            case Op::CmpGt:
            case Op::CmpGe:
                line(stack(depth - 2) + " = khu_val_compare(" +
                     std::string(compare_name(insn.op)) + ", " + decimal(insn.a) + "u, " +
                     stack_addr(depth - 2) + ", " + stack_addr(depth - 1) + ");");
                break;

            case Op::RefEq:
            case Op::RefNe:
                line(stack(depth - 2) + " = khu_make_bool(khu_rt_ref_same(" +
                     stack_addr(depth - 2) + ", " + stack_addr(depth - 1) + ")" +
                     (insn.op == Op::RefEq ? "" : " == 0") + ");");
                break;

            case Op::LogicalNot:
                line(stack(depth - 1) + " = khu_make_bool(!khu_truthy(" + stack_addr(depth - 1) +
                     "));");
                break;

            case Op::Convert:
                publish();
                line("if (khu_val_convert(" + decimal(insn.a) + "u, " + decimal(insn.b) + "u, " +
                     stack_addr(depth - 1) + ", " + stack_addr(depth - 1) + ")) KHU_TRAP(&F, " +
                     quoted(std::string("cannot convert ") +
                            bytecode::type_tag_name(static_cast<TypeTag>(insn.a)) + " to " +
                            bytecode::type_tag_name(static_cast<TypeTag>(insn.b))) + ");");
                break;

            case Op::Jump:
                line("goto " + target_label() + ";");
                break;
            case Op::JumpIfFalse:
                line("if (!khu_truthy(" + stack_addr(depth - 1) + ")) goto " + target_label() +
                     ";");
                break;
            case Op::JumpIfTrue:
                line("if (khu_truthy(" + stack_addr(depth - 1) + ")) goto " + target_label() +
                     ";");
                break;

            case Op::Return:
            case Op::Halt:
                line("KHU_RETURN(&F);");
                break;
            case Op::ReturnValue:
                line("*out = " + stack(depth - 1) + "; KHU_RETURN(&F);");
                break;

            case Op::CallDirect: {
                const bytecode::MethodEntry* callee =
                    module_.method_at(static_cast<std::int32_t>(insn.a));
                int receiver = depth - static_cast<int>(callee->param_count) - 1;
                publish();
                line("if (khu_rt_call_direct(&F, " + decimal(insn.a) + "u, " +
                     stack_addr(receiver) + ", " + stack_addr(receiver + 1) + ", " +
                     stack_addr(receiver) + ")) KHU_UNWIND(&F);");
                break;
            }

            case Op::CallVirtual: {
                bool expects = virtual_expects[i] != 0;
                int receiver = depth - static_cast<int>(insn.b) - 1;
                publish();
                line("if (khu_rt_call_virtual(&F, " + decimal(insn.a) + "u, " + decimal(insn.b) +
                     "u, " + stack_addr(receiver) + ", " + stack_addr(receiver + 1) + ", " +
                     stack_addr(receiver) + ", " + (expects ? "1" : "0") + ")) KHU_UNWIND(&F);");
                break;
            }

            case Op::CallNative: {
                int first = depth - static_cast<int>(insn.b);
                publish();
                line("if (khu_rt_call_native(&F, " + decimal(insn.a) + "u, " + decimal(insn.b) +
                     "u, " + stack_addr(first) + ", " + stack_addr(first) + ", " +
                     decimal(native_result_count(insn.a)) + ")) KHU_UNWIND(&F);");
                break;
            }

            case Op::Materialize: {
                const bytecode::ClassEntry* type =
                    module_.class_at(static_cast<std::int32_t>(insn.a));
                int argc = type->materialize_argc;
                int first = depth - argc;
                publish();
                line("if (khu_rt_materialize(&F, " + decimal(insn.a) + "u, " + decimal(insn.b) +
                     "u, " + stack_addr(first) + ", " + decimal(argc) + "u, " +
                     stack_addr(first) + ")) KHU_UNWIND(&F);");
                break;
            }

            case Op::Alloc:
            case Op::ManualAlloc:
                publish();
                line("if (khu_rt_alloc(&F, " + decimal(insn.a) + "u, " +
                     (insn.op == Op::ManualAlloc ? "1" : "0") + ", " + stack_addr(depth) +
                     ")) KHU_UNWIND(&F);");
                break;

            case Op::Free:
                publish();
                line("if (khu_rt_free(&F, " + stack_addr(depth - 1) + ")) KHU_UNWIND(&F);");
                break;

            case Op::Pin:
            case Op::Unpin:
                publish();
                line("if (khu_rt_pin(&F, " + stack_addr(depth - 1) + ", " +
                     (insn.op == Op::Pin ? "1" : "0") + ")) KHU_UNWIND(&F);");
                break;

            case Op::Count:
                fail("invalid opcode");
                return false;
        }
    }

    // Running off the end is an implicit `return;`, exactly as the interpreter
    // treats it. When no path gets here the statement is still emitted, so the
    // C compiler can see every path returns without having to prove it.
    if (!code.empty() && is_target[code.size()]) {
        out_ += "  L" + decimal(method.code.size()) + ":\n";
    } else if (!code.empty() && !entry[code.size()].reached) {
        line("/* not reached: every path above returns */");
    }
    line("KHU_RETURN(&F);");
    out_ += "}\n";
    return true;
}

EmitResult Emitter::run() {
    out_ += "/* Generated by the Khudra native backend -- do not edit.\n";
    out_ += " *\n";
    out_ += " * Lowered from ";
    out_ += std::string(module_.string_at(module_.source_file));
    out_ += "\n * Constants: " + decimal(static_cast<std::int64_t>(module_.constants.size()));
    out_ += "  Classes: " + decimal(static_cast<std::int64_t>(module_.classes.size()));
    out_ += "  Methods: " + decimal(static_cast<std::int64_t>(module_.methods.size())) + "\n";
    out_ += " */\n\n";

    // The translation unit carries the ABI it was compiled against, so it needs
    // nothing from the Khudra source tree to build.
    out_ += native_abi_source();
    out_ += "\n";
    out_ += "/* Leaving a frame on the way out of a lowered method. KHU_TRAP raises before\n";
    out_ += " * unlinking, so the trace the host assembles still has this frame on it. */\n";
    out_ += "#define KHU_RETURN(f) do { khu_rt_frame_leave(f); return 0; } while (0)\n";
    out_ += "#define KHU_UNWIND(f) do { khu_rt_frame_leave(f); return 1; } while (0)\n";
    out_ += "#define KHU_TRAP(f, msg) do { int khu_s = khu_rt_trap((f), (msg)); \\\n";
    out_ += "                              khu_rt_frame_leave(f); return khu_s; } while (0)\n";

    out_ += "\n/* --- method table ------------------------------------------------------- */\n";
    for (std::size_t i = 0; i < module_.methods.size(); ++i) {
        out_ += "static int khu_m" + decimal(static_cast<std::int64_t>(i)) +
                "(KhuValue self, const KhuValue* argv, KhuValue* out);\n";
    }

    for (std::size_t i = 0; i < module_.methods.size(); ++i) {
        if (!emit_method(static_cast<std::int32_t>(i), module_.methods[i])) {
            result_.source.clear();
            return result_;
        }
    }

    out_ += "\nconst KhuNativeMethod khu_native_methods[] = {\n";
    if (module_.methods.empty()) {
        out_ += "    0\n";
    } else {
        for (std::size_t i = 0; i < module_.methods.size(); ++i) {
            out_ += "    &khu_m" + decimal(static_cast<std::int64_t>(i)) + ",\n";
        }
    }
    out_ += "};\n";
    out_ += "const uint32_t khu_native_method_count = " +
            decimal(static_cast<std::int64_t>(module_.methods.size())) + "u;\n";

    out_ += "\n/* --- embedded image ----------------------------------------------------- */\n";
    out_ += "const unsigned char khu_native_image[] = {";
    if (options_.image.empty()) {
        out_ += "0";
    } else {
        char scratch[8];
        for (std::size_t i = 0; i < options_.image.size(); ++i) {
            if (i % 16 == 0) out_ += "\n    ";
            std::snprintf(scratch, sizeof(scratch), "0x%02x,",
                          static_cast<unsigned>(static_cast<unsigned char>(options_.image[i])));
            out_ += scratch;
        }
        out_ += "\n";
    }
    out_ += "};\n";
    out_ += "const uint32_t khu_native_image_size = " +
            decimal(static_cast<std::int64_t>(options_.image.size())) + "u;\n";

    result_.source = std::move(out_);
    return result_;
}

}  // namespace

EmitResult emit_c(const bytecode::Module& module, const EmitOptions& options) {
    Emitter emitter(module, options);
    return emitter.run();
}

}  // namespace khu::native
