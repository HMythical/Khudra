// Recursive-descent parser for Khudra.
//
// The parser collects errors rather than bailing on the first one: after a bad
// declaration or statement it synchronizes to the next safe boundary and keeps
// going, so one run reports many problems.
#ifndef KHU_PARSER_PARSER_H
#define KHU_PARSER_PARSER_H

#include <cstdint>
#include <string_view>

#include "diag/diagnostic.h"
#include "lexer/token.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "util/array.h"

namespace khu::parser {

class Parser {
public:
    Parser(util::Array<lexer::Token> tokens, std::uint32_t file_id,
           diag::DiagnosticEngine& diagnostics, util::Arena& arena);

    // Parses a whole compilation unit. Always returns a unit, possibly partial.
    ast::CompilationUnit* parse_unit();

private:
    using Token = lexer::Token;
    using TokenKind = lexer::TokenKind;

    // --- token cursor ---
    const Token& peek(std::size_t ahead = 0) const;
    const Token& current() const { return peek(0); }
    bool check(TokenKind kind) const { return current().kind == kind; }
    bool at_end() const { return check(TokenKind::EndOfFile); }
    const Token& advance();
    bool match(TokenKind kind);
    // Reports "expected X, found Y" and returns false without consuming.
    bool expect(TokenKind kind, std::string_view context);

    void error_at(const Token& token, std::string message);
    void synchronize_to_member();
    void synchronize_to_statement();

    // --- grammar ---
    ast::ImportDecl* parse_import();
    ast::ClassDecl* parse_class(ast::Visibility visibility, bool explicit_visibility,
                                diag::SourceLocation start, bool is_namespace);
    ast::Decl* parse_member(std::string_view class_name);
    ast::MethodDecl* parse_method(ast::MethodForm form, ast::Visibility visibility,
                                  bool explicit_visibility, bool is_native,
                                  diag::SourceLocation start);
    ast::ProceduresDecl* parse_procedures(ast::Visibility visibility, bool explicit_visibility,
                                          diag::SourceLocation start);
    ast::FieldDecl* parse_field(ast::Visibility visibility, bool explicit_visibility,
                                diag::SourceLocation start);
    bool parse_param_list(util::Array<ast::ParamDecl*>& out);

    ast::TypeNode* parse_type();
    void close_type_arguments();
    void parse_type_arguments(util::Array<ast::TypeNode*>& out);
    std::size_t skip_type_arguments(std::size_t index) const;
    bool at_type_start() const;
    // True when the cursor is at `Type name` -- the only shape that starts a
    // local declaration.
    bool at_local_declaration() const;

    ast::BlockStmt* parse_block();
    ast::Stmt* parse_statement();
    ast::VarDeclStmt* parse_local(ast::Visibility visibility, bool explicit_visibility,
                                  diag::SourceLocation start);

    ast::Expr* parse_expression();
    ast::Expr* parse_assignment();
    ast::Expr* parse_binary(int min_precedence);
    ast::Expr* parse_unary();
    ast::Expr* parse_postfix();
    ast::Expr* parse_primary();
    ast::CallExpr* parse_call(ast::Expr* callee, diag::SourceLocation start);

    util::Array<Token> tokens_;
    std::uint32_t file_id_;
    diag::DiagnosticEngine& diagnostics_;
    util::Arena& arena_;
    std::size_t cursor_ = 0;
};

}  // namespace khu::parser

#endif  // KHU_PARSER_PARSER_H
