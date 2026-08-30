#include "bytecode/disassembler.h"

#include <cinttypes>
#include <cstdio>

#include "util/string_builder.h"

namespace khu::bytecode {
namespace {

std::uint16_t read_u16(const MethodEntry& method, std::uint32_t offset) {
    if (offset + 1 >= method.code.size()) return 0;
    return static_cast<std::uint16_t>(method.code[offset] |
                                      (static_cast<std::uint16_t>(method.code[offset + 1]) << 8));
}

std::int32_t read_i32(const MethodEntry& method, std::uint32_t offset) {
    std::uint32_t bits = 0;
    for (int i = 0; i < 4; ++i) {
        if (offset + static_cast<std::uint32_t>(i) >= method.code.size()) return 0;
        bits |= static_cast<std::uint32_t>(method.code[offset + static_cast<std::uint32_t>(i)])
                << (i * 8);
    }
    return static_cast<std::int32_t>(bits);
}

const char* strategy_name(std::uint8_t value) {
    switch (static_cast<StrategyByte>(value)) {
        case StrategyByte::Gc: return "standard";
        case StrategyByte::Manual: return "manual";
        case StrategyByte::ClassDefault: return "class-default";
    }
    return "?";
}

void pad_to(util::StringBuilder& out, std::size_t start, std::size_t width) {
    while (out.size() - start < width) out.append(' ');
}

}  // namespace

std::string describe_constant(const Module& module, std::uint32_t index) {
    if (index >= module.constants.size()) return "<bad-constant>";
    const Constant& entry = module.constants[index];

    char scratch[64];
    switch (entry.tag) {
        case TypeTag::String: {
            std::string text = "\"";
            for (char c : entry.text) {
                switch (c) {
                    case '\n': text += "\\n"; break;
                    case '\t': text += "\\t"; break;
                    case '"': text += "\\\""; break;
                    case '\\': text += "\\\\"; break;
                    default: text += c; break;
                }
            }
            return text + "\"";
        }
        case TypeTag::Bool:
            return entry.as_uint != 0 ? "true" : "false";
        case TypeTag::Float32:
        case TypeTag::Float64:
            std::snprintf(scratch, sizeof(scratch), "%g", entry.as_float);
            return scratch;
        case TypeTag::Memory:
            return std::string("strategy:") +
                   strategy_name(static_cast<std::uint8_t>(entry.as_uint));
        default:
            if (is_signed_integer(entry.tag)) {
                std::snprintf(scratch, sizeof(scratch), "%" PRId64, entry.as_int);
            } else {
                std::snprintf(scratch, sizeof(scratch), "%" PRIu64, entry.as_uint);
            }
            return scratch;
    }
}

std::string disassemble_instruction(const Module& module, const MethodEntry& method,
                                    std::uint32_t offset, std::uint32_t& next) {
    util::StringBuilder out;

    char scratch[32];
    std::snprintf(scratch, sizeof(scratch), "%5u  ", offset);
    out.append(scratch);

    if (offset >= method.code.size()) {
        next = offset + 1;
        return out.append("<past end>").str();
    }

    auto op = static_cast<Op>(method.code[offset]);
    if (op >= Op::Count) {
        next = offset + 1;
        std::snprintf(scratch, sizeof(scratch), "<bad opcode 0x%02x>", method.code[offset]);
        return out.append(scratch).str();
    }

    std::size_t mnemonic_start = out.size();
    out.append(mnemonic(op));
    OperandFormat format = operand_format(op);
    next = offset + instruction_size(op);
    if (format == OperandFormat::None) return out.str();

    pad_to(out, mnemonic_start, 16);
    std::uint32_t operand = offset + 1;

    switch (format) {
        case OperandFormat::U8:
            out.append_uint(method.code[operand]);
            break;

        case OperandFormat::U16: {
            std::uint16_t value = read_u16(method, operand);
            out.append_uint(value);
            // Annotate the pool index and call targets so a listing reads
            // without cross-referencing tables by hand.
            if (op == Op::LoadConst) {
                out.append("    ; ").append(describe_constant(module, value));
            } else if (op == Op::CallDirect) {
                if (const MethodEntry* target = module.method_at(value)) {
                    out.append("    ; ").append(module.string_at(target->name));
                }
            } else if (op == Op::Alloc || op == Op::ManualAlloc) {
                if (const ClassEntry* target = module.class_at(value)) {
                    out.append("    ; ").append(module.string_at(target->name));
                }
            }
            break;
        }

        case OperandFormat::I32: {
            std::int32_t delta = read_i32(method, operand);
            out.append_int(delta);
            out.append("    ; -> ").append_uint(static_cast<std::uint64_t>(
                static_cast<std::int64_t>(next) + delta));
            break;
        }

        case OperandFormat::Type:
            out.append(type_tag_name(static_cast<TypeTag>(method.code[operand])));
            break;

        case OperandFormat::TypeType:
            out.append(type_tag_name(static_cast<TypeTag>(method.code[operand])));
            out.append(" -> ");
            out.append(type_tag_name(static_cast<TypeTag>(method.code[operand + 1])));
            break;

        case OperandFormat::U16U8: {
            std::uint16_t first = read_u16(method, operand);
            std::uint8_t second =
                operand + 2 < method.code.size() ? method.code[operand + 2] : 0;
            out.append_uint(first).append(", ");
            if (op == Op::Materialize) {
                out.append(strategy_name(second));
                if (const ClassEntry* target = module.class_at(first)) {
                    out.append("    ; ").append(module.string_at(target->name));
                }
            } else {
                out.append_uint(second);
            }
            break;
        }

        case OperandFormat::U16U8U8: {
            std::uint16_t slot = read_u16(method, operand);
            std::uint8_t argc = operand + 2 < method.code.size() ? method.code[operand + 2] : 0;
            std::uint8_t result = operand + 3 < method.code.size() ? method.code[operand + 3] : 0;
            out.append_uint(slot).append(", ").append_uint(argc).append(", ").append_uint(result);
            out.append("    ; slot, argc, leaves a value");
            break;
        }

        case OperandFormat::None:
            break;
    }

    return out.str();
}

std::string disassemble_method(const Module& module, const MethodEntry& method) {
    util::StringBuilder out;
    std::uint32_t offset = 0;
    while (offset < method.code.size()) {
        std::uint32_t next = offset;
        out.append_line(disassemble_instruction(module, method, offset, next));
        if (next <= offset) break;  // never loop on a malformed image
        offset = next;
    }
    return out.str();
}

std::string disassemble(const Module& module) {
    util::StringBuilder out;

    out.append("; Khudra bytecode v").append_uint(kVersionMajor).append('.').append_uint(
        kVersionMinor);
    out.append("  source=").append_line(module.string_at(module.source_file));
    out.append("; entry: ");
    if (module.main_method >= 0) {
        const MethodEntry* main = module.method_at(module.main_method);
        out.append("main -> method #").append_int(module.main_method);
        if (main) out.append(" (").append(module.string_at(main->name)).append(')');
    } else if (module.root_class >= 0) {
        const ClassEntry* root = module.class_at(module.root_class);
        out.append("root-class materialization -> class #").append_int(module.root_class);
        if (root) out.append(" (").append(module.string_at(root->name)).append(')');
    } else {
        out.append("none");
    }
    out.append_line();

    out.append_line();
    out.append("; constants (").append_uint(module.constants.size()).append_line(")");
    for (std::size_t i = 0; i < module.constants.size(); ++i) {
        out.append("  #").append_uint(i).append("  ")
            .append(type_tag_name(module.constants[i].tag))
            .append("  ")
            .append_line(describe_constant(module, static_cast<std::uint32_t>(i)));
    }

    out.append_line();
    out.append("; classes (").append_uint(module.classes.size()).append_line(")");
    for (std::size_t i = 0; i < module.classes.size(); ++i) {
        const ClassEntry& entry = module.classes[i];
        out.append("  class #").append_uint(i).append("  ").append(module.string_at(entry.name));
        out.append("  strategy=")
            .append((entry.flags & kClassManual) ? "manual" : "standard");
        out.append("  size=").append_uint(entry.object_size);
        if (entry.base >= 0) {
            const ClassEntry* base = module.class_at(entry.base);
            out.append("  extends ").append(base ? module.string_at(base->name) : "?");
        }
        out.append_line();

        for (std::size_t f = 0; f < entry.fields.size(); ++f) {
            const FieldEntry& field = entry.fields[f];
            const char* kind = field.ref_kind == kRefManaged    ? "managed-ref"
                               : field.ref_kind == kRefManual  ? "manual-ref"
                               : field.ref_kind == kRefDynamic ? "dynamic-ref"
                                                              : "raw";
            out.append("      slot ").append_uint(f).append("  ")
                .append(field.is_public ? "public  " : "private ")
                .append(type_tag_name(field.type))
                .append(' ')
                .append(module.string_at(field.name))
                .append("  @").append_uint(field.offset)
                .append("  ").append_line(kind);
        }
        for (std::size_t v = 0; v < entry.vtable.size(); ++v) {
            const MethodEntry* target = module.method_at(static_cast<std::int32_t>(entry.vtable[v]));
            out.append("      vtable ").append_uint(v).append("  -> method #")
                .append_uint(entry.vtable[v]);
            if (target) out.append("  ").append(module.string_at(target->name));
            out.append_line();
        }
        if (entry.constructor >= 0) {
            out.append("      constructor -> method #").append_int(entry.constructor).append_line();
        }
        if (entry.procedures >= 0) {
            out.append("      Procedures  -> method #").append_int(entry.procedures).append_line();
        }
    }

    out.append_line();
    out.append("; methods (").append_uint(module.methods.size()).append_line(")");
    for (std::size_t i = 0; i < module.methods.size(); ++i) {
        const MethodEntry& method = module.methods[i];
        const ClassEntry* owner = module.class_at(static_cast<std::int32_t>(method.owner_class));

        out.append_line();
        out.append("method #").append_uint(i).append("  ");
        if (owner) out.append(module.string_at(owner->name)).append('.');
        out.append(module.string_at(method.name));
        out.append("  params=").append_uint(method.param_count);
        out.append("  frame=").append_uint(method.frame_size);
        out.append("  returns=").append(type_tag_name(method.return_type));
        if (method.flags & kMethodStatic) out.append("  [static]");
        if (method.flags & kMethodNative) out.append("  [native]");
        if (method.flags & kMethodConstructor) out.append("  [constructor]");
        if (method.flags & kMethodProcedures) out.append("  [Procedures]");
        out.append_line();
        out.append(disassemble_method(module, method));
    }

    return out.str();
}

}  // namespace khu::bytecode
