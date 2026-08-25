#include "parser/pretty_printer.h"

#include <string>

namespace khu::parser {
namespace {

// Mirrors the parser's precedence ladder so the printer knows when a
// subexpression needs parentheses.
constexpr int kAssignPrecedence = 0;
constexpr int kUnaryPrecedence = 11;
constexpr int kPrimaryPrecedence = 12;

int binary_precedence(ast::BinaryOp op) {
    switch (op) {
        case ast::BinaryOp::LogicalOr: return 1;
        case ast::BinaryOp::LogicalAnd: return 2;
        case ast::BinaryOp::BitOr: return 3;
        case ast::BinaryOp::BitXor: return 4;
        case ast::BinaryOp::BitAnd: return 5;
        case ast::BinaryOp::Equal:
        case ast::BinaryOp::NotEqual: return 6;
        case ast::BinaryOp::Less:
        case ast::BinaryOp::LessEqual:
        case ast::BinaryOp::Greater:
        case ast::BinaryOp::GreaterEqual: return 7;
        case ast::BinaryOp::ShiftLeft:
        case ast::BinaryOp::ShiftRight: return 8;
        case ast::BinaryOp::Add:
        case ast::BinaryOp::Sub: return 9;
        case ast::BinaryOp::Mul:
        case ast::BinaryOp::Div:
        case ast::BinaryOp::Rem: return 10;
    }
    return kPrimaryPrecedence;
}

int precedence_of(const ast::Expr* expr) {
    if (!expr) return kPrimaryPrecedence;
    switch (expr->kind) {
        case ast::ExprKind::Assign: return kAssignPrecedence;
        case ast::ExprKind::Binary:
            return binary_precedence(static_cast<const ast::BinaryExpr*>(expr)->op);
        case ast::ExprKind::Unary: return kUnaryPrecedence;
        default: return kPrimaryPrecedence;
    }
}

// A member that owns a brace block gets blank lines around it.
bool is_block_member(const ast::Decl& decl) {
    return decl.kind == ast::DeclKind::Method || decl.kind == ast::DeclKind::Procedures;
}

}  // namespace

std::string PrettyPrinter::print(const ast::CompilationUnit& unit) {
    out_.clear();
    depth_ = 0;

    for (const ast::ImportDecl* import : unit.imports) emit_import(*import);
    if (!unit.imports.empty() && !unit.classes.empty()) out_.append('\n');

    for (std::size_t i = 0; i < unit.classes.size(); ++i) {
        if (i != 0) out_.append('\n');
        emit_class(*unit.classes[i]);
    }
    return out_.str();
}

std::string PrettyPrinter::print_expr(const ast::Expr& expr) {
    out_.clear();
    depth_ = 0;
    emit_expr(&expr);
    return out_.str();
}

void PrettyPrinter::emit_import(const ast::ImportDecl& decl) {
    out_.append("bring ");
    for (std::size_t i = 0; i < decl.path.size(); ++i) {
        if (i != 0) out_.append("::");
        out_.append(decl.path[i]);
    }
    out_.append_line(";");
}

void PrettyPrinter::emit_visibility(ast::Visibility visibility, bool explicit_visibility) {
    // Only spell out a modifier that was written: `func` is public by default
    // and `method` is private by default, and reprinting those defaults would
    // change the source without changing its meaning.
    if (!explicit_visibility) return;
    out_.append(ast::visibility_name(visibility)).append(' ');
}

void PrettyPrinter::emit_class(const ast::ClassDecl& decl) {
    emit_visibility(decl.visibility, decl.explicit_visibility);
    out_.append("class ").append(decl.name);
    if (!decl.base_name.empty()) out_.append(" extends ").append(decl.base_name);
    out_.append_line(" {");

    ++depth_;
    for (std::size_t i = 0; i < decl.members.size(); ++i) {
        const ast::Decl& member = *decl.members[i];
        bool needs_gap = i != 0 && (is_block_member(member) || is_block_member(*decl.members[i - 1]));
        if (needs_gap) out_.append('\n');
        emit_member(member);
    }
    --depth_;

    out_.indent(depth_).append_line("}");
}

void PrettyPrinter::emit_member(const ast::Decl& decl) {
    switch (decl.kind) {
        case ast::DeclKind::Field:
            emit_field(static_cast<const ast::FieldDecl&>(decl));
            break;
        case ast::DeclKind::Method:
            emit_method(static_cast<const ast::MethodDecl&>(decl));
            break;
        case ast::DeclKind::Procedures:
            emit_procedures(static_cast<const ast::ProceduresDecl&>(decl));
            break;
        default:
            break;
    }
}

void PrettyPrinter::emit_field(const ast::FieldDecl& decl) {
    out_.indent(depth_);
    emit_visibility(decl.visibility, decl.explicit_visibility);
    emit_type(decl.type);
    out_.append(' ').append(decl.name);
    if (decl.init) {
        out_.append(" = ");
        emit_expr(decl.init);
    }
    out_.append_line(";");
}

void PrettyPrinter::emit_params(const util::Array<ast::ParamDecl*>& params) {
    out_.append('(');
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i != 0) out_.append(", ");
        emit_type(params[i]->type);
        out_.append(' ').append(params[i]->name);
    }
    out_.append(')');
}

