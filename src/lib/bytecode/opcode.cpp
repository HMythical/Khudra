#include "bytecode/opcode.h"

namespace khu::bytecode {
namespace {

struct OpInfo {
    const char* mnemonic;
    OperandFormat format;
};

const OpInfo* op_table() {
    static const OpInfo table[] = {
#define KHU_OPCODE_ROW(name, text, fmt) OpInfo{text, fmt},
        KHU_OPCODES(KHU_OPCODE_ROW)
#undef KHU_OPCODE_ROW
    };
    return table;
}

}  // namespace

const char* mnemonic(Op op) {
    if (op >= Op::Count) return "<bad-opcode>";
    return op_table()[static_cast<std::size_t>(op)].mnemonic;
}

OperandFormat operand_format(Op op) {
    if (op >= Op::Count) return OperandFormat::None;
    return op_table()[static_cast<std::size_t>(op)].format;
}

std::uint32_t operand_size(OperandFormat format) {
    switch (format) {
        case OperandFormat::None: return 0;
        case OperandFormat::U8: return 1;
        case OperandFormat::U16: return 2;
        case OperandFormat::I32: return 4;
        case OperandFormat::Type: return 1;
        case OperandFormat::TypeType: return 2;
        case OperandFormat::U16U8: return 3;
    }
    return 0;
}

std::uint32_t instruction_size(Op op) { return 1 + operand_size(operand_format(op)); }

const char* type_tag_name(TypeTag tag) {
    switch (tag) {
        case TypeTag::Void: return "void";
        case TypeTag::Int8: return "int8";
        case TypeTag::Int16: return "int16";
        case TypeTag::Int32: return "int32";
        case TypeTag::Int64: return "int64";
        case TypeTag::UInt8: return "uint8";
        case TypeTag::UInt16: return "uint16";
        case TypeTag::UInt32: return "uint32";
        case TypeTag::UInt64: return "uint64";
        case TypeTag::Float32: return "float";
        case TypeTag::Float64: return "dfloat";
        case TypeTag::Bool: return "bool";
        case TypeTag::String: return "string";
        case TypeTag::Array: return "Array";
        case TypeTag::Ref: return "ref";
        case TypeTag::Ptr: return "ptr";
        case TypeTag::Memory: return "MemoryAllocationTypeObject";
        case TypeTag::Null: return "null";
        case TypeTag::Count: break;
    }
    return "<type>";
}

bool is_signed_integer(TypeTag tag) {
    return tag >= TypeTag::Int8 && tag <= TypeTag::Int64;
}

bool is_integer(TypeTag tag) { return tag >= TypeTag::Int8 && tag <= TypeTag::UInt64; }

bool is_float(TypeTag tag) { return tag == TypeTag::Float32 || tag == TypeTag::Float64; }

bool is_numeric(TypeTag tag) { return is_integer(tag) || is_float(tag); }

bool is_reference(TypeTag tag) {
    switch (tag) {
        case TypeTag::String:
        case TypeTag::Array:
        case TypeTag::Ref:
        case TypeTag::Ptr:
        case TypeTag::Null:
            return true;
        default:
            return false;
    }
}

std::uint32_t type_width(TypeTag tag) {
    switch (tag) {
        case TypeTag::Int8:
        case TypeTag::UInt8: return 8;
        case TypeTag::Int16:
        case TypeTag::UInt16: return 16;
        case TypeTag::Int32:
        case TypeTag::UInt32:
        case TypeTag::Float32: return 32;
        case TypeTag::Int64:
        case TypeTag::UInt64:
        case TypeTag::Float64: return 64;
        default: return 0;
    }
}

}  // namespace khu::bytecode
