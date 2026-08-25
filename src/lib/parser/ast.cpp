#include "parser/ast.h"

namespace khu::ast {

const char* visibility_name(Visibility visibility) {
    return visibility == Visibility::Public ? "public" : "private";
}

const char* strategy_name(Strategy strategy) {
    switch (strategy) {
        case Strategy::ClassDefault: return "class-default";
        case Strategy::Gc: return "standard";
        case Strategy::Manual: return "manual";
    }
    return "class-default";
}

const char* builtin_canonical_name(BuiltinType type) {
    switch (type) {
        case BuiltinType::Int8: return "int8";
        case BuiltinType::Int16: return "int16";
        case BuiltinType::Int32: return "int32";
        case BuiltinType::Int64: return "int64";
        case BuiltinType::UInt8: return "uint8";
        case BuiltinType::UInt16: return "uint16";
        case BuiltinType::UInt32: return "uint32";
        case BuiltinType::UInt64: return "uint64";
        case BuiltinType::Float32: return "float";
        case BuiltinType::Float64: return "dfloat";
        case BuiltinType::Bool: return "bool";
        case BuiltinType::String: return "string";
        case BuiltinType::Array: return "Array";
        case BuiltinType::MemoryAllocationTypeObject: return "MemoryAllocationTypeObject";
        case BuiltinType::Void: return "void";
    }
    return "<type>";
}

const char* unary_op_spelling(UnaryOp op) {
    switch (op) {
        case UnaryOp::Plus: return "+";
        case UnaryOp::Negate: return "-";
        case UnaryOp::Not: return "!";
        case UnaryOp::BitNot: return "~";
    }
    return "?";
}

const char* binary_op_spelling(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Sub: return "-";
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Rem: return "%";
        case BinaryOp::Equal: return "==";
        case BinaryOp::NotEqual: return "!=";
        case BinaryOp::Less: return "<";
        case BinaryOp::LessEqual: return "<=";
        case BinaryOp::Greater: return ">";
        case BinaryOp::GreaterEqual: return ">=";
        case BinaryOp::LogicalAnd: return "&&";
        case BinaryOp::LogicalOr: return "||";
        case BinaryOp::BitAnd: return "&";
        case BinaryOp::BitOr: return "|";
        case BinaryOp::BitXor: return "^";
        case BinaryOp::ShiftLeft: return "<<";
        case BinaryOp::ShiftRight: return ">>";
    }
    return "?";
}

}  // namespace khu::ast