void PrettyPrinter::emit_method(const ast::MethodDecl& decl) {
    out_.indent(depth_);
    emit_visibility(decl.visibility, decl.explicit_visibility);
    switch (decl.form) {
        case ast::MethodForm::Func: out_.append("func "); break;
        case ast::MethodForm::Method: out_.append("method "); break;
        case ast::MethodForm::Constructor: break;  // named after the class
    }
    out_.append(decl.name);
    emit_params(decl.params);

    if (decl.return_type) {
        out_.append(" -> ");
        emit_type(decl.return_type);
    } else if (decl.explicit_void) {
        out_.append(" -> void");
    }

    out_.append(' ');
    emit_block(decl.body);
}

void PrettyPrinter::emit_procedures(const ast::ProceduresDecl& decl) {
    out_.indent(depth_);
    emit_visibility(decl.visibility, decl.explicit_visibility);
    out_.append("Procedures");
    if (decl.has_param_list) emit_params(decl.params);
    out_.append(' ');
    emit_block(decl.body);
}

void PrettyPrinter::emit_type(const ast::TypeNode* type) {
    if (!type) {
        out_.append("<error-type>");
        return;
    }
    switch (type->kind) {
        case ast::TypeNode::Kind::Pointer:
            out_.append('*');
            emit_type(type->pointee);
            break;
        case ast::TypeNode::Kind::Builtin:
            // Preserve the alias as written (`i32` stays `i32`).
            out_.append(type->name.empty() ? ast::builtin_canonical_name(type->builtin)
                                           : type->name);
            break;
        case ast::TypeNode::Kind::Named:
            out_.append(type->name);
            break;
    }
}

void PrettyPrinter::emit_block(const ast::BlockStmt* block) {
    if (!block) {
        out_.append_line("{ }");
        return;
    }
    out_.append_line("{");
    ++depth_;
    for (const ast::Stmt* statement : block->statements) emit_statement(statement);
    --depth_;
    out_.indent(depth_).append_line("}");
}

void PrettyPrinter::emit_statement(const ast::Stmt* statement) {
    if (!statement) return;

    switch (statement->kind) {
        case ast::StmtKind::Block:
            out_.indent(depth_);
            emit_block(static_cast<const ast::BlockStmt*>(statement));
            break;

        case ast::StmtKind::Empty:
            out_.indent(depth_).append_line(";");
            break;

        case ast::StmtKind::VarDecl: {
            const auto& decl = *static_cast<const ast::VarDeclStmt*>(statement);
            out_.indent(depth_);
            emit_visibility(decl.visibility, decl.explicit_visibility);
            emit_type(decl.type);
            out_.append(' ').append(decl.name);
            if (decl.init) {
                out_.append(" = ");
                emit_expr(decl.init);
            }
            out_.append_line(";");
            break;
        }

        case ast::StmtKind::Expr:
            out_.indent(depth_);
            emit_expr(static_cast<const ast::ExprStmt*>(statement)->expr);
            out_.append_line(";");
            break;

        case ast::StmtKind::Return: {
            const auto& decl = *static_cast<const ast::ReturnStmt*>(statement);
            out_.indent(depth_).append("return");
            if (decl.value) {
                out_.append(' ');
                emit_expr(decl.value);
            }
            out_.append_line(";");
            break;
        }

        case ast::StmtKind::If:
            emit_if(*static_cast<const ast::IfStmt*>(statement), true);
            break;

        case ast::StmtKind::While: {
            const auto& decl = *static_cast<const ast::WhileStmt*>(statement);
            out_.indent(depth_).append("while (");
            emit_expr(decl.condition);
            out_.append(") ");
            emit_branch(decl.body);
            break;
        }

        case ast::StmtKind::Free: {
            const auto& decl = *static_cast<const ast::FreeStmt*>(statement);
            out_.indent(depth_).append(decl.is_dispose ? "dispose(" : "free(");
            emit_expr(decl.target);
            out_.append_line(");");
            break;
        }
    }
}

