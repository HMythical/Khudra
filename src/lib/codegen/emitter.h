// AST + symbol table -> bytecode module.
//
// The emitter reads the annotations Sema left on the tree (ExprInfo), so it
// never has to re-derive a type or re-resolve a name.
#ifndef KHU_CODEGEN_EMITTER_H
#define KHU_CODEGEN_EMITTER_H

#include <cstdint>
#include <string_view>

#include "bytecode/module.h"
#include "diag/diagnostic.h"
#include "parser/ast.h"
#include "sema/checker.h"
#include "sema/symbol.h"
#include "util/array.h"

namespace khu::codegen {

class Emitter {
public:
    Emitter(sema::Program& program, diag::DiagnosticEngine& diagnostics)
        : program_(program), diagnostics_(diagnostics) {}

    // Fills `out`. Returns false when an error was reported.
    bool emit(ast::CompilationUnit& unit, std::string_view source_path, bytecode::Module& out);

private:
    // A code buffer plus the line table being built alongside it.
    struct CodeBuffer {
        util::Array<std::uint8_t> code;
        util::Array<bytecode::LineEntry> lines;
        std::uint32_t last_line = 0;
    };

    void assign_method_indices();
    void emit_classes();
    void emit_method(sema::MethodSymbol& method);
    void emit_procedures(sema::ProcedureSymbol& procedures);
    // Synthetic method holding a class's field initializers; it runs during
    // object linking, before the Procedures block.
    void emit_field_initializer(sema::ClassSymbol& symbol);
    bool needs_field_initializer(const sema::ClassSymbol& symbol) const;

    bytecode::TypeTag tag_of(const sema::Type* type) const;

    // --- instruction writing ---
    void mark(diag::SourceLocation loc);
    void write_op(bytecode::Op op);
    void write_u8(std::uint8_t value);
    void write_u16(std::uint16_t value);
    void write_i32(std::int32_t value);
    void op(bytecode::Op code);
    void op_u16(bytecode::Op code, std::uint16_t operand);
    void op_type(bytecode::Op code, bytecode::TypeTag tag);
    void op_u16_u8(bytecode::Op code, std::uint16_t first, std::uint8_t second);
    // Emits a jump with a placeholder target and returns the patch site.
    std::size_t emit_jump(bytecode::Op code);
    void patch_jump(std::size_t site);
    std::size_t here() const { return buffer_->code.size(); }

    // --- statements and expressions ---
    void emit_block(const ast::BlockStmt* block);
    void emit_stmt(const ast::Stmt* statement);
    void emit_expr(const ast::Expr* expr);
    void emit_expr_discard(const ast::Expr* expr);
    void emit_default_value(const sema::Type* type, diag::SourceLocation loc);
    void emit_call(const ast::CallExpr& call, bool discard);
    void emit_intrinsic(const ast::CallExpr& call, sema::MethodSymbol& method);
    void emit_assign(const ast::AssignExpr& assign, bool keep_value);
    void emit_logical(const ast::BinaryExpr& binary);
    void emit_load_var(const sema::VarSymbol& var, const ast::Expr* object);

    void error(diag::SourceLocation loc, std::string message);

    // Module index of each class's synthetic field initializer, or -1.
    util::Array<std::int32_t> field_init_indices_;

    sema::Program& program_;
    diag::DiagnosticEngine& diagnostics_;
    bytecode::Module* module_ = nullptr;
    CodeBuffer* buffer_ = nullptr;
    const sema::Type* current_return_ = nullptr;
    bool failed_ = false;
};

}  // namespace khu::codegen

#endif  // KHU_CODEGEN_EMITTER_H
