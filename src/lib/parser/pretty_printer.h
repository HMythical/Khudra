// AST -> Khudra source.
//
// Round-trip debugging aid and the Phase 1 acceptance check: printing a unit,
// reparsing the output and printing it again must produce identical text, and
// parentheses are emitted wherever they are needed to preserve the tree.
#ifndef KHU_PARSER_PRETTY_PRINTER_H
#define KHU_PARSER_PRETTY_PRINTER_H

#include <string>

#include "parser/ast.h"
#include "util/string_builder.h"

namespace khu::parser {

class PrettyPrinter {
public:
    std::string print(const ast::CompilationUnit& unit);
    std::string print_expr(const ast::Expr& expr);

private:
    void emit_import(const ast::ImportDecl& decl);
    void emit_class(const ast::ClassDecl& decl);
    void emit_member(const ast::Decl& decl);
    void emit_field(const ast::FieldDecl& decl);
    void emit_method(const ast::MethodDecl& decl);
    void emit_procedures(const ast::ProceduresDecl& decl);
    void emit_params(const util::Array<ast::ParamDecl*>& params);
    void emit_visibility(ast::Visibility visibility, bool explicit_visibility);

    void emit_type(const ast::TypeNode* type);
    void emit_type_arguments(const ast::TypeNode* type);
    void emit_block(const ast::BlockStmt* block);
    void emit_statement(const ast::Stmt* statement);
    // `indent_first` is false when this `if` follows an `else` on the same line.
    void emit_if(const ast::IfStmt& statement, bool indent_first);
    // Emits a branch body: braced blocks stay inline, single statements indent.
    void emit_branch(const ast::Stmt* branch);
    void emit_expr(const ast::Expr* expr);
    // Emits `expr`, wrapping it in parentheses when its precedence is looser
    // than `parent_precedence` (or equal to it, for `right_side` operands).
    void emit_operand(const ast::Expr* expr, int parent_precedence, bool right_side);
    void emit_string_literal(std::string_view value);

    util::StringBuilder out_;
    int depth_ = 0;
};

// True when `left` and `right` describe the same tree. Used by the round-trip
// test to prove the printer is structure-preserving, not just text-stable.
bool ast_equal(const ast::CompilationUnit& left, const ast::CompilationUnit& right);

}  // namespace khu::parser

#endif  // KHU_PARSER_PRETTY_PRINTER_H
