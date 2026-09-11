#include "parser/parser.h"

#include <string>

namespace khu::parser {
namespace {

using lexer::TokenKind;

// Binary operator precedence, loosest binding first. Zero means "not a binary
// operator". Assignment is handled separately because it is right-associative.
int precedence_of(TokenKind kind) {
    switch (kind) {
        case TokenKind::PipePipe: return 1;
        case TokenKind::AmpAmp: return 2;
        case TokenKind::Pipe: return 3;
        case TokenKind::Caret: return 4;
        case TokenKind::Amp: return 5;
        case TokenKind::EqualEqual:
        case TokenKind::BangEqual: return 6;
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual: return 7;
        case TokenKind::LessLess:
        case TokenKind::GreaterGreater: return 8;
        case TokenKind::Plus:
        case TokenKind::Minus: return 9;
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent: return 10;
        default: return 0;
    }
}

ast::BinaryOp binary_op_of(TokenKind kind) {
    switch (kind) {
        case TokenKind::Plus: return ast::BinaryOp::Add;
        case TokenKind::Minus: return ast::BinaryOp::Sub;
        case TokenKind::Star: return ast::BinaryOp::Mul;
        case TokenKind::Slash: return ast::BinaryOp::Div;
        case TokenKind::Percent: return ast::BinaryOp::Rem;
        case TokenKind::EqualEqual: return ast::BinaryOp::Equal;
        case TokenKind::BangEqual: return ast::BinaryOp::NotEqual;
        case TokenKind::Less: return ast::BinaryOp::Less;
        case TokenKind::LessEqual: return ast::BinaryOp::LessEqual;
        case TokenKind::Greater: return ast::BinaryOp::Greater;
        case TokenKind::GreaterEqual: return ast::BinaryOp::GreaterEqual;
        case TokenKind::AmpAmp: return ast::BinaryOp::LogicalAnd;
        case TokenKind::PipePipe: return ast::BinaryOp::LogicalOr;
        case TokenKind::Amp: return ast::BinaryOp::BitAnd;
        case TokenKind::Pipe: return ast::BinaryOp::BitOr;
        case TokenKind::Caret: return ast::BinaryOp::BitXor;
        case TokenKind::LessLess: return ast::BinaryOp::ShiftLeft;
        case TokenKind::GreaterGreater: return ast::BinaryOp::ShiftRight;
        default: return ast::BinaryOp::Add;
    }
}

// Maps a reserved type name to its AST builtin. Aliases keep their spelling in
// TypeNode::name and canonicalize later, in Sema.
bool builtin_of(TokenKind kind, ast::BuiltinType& out) {
    switch (kind) {
        case TokenKind::KwInt8: case TokenKind::KwI8: out = ast::BuiltinType::Int8; return true;
        case TokenKind::KwInt16: case TokenKind::KwI16: out = ast::BuiltinType::Int16; return true;
        case TokenKind::KwInt32: case TokenKind::KwI32:
        case TokenKind::KwInt: out = ast::BuiltinType::Int32; return true;
        case TokenKind::KwInt64: case TokenKind::KwI64: out = ast::BuiltinType::Int64; return true;
        case TokenKind::KwUInt8: case TokenKind::KwU8:
        case TokenKind::KwByte: out = ast::BuiltinType::UInt8; return true;
        case TokenKind::KwUInt16: case TokenKind::KwU16: out = ast::BuiltinType::UInt16; return true;
        case TokenKind::KwUInt32: case TokenKind::KwU32: out = ast::BuiltinType::UInt32; return true;
        case TokenKind::KwUInt64: case TokenKind::KwU64: out = ast::BuiltinType::UInt64; return true;
        case TokenKind::KwFloat: out = ast::BuiltinType::Float32; return true;
        case TokenKind::KwDFloat: out = ast::BuiltinType::Float64; return true;
        case TokenKind::KwBool: out = ast::BuiltinType::Bool; return true;
        case TokenKind::KwString: out = ast::BuiltinType::String; return true;
        case TokenKind::KwArray: out = ast::BuiltinType::Array; return true;
        case TokenKind::KwMemoryAllocationTypeObject:
            out = ast::BuiltinType::MemoryAllocationTypeObject;
            return true;
        case TokenKind::KwVoid: out = ast::BuiltinType::Void; return true;
        default: return false;
    }
}

// A quoted description of a token, for "expected X, found Y" messages.
std::string describe(const lexer::Token& token) {
    if (token.kind == TokenKind::EndOfFile) return "end of file";
    if (!token.text.empty()) return "'" + std::string(token.text) + "'";
    return lexer::token_name(token.kind);
}

std::string describe_kind(TokenKind kind) {
    std::string_view spelling = lexer::token_spelling(kind);
    if (!spelling.empty()) return "'" + std::string(spelling) + "'";
    return lexer::token_name(kind);
}

}  // namespace

Parser::Parser(util::Array<lexer::Token> tokens, std::uint32_t file_id,
               diag::DiagnosticEngine& diagnostics, util::Arena& arena)
    : tokens_(std::move(tokens)), file_id_(file_id), diagnostics_(diagnostics), arena_(arena) {}

const lexer::Token& Parser::peek(std::size_t ahead) const {
    std::size_t index = cursor_ + ahead;
    if (index >= tokens_.size()) index = tokens_.size() - 1;  // the EndOfFile token
    return tokens_[index];
}

const lexer::Token& Parser::advance() {
    const Token& token = current();
    if (!at_end()) ++cursor_;
    return token;
}

bool Parser::match(TokenKind kind) {
    if (!check(kind)) return false;
    advance();
    return true;
}

void Parser::error_at(const Token& token, std::string message) {
    diagnostics_.error(token.loc, std::move(message));
}

bool Parser::expect(TokenKind kind, std::string_view context) {
    if (match(kind)) return true;
    error_at(current(), "expected " + describe_kind(kind) + " " + std::string(context) +
                            ", found " + describe(current()));
    return false;
}

// After a broken member, skip to something that can start the next one.
void Parser::synchronize_to_member() {
    int depth = 0;
    while (!at_end()) {
        TokenKind kind = current().kind;
        if (kind == TokenKind::LBrace) ++depth;
        if (kind == TokenKind::RBrace) {
            if (depth == 0) return;  // end of the class body
            --depth;
            advance();
            if (depth == 0) return;
            continue;
        }
        if (depth == 0) {
            if (kind == TokenKind::Semicolon) {
                advance();
                return;
            }
            switch (kind) {
                case TokenKind::KwPublic:
                case TokenKind::KwPrivate:
                case TokenKind::KwFunc:
                case TokenKind::KwMethod:
                case TokenKind::KwProcedures:
                    return;
                default:
                    break;
            }
        }
        advance();
    }
}

void Parser::synchronize_to_statement() {
    while (!at_end()) {
        if (current().kind == TokenKind::Semicolon) {
            advance();
            return;
        }
        if (current().kind == TokenKind::RBrace) return;
        advance();
    }
}

// ---------------------------------------------------------------------------
// Compilation unit
// ---------------------------------------------------------------------------

ast::CompilationUnit* Parser::parse_unit() {
    auto* unit = arena_.create<ast::CompilationUnit>();
    unit->file_id = file_id_;

    while (check(TokenKind::KwBring)) {
        if (ast::ImportDecl* import = parse_import()) unit->imports.push(import);
    }

    while (!at_end()) {
        diag::SourceLocation start = current().loc;
        ast::Visibility visibility = ast::Visibility::Public;
        bool explicit_visibility = false;
        if (match(TokenKind::KwPublic)) {
            explicit_visibility = true;
        } else if (match(TokenKind::KwPrivate)) {
            visibility = ast::Visibility::Private;
            explicit_visibility = true;
        }

        if (check(TokenKind::KwBring)) {
            error_at(current(), "'bring' imports must appear before any class declaration");
            if (ast::ImportDecl* import = parse_import()) unit->imports.push(import);
            continue;
        }

        bool is_namespace = check(TokenKind::KwNamespace);
        if (!is_namespace && !check(TokenKind::KwClass)) {
            error_at(current(), "expected a class or namespace declaration at top level, found " +
                                    describe(current()));
            // Skip the offending token so the loop makes progress.
            advance();
            continue;
        }

        if (ast::ClassDecl* decl =
                parse_class(visibility, explicit_visibility, start, is_namespace)) {
            unit->classes.push(decl);
        }
    }

    return unit;
}

ast::ImportDecl* Parser::parse_import() {
    diag::SourceLocation start = current().loc;
    advance();  // 'bring'

    auto* decl = arena_.create<ast::ImportDecl>(start);
    while (true) {
        if (!check(TokenKind::Identifier)) {
            error_at(current(), "expected a namespace segment after 'bring', found " +
                                    describe(current()));
            synchronize_to_statement();
            return decl->path.empty() ? nullptr : decl;
        }
        decl->path.push(advance().text);
        if (!match(TokenKind::ColonColon)) break;
    }
    expect(TokenKind::Semicolon, "after an import");
    return decl;
}

// ---------------------------------------------------------------------------
// Classes and members
// ---------------------------------------------------------------------------

ast::ClassDecl* Parser::parse_class(ast::Visibility visibility, bool explicit_visibility,
                                    diag::SourceLocation start, bool is_namespace) {
    advance();  // 'class' or 'namespace'

    auto* decl = arena_.create<ast::ClassDecl>(start);
    decl->visibility = visibility;
    decl->explicit_visibility = explicit_visibility;
    decl->is_namespace = is_namespace;

    if (!check(TokenKind::Identifier)) {
        error_at(current(), std::string("expected a ") + (is_namespace ? "namespace" : "class") +
                                " name, found " + describe(current()));
        synchronize_to_member();
        return nullptr;
    }
    decl->name = current().text;
    decl->name_loc = current().loc;
    advance();

    // `class List<T>`. A namespace has no instances, so it has nothing to
    // parameterize.
    if (check(TokenKind::Less)) {
        if (is_namespace) error_at(current(), "a namespace cannot take type parameters");
        advance();
        while (true) {
            if (!check(TokenKind::Identifier)) {
                error_at(current(), "expected a type parameter name, found " +
                                        describe(current()));
                break;
            }
            decl->type_params.push(current().text);
            decl->type_param_locs.push(current().loc);
            advance();
            if (!match(TokenKind::Comma)) break;
        }
        close_type_arguments();
    }

    if (is_namespace && check(TokenKind::KwExtends)) {
        error_at(current(), "a namespace cannot extend anything");
    }
    if (match(TokenKind::KwExtends)) {
        if (!check(TokenKind::Identifier)) {
            error_at(current(), "expected a base class name after 'extends', found " +
                                    describe(current()));
        } else {
            decl->base_name = current().text;
            decl->base_loc = current().loc;
            advance();
        }
    }

    if (!expect(TokenKind::LBrace, "to open the class body")) return decl;

    while (!at_end() && !check(TokenKind::RBrace)) {
        std::size_t before = cursor_;
        if (ast::Decl* member = parse_member(decl->name)) {
            decl->members.push(member);
        }
        if (cursor_ == before) advance();  // guarantee progress
    }
    expect(TokenKind::RBrace, "to close the class body");
    return decl;
}

ast::Decl* Parser::parse_member(std::string_view class_name) {
    diag::SourceLocation start = current().loc;

    bool explicit_visibility = false;
    bool visibility_is_public = false;
    if (match(TokenKind::KwPublic)) {
        explicit_visibility = true;
        visibility_is_public = true;
    } else if (match(TokenKind::KwPrivate)) {
        explicit_visibility = true;
    }

    // `native` marks a member the toolchain implements; it has no body.
    bool is_native = match(TokenKind::KwNative);

    // `func` is public by default, `method` is private by default; an explicit
    // modifier overrides that default (KHU-PLAN.md, Members).
    if (check(TokenKind::KwFunc) || check(TokenKind::KwMethod)) {
        bool is_func = check(TokenKind::KwFunc);
        advance();
        ast::Visibility visibility = is_func ? ast::Visibility::Public : ast::Visibility::Private;
        if (explicit_visibility) {
            visibility = visibility_is_public ? ast::Visibility::Public : ast::Visibility::Private;
        }
        return parse_method(is_func ? ast::MethodForm::Func : ast::MethodForm::Method, visibility,
                            explicit_visibility, is_native, start);
    }

    if (is_native) {
        error_at(current(), "'native' can only mark a func or a method, found " +
                                describe(current()));
        synchronize_to_member();
        return nullptr;
    }

    if (check(TokenKind::KwProcedures)) {
        advance();
        ast::Visibility visibility =
            explicit_visibility && visibility_is_public ? ast::Visibility::Public
                                                        : ast::Visibility::Private;
        return parse_procedures(visibility, explicit_visibility, start);
    }

    ast::Visibility visibility =
        explicit_visibility && visibility_is_public ? ast::Visibility::Public
                                                    : ast::Visibility::Private;

    // A constructor is an identifier matching the class name, followed by '('.
    if (check(TokenKind::Identifier) && current().text == class_name &&
        peek(1).kind == TokenKind::LParen) {
        if (!explicit_visibility) visibility = ast::Visibility::Public;
        return parse_method(ast::MethodForm::Constructor, visibility, explicit_visibility, false,
                            start);
    }

    if (!at_type_start()) {
        error_at(current(), "expected a field, function, method, constructor or Procedures "
                            "block, found " +
                                describe(current()));
        synchronize_to_member();
        return nullptr;
    }
    return parse_field(visibility, explicit_visibility, start);
}

ast::MethodDecl* Parser::parse_method(ast::MethodForm form, ast::Visibility visibility,
                                      bool explicit_visibility, bool is_native,
                                      diag::SourceLocation start) {
    auto* decl = arena_.create<ast::MethodDecl>(start);
    decl->form = form;
    decl->visibility = visibility;
    decl->explicit_visibility = explicit_visibility;
    decl->is_native = is_native;

    if (!check(TokenKind::Identifier)) {
        error_at(current(), "expected a name, found " + describe(current()));
        synchronize_to_member();
        return nullptr;
    }
    decl->name = current().text;
    decl->name_loc = current().loc;
    advance();

    if (!parse_param_list(decl->params)) {
        synchronize_to_member();
        return nullptr;
    }

    // "no arrow = void"; `-> void` is the explicit synonym.
    if (match(TokenKind::Arrow)) {
        ast::TypeNode* type = parse_type();
        if (type && type->kind == ast::TypeNode::Kind::Builtin &&
            type->builtin == ast::BuiltinType::Void) {
            decl->explicit_void = true;
        } else {
            decl->return_type = type;
        }
    }

    if (form == ast::MethodForm::Constructor && (decl->return_type || decl->explicit_void)) {
        error_at(current(), "a constructor cannot declare a return type");
        decl->return_type = nullptr;
        decl->explicit_void = false;
    }

    // A native member is a declaration, not a definition.
    if (decl->is_native) {
        if (check(TokenKind::LBrace)) {
            error_at(current(), "a native member cannot have a body");
            decl->body = parse_block();
            return decl;
        }
        expect(TokenKind::Semicolon, "after a native declaration");
        return decl;
    }

    if (!check(TokenKind::LBrace)) {
        error_at(current(), "expected '{' to open the body of '" + std::string(decl->name) +
                                "', found " + describe(current()));
        synchronize_to_member();
        return decl;
    }
    decl->body = parse_block();
    return decl;
}

ast::ProceduresDecl* Parser::parse_procedures(ast::Visibility visibility,
                                              bool explicit_visibility,
                                              diag::SourceLocation start) {
    auto* decl = arena_.create<ast::ProceduresDecl>(start);
    decl->visibility = visibility;
    decl->explicit_visibility = explicit_visibility;

    // The parameter list is optional: `Procedures { ... }` is as valid as
    // `Procedures(int32 x) { ... }`.
    if (check(TokenKind::LParen)) {
        decl->has_param_list = true;
        if (!parse_param_list(decl->params)) {
            synchronize_to_member();
            return nullptr;
        }
    }

    if (check(TokenKind::Arrow)) {
        const Token& arrow = current();
        error_at(arrow, "a Procedures block cannot declare a return type");
        advance();
        parse_type();
    }

    if (!check(TokenKind::LBrace)) {
        error_at(current(), "expected '{' to open the Procedures block, found " +
                                describe(current()));
        synchronize_to_member();
        return decl;
    }
    decl->body = parse_block();
    return decl;
}

ast::FieldDecl* Parser::parse_field(ast::Visibility visibility, bool explicit_visibility,
                                    diag::SourceLocation start) {
    auto* decl = arena_.create<ast::FieldDecl>(start);
    decl->visibility = visibility;
    decl->explicit_visibility = explicit_visibility;
    decl->type = parse_type();

    if (!check(TokenKind::Identifier)) {
        error_at(current(), "expected a field name, found " + describe(current()));
        synchronize_to_member();
        return nullptr;
    }
    decl->name = current().text;
    decl->name_loc = current().loc;
    advance();

    if (match(TokenKind::Assign)) decl->init = parse_expression();
    expect(TokenKind::Semicolon, "after a field declaration");
    return decl;
}

bool Parser::parse_param_list(util::Array<ast::ParamDecl*>& out) {
    if (!expect(TokenKind::LParen, "to open a parameter list")) return false;
    if (match(TokenKind::RParen)) return true;

    while (true) {
        diag::SourceLocation start = current().loc;
        if (!at_type_start()) {
            error_at(current(), "expected a parameter type, found " + describe(current()));
            return false;
        }
        auto* param = arena_.create<ast::ParamDecl>(start);
        param->type = parse_type();
        if (!check(TokenKind::Identifier)) {
            error_at(current(), "expected a parameter name, found " + describe(current()));
            return false;
        }
        param->name = current().text;
        advance();
        out.push(param);

        if (match(TokenKind::Comma)) continue;
        break;
    }
    return expect(TokenKind::RParen, "to close a parameter list");
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

bool Parser::at_type_start() const {
    TokenKind kind = current().kind;
    return kind == TokenKind::Star || kind == TokenKind::Identifier ||
           lexer::is_type_keyword(kind);
}

// Parses `< type, type, ... >` into `out`. The caller has checked that the
// current token is the `<`.
void Parser::parse_type_arguments(util::Array<ast::TypeNode*>& out) {
    advance();  // '<'
    if (check(TokenKind::Greater) || check(TokenKind::GreaterGreater)) {
        error_at(current(), "expected a type argument");
        close_type_arguments();
        return;
    }
    while (true) {
        ast::TypeNode* argument = parse_type();
        if (!argument) break;
        out.push(argument);
        if (!match(TokenKind::Comma)) break;
    }
    close_type_arguments();
}

// Consumes the `>` that closes a type argument list.
//
// `Array<Array<int32>>` ends in a token the lexer scanned as `>>`, because it
// scanned it without knowing it was closing two lists rather than shifting.
// Rather than teach the lexer about context, the parser splits the token: it
// takes one `>` and leaves a `>` behind for the enclosing list to take.
void Parser::close_type_arguments() {
    if (check(TokenKind::GreaterGreater)) {
        lexer::Token& token = tokens_[cursor_];
        token.kind = TokenKind::Greater;
        token.text = token.text.substr(1);
        token.loc.column += 1;
        return;
    }
    expect(TokenKind::Greater, "to close a type argument list");
}

ast::TypeNode* Parser::parse_type() {
    diag::SourceLocation start = current().loc;

    if (match(TokenKind::Star)) {
        auto* node = arena_.create<ast::TypeNode>();
        node->kind = ast::TypeNode::Kind::Pointer;
        node->loc = start;
        node->pointee = parse_type();
        return node;
    }

    ast::BuiltinType builtin{};
    if (builtin_of(current().kind, builtin)) {
        auto* node = arena_.create<ast::TypeNode>();
        node->kind = ast::TypeNode::Kind::Builtin;
        node->loc = start;
        node->builtin = builtin;
        node->name = current().text;  // preserve the alias as written
        advance();
        // `Array<T>`. Array is the only parameterized builtin, so `<` after any
        // other type name is not a type argument list and is left for the
        // expression grammar.
        if (builtin == ast::BuiltinType::Array && check(TokenKind::Less)) {
            parse_type_arguments(node->arguments);
        }
        return node;
    }

    if (check(TokenKind::Identifier)) {
        auto* node = arena_.create<ast::TypeNode>();
        node->kind = ast::TypeNode::Kind::Named;
        node->loc = start;
        node->name = current().text;
        advance();
        if (check(TokenKind::Less)) parse_type_arguments(node->arguments);
        return node;
    }

    error_at(current(), "expected a type, found " + describe(current()));
    return nullptr;
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

ast::BlockStmt* Parser::parse_block() {
    diag::SourceLocation start = current().loc;
    auto* block = arena_.create<ast::BlockStmt>(start);
    expect(TokenKind::LBrace, "to open a block");

    while (!at_end() && !check(TokenKind::RBrace)) {
        std::size_t before = cursor_;
        if (ast::Stmt* statement = parse_statement()) block->statements.push(statement);
        if (cursor_ == before) advance();  // guarantee progress
    }
    expect(TokenKind::RBrace, "to close a block");
    return block;
}

// Skips a `<...>` type argument list starting at `index` (which must be the
// `<`), and returns the offset just past its `>`. Returns 0 when the list does
// not close, so a malformed one falls back to the expression grammar rather
// than swallowing the rest of the block. `>>` closes two levels, because the
// lexer scanned it before it could know it was closing anything.
std::size_t Parser::skip_type_arguments(std::size_t index) const {
    int depth = 0;
    for (std::size_t i = index; i < tokens_.size(); ++i) {
        TokenKind kind = tokens_[i].kind;
        if (kind == TokenKind::Less) {
            ++depth;
        } else if (kind == TokenKind::Greater) {
            if (--depth == 0) return i + 1;
        } else if (kind == TokenKind::GreaterGreater) {
            depth -= 2;
            if (depth == 0) return i + 1;
            if (depth < 0) return 0;
        } else if (kind == TokenKind::Semicolon || kind == TokenKind::LBrace ||
                   kind == TokenKind::RBrace || kind == TokenKind::EndOfFile) {
            return 0;
        }
    }
    return 0;
}

// Only `Type name` starts a local declaration; everything else beginning with
// an identifier is an expression.
bool Parser::at_local_declaration() const {
    TokenKind kind = current().kind;
    if (kind == TokenKind::Star) return true;
    if (lexer::is_type_keyword(kind)) {
        // `Array<int32> counts` is a declaration; `Array<int32>` on its own is
        // a type reference used as an argument. Which one it is only shows up
        // past the type argument list, so the list is skipped to find out.
        if (peek(1).kind == TokenKind::Less) {
            std::size_t after = skip_type_arguments(cursor_ + 1);
            return after != 0 && after < tokens_.size() &&
                   tokens_[after].kind == TokenKind::Identifier;
        }
        // `int64` alone is a type reference used as an argument, not a decl.
        return peek(1).kind == TokenKind::Identifier;
    }
    if (kind == TokenKind::Identifier) {
        // The same question for a class name: `Box<int32> b` is a declaration,
        // `a < b > c` is a chain of comparisons.
        if (peek(1).kind == TokenKind::Less) {
            std::size_t after = skip_type_arguments(cursor_ + 1);
            return after != 0 && after < tokens_.size() &&
                   tokens_[after].kind == TokenKind::Identifier;
        }
        return peek(1).kind == TokenKind::Identifier;
    }
    return false;
}

ast::Stmt* Parser::parse_statement() {
    diag::SourceLocation start = current().loc;

    if (check(TokenKind::LBrace)) return parse_block();

    if (match(TokenKind::Semicolon)) return arena_.create<ast::EmptyStmt>(start);

    if (check(TokenKind::KwPublic) || check(TokenKind::KwPrivate)) {
        bool is_public = check(TokenKind::KwPublic);
        advance();
        return parse_local(is_public ? ast::Visibility::Public : ast::Visibility::Private, true,
                           start);
    }

    if (match(TokenKind::KwReturn)) {
        ast::Expr* value = nullptr;
        if (!check(TokenKind::Semicolon)) value = parse_expression();
        expect(TokenKind::Semicolon, "after a return statement");
        return arena_.create<ast::ReturnStmt>(start, value);
    }

    if (match(TokenKind::KwIf)) {
        auto* statement = arena_.create<ast::IfStmt>(start);
        expect(TokenKind::LParen, "after 'if'");
        statement->condition = parse_expression();
        expect(TokenKind::RParen, "after an if condition");
        statement->then_branch = parse_statement();
        if (match(TokenKind::KwElse)) statement->else_branch = parse_statement();
        return statement;
    }

    if (match(TokenKind::KwWhile)) {
        auto* statement = arena_.create<ast::WhileStmt>(start);
        expect(TokenKind::LParen, "after 'while'");
        statement->condition = parse_expression();
        expect(TokenKind::RParen, "after a while condition");
        statement->body = parse_statement();
        return statement;
    }

    if (check(TokenKind::KwFree) || check(TokenKind::KwDispose)) {
        bool is_dispose = check(TokenKind::KwDispose);
        advance();
        auto* statement = arena_.create<ast::FreeStmt>(start);
        statement->is_dispose = is_dispose;
        expect(TokenKind::LParen, is_dispose ? "after 'dispose'" : "after 'free'");
        statement->target = parse_expression();
        expect(TokenKind::RParen, "after the freed object");
        expect(TokenKind::Semicolon, "after a free statement");
        return statement;
    }

    if (check(TokenKind::KwInlineC)) return parse_inline_block(TokenKind::KwInlineC, start);
    if (check(TokenKind::KwInlineAsm)) return parse_inline_block(TokenKind::KwInlineAsm, start);

    if (at_local_declaration()) {
        return parse_local(ast::Visibility::Private, false, start);
    }

    ast::Expr* expr = parse_expression();
    if (!expr) {
        synchronize_to_statement();
        return nullptr;
    }
    expect(TokenKind::Semicolon, "after an expression statement");
    return arena_.create<ast::ExprStmt>(start, expr);
}

ast::Stmt* Parser::parse_inline_block(TokenKind keyword, diag::SourceLocation start) {
    advance();
    if (check(TokenKind::RawBlock)) {
        const Token& block = current();
        advance();
        // The lexer keeps the whole span, braces included, so reparsing the
        // printed source rebuilds the same tree.
        std::string_view full = block.text;
        std::string_view body =
            full.size() >= 2 ? full.substr(1, full.size() - 2) : std::string_view();
        if (keyword == TokenKind::KwInlineAsm) {
            return arena_.create<ast::InlineAsmStmt>(start, full, body);
        }
        return arena_.create<ast::InlineCStmt>(start, full, body);
    }
    if (check(TokenKind::Invalid) && current().text.size() >= 2 && current().text.front() == '{') {
        // The lexer already reported the unterminated block; swallow it so it
        // does not cascade into a second diagnostic.
        advance();
        return nullptr;
    }
    error_at(current(), keyword == TokenKind::KwInlineAsm
                           ? "expected '{' to open an inline asm block"
                           : "expected '{' to open an inline C block");
    return nullptr;
}

ast::VarDeclStmt* Parser::parse_local(ast::Visibility visibility, bool explicit_visibility,
                                      diag::SourceLocation start) {
    auto* decl = arena_.create<ast::VarDeclStmt>(start);
    decl->visibility = visibility;
    decl->explicit_visibility = explicit_visibility;
    decl->type = parse_type();

    if (!check(TokenKind::Identifier)) {
        error_at(current(), "expected a variable name, found " + describe(current()));
        synchronize_to_statement();
        return nullptr;
    }
    decl->name = current().text;
    decl->name_loc = current().loc;
    advance();

    if (match(TokenKind::Assign)) decl->init = parse_expression();
    expect(TokenKind::Semicolon, "after a local declaration");
    return decl;
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

ast::Expr* Parser::parse_expression() { return parse_assignment(); }

ast::Expr* Parser::parse_assignment() {
    ast::Expr* left = parse_binary(1);
    if (!left) return nullptr;
    if (!check(TokenKind::Assign)) return left;

    diag::SourceLocation loc = current().loc;
    advance();
    ast::Expr* value = parse_assignment();  // right-associative
    return arena_.create<ast::AssignExpr>(loc, left, value);
}

ast::Expr* Parser::parse_binary(int min_precedence) {
    ast::Expr* left = parse_unary();
    if (!left) return nullptr;

    while (true) {
        int precedence = precedence_of(current().kind);
        if (precedence < min_precedence) break;
        TokenKind op_kind = current().kind;
        diag::SourceLocation loc = current().loc;
        advance();
        // Left-associative: the right side binds one level tighter.
        ast::Expr* right = parse_binary(precedence + 1);
        if (!right) return left;
        left = arena_.create<ast::BinaryExpr>(loc, binary_op_of(op_kind), left, right);
    }
    return left;
}

ast::Expr* Parser::parse_unary() {
    diag::SourceLocation loc = current().loc;
    switch (current().kind) {
        case TokenKind::Minus:
            advance();
            return arena_.create<ast::UnaryExpr>(loc, ast::UnaryOp::Negate, parse_unary());
        case TokenKind::Plus:
            advance();
            return arena_.create<ast::UnaryExpr>(loc, ast::UnaryOp::Plus, parse_unary());
        case TokenKind::Bang:
            advance();
            return arena_.create<ast::UnaryExpr>(loc, ast::UnaryOp::Not, parse_unary());
        case TokenKind::Tilde:
            advance();
            return arena_.create<ast::UnaryExpr>(loc, ast::UnaryOp::BitNot, parse_unary());
        default:
            return parse_postfix();
    }
}

ast::Expr* Parser::parse_postfix() {
    ast::Expr* expr = parse_primary();
    if (!expr) return nullptr;

    while (true) {
        if (check(TokenKind::Dot)) {
            diag::SourceLocation loc = current().loc;
            advance();
            // `Procedures` is a keyword, but accepting it here lets Sema give
            // the real diagnostic: a Procedures block is never invocable.
            if (!check(TokenKind::Identifier) && !check(TokenKind::KwProcedures)) {
                error_at(current(), "expected a member name after '.', found " +
                                        describe(current()));
                return expr;
            }
            std::string_view name = current().text;
            diag::SourceLocation name_loc = current().loc;
            advance();
            expr = arena_.create<ast::MemberExpr>(loc, expr, name, name_loc);
            continue;
        }
        if (check(TokenKind::LParen)) {
            diag::SourceLocation loc = current().loc;
            expr = parse_call(expr, loc);
            continue;
        }
        if (check(TokenKind::LBracket)) {
            diag::SourceLocation loc = current().loc;
            advance();
            ast::Expr* index = parse_expression();
            expect(TokenKind::RBracket, "to close an index expression");
            expr = arena_.create<ast::IndexExpr>(loc, expr, index);
            continue;
        }
        break;
    }
    return expr;
}

ast::CallExpr* Parser::parse_call(ast::Expr* callee, diag::SourceLocation start) {
    advance();  // '('
    auto* call = arena_.create<ast::CallExpr>(start, callee);

    if (!match(TokenKind::RParen)) {
        while (true) {
            ast::Expr* argument = parse_expression();
            if (!argument) break;
            call->args.push(argument);
            if (match(TokenKind::Comma)) continue;
            break;
        }
        expect(TokenKind::RParen, "to close an argument list");
    }

    // A leading `manual` / `standard` token is the allocation-site strategy
    // override, not an argument: the allocator consumes it and the rest bind to
    // the class's Procedures parameters (docs/memory-model.md).
    if (!call->args.empty()) {
        if (auto* strategy = call->args[0]->as<ast::StrategyExpr>()) {
            call->strategy = strategy->strategy;
            call->has_strategy_token = true;
            call->strategy_loc = strategy->loc;
            util::Array<ast::Expr*> rest;
            for (std::size_t i = 1; i < call->args.size(); ++i) rest.push(call->args[i]);
            call->args = std::move(rest);
        }
    }
    return call;
}

ast::Expr* Parser::parse_primary() {
    const Token& token = current();
    diag::SourceLocation loc = token.loc;

    switch (token.kind) {
        case TokenKind::IntLiteral:
            advance();
            return arena_.create<ast::IntLiteralExpr>(loc, token.int_value);
        case TokenKind::FloatLiteral:
            advance();
            return arena_.create<ast::FloatLiteralExpr>(loc, token.float_value);
        case TokenKind::StringLiteral:
            advance();
            return arena_.create<ast::StringLiteralExpr>(loc, token.string_value);
        case TokenKind::KwTrue:
            advance();
            return arena_.create<ast::BoolLiteralExpr>(loc, true);
        case TokenKind::KwFalse:
            advance();
            return arena_.create<ast::BoolLiteralExpr>(loc, false);
        case TokenKind::KwNull:
            advance();
            return arena_.create<ast::NullLiteralExpr>(loc);
        case TokenKind::KwThis:
            advance();
            return arena_.create<ast::ThisExpr>(loc);
        case TokenKind::KwManual:
            advance();
            return arena_.create<ast::StrategyExpr>(loc, ast::Strategy::Manual);
        case TokenKind::KwStandard:
            advance();
            return arena_.create<ast::StrategyExpr>(loc, ast::Strategy::Gc);
        case TokenKind::Identifier: {
            // `List<int32>` in expression position is a type: an allocation
            // site (`List<int32>(...)`) or a type argument
            // (`arrayCreate(Box<int32>, 2)`). `a < b > c` is a chain of
            // comparisons. They are told apart by what follows the balanced
            // `>`: a comparison cannot be followed by `(`, `,` or `)`, and each
            // of those is somewhere a type belongs.
            if (peek(1).kind == TokenKind::Less) {
                std::size_t after = skip_type_arguments(cursor_ + 1);
                if (after != 0 && after < tokens_.size() &&
                    (tokens_[after].kind == TokenKind::LParen ||
                     tokens_[after].kind == TokenKind::Comma ||
                     tokens_[after].kind == TokenKind::RParen)) {
                    ast::TypeNode* type = parse_type();
                    return arena_.create<ast::TypeRefExpr>(loc, type);
                }
            }
            advance();
            return arena_.create<ast::IdentifierExpr>(loc, token.text);
        }
        case TokenKind::KwProcedures:
            // Same reason as above: let Sema explain why this cannot be called.
            advance();
            return arena_.create<ast::IdentifierExpr>(loc, token.text);
        case TokenKind::LParen: {
            advance();
            ast::Expr* inner = parse_expression();
            expect(TokenKind::RParen, "to close a parenthesized expression");
            return inner;
        }
        default:
            break;
    }

    // A type name in expression position: `khuStdMath.convertTo(int64, y)` and
    // `MemoryAllocationTypeObject.setStandard()`.
    if (lexer::is_type_keyword(token.kind)) {
        ast::TypeNode* type = parse_type();
        return arena_.create<ast::TypeRefExpr>(loc, type);
    }

    error_at(token, "expected an expression, found " + describe(token));
    return nullptr;
}

}  // namespace khu::parser
