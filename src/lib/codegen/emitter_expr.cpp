// Expression emission.
#include <string>

#include "bytecode/native.h"
#include "codegen/emitter.h"
#include "sema/builtins.h"

namespace khu::codegen {

using bytecode::Op;
using bytecode::TypeTag;

namespace {

Op arithmetic_op(ast::BinaryOp op) {
    switch (op) {
        case ast::BinaryOp::Add: return Op::Add;
        case ast::BinaryOp::Sub: return Op::Sub;
        case ast::BinaryOp::Mul: return Op::Mul;
        case ast::BinaryOp::Div: return Op::Div;
        case ast::BinaryOp::Rem: return Op::Rem;
        case ast::BinaryOp::BitAnd: return Op::BitAnd;
        case ast::BinaryOp::BitOr: return Op::BitOr;
        case ast::BinaryOp::BitXor: return Op::BitXor;
        case ast::BinaryOp::ShiftLeft: return Op::Shl;
        case ast::BinaryOp::ShiftRight: return Op::Shr;
        case ast::BinaryOp::Equal: return Op::CmpEq;
        case ast::BinaryOp::NotEqual: return Op::CmpNe;
        case ast::BinaryOp::Less: return Op::CmpLt;
        case ast::BinaryOp::LessEqual: return Op::CmpLe;
        case ast::BinaryOp::Greater: return Op::CmpGt;
        case ast::BinaryOp::GreaterEqual: return Op::CmpGe;
        case ast::BinaryOp::LogicalAnd:
        case ast::BinaryOp::LogicalOr: break;
    }
    return Op::Nop;
}

Op intrinsic_op(sema::Intrinsic which) {
    switch (which) {
        case sema::Intrinsic::Add: return Op::Add;
        case sema::Intrinsic::Subtract: return Op::Sub;
        case sema::Intrinsic::Multiply: return Op::Mul;
        case sema::Intrinsic::Divide: return Op::Div;
        case sema::Intrinsic::Remainder: return Op::Rem;
        case sema::Intrinsic::Neg: return Op::Neg;
        case sema::Intrinsic::ArraySize: return Op::ArrayLen;
        default: break;
    }
    return Op::Nop;
}

const sema::ExprInfo* info_of(const ast::Expr* expr) { return expr ? expr->info : nullptr; }

const sema::Type* type_of(const ast::Expr* expr) {
    const sema::ExprInfo* info = info_of(expr);
    return info ? info->type : nullptr;
}

}  // namespace

void Emitter::emit_load_var(const sema::VarSymbol& var, const ast::Expr* object) {
    if (var.is_field()) {
        if (object) {
            emit_expr(object);
        } else {
            op(Op::LoadThis);
        }
        op_u16(Op::GetField, static_cast<std::uint16_t>(var.slot));
        return;
    }
    op_u16(Op::LoadLocal, static_cast<std::uint16_t>(var.frame_index));
}

void Emitter::emit_expr(const ast::Expr* expr) {
    if (!expr) {
        op(Op::LoadNull);
        return;
    }
    mark(expr->loc);

    switch (expr->kind) {
        case ast::ExprKind::IntLiteral: {
            const auto& literal = *static_cast<const ast::IntLiteralExpr*>(expr);
            TypeTag tag = tag_of(type_of(expr));
            std::uint32_t index =
                bytecode::is_signed_integer(tag)
                    ? module_->intern_int(tag, static_cast<std::int64_t>(literal.value))
                    : module_->intern_uint(tag, literal.value);
            op_u16(Op::LoadConst, static_cast<std::uint16_t>(index));
            break;
        }

        case ast::ExprKind::FloatLiteral: {
            const auto& literal = *static_cast<const ast::FloatLiteralExpr*>(expr);
            op_u16(Op::LoadConst,
                   static_cast<std::uint16_t>(
                       module_->intern_float(tag_of(type_of(expr)), literal.value)));
            break;
        }

        case ast::ExprKind::BoolLiteral:
            op(static_cast<const ast::BoolLiteralExpr*>(expr)->value ? Op::LoadTrue
                                                                     : Op::LoadFalse);
            break;

        case ast::ExprKind::StringLiteral:
            op_u16(Op::LoadConst,
                   static_cast<std::uint16_t>(module_->intern_string(
                       static_cast<const ast::StringLiteralExpr*>(expr)->value)));
            break;

        case ast::ExprKind::NullLiteral:
            op(Op::LoadNull);
            break;

        case ast::ExprKind::This:
            op(Op::LoadThis);
            break;

        case ast::ExprKind::Identifier: {
            const sema::ExprInfo* info = info_of(expr);
            if (!info || !info->var) {
                error(expr->loc, "unresolved name reached code generation");
                op(Op::LoadNull);
                break;
            }
            emit_load_var(*info->var, nullptr);
            break;
        }

        case ast::ExprKind::Member: {
            const auto& member = *static_cast<const ast::MemberExpr*>(expr);
            const sema::ExprInfo* info = info_of(expr);
            if (!info || !info->var) {
                error(expr->loc, "unresolved member reached code generation");
                op(Op::LoadNull);
                break;
            }
            emit_load_var(*info->var, member.object);
            break;
        }

        case ast::ExprKind::Call:
            emit_call(*static_cast<const ast::CallExpr*>(expr), false);
            break;

        case ast::ExprKind::Assign:
            emit_assign(*static_cast<const ast::AssignExpr*>(expr), true);
            break;

        case ast::ExprKind::Unary: {
            const auto& unary = *static_cast<const ast::UnaryExpr*>(expr);
            emit_expr(unary.operand);
            switch (unary.op) {
                case ast::UnaryOp::Plus:
                    break;  // a no-op on every numeric type
                case ast::UnaryOp::Negate:
                    op_type(Op::Neg, tag_of(type_of(expr)));
                    break;
                case ast::UnaryOp::Not:
                    op(Op::LogicalNot);
                    break;
                case ast::UnaryOp::BitNot:
                    op_type(Op::BitNot, tag_of(type_of(expr)));
                    break;
            }
            break;
        }

        case ast::ExprKind::Binary: {
            const auto& binary = *static_cast<const ast::BinaryExpr*>(expr);
            if (binary.op == ast::BinaryOp::LogicalAnd || binary.op == ast::BinaryOp::LogicalOr) {
                emit_logical(binary);
                break;
            }

            emit_expr(binary.left);
            emit_expr(binary.right);

            bool is_comparison = binary.op == ast::BinaryOp::Equal ||
                                 binary.op == ast::BinaryOp::NotEqual ||
                                 binary.op == ast::BinaryOp::Less ||
                                 binary.op == ast::BinaryOp::LessEqual ||
                                 binary.op == ast::BinaryOp::Greater ||
                                 binary.op == ast::BinaryOp::GreaterEqual;

            // A comparison's own type is bool, so the width comes from the
            // operands.
            const sema::Type* operand = type_of(binary.left);
            TypeTag tag = tag_of(is_comparison ? operand : type_of(expr));

            if (is_comparison && !bytecode::is_numeric(tag)) {
                // References compare by identity; bools compare by value.
                bool equality = binary.op == ast::BinaryOp::Equal ||
                                binary.op == ast::BinaryOp::NotEqual;
                if (!equality) {
                    error(expr->loc, "ordering comparison on a non-numeric type reached code "
                                     "generation");
                    break;
                }
                bool wants_equal = binary.op == ast::BinaryOp::Equal;
                if (tag == TypeTag::Bool) {
                    op_type(wants_equal ? Op::CmpEq : Op::CmpNe, TypeTag::Bool);
                } else {
                    op(wants_equal ? Op::RefEq : Op::RefNe);
                }
                break;
            }

            Op code = arithmetic_op(binary.op);
            if (code == Op::Nop) {
                error(expr->loc, "unsupported binary operator reached code generation");
                break;
            }
            op_type(code, tag);
            break;
        }

        case ast::ExprKind::Index:
            emit_index_read(*static_cast<const ast::IndexExpr*>(expr));
            break;

        case ast::ExprKind::TypeRef:
        case ast::ExprKind::Strategy:
            error(expr->loc, "this expression has no runtime value");
            op(Op::LoadNull);
            break;
    }
}

// `a[i]` as a read. An array element is one instruction; a string byte is the
// same operation `khuStdString.charAt` performs, so it is that native rather
// than a second implementation of it.
void Emitter::emit_index_read(const ast::IndexExpr& index) {
    const sema::Type* object = type_of(index.object);
    emit_expr(index.object);
    emit_expr(index.index);

    if (object && object->kind == sema::TypeKind::String) {
        op_u16_u8(Op::CallNative,
                  static_cast<std::uint16_t>(bytecode::NativeId::StrCharAt), 2);
        return;
    }
    if (object && object->is_typed_array()) {
        op(Op::ArrayGet);
        return;
    }
    if (object && object->kind == sema::TypeKind::Pointer) {
        op_type(Op::PtrGet, tag_of(object->pointee));
        return;
    }
    error(index.loc, "this expression cannot be indexed");
    op(Op::LoadNull);
}

void Emitter::emit_expr_discard(const ast::Expr* expr) {
    if (!expr) return;

    // An assignment or a void call leaves nothing behind, so no pop is needed.
    if (expr->kind == ast::ExprKind::Assign) {
        emit_assign(*static_cast<const ast::AssignExpr*>(expr), false);
        return;
    }
    if (expr->kind == ast::ExprKind::Call) {
        emit_call(*static_cast<const ast::CallExpr*>(expr), true);
        return;
    }

    emit_expr(expr);
    const sema::Type* type = type_of(expr);
    if (type && !type->is_void() && !type->is_error()) op(Op::Pop);
}

// `&&` and `||` short-circuit, so the right operand sits behind a jump.
void Emitter::emit_logical(const ast::BinaryExpr& binary) {
    emit_expr(binary.left);
    op(Op::Dup);
    std::size_t skip = emit_jump(binary.op == ast::BinaryOp::LogicalAnd ? Op::JumpIfFalse
                                                                       : Op::JumpIfTrue);
    // The duplicated result is only kept when it decides the answer.
    op(Op::Pop);
    emit_expr(binary.right);
    patch_jump(skip);
}

void Emitter::emit_assign(const ast::AssignExpr& assign, bool keep_value) {
    const sema::ExprInfo* target_info = info_of(assign.target);

    // `a[i] = v`. arrayset and ptrset both consume the target, the index and
    // the value, so leaving the value behind means tucking a copy under all
    // three first.
    if (const auto* index = assign.target->as<ast::IndexExpr>()) {
        const sema::Type* object = type_of(index->object);
        mark(assign.loc);
        emit_expr(index->object);
        emit_expr(index->index);
        emit_expr(assign.value);
        if (keep_value) op(Op::DupX2);
        if (object && object->kind == sema::TypeKind::Pointer) {
            op_type(Op::PtrSet, tag_of(object->pointee));
        } else {
            op(Op::ArraySet);
        }
        return;
    }

    if (!target_info || !target_info->var) {
        error(assign.loc, "unresolved assignment target reached code generation");
        if (keep_value) op(Op::LoadNull);
        return;
    }
    const sema::VarSymbol& var = *target_info->var;
    mark(assign.loc);

    if (!var.is_field()) {
        emit_expr(assign.value);
        if (keep_value) op(Op::Dup);
        op_u16(Op::StoreLocal, static_cast<std::uint16_t>(var.frame_index));
        return;
    }

    if (const auto* member = assign.target->as<ast::MemberExpr>()) {
        emit_expr(member->object);
    } else {
        op(Op::LoadThis);
    }
    emit_expr(assign.value);
    // putfield pops the value and then the receiver, so leaving the value
    // behind means tucking a copy underneath the receiver first.
    if (keep_value) op(Op::DupX1);
    op_u16(Op::PutField, static_cast<std::uint16_t>(var.slot));
}

void Emitter::emit_call(const ast::CallExpr& call, bool discard) {
    const sema::ExprInfo* info = info_of(&call);
    mark(call.loc);

    if (info && info->is_allocation && info->alloc_class) {
        for (const ast::Expr* argument : call.args) emit_expr(argument);
        op_u16_u8(Op::Materialize, static_cast<std::uint16_t>(info->alloc_class->class_id),
                  static_cast<std::uint8_t>(info->alloc_strategy == sema::Strategy::Manual
                                                ? bytecode::StrategyByte::Manual
                                                : bytecode::StrategyByte::Gc));
        if (discard) op(Op::Pop);
        return;
    }

    if (info && info->is_strategy_factory) {
        // The strategy is a tag stamped into the object header at allocation
        // time; the field itself just records which one.
        op_u16(Op::LoadConst,
               static_cast<std::uint16_t>(module_->intern_uint(
                   TypeTag::Memory,
                   info->alloc_strategy == sema::Strategy::Manual
                       ? static_cast<std::uint64_t>(bytecode::StrategyByte::Manual)
                       : static_cast<std::uint64_t>(bytecode::StrategyByte::Gc))));
        if (discard) op(Op::Pop);
        return;
    }

    // khuStdCollection.arrayCreate is the only call the checker gives an
    // element type to; an index expression is not a call.
    if (info && info->array_element && call.args.size() == 2) {
        // khuStdCollection.arrayCreate(<type>, count). The type is consumed
        // here -- it is the element tag arraynew fills the fresh array with --
        // and only the count is a runtime value.
        emit_expr(call.args[1]);
        op_type(Op::ArrayNew, tag_of(info->array_element));
        if (discard) op(Op::Pop);
        return;
    }

    if (info && info->convert_target) {
        const ast::Expr* source = call.args.size() == 2 ? call.args[1] : nullptr;
        emit_expr(source);
        write_op(Op::Convert);
        write_u8(static_cast<std::uint8_t>(tag_of(type_of(source))));
        write_u8(static_cast<std::uint8_t>(tag_of(info->convert_target)));
        if (discard) op(Op::Pop);
        return;
    }

    sema::MethodSymbol* method = info ? info->method : nullptr;
    if (!method) {
        error(call.loc, "unresolved call reached code generation");
        op(Op::LoadNull);
        return;
    }

    if (method->is_intrinsic) {
        emit_intrinsic(call, *method);
        if (discard) op(Op::Pop);
        return;
    }

    if (method->is_native) {
        for (const ast::Expr* argument : call.args) emit_expr(argument);
        op_u16_u8(Op::CallNative, static_cast<std::uint16_t>(method->native_id),
                  static_cast<std::uint8_t>(call.args.size()));
        if (discard && method->return_type && !method->return_type->is_void()) op(Op::Pop);
        return;
    }

    // Receiver first, then arguments: the frame is built from the stack top
    // down, so this order matches what CallDirect/CallVirtual expect.
    const auto* member = call.callee->as<ast::MemberExpr>();
    if (member) {
        emit_expr(member->object);
    } else {
        op(Op::LoadThis);
    }
    for (const ast::Expr* argument : call.args) emit_expr(argument);

    // A private member or a constructor can never be overridden, so it is
    // called directly; everything else goes through the vtable.
    bool direct = method->visibility == ast::Visibility::Private || method->is_constructor();
    bool leaves_value = method->return_type && !method->return_type->is_void();
    if (direct) {
        op_u16(Op::CallDirect, static_cast<std::uint16_t>(method->method_id));
    } else {
        op_u16_u8_u8(Op::CallVirtual, static_cast<std::uint16_t>(method->vtable_slot),
                     static_cast<std::uint8_t>(call.args.size()),
                     static_cast<std::uint8_t>(leaves_value ? 1 : 0));
    }

    if (discard && method->return_type && !method->return_type->is_void()) op(Op::Pop);
}

void Emitter::emit_intrinsic(const ast::CallExpr& call, sema::MethodSymbol& method) {
    auto which = static_cast<sema::Intrinsic>(method.intrinsic_id);
    Op code = intrinsic_op(which);
    // An intrinsic's operands are its arguments, in order; the only thing that
    // varies is how many of them there are.
    if (code == Op::Nop || call.args.size() != sema::intrinsic_arity(which)) {
        error(call.loc, "unsupported intrinsic reached code generation");
        op(Op::LoadNull);
        return;
    }
    for (const ast::Expr* argument : call.args) emit_expr(argument);
    // Arithmetic carries the width it operates at; `arraylen` does not operate
    // at a width at all, so it carries nothing.
    if (bytecode::operand_format(code) == bytecode::OperandFormat::Type) {
        op_type(code, tag_of(method.return_type));
    } else {
        op(code);
    }
}

}  // namespace khu::codegen