void PrettyPrinter::emit_branch(const ast::Stmt* branch) {
    if (branch && branch->kind == ast::StmtKind::Block) {
        emit_block(static_cast<const ast::BlockStmt*>(branch));
        return;
    }
    out_.append_line();
    ++depth_;
    emit_statement(branch);
    --depth_;
}

void PrettyPrinter::emit_if(const ast::IfStmt& statement, bool indent_first) {
    if (indent_first) out_.indent(depth_);
    out_.append("if (");
    emit_expr(statement.condition);
    out_.append(") ");
    emit_branch(statement.then_branch);

    if (!statement.else_branch) return;
    out_.indent(depth_).append("else ");
    if (statement.else_branch->kind == ast::StmtKind::If) {
        // `else if` chains stay on one line rather than nesting a level deeper.
        emit_if(*static_cast<const ast::IfStmt*>(statement.else_branch), false);
        return;
    }
    emit_branch(statement.else_branch);
}

void PrettyPrinter::emit_string_literal(std::string_view value) {
    out_.append('"');
    for (char c : value) {
        switch (c) {
            case '\n': out_.append("\\n"); break;
            case '\t': out_.append("\\t"); break;
            case '\r': out_.append("\\r"); break;
            case '\0': out_.append("\\0"); break;
            case '\\': out_.append("\\\\"); break;
            case '"': out_.append("\\\""); break;
            default: out_.append(c); break;
        }
    }
    out_.append('"');
}

void PrettyPrinter::emit_operand(const ast::Expr* expr, int parent_precedence, bool right_side) {
    int precedence = precedence_of(expr);
    bool needs_parens =
        right_side ? precedence <= parent_precedence : precedence < parent_precedence;
    if (needs_parens) out_.append('(');
    emit_expr(expr);
    if (needs_parens) out_.append(')');
}

