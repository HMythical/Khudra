#include "codegen/emitter.h"

#include <string>

#include "sema/builtins.h"

namespace khu::codegen {

using bytecode::Op;
using bytecode::TypeTag;

namespace {

std::uint8_t ref_kind_byte(sema::RefKind kind) {
    switch (kind) {
        case sema::RefKind::ManagedRef: return bytecode::kRefManaged;
        case sema::RefKind::ManualRef: return bytecode::kRefManual;
        case sema::RefKind::Raw: break;
    }
    return bytecode::kRefRaw;
}

}  // namespace

void Emitter::error(diag::SourceLocation loc, std::string message) {
    diagnostics_.error(loc, std::move(message));
    failed_ = true;
}

bytecode::TypeTag Emitter::tag_of(const sema::Type* type) const {
    if (!type) return TypeTag::Void;
    switch (type->kind) {
        case sema::TypeKind::Void: return TypeTag::Void;
        case sema::TypeKind::Bool: return TypeTag::Bool;
        case sema::TypeKind::Int:
            switch (type->width) {
                case 8: return TypeTag::Int8;
                case 16: return TypeTag::Int16;
                case 64: return TypeTag::Int64;
                default: return TypeTag::Int32;
            }
        case sema::TypeKind::UInt:
            switch (type->width) {
                case 8: return TypeTag::UInt8;
                case 16: return TypeTag::UInt16;
                case 64: return TypeTag::UInt64;
                default: return TypeTag::UInt32;
            }
        case sema::TypeKind::Float:
            return type->width == 32 ? TypeTag::Float32 : TypeTag::Float64;
        case sema::TypeKind::String: return TypeTag::String;
        case sema::TypeKind::Array: return TypeTag::Array;
        case sema::TypeKind::Pointer: return TypeTag::Ptr;
        case sema::TypeKind::Class: return TypeTag::Ref;
        case sema::TypeKind::Memory: return TypeTag::Memory;
        case sema::TypeKind::Null: return TypeTag::Null;
        case sema::TypeKind::Error: return TypeTag::Void;
    }
    return TypeTag::Void;
}

// ---------------------------------------------------------------------------
// Instruction writing
// ---------------------------------------------------------------------------

void Emitter::write_u8(std::uint8_t value) { buffer_->code.push(value); }

void Emitter::write_u16(std::uint16_t value) {
    write_u8(static_cast<std::uint8_t>(value & 0xff));
    write_u8(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void Emitter::write_i32(std::int32_t value) {
    auto bits = static_cast<std::uint32_t>(value);
    for (int i = 0; i < 4; ++i) write_u8(static_cast<std::uint8_t>((bits >> (i * 8)) & 0xff));
}

void Emitter::write_op(Op code) { write_u8(static_cast<std::uint8_t>(code)); }

// Records a source position for the instruction about to be written, so the VM
// can map a trap back to a line.
void Emitter::mark(diag::SourceLocation loc) {
    if (!buffer_ || !loc.valid() || loc.line == buffer_->last_line) return;
    buffer_->lines.push(bytecode::LineEntry{static_cast<std::uint32_t>(buffer_->code.size()),
                                            loc.line, loc.column});
    buffer_->last_line = loc.line;
}

void Emitter::op(Op code) { write_op(code); }

void Emitter::op_u16(Op code, std::uint16_t operand) {
    write_op(code);
    write_u16(operand);
}

void Emitter::op_type(Op code, TypeTag tag) {
    write_op(code);
    write_u8(static_cast<std::uint8_t>(tag));
}

void Emitter::op_u16_u8(Op code, std::uint16_t first, std::uint8_t second) {
    write_op(code);
    write_u16(first);
    write_u8(second);
}

std::size_t Emitter::emit_jump(Op code) {
    write_op(code);
    std::size_t site = buffer_->code.size();
    write_i32(0);
    return site;
}

// Jump operands are relative to the instruction's end, so code stays position
// independent when methods are concatenated.
void Emitter::patch_jump(std::size_t site) {
    auto target = static_cast<std::int64_t>(buffer_->code.size());
    auto after = static_cast<std::int64_t>(site + 4);
    auto offset = static_cast<std::int32_t>(target - after);
    auto bits = static_cast<std::uint32_t>(offset);
    for (int i = 0; i < 4; ++i) {
        buffer_->code[site + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((bits >> (i * 8)) & 0xff);
    }
}

// ---------------------------------------------------------------------------
// Module assembly
// ---------------------------------------------------------------------------

bool Emitter::needs_field_initializer(const sema::ClassSymbol& symbol) const {
    for (const sema::VarSymbol* field : symbol.fields) {
        if (field->field_decl && field->field_decl->init) return true;
    }
    return false;
}

void Emitter::assign_method_indices() {
    std::uint32_t next = 0;
    field_init_indices_.resize(program_.classes.size(), -1);
    for (sema::ClassSymbol* symbol : program_.classes) {
        for (sema::MethodSymbol* method : symbol->methods) method->method_id = next++;
        if (symbol->constructor) symbol->constructor->method_id = next++;
        if (symbol->procedures) symbol->procedures->method_id = next++;
        if (needs_field_initializer(*symbol)) {
            field_init_indices_[symbol->class_id] = static_cast<std::int32_t>(next++);
        }
    }
    module_->methods.resize(next);
}

// How many allocation-site arguments a class binds. One site supplies them to
// every Procedures block and constructor in the chain, and the checker has
// already required those lists to agree, so the nearest declared one wins.
std::uint8_t materialization_argc(const sema::ClassSymbol& symbol) {
    for (const sema::ClassSymbol* current = &symbol; current; current = current->base) {
        if (current->procedures && !current->procedures->params.empty()) {
            return static_cast<std::uint8_t>(current->procedures->params.size());
        }
        if (current->constructor && !current->constructor->params.empty()) {
            return static_cast<std::uint8_t>(current->constructor->params.size());
        }
    }
    return 0;
}

void Emitter::emit_classes() {
    for (sema::ClassSymbol* symbol : program_.classes) {
        bytecode::ClassEntry entry;
        entry.name = module_->intern_string(symbol->name);
        entry.base = symbol->base ? static_cast<std::int32_t>(symbol->base->class_id) : -1;
        entry.flags = symbol->strategy == sema::Strategy::Manual
                          ? static_cast<std::uint32_t>(bytecode::kClassManual)
                          : static_cast<std::uint32_t>(bytecode::kClassGc);
        entry.object_size = symbol->object_size;
        entry.constructor =
            symbol->constructor ? static_cast<std::int32_t>(symbol->constructor->method_id) : -1;
        entry.procedures =
            symbol->procedures ? static_cast<std::int32_t>(symbol->procedures->method_id) : -1;
        entry.field_init = field_init_indices_[symbol->class_id];
        entry.materialize_argc = materialization_argc(*symbol);

        for (sema::VarSymbol* field : symbol->layout) {
            bytecode::FieldEntry slot;
            slot.name = module_->intern_string(field->name);
            slot.type = tag_of(field->type);
            slot.offset = field->offset;
            slot.ref_kind = ref_kind_byte(field->ref_kind);
            slot.is_public = field->visibility == ast::Visibility::Public ? 1 : 0;
            if (field->type && field->type->is_class() && field->type->class_symbol) {
                slot.class_ref = field->type->class_symbol->class_id;
            }
            entry.fields.push(slot);
        }

        for (sema::MethodSymbol* method : symbol->vtable) entry.vtable.push(method->method_id);

        module_->classes.push(std::move(entry));
    }
}

bool Emitter::emit(ast::CompilationUnit& unit, std::string_view source_path,
                   bytecode::Module& out) {
    (void)unit;
    module_ = &out;
    failed_ = false;

    out.source_file = out.intern_string(source_path);
    assign_method_indices();
    emit_classes();

    for (sema::ClassSymbol* symbol : program_.classes) {
        for (sema::MethodSymbol* method : symbol->methods) emit_method(*method);
        if (symbol->constructor) emit_method(*symbol->constructor);
        if (symbol->procedures) emit_procedures(*symbol->procedures);
        if (field_init_indices_[symbol->class_id] >= 0) emit_field_initializer(*symbol);
    }

    out.main_method =
        program_.main_function ? static_cast<std::int32_t>(program_.main_function->method_id) : -1;
    out.root_class =
        program_.root_class ? static_cast<std::int32_t>(program_.root_class->class_id) : -1;

    return !failed_;
}

void Emitter::emit_method(sema::MethodSymbol& method) {
    CodeBuffer buffer;
    buffer_ = &buffer;
    current_return_ = method.return_type;

    if (method.decl && method.decl->body) emit_block(method.decl->body);
    // A void method may fall off the end; a value-returning one cannot, because
    // the checker already rejected that.
    mark(method.loc);
    op(Op::Return);

    bytecode::MethodEntry& entry = module_->methods[method.method_id];
    entry.name = module_->intern_string(method.name);
    entry.owner_class = method.owner ? method.owner->class_id : 0;
    entry.flags = method.is_constructor()
                      ? static_cast<std::uint32_t>(bytecode::kMethodConstructor)
                      : 0u;
    entry.param_count = static_cast<std::uint8_t>(method.params.size());
    entry.frame_size = static_cast<std::uint16_t>(method.frame_size);
    entry.return_type = tag_of(method.return_type);
    entry.code = std::move(buffer.code);
    entry.lines = std::move(buffer.lines);

    buffer_ = nullptr;
    current_return_ = nullptr;
}

// One `this.field = <initializer>` per declared initializer, in source order.
// The VM runs these base class first, so a derived class never sees an
// uninitialized inherited slot.
void Emitter::emit_field_initializer(sema::ClassSymbol& symbol) {
    CodeBuffer buffer;
    buffer_ = &buffer;
    current_return_ = nullptr;

    for (sema::VarSymbol* field : symbol.fields) {
        if (!field->field_decl || !field->field_decl->init) continue;
        mark(field->loc);
        op(Op::LoadThis);
        emit_expr(field->field_decl->init);
        op_u16(Op::PutField, static_cast<std::uint16_t>(field->slot));
    }
    op(Op::Return);

    bytecode::MethodEntry& entry =
        module_->methods[static_cast<std::size_t>(field_init_indices_[symbol.class_id])];
    entry.name = module_->intern_string("<fieldinit>");
    entry.owner_class = symbol.class_id;
    entry.flags = static_cast<std::uint32_t>(bytecode::kMethodFieldInit);
    entry.param_count = 0;
    entry.frame_size = 0;
    entry.return_type = TypeTag::Void;
    entry.code = std::move(buffer.code);
    entry.lines = std::move(buffer.lines);

    buffer_ = nullptr;
}

void Emitter::emit_procedures(sema::ProcedureSymbol& procedures) {
    CodeBuffer buffer;
    buffer_ = &buffer;
    current_return_ = nullptr;

    if (procedures.decl && procedures.decl->body) emit_block(procedures.decl->body);
    mark(procedures.loc);
    op(Op::Return);

    bytecode::MethodEntry& entry = module_->methods[procedures.method_id];
    entry.name = module_->intern_string("Procedures");
    entry.owner_class = procedures.owner ? procedures.owner->class_id : 0;
    entry.flags = static_cast<std::uint32_t>(bytecode::kMethodProcedures);
    entry.param_count = static_cast<std::uint8_t>(procedures.params.size());
    entry.frame_size = static_cast<std::uint16_t>(procedures.frame_size);
    entry.return_type = TypeTag::Void;
    entry.code = std::move(buffer.code);
    entry.lines = std::move(buffer.lines);

    buffer_ = nullptr;
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

void Emitter::emit_block(const ast::BlockStmt* block) {
    if (!block) return;
    for (const ast::Stmt* statement : block->statements) emit_stmt(statement);
}

void Emitter::emit_stmt(const ast::Stmt* statement) {
    if (!statement) return;
    mark(statement->loc);

    switch (statement->kind) {
        case ast::StmtKind::Empty:
            break;

        case ast::StmtKind::Block:
            emit_block(static_cast<const ast::BlockStmt*>(statement));
            break;

        case ast::StmtKind::VarDecl: {
            const auto& decl = *static_cast<const ast::VarDeclStmt*>(statement);
            if (!decl.symbol) break;
            if (decl.init) {
                emit_expr(decl.init);
            } else {
                // A local with no initializer holds its type's default.
                emit_default_value(decl.symbol->type, decl.loc);
            }
            op_u16(Op::StoreLocal, static_cast<std::uint16_t>(decl.symbol->frame_index));
            break;
        }

        case ast::StmtKind::Expr:
            emit_expr_discard(static_cast<const ast::ExprStmt*>(statement)->expr);
            break;

        case ast::StmtKind::Return: {
            const auto& node = *static_cast<const ast::ReturnStmt*>(statement);
            if (node.value) {
                emit_expr(node.value);
                op(Op::ReturnValue);
            } else {
                op(Op::Return);
            }
            break;
        }

        case ast::StmtKind::If: {
            const auto& node = *static_cast<const ast::IfStmt*>(statement);
            emit_expr(node.condition);
            std::size_t to_else = emit_jump(Op::JumpIfFalse);
            emit_stmt(node.then_branch);
            if (node.else_branch) {
                std::size_t to_end = emit_jump(Op::Jump);
                patch_jump(to_else);
                emit_stmt(node.else_branch);
                patch_jump(to_end);
            } else {
                patch_jump(to_else);
            }
            break;
        }

        case ast::StmtKind::While: {
            const auto& node = *static_cast<const ast::WhileStmt*>(statement);
            std::size_t top = here();
            emit_expr(node.condition);
            std::size_t to_end = emit_jump(Op::JumpIfFalse);
            emit_stmt(node.body);
            // Jump back to the condition; the operand is relative to the end of
            // this instruction.
            write_op(Op::Jump);
            std::size_t site = buffer_->code.size();
            write_i32(0);
            auto offset = static_cast<std::int32_t>(static_cast<std::int64_t>(top) -
                                                    static_cast<std::int64_t>(site + 4));
            auto bits = static_cast<std::uint32_t>(offset);
            for (int i = 0; i < 4; ++i) {
                buffer_->code[site + static_cast<std::size_t>(i)] =
                    static_cast<std::uint8_t>((bits >> (i * 8)) & 0xff);
            }
            patch_jump(to_end);
            break;
        }

        case ast::StmtKind::Free: {
            const auto& node = *static_cast<const ast::FreeStmt*>(statement);
            emit_expr(node.target);
            op(Op::Free);
            break;
        }
    }
}

void Emitter::emit_default_value(const sema::Type* type, diag::SourceLocation loc) {
    if (!type) {
        op(Op::LoadNull);
        return;
    }
    switch (type->kind) {
        case sema::TypeKind::Bool:
            op(Op::LoadFalse);
            break;
        case sema::TypeKind::Int:
            op_u16(Op::LoadConst, static_cast<std::uint16_t>(
                                      module_->intern_int(tag_of(type), 0)));
            break;
        case sema::TypeKind::UInt:
            op_u16(Op::LoadConst, static_cast<std::uint16_t>(
                                      module_->intern_uint(tag_of(type), 0)));
            break;
        case sema::TypeKind::Float:
            op_u16(Op::LoadConst, static_cast<std::uint16_t>(
                                      module_->intern_float(tag_of(type), 0.0)));
            break;
        case sema::TypeKind::Memory:
            // The strategy descriptor is a tag, not an object.
            op_u16(Op::LoadConst,
                   static_cast<std::uint16_t>(module_->intern_uint(TypeTag::Memory, 0)));
            break;
        case sema::TypeKind::Error:
            error(loc, "cannot emit a default value for an unresolved type");
            op(Op::LoadNull);
            break;
        default:
            // References start as `null`: not instantiated yet.
            op(Op::LoadNull);
            break;
    }
}

}  // namespace khu::codegen
