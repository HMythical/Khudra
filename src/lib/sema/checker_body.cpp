// Body checking: statements and expressions.
//
// Split out from checker.cpp, which handles declarations and the whole-program
// passes. This half is where the type rules, the access-control context and the
// memory-model rules actually meet the code.
#include <string>

#include "sema/builtins.h"
#include "sema/checker.h"

namespace khu::sema {
namespace {

// Conservative "every path returns" analysis, enough to catch a function that
// declares a result and forgets to produce one.
bool always_returns(const ast::Stmt* statement) {
    if (!statement) return false;
    switch (statement->kind) {
        case ast::StmtKind::Return:
            return true;
        case ast::StmtKind::Block: {
            const auto& block = *static_cast<const ast::BlockStmt*>(statement);
            for (const ast::Stmt* child : block.statements) {
                if (always_returns(child)) return true;
            }
            return false;
        }
        case ast::StmtKind::If: {
            const auto& branch = *static_cast<const ast::IfStmt*>(statement);
            return branch.else_branch && always_returns(branch.then_branch) &&
                   always_returns(branch.else_branch);
        }
        default:
            // A `while` may run zero times, so it never guarantees a return.
            return false;
    }
}

// Collects the names a constructor assigns, so the Procedures block -- which
// runs first -- can warn about reading them.
void collect_assigned_names(const ast::Expr* expr, util::Array<std::string_view>& out);

void collect_assigned_names_stmt(const ast::Stmt* statement,
                                 util::Array<std::string_view>& out) {
    if (!statement) return;
    switch (statement->kind) {
        case ast::StmtKind::Block:
            for (const ast::Stmt* child : static_cast<const ast::BlockStmt*>(statement)->statements) {
                collect_assigned_names_stmt(child, out);
            }
            break;
        case ast::StmtKind::Expr:
            collect_assigned_names(static_cast<const ast::ExprStmt*>(statement)->expr, out);
            break;
        case ast::StmtKind::VarDecl:
            collect_assigned_names(static_cast<const ast::VarDeclStmt*>(statement)->init, out);
            break;
        case ast::StmtKind::If: {
            const auto& branch = *static_cast<const ast::IfStmt*>(statement);
            collect_assigned_names(branch.condition, out);
            collect_assigned_names_stmt(branch.then_branch, out);
            collect_assigned_names_stmt(branch.else_branch, out);
            break;
        }
        case ast::StmtKind::While: {
            const auto& loop = *static_cast<const ast::WhileStmt*>(statement);
            collect_assigned_names(loop.condition, out);
            collect_assigned_names_stmt(loop.body, out);
            break;
        }
        case ast::StmtKind::Return:
            collect_assigned_names(static_cast<const ast::ReturnStmt*>(statement)->value, out);
            break;
        default:
            break;
    }
}

void collect_assigned_names(const ast::Expr* expr, util::Array<std::string_view>& out) {
    if (!expr) return;
    if (expr->kind != ast::ExprKind::Assign) return;
    const auto& assign = *static_cast<const ast::AssignExpr*>(expr);
    const ast::Expr* target = assign.target;
    if (!target) return;

    if (target->kind == ast::ExprKind::Identifier) {
        out.push(static_cast<const ast::IdentifierExpr*>(target)->name);
    } else if (target->kind == ast::ExprKind::Member) {
        const auto& member = *static_cast<const ast::MemberExpr*>(target);
        if (member.object && member.object->kind == ast::ExprKind::This) out.push(member.name);
    }
    collect_assigned_names(assign.value, out);
}

}  // namespace

// ---------------------------------------------------------------------------
// Bodies
// ---------------------------------------------------------------------------

void Checker::collect_constructor_assignments(ClassSymbol& symbol) {
    constructor_assigned_.clear();
    if (!symbol.constructor || !symbol.constructor->decl) return;
    collect_assigned_names_stmt(symbol.constructor->decl->body, constructor_assigned_);
}

bool Checker::is_constructor_assigned(std::string_view field_name) const {
    for (std::string_view name : constructor_assigned_) {
        if (name == field_name) return true;
    }
    return false;
}

void Checker::check_bodies() {
    for (ClassSymbol* symbol : program_->classes) check_class_bodies(*symbol);
}

void Checker::check_class_bodies(ClassSymbol& symbol) {
    current_class_ = &symbol;
    collect_constructor_assignments(symbol);

    // Field initializers run during object linking, before the Procedures
    // block; they see `this` but no locals.
    scopes_.clear();
    next_frame_index_ = 0;
    pins_.clear();
    push_scope();
    for (VarSymbol* field : symbol.fields) {
        if (!field->field_decl || !field->field_decl->init) continue;
        const Type* init = check_expr(field->field_decl->init, field->type);
        if (!assignable(init, field->type)) {
            report_mismatch(field->field_decl->init->loc, init, field->type,
                            "in the initializer of field '" + std::string(field->name) + "'");
        }
    }
    pop_scope();

    if (symbol.procedures) check_procedures_body(*symbol.procedures);
    if (symbol.constructor) check_method_body(*symbol.constructor);
    for (MethodSymbol* method : symbol.methods) check_method_body(*method);

    current_class_ = nullptr;
}

void Checker::check_method_body(MethodSymbol& method) {
    current_method_ = &method;
    current_procedure_ = nullptr;
    pins_.clear();
    scopes_.clear();
    next_frame_index_ = 0;

    push_scope();
    for (VarSymbol* param : method.params) declare_variable(param, param->loc);
    if (method.decl && method.decl->body) check_block(method.decl->body, false);
    pop_scope();
    method.frame_size = next_frame_index_;

    bool wants_value = method.return_type && !method.return_type->is_void() &&
                       !method.return_type->is_error();
    if (wants_value && method.decl && method.decl->body &&
        !always_returns(method.decl->body)) {
        diagnostics_.error(method.loc, "'" + std::string(method.name) + "' declares -> " +
                                           method.return_type->display() +
                                           " but control can reach the end of its body without "
                                           "returning a value");
    }

    current_method_ = nullptr;
}

void Checker::check_procedures_body(ProcedureSymbol& procedures) {
    current_method_ = nullptr;
    current_procedure_ = &procedures;
    pins_.clear();
    scopes_.clear();
    next_frame_index_ = 0;

    push_scope();
    for (VarSymbol* param : procedures.params) declare_variable(param, param->loc);
    if (procedures.decl && procedures.decl->body) check_block(procedures.decl->body, false);
    pop_scope();
    procedures.frame_size = next_frame_index_;

    current_procedure_ = nullptr;
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

void Checker::check_block(ast::BlockStmt* block, bool open_scope) {
    if (!block) return;
    if (open_scope) push_scope();
    for (ast::Stmt* statement : block->statements) check_stmt(statement);
    if (open_scope) pop_scope();
}

void Checker::check_stmt(ast::Stmt* statement) {
    if (!statement) return;

    switch (statement->kind) {
        case ast::StmtKind::Empty:
            break;

        case ast::StmtKind::Block:
            check_block(static_cast<ast::BlockStmt*>(statement), true);
            break;

        case ast::StmtKind::VarDecl:
            check_local(*static_cast<ast::VarDeclStmt*>(statement));
            break;

        case ast::StmtKind::Expr:
            check_expr(static_cast<ast::ExprStmt*>(statement)->expr);
            break;

        case ast::StmtKind::Return: {
            auto& node = *static_cast<ast::ReturnStmt*>(statement);
            if (current_procedure_) {
                if (node.value) {
                    diagnostics_
                        .error(node.loc, "a Procedures block cannot return a value")
                        .note(current_procedure_->loc,
                              "it runs during materialization, and nothing is waiting for a "
                              "result; use a bare 'return;' to leave early");
                    check_expr(node.value);
                }
                break;
            }
            if (!current_method_) break;

            const Type* declared = current_method_->return_type;
            if (!node.value) {
                if (declared && !declared->is_void() && !declared->is_error()) {
                    diagnostics_.error(node.loc, "'" + std::string(current_method_->name) +
                                                     "' must return a value of type " +
                                                     declared->display());
                }
                break;
            }
            const Type* value = check_expr(node.value, declared);
            if (declared && declared->is_void()) {
                diagnostics_
                    .error(node.loc, "'" + std::string(current_method_->name) +
                                         "' returns void, so it cannot return a value")
                    .note(current_method_->loc,
                          "no '->' on a declaration means void; add '-> " + value->display() +
                              "' to return one");
                break;
            }
            if (!assignable(value, declared)) {
                report_mismatch(node.value->loc, value, declared,
                                "in a return from '" + std::string(current_method_->name) + "'");
            }
            break;
        }

        case ast::StmtKind::If: {
            auto& node = *static_cast<ast::IfStmt*>(statement);
            const Type* condition = check_expr(node.condition, types_.bool_type());
            if (condition && !condition->is_bool() && !condition->is_error()) {
                diagnostics_.error(node.condition->loc,
                                   "an if condition must be bool, found " + condition->display());
            }
            check_stmt(node.then_branch);
            check_stmt(node.else_branch);
            break;
        }

        case ast::StmtKind::While: {
            auto& node = *static_cast<ast::WhileStmt*>(statement);
            const Type* condition = check_expr(node.condition, types_.bool_type());
            if (condition && !condition->is_bool() && !condition->is_error()) {
                diagnostics_.error(node.condition->loc,
                                   "a while condition must be bool, found " +
                                       condition->display());
            }
            check_stmt(node.body);
            break;
        }

        case ast::StmtKind::Free:
            check_free(*static_cast<ast::FreeStmt*>(statement));
            break;
    }
}

void Checker::check_local(ast::VarDeclStmt& decl) {
    const Type* declared = resolve_type(decl.type);

    auto* symbol = arena_.create<VarSymbol>();
    symbol->name = decl.name;
    symbol->type = declared;
    symbol->role = VarRole::Local;
    symbol->visibility = decl.visibility;
    symbol->explicit_visibility = decl.explicit_visibility;
    symbol->loc = decl.name_loc;
    symbol->has_initializer = decl.init != nullptr;

    // The initializer is checked before the name is in scope, so
    // `int32 x = x;` reports an unknown name rather than reading itself.
    if (decl.init) {
        const Type* init = check_expr(decl.init, declared);
        if (!assignable(init, declared)) {
            report_mismatch(decl.init->loc, init, declared,
                            "in the initializer of '" + std::string(decl.name) + "'");
        }
        // Remember an allocation-site override so `free` judges by the strategy
        // the object was actually allocated under.
        const auto* site = decl.init->as<ast::CallExpr>();
        if (site && site->has_strategy_token && decl.init->info &&
            decl.init->info->is_allocation) {
            symbol->has_site_strategy = true;
            symbol->site_strategy = decl.init->info->alloc_strategy;
        }
    }

    decl.symbol = declare_variable(symbol, decl.name_loc);
}

void Checker::check_free(ast::FreeStmt& statement) {
    const char* verb = statement.is_dispose ? "dispose" : "free";
    const Type* type = check_expr(statement.target);
    if (!type || type->is_error()) return;

    if (!type->is_class()) {
        diagnostics_.error(statement.target->loc, std::string(verb) +
                                                      " expects a manually allocated object, "
                                                      "found " +
                                                      type->display());
        return;
    }

    const ClassSymbol* target = type->class_symbol;
    VarSymbol* local = statement.target->info ? statement.target->info->var : nullptr;

    // An allocation-site token wins over the class default, so a collected
    // class allocated with `manual` is releasable and vice versa.
    Strategy effective = target ? target->strategy : Strategy::Gc;
    bool from_site = local && local->has_site_strategy;
    if (from_site) effective = local->site_strategy;

    if (effective != Strategy::Manual) {
        auto builder = diagnostics_.error(
            statement.target->loc, "cannot " + std::string(verb) + " '" +
                                       (target ? std::string(target->name) : type->display()) +
                                       "': it is garbage collected");
        if (from_site) {
            builder.note(local->loc,
                         "this allocation site selects 'standard', which overrides the class "
                         "default");
        } else if (target && target->strategy_field) {
            builder.note(target->strategy_field->loc,
                         "its strategy field selects setStandard(); use setManual(), or drop the "
                         "reference and let the collector reclaim it");
        } else if (target) {
            builder.note(target->loc,
                         "a class with no MemoryAllocationTypeObject field is collected by "
                         "default");
        }
        return;
    }

    if (const PinTracker::PinSite* pin = pins_.find_pin(local)) {
        diagnostics_
            .error(statement.target->loc,
                   "cannot " + std::string(verb) + " pinned object '" +
                       std::string(local ? local->name : std::string_view("<value>")) +
                       "': it is still held by '" + pin->holder + "'")
            .note(pin->loc, "pinned here; assigning a manual object into a managed slot "
                            "increments its pin count")
            .note(statement.target->loc,
                  "clear '" + pin->holder + "' before releasing the object");
        return;
    }

    if (const diag::SourceLocation* previous = pins_.find_free(local)) {
        diagnostics_
            .error(statement.target->loc, "'" +
                                              std::string(local ? local->name
                                                                : std::string_view("<value>")) +
                                              "' is released twice")
            .note(*previous, "already released here");
        return;
    }

    pins_.record_free(local, statement.target->loc);
}

}  // namespace khu::sema