void PrettyPrinter::emit_expr(const ast::Expr* expr) {
    if (!expr) {
        out_.append("<error-expr>");
        return;
    }

    switch (expr->kind) {
        case ast::ExprKind::IntLiteral:
            out_.append_uint(static_cast<const ast::IntLiteralExpr*>(expr)->value);
            break;

        case ast::ExprKind::FloatLiteral:
            out_.append_double(static_cast<const ast::FloatLiteralExpr*>(expr)->value);
            break;

        case ast::ExprKind::BoolLiteral:
            out_.append(static_cast<const ast::BoolLiteralExpr*>(expr)->value ? "true" : "false");
            break;

        case ast::ExprKind::StringLiteral:
            emit_string_literal(static_cast<const ast::StringLiteralExpr*>(expr)->value);
            break;

        case ast::ExprKind::NullLiteral:
            out_.append("null");
            break;

        case ast::ExprKind::Identifier:
            out_.append(static_cast<const ast::IdentifierExpr*>(expr)->name);
            break;

        case ast::ExprKind::This:
            out_.append("this");
            break;

        case ast::ExprKind::TypeRef:
            emit_type(static_cast<const ast::TypeRefExpr*>(expr)->type);
            break;

        case ast::ExprKind::Strategy:
            out_.append(static_cast<const ast::StrategyExpr*>(expr)->strategy ==
                                ast::Strategy::Manual
                            ? "manual"
                            : "standard");
            break;

        case ast::ExprKind::Member: {
            const auto& member = *static_cast<const ast::MemberExpr*>(expr);
            emit_operand(member.object, kPrimaryPrecedence, false);
            out_.append('.').append(member.name);
            break;
        }

        case ast::ExprKind::Index: {
            const auto& index = *static_cast<const ast::IndexExpr*>(expr);
            emit_operand(index.object, kPrimaryPrecedence, false);
            out_.append('[');
            emit_expr(index.index);
            out_.append(']');
            break;
        }

        case ast::ExprKind::Call: {
            const auto& call = *static_cast<const ast::CallExpr*>(expr);
            emit_operand(call.callee, kPrimaryPrecedence, false);
            out_.append('(');
            bool first = true;
            if (call.has_strategy_token) {
                out_.append(call.strategy == ast::Strategy::Manual ? "manual" : "standard");
                first = false;
            }
            for (const ast::Expr* argument : call.args) {
                if (!first) out_.append(", ");
                first = false;
                emit_expr(argument);
            }
            out_.append(')');
            break;
        }

        case ast::ExprKind::Unary: {
            const auto& unary = *static_cast<const ast::UnaryExpr*>(expr);
            out_.append(ast::unary_op_spelling(unary.op));
            // Bracket a nested unary: `- -x` would otherwise print as `--x`,
            // which reads back the same but is needlessly obscure.
            if (unary.operand && unary.operand->kind == ast::ExprKind::Unary) {
                out_.append('(');
                emit_expr(unary.operand);
                out_.append(')');
            } else {
                emit_operand(unary.operand, kUnaryPrecedence, false);
            }
            break;
        }

        case ast::ExprKind::Binary: {
            const auto& binary = *static_cast<const ast::BinaryExpr*>(expr);
            int precedence = binary_precedence(binary.op);
            emit_operand(binary.left, precedence, false);
            out_.append(' ').append(ast::binary_op_spelling(binary.op)).append(' ');
            emit_operand(binary.right, precedence, true);
            break;
        }

        case ast::ExprKind::Assign: {
            const auto& assign = *static_cast<const ast::AssignExpr*>(expr);
            emit_operand(assign.target, kAssignPrecedence, true);
            out_.append(" = ");
            emit_operand(assign.value, kAssignPrecedence, false);
            break;
        }
    }
}


// ---------------------------------------------------------------------------
// Structural comparison
// ---------------------------------------------------------------------------
//
// The round-trip check needs more than "the text is stable": it needs proof
// that reparsing the printed source rebuilds the same tree. Source locations
// are deliberately ignored -- reformatting moves them.

namespace {

bool equal_type(const ast::TypeNode* left, const ast::TypeNode* right);
bool equal_expr(const ast::Expr* left, const ast::Expr* right);
bool equal_stmt(const ast::Stmt* left, const ast::Stmt* right);
bool equal_decl(const ast::Decl* left, const ast::Decl* right);

template <typename T, typename Fn>
bool equal_list(const util::Array<T>& left, const util::Array<T>& right, Fn&& compare) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (!compare(left[i], right[i])) return false;
    }
    return true;
}

bool equal_type(const ast::TypeNode* left, const ast::TypeNode* right) {
    if (!left || !right) return left == right;
    if (left->kind != right->kind) return false;
    switch (left->kind) {
        case ast::TypeNode::Kind::Builtin:
            return left->builtin == right->builtin && left->name == right->name;
        case ast::TypeNode::Kind::Named:
            return left->name == right->name;
        case ast::TypeNode::Kind::Pointer:
            return equal_type(left->pointee, right->pointee);
    }
    return false;
}

bool equal_expr(const ast::Expr* left, const ast::Expr* right) {
    if (!left || !right) return left == right;
    if (left->kind != right->kind) return false;

    switch (left->kind) {
        case ast::ExprKind::IntLiteral:
            return static_cast<const ast::IntLiteralExpr*>(left)->value ==
                   static_cast<const ast::IntLiteralExpr*>(right)->value;
        case ast::ExprKind::FloatLiteral:
            return static_cast<const ast::FloatLiteralExpr*>(left)->value ==
                   static_cast<const ast::FloatLiteralExpr*>(right)->value;
        case ast::ExprKind::BoolLiteral:
            return static_cast<const ast::BoolLiteralExpr*>(left)->value ==
                   static_cast<const ast::BoolLiteralExpr*>(right)->value;
        case ast::ExprKind::StringLiteral:
            return static_cast<const ast::StringLiteralExpr*>(left)->value ==
                   static_cast<const ast::StringLiteralExpr*>(right)->value;
        case ast::ExprKind::NullLiteral:
        case ast::ExprKind::This:
            return true;
        case ast::ExprKind::Identifier:
            return static_cast<const ast::IdentifierExpr*>(left)->name ==
                   static_cast<const ast::IdentifierExpr*>(right)->name;
        case ast::ExprKind::TypeRef:
            return equal_type(static_cast<const ast::TypeRefExpr*>(left)->type,
                              static_cast<const ast::TypeRefExpr*>(right)->type);
        case ast::ExprKind::Strategy:
            return static_cast<const ast::StrategyExpr*>(left)->strategy ==
                   static_cast<const ast::StrategyExpr*>(right)->strategy;
        case ast::ExprKind::Member: {
            const auto& a = *static_cast<const ast::MemberExpr*>(left);
            const auto& b = *static_cast<const ast::MemberExpr*>(right);
            return a.name == b.name && equal_expr(a.object, b.object);
        }
        case ast::ExprKind::Index: {
            const auto& a = *static_cast<const ast::IndexExpr*>(left);
            const auto& b = *static_cast<const ast::IndexExpr*>(right);
            return equal_expr(a.object, b.object) && equal_expr(a.index, b.index);
        }
        case ast::ExprKind::Call: {
            const auto& a = *static_cast<const ast::CallExpr*>(left);
            const auto& b = *static_cast<const ast::CallExpr*>(right);
            if (a.has_strategy_token != b.has_strategy_token || a.strategy != b.strategy) {
                return false;
            }
            if (!equal_expr(a.callee, b.callee)) return false;
            return equal_list(a.args, b.args, equal_expr);
        }
        case ast::ExprKind::Unary: {
            const auto& a = *static_cast<const ast::UnaryExpr*>(left);
            const auto& b = *static_cast<const ast::UnaryExpr*>(right);
            return a.op == b.op && equal_expr(a.operand, b.operand);
        }
        case ast::ExprKind::Binary: {
            const auto& a = *static_cast<const ast::BinaryExpr*>(left);
            const auto& b = *static_cast<const ast::BinaryExpr*>(right);
            return a.op == b.op && equal_expr(a.left, b.left) && equal_expr(a.right, b.right);
        }
        case ast::ExprKind::Assign: {
            const auto& a = *static_cast<const ast::AssignExpr*>(left);
            const auto& b = *static_cast<const ast::AssignExpr*>(right);
            return equal_expr(a.target, b.target) && equal_expr(a.value, b.value);
        }
    }
    return false;
}

bool equal_stmt(const ast::Stmt* left, const ast::Stmt* right) {
    if (!left || !right) return left == right;
    if (left->kind != right->kind) return false;

    switch (left->kind) {
        case ast::StmtKind::Empty:
            return true;
        case ast::StmtKind::Block:
            return equal_list(static_cast<const ast::BlockStmt*>(left)->statements,
                              static_cast<const ast::BlockStmt*>(right)->statements, equal_stmt);
        case ast::StmtKind::VarDecl: {
            const auto& a = *static_cast<const ast::VarDeclStmt*>(left);
            const auto& b = *static_cast<const ast::VarDeclStmt*>(right);
            return a.name == b.name && a.visibility == b.visibility &&
                   a.explicit_visibility == b.explicit_visibility && equal_type(a.type, b.type) &&
                   equal_expr(a.init, b.init);
        }
        case ast::StmtKind::Expr:
            return equal_expr(static_cast<const ast::ExprStmt*>(left)->expr,
                              static_cast<const ast::ExprStmt*>(right)->expr);
        case ast::StmtKind::Return:
            return equal_expr(static_cast<const ast::ReturnStmt*>(left)->value,
                              static_cast<const ast::ReturnStmt*>(right)->value);
        case ast::StmtKind::If: {
            const auto& a = *static_cast<const ast::IfStmt*>(left);
            const auto& b = *static_cast<const ast::IfStmt*>(right);
            return equal_expr(a.condition, b.condition) &&
                   equal_stmt(a.then_branch, b.then_branch) &&
                   equal_stmt(a.else_branch, b.else_branch);
        }
        case ast::StmtKind::While: {
            const auto& a = *static_cast<const ast::WhileStmt*>(left);
            const auto& b = *static_cast<const ast::WhileStmt*>(right);
            return equal_expr(a.condition, b.condition) && equal_stmt(a.body, b.body);
        }
        case ast::StmtKind::Free: {
            const auto& a = *static_cast<const ast::FreeStmt*>(left);
            const auto& b = *static_cast<const ast::FreeStmt*>(right);
            return a.is_dispose == b.is_dispose && equal_expr(a.target, b.target);
        }
    }
    return false;
}

bool equal_param(const ast::ParamDecl* left, const ast::ParamDecl* right) {
    if (!left || !right) return left == right;
    return left->name == right->name && equal_type(left->type, right->type);
}

bool equal_decl(const ast::Decl* left, const ast::Decl* right) {
    if (!left || !right) return left == right;
    if (left->kind != right->kind) return false;

    switch (left->kind) {
        case ast::DeclKind::Field: {
            const auto& a = *static_cast<const ast::FieldDecl*>(left);
            const auto& b = *static_cast<const ast::FieldDecl*>(right);
            return a.name == b.name && a.visibility == b.visibility &&
                   a.explicit_visibility == b.explicit_visibility && equal_type(a.type, b.type) &&
                   equal_expr(a.init, b.init);
        }
        case ast::DeclKind::Method: {
            const auto& a = *static_cast<const ast::MethodDecl*>(left);
            const auto& b = *static_cast<const ast::MethodDecl*>(right);
            return a.name == b.name && a.form == b.form && a.visibility == b.visibility &&
                   a.explicit_visibility == b.explicit_visibility &&
                   a.explicit_void == b.explicit_void &&
                   equal_type(a.return_type, b.return_type) &&
                   equal_list(a.params, b.params, equal_param) &&
                   equal_stmt(a.body, b.body);
        }
        case ast::DeclKind::Procedures: {
            const auto& a = *static_cast<const ast::ProceduresDecl*>(left);
            const auto& b = *static_cast<const ast::ProceduresDecl*>(right);
            return a.visibility == b.visibility &&
                   a.explicit_visibility == b.explicit_visibility &&
                   a.has_param_list == b.has_param_list &&
                   equal_list(a.params, b.params, equal_param) && equal_stmt(a.body, b.body);
        }
        case ast::DeclKind::Class: {
            const auto& a = *static_cast<const ast::ClassDecl*>(left);
            const auto& b = *static_cast<const ast::ClassDecl*>(right);
            return a.name == b.name && a.base_name == b.base_name &&
                   a.visibility == b.visibility &&
                   a.explicit_visibility == b.explicit_visibility &&
                   equal_list(a.members, b.members, equal_decl);
        }
        case ast::DeclKind::Import: {
            const auto& a = *static_cast<const ast::ImportDecl*>(left);
            const auto& b = *static_cast<const ast::ImportDecl*>(right);
            return equal_list(a.path, b.path,
                              [](std::string_view x, std::string_view y) { return x == y; });
        }
        case ast::DeclKind::Param:
            return equal_param(static_cast<const ast::ParamDecl*>(left),
                               static_cast<const ast::ParamDecl*>(right));
    }
    return false;
}

}  // namespace

bool ast_equal(const ast::CompilationUnit& left, const ast::CompilationUnit& right) {
    auto compare_import = [](const ast::ImportDecl* a, const ast::ImportDecl* b) {
        return equal_decl(a, b);
    };
    auto compare_class = [](const ast::ClassDecl* a, const ast::ClassDecl* b) {
        return equal_decl(a, b);
    };
    return equal_list(left.imports, right.imports, compare_import) &&
           equal_list(left.classes, right.classes, compare_class);
}

}  // namespace khu::parser
