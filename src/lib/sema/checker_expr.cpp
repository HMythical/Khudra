// Expression checking.
//
// Every expression is typed here, and the same walk answers the questions the
// other passes need: which symbol does this name resolve to, is it reached
// through the current object or another one, and does an assignment cross the
// managed/manual boundary.
#include <string>

#include "sema/builtins.h"
#include "sema/checker.h"

namespace khu::sema {
namespace {

bool is_numeric_literal(const ast::Expr* expr) {
    return expr && (expr->kind == ast::ExprKind::IntLiteral ||
                    expr->kind == ast::ExprKind::FloatLiteral);
}

bool literal_fits(std::uint64_t value, const Type& type) {
    if (type.kind == TypeKind::UInt) {
        if (type.width >= 64) return true;
        return value <= ((1ull << type.width) - 1);
    }
    if (type.kind == TypeKind::Int) {
        if (type.width >= 64) return value <= 0x8000000000000000ull;
        return value <= (1ull << (type.width - 1));
    }
    return true;
}

const util::Array<VarSymbol*>& empty_params() {
    static const util::Array<VarSymbol*> none;
    return none;
}

// The parameters an allocation site binds to: the Procedures block's when it
// declares any, otherwise the constructor's (docs/procedures.md).
const util::Array<VarSymbol*>& materialization_params(const ClassSymbol& symbol) {
    if (symbol.procedures && !symbol.procedures->params.empty()) return symbol.procedures->params;
    if (symbol.constructor) return symbol.constructor->params;
    return empty_params();
}

std::string signature_of(const MethodSymbol& method) {
    std::string text(method.name);
    text += '(';
    for (std::size_t i = 0; i < method.params.size(); ++i) {
        if (i != 0) text += ", ";
        text += method.params[i]->type ? method.params[i]->type->display() : "<error>";
    }
    text += ')';
    return text;
}

}  // namespace

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

const Type* Checker::check_expr(ast::Expr* expr, const Type* expected) {
    if (!expr) return types_.error();
    ExprInfo& info = info_for(*expr);

    switch (expr->kind) {
        case ast::ExprKind::IntLiteral: {
            auto& literal = *static_cast<ast::IntLiteralExpr*>(expr);
            // Integer literals default to int32 unless context forces a width.
            const Type* type = expected && expected->is_integer() ? expected : types_.int32();
            if (!literal_fits(literal.value, *type)) {
                diagnostics_.error(literal.loc, "integer literal " +
                                                    std::to_string(literal.value) +
                                                    " does not fit in " + type->display());
            }
            info.type = type;
            break;
        }

        case ast::ExprKind::FloatLiteral:
            // Float literals default to the wider `dfloat` unless context asks
            // for `float`, mirroring how integer literals take their context.
            info.type = expected && expected->is_float() ? expected : types_.float_type(64);
            break;

        case ast::ExprKind::BoolLiteral:
            info.type = types_.bool_type();
            break;

        case ast::ExprKind::StringLiteral:
            info.type = types_.string_type();
            break;

        case ast::ExprKind::NullLiteral:
            info.type = types_.null_type();
            break;

        case ast::ExprKind::This:
            if (!current_class_) {
                diagnostics_.error(expr->loc, "'this' is only valid inside a class body");
                info.type = types_.error();
                break;
            }
            info.through_this = true;
            info.type = types_.class_type(current_class_);
            break;

        case ast::ExprKind::Identifier:
            info.type = check_identifier(*static_cast<ast::IdentifierExpr*>(expr));
            break;

        case ast::ExprKind::Member:
            info.type = check_member(*static_cast<ast::MemberExpr*>(expr), false);
            break;

        case ast::ExprKind::Call:
            info.type = check_call(*static_cast<ast::CallExpr*>(expr), expected);
            break;

        case ast::ExprKind::Index:
            info.type = check_index(*static_cast<ast::IndexExpr*>(expr));
            break;

        case ast::ExprKind::Unary:
            info.type = check_unary(*static_cast<ast::UnaryExpr*>(expr), expected);
            break;

        case ast::ExprKind::Binary:
            info.type = check_binary(*static_cast<ast::BinaryExpr*>(expr), expected);
            break;

        case ast::ExprKind::Assign:
            info.type = check_assign(*static_cast<ast::AssignExpr*>(expr));
            break;

        case ast::ExprKind::TypeRef:
            diagnostics_
                .error(expr->loc, "a type is not a value here")
                .note(expr->loc,
                      "types are only accepted where an intrinsic asks for one, as in "
                      "khuStdMath.convertTo(int64, x)");
            info.type = types_.error();
            break;

        case ast::ExprKind::Strategy:
            diagnostics_
                .error(expr->loc, "'" +
                                      std::string(static_cast<ast::StrategyExpr*>(expr)->strategy ==
                                                          ast::Strategy::Manual
                                                      ? "manual"
                                                      : "standard") +
                                      "' is only valid as the first argument of an allocation site")
                .note(expr->loc, "for example: FrameBuffer(manual)");
            info.type = types_.error();
            break;
    }

    if (!info.type) info.type = types_.error();
    return info.type;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

const Type* Checker::check_identifier(ast::IdentifierExpr& expr) {
    ExprInfo& info = info_for(expr);

    if (expr.name == "Procedures") {
        auto builder = diagnostics_.error(
            expr.loc, "a Procedures block is not invocable by name");
        if (current_class_ && current_class_->procedures) {
            builder.note(current_class_->procedures->loc,
                         "it runs automatically when an instance is materialized");
        }
        return types_.error();
    }

    if (!scopes_.empty()) {
        if (VarSymbol* local = scopes_.back()->lookup(expr.name)) {
            access_.check_local(*local, AccessContext::CurrentObject, false, expr.loc);
            info.var = local;
            info.is_lvalue = true;
            return local->type;
        }
    }

    if (current_class_) {
        if (VarSymbol* field = current_class_->find_field(expr.name)) {
            access_.check_field(*field, AccessContext::CurrentObject, false, expr.loc);
            if (current_procedure_ && !field->has_initializer &&
                is_constructor_assigned(field->name)) {
                diagnostics_
                    .warning(expr.loc, "field '" + std::string(field->name) +
                                           "' is assigned by the constructor, which runs after "
                                           "this Procedures block")
                    .note(field->loc, "it still holds its default value here");
            }
            info.var = field;
            info.through_this = true;
            info.is_lvalue = true;
            return field->type;
        }
    }

    if (ClassSymbol* symbol = program_->find_class(expr.name)) {
        if (symbol->is_namespace) {
            diagnostics_.error(expr.loc, "'" + std::string(expr.name) +
                                             "' is a namespace; call one of its members");
        } else {
            diagnostics_
                .error(expr.loc, "'" + std::string(expr.name) + "' is a class, not a value")
                .note(expr.loc, "write " + std::string(expr.name) +
                                    "(...) to materialize an instance");
        }
        return types_.error();
    }

    diagnostics_.error(expr.loc, "unknown name '" + std::string(expr.name) + "'");
    return types_.error();
}

// A receiver that names a built-in namespace rather than a value.
ClassSymbol* Checker::namespace_receiver(ast::Expr* object) {
    auto* ident = object ? object->as<ast::IdentifierExpr>() : nullptr;
    if (!ident) return nullptr;
    // A local or field with the same name wins; namespaces are not reserved.
    if (!scopes_.empty() && scopes_.back()->lookup(ident->name)) return nullptr;
    if (current_class_ && current_class_->find_field(ident->name)) return nullptr;

    ClassSymbol* symbol = program_->find_class(ident->name);
    return symbol && symbol->is_namespace ? symbol : nullptr;
}

const Type* Checker::check_member(ast::MemberExpr& expr, bool writing) {
    ExprInfo& info = info_for(expr);

    if (expr.name == "Procedures") {
        auto builder = diagnostics_.error(expr.name_loc,
                                          "a Procedures block is not invocable by name");
        builder.note(expr.name_loc,
                     "it runs automatically when the object is materialized, before the "
                     "allocation site resumes");
        return types_.error();
    }

    if (ClassSymbol* space = namespace_receiver(expr.object)) {
        info.namespace_ref = space;
        if (space->method_index.find(expr.name)) {
            diagnostics_.error(expr.name_loc, "'" + std::string(space->name) + "." +
                                                  std::string(expr.name) +
                                                  "' is a function; call it");
        } else {
            diagnostics_.error(expr.name_loc, "namespace '" + std::string(space->name) +
                                                  "' has no member '" + std::string(expr.name) +
                                                  "'");
        }
        return types_.error();
    }

    const Type* object = check_expr(expr.object);
    if (!object || object->is_error()) return types_.error();

    if (!object->is_class() || !object->class_symbol) {
        diagnostics_.error(expr.name_loc,
                           "type " + object->display() + " has no member '" +
                               std::string(expr.name) + "'");
        return types_.error();
    }

    auto* owner = const_cast<ClassSymbol*>(object->class_symbol);
    // Object-level visibility: only `this` counts as the current object, so a
    // private member of another instance of the same class is still off limits.
    bool through_this = expr.object && expr.object->kind == ast::ExprKind::This;
    AccessContext context =
        through_this ? AccessContext::CurrentObject : AccessContext::OtherObject;

    if (VarSymbol* field = owner->find_field(expr.name)) {
        access_.check_field(*field, context, writing, expr.name_loc);
        if (through_this && current_procedure_ && !writing && !field->has_initializer &&
            is_constructor_assigned(field->name)) {
            diagnostics_
                .warning(expr.name_loc, "field '" + std::string(field->name) +
                                            "' is assigned by the constructor, which runs after "
                                            "this Procedures block")
                .note(field->loc, "it still holds its default value here");
        }
        info.var = field;
        info.through_this = through_this;
        info.is_lvalue = true;
        return field->type;
    }

    util::Array<MethodSymbol*> candidates;
    owner->find_methods(expr.name, candidates);
    if (!candidates.empty()) {
        diagnostics_.error(expr.name_loc, "'" + std::string(owner->name) + "." +
                                              std::string(expr.name) +
                                              "' is a function; call it");
        return types_.error();
    }

    diagnostics_.error(expr.name_loc, "class '" + std::string(owner->name) +
                                          "' has no member '" + std::string(expr.name) + "'");
    return types_.error();
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------

bool Checker::bind_arguments(const util::Array<VarSymbol*>& params, ast::CallExpr& call,
                             std::string_view what, diag::SourceLocation loc) {
    if (call.args.size() != params.size()) {
        diagnostics_.error(loc, std::string(what) + " expects " +
                                    std::to_string(params.size()) + " argument" +
                                    (params.size() == 1 ? "" : "s") + ", found " +
                                    std::to_string(call.args.size()));
        for (ast::Expr* argument : call.args) check_expr(argument);
        return false;
    }

    bool ok = true;
    for (std::size_t i = 0; i < params.size(); ++i) {
        const Type* wanted = params[i]->type;
        const Type* got = call.args[i]->info ? call.args[i]->info->type : nullptr;
        // Re-check literals so they take the parameter's width.
        if (!got || is_numeric_literal(call.args[i])) got = check_expr(call.args[i], wanted);
        if (!assignable(got, wanted)) {
            report_mismatch(call.args[i]->loc, got, wanted,
                            "in argument " + std::to_string(i + 1) + " of " + std::string(what));
            ok = false;
        }
    }
    return ok;
}

MethodSymbol* Checker::resolve_overload(util::Array<MethodSymbol*>& candidates,
                                        ast::CallExpr& call, std::string_view display_name,
                                        const Type* expected) {
    util::Array<MethodSymbol*> by_arity;
    for (MethodSymbol* candidate : candidates) {
        if (candidate->params.size() == call.args.size()) by_arity.push(candidate);
    }

    if (by_arity.empty()) {
        auto builder = diagnostics_.error(
            call.loc, "no declaration of '" + std::string(display_name) + "' takes " +
                          std::to_string(call.args.size()) + " argument" +
                          (call.args.size() == 1 ? "" : "s"));
        note_candidates(builder, candidates, call.loc);
        for (ast::Expr* argument : call.args) check_expr(argument);
        return nullptr;
    }

    if (by_arity.size() == 1) {
        bind_arguments(by_arity[0]->params, call, "call to '" + std::string(display_name) + "'",
                       call.loc);
        return by_arity[0];
    }

    // Type the arguments once, with no expectation, then pick the single
    // candidate they fit. Re-typing per candidate would report the same errors
    // inside an argument several times over.
    util::Array<const Type*> actual;
    for (ast::Expr* argument : call.args) actual.push(check_expr(argument));

    // Three rounds, narrowest first, so a call that mixes committed values with
    // uncommitted literals lands on one answer instead of being ambiguous:
    //   1. the candidate whose result the context already asked for,
    //   2. an exact match on the arguments' natural types,
    //   3. anything a literal could still adapt to.
    auto accepts = [&](MethodSymbol* candidate, bool exact) {
        for (std::size_t i = 0; i < actual.size(); ++i) {
            const Type* wanted = candidate->params[i]->type;
            if (exact ? (actual[i] == wanted) : assignable(actual[i], wanted)) continue;
            // In the loose round a numeric literal has not committed to a
            // width yet, so it fits any numeric parameter. In the exact round
            // it has already taken its natural type and must match outright --
            // otherwise every width would look equally good.
            if (!exact && is_numeric_literal(call.args[i]) && wanted && wanted->is_numeric()) {
                continue;
            }
            return false;
        }
        return true;
    };

    if (expected && expected->is_numeric()) {
        for (MethodSymbol* candidate : by_arity) {
            if (candidate->return_type != expected) continue;
            if (!accepts(candidate, false)) continue;
            bind_arguments(candidate->params, call,
                           "call to '" + std::string(display_name) + "'", call.loc);
            return candidate;
        }
    }

    util::Array<MethodSymbol*> exact;
    for (MethodSymbol* candidate : by_arity) {
        if (accepts(candidate, true)) exact.push(candidate);
    }
    if (exact.size() == 1) {
        bind_arguments(exact[0]->params, call, "call to '" + std::string(display_name) + "'",
                       call.loc);
        return exact[0];
    }

    util::Array<MethodSymbol*> viable;
    for (MethodSymbol* candidate : by_arity) {
        if (accepts(candidate, false)) viable.push(candidate);
    }

    if (viable.size() == 1) {
        bind_arguments(viable[0]->params, call, "call to '" + std::string(display_name) + "'",
                       call.loc);
        return viable[0];
    }

    if (viable.empty()) {
        std::string got;
        for (std::size_t i = 0; i < actual.size(); ++i) {
            if (i != 0) got += ", ";
            got += actual[i] ? actual[i]->display() : "<error>";
        }
        auto builder = diagnostics_.error(call.loc, "no declaration of '" +
                                                        std::string(display_name) +
                                                        "' matches (" + got + ")");
        builder.note(call.loc,
                     "Khudra has no implicit widening or narrowing; convert with "
                     "khuStdMath.convertTo(<type>, <expr>) so every operand has one width");
        note_candidates(builder, by_arity, call.loc);
        return nullptr;
    }

    auto builder = diagnostics_.error(call.loc, "call to '" + std::string(display_name) +
                                                    "' is ambiguous");
    note_candidates(builder, viable, call.loc);
    return nullptr;
}

// Built-in overload sets are wide (one per integer width) and carry no source
// location, so they collapse into a single note instead of ten.
void Checker::note_candidates(diag::DiagnosticEngine::Builder& builder,
                              const util::Array<MethodSymbol*>& candidates,
                              diag::SourceLocation loc) {
    bool any_located = false;
    for (MethodSymbol* candidate : candidates) {
        if (candidate->loc.valid()) any_located = true;
    }

    if (any_located) {
        std::size_t shown = 0;
        for (MethodSymbol* candidate : candidates) {
            if (!candidate->loc.valid()) continue;
            if (shown++ == 6) {
                builder.note(loc, "...and " + std::to_string(candidates.size() - shown + 1) +
                                      " more");
                break;
            }
            builder.note(candidate->loc, "candidate: " + signature_of(*candidate));
        }
        return;
    }

    std::string list;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (i == 8) {
            list += ", ...";
            break;
        }
        if (i != 0) list += ", ";
        list += signature_of(*candidates[i]);
    }
    builder.note(loc, "declared as: " + list);
}

void Checker::report_stray_strategy(const ast::CallExpr& expr) {
    diagnostics_
        .error(expr.strategy_loc,
               std::string("'") +
                   (expr.strategy == ast::Strategy::Manual ? "manual" : "standard") +
                   "' is only valid as the first argument of an allocation site")
        .note(expr.strategy_loc, "for example: FrameBuffer(manual)");
}

const Type* Checker::check_call(ast::CallExpr& expr, const Type* expected) {
    ExprInfo& info = info_for(expr);

    // A strategy token anywhere but the leading position never reached
    // CallExpr::strategy, so it is still sitting in the argument list.
    for (ast::Expr* argument : expr.args) {
        if (argument->kind == ast::ExprKind::Strategy) check_expr(argument);
    }

    // Only a bare class name can start an allocation site, so a leading
    // strategy token on any other callee is wrong before resolution begins.
    if (expr.has_strategy_token && expr.callee->kind != ast::ExprKind::Identifier) {
        report_stray_strategy(expr);
    }

    if (auto* ident = expr.callee->as<ast::IdentifierExpr>()) {
        if (ident->name == "Procedures") {
            auto builder = diagnostics_.error(expr.callee->loc,
                                              "a Procedures block is not invocable by name");
            if (current_class_ && current_class_->procedures) {
                builder.note(current_class_->procedures->loc,
                             "it runs automatically when an instance is materialized, before the "
                             "constructor body");
            }
            for (ast::Expr* argument : expr.args) check_expr(argument);
            return types_.error();
        }

        bool shadowed = (!scopes_.empty() && scopes_.back()->lookup(ident->name)) ||
                        (current_class_ && current_class_->find_field(ident->name));

        if (!shadowed) {
            if (ClassSymbol* target = program_->find_class(ident->name)) {
                if (target->is_namespace) {
                    diagnostics_.error(expr.callee->loc, "'" + std::string(ident->name) +
                                                             "' is a namespace, not a function");
                    return types_.error();
                }
                return check_allocation(expr, *target);
            }
        }

        if (current_class_) {
            util::Array<MethodSymbol*> candidates;
            current_class_->find_methods(ident->name, candidates);
            if (!candidates.empty()) {
                if (expr.has_strategy_token) report_stray_strategy(expr);
                MethodSymbol* method = resolve_overload(candidates, expr, ident->name, expected);
                if (!method) return types_.error();
                access_.check_method(*method, AccessContext::CurrentObject, expr.callee->loc);
                info.method = method;
                info.through_this = true;
                return method->return_type;
            }
        }

        diagnostics_.error(expr.callee->loc, "unknown function '" + std::string(ident->name) +
                                                 "'");
        for (ast::Expr* argument : expr.args) check_expr(argument);
        return types_.error();
    }

    if (auto* member = expr.callee->as<ast::MemberExpr>()) {
        if (member->object && member->object->kind == ast::ExprKind::TypeRef) {
            return check_strategy_factory(expr, *member);
        }

        if (ClassSymbol* space = namespace_receiver(member->object)) {
            info.namespace_ref = space;
            if (space->name == kMathNamespace && member->name == "convertTo") {
                return check_convert_intrinsic(expr);
            }

            util::Array<MethodSymbol*> candidates;
            space->find_methods(member->name, candidates);
            if (candidates.empty()) {
                diagnostics_.error(member->name_loc, "namespace '" + std::string(space->name) +
                                                         "' has no member '" +
                                                         std::string(member->name) + "'");
                for (ast::Expr* argument : expr.args) check_expr(argument);
                return types_.error();
            }
            std::string display = std::string(space->name) + "." + std::string(member->name);
            MethodSymbol* method = resolve_overload(candidates, expr, display, expected);
            if (!method) return types_.error();
            info.method = method;
            return method->return_type;
        }

        if (member->name == "Procedures") {
            check_expr(member->object);
            diagnostics_
                .error(member->name_loc, "a Procedures block is not invocable by name")
                .note(member->name_loc,
                      "it runs automatically when the object is materialized, before the "
                      "allocation site resumes");
            return types_.error();
        }

        const Type* object = check_expr(member->object);
        if (!object || object->is_error()) {
            for (ast::Expr* argument : expr.args) check_expr(argument);
            return types_.error();
        }
        if (!object->is_class() || !object->class_symbol) {
            diagnostics_.error(member->name_loc, "type " + object->display() +
                                                     " has no member '" +
                                                     std::string(member->name) + "'");
            for (ast::Expr* argument : expr.args) check_expr(argument);
            return types_.error();
        }

        auto* owner = const_cast<ClassSymbol*>(object->class_symbol);
        util::Array<MethodSymbol*> candidates;
        owner->find_methods(member->name, candidates);
        if (candidates.empty()) {
            diagnostics_.error(member->name_loc, "class '" + std::string(owner->name) +
                                                     "' has no function or method '" +
                                                     std::string(member->name) + "'");
            for (ast::Expr* argument : expr.args) check_expr(argument);
            return types_.error();
        }

        std::string display = std::string(owner->name) + "." + std::string(member->name);
        MethodSymbol* method = resolve_overload(candidates, expr, display, expected);
        if (!method) return types_.error();

        bool through_this = member->object->kind == ast::ExprKind::This;
        access_.check_method(*method,
                             through_this ? AccessContext::CurrentObject
                                          : AccessContext::OtherObject,
                             member->name_loc);
        info.method = method;
        info.through_this = through_this;
        return method->return_type;
    }

    check_expr(expr.callee);
    diagnostics_.error(expr.loc, "this expression is not callable");
    for (ast::Expr* argument : expr.args) check_expr(argument);
    return types_.error();
}

const Type* Checker::check_allocation(ast::CallExpr& expr, ClassSymbol& target) {
    ExprInfo& info = info_for(expr);
    info.is_allocation = true;
    info.alloc_class = &target;
    info.alloc_strategy = memory_.resolve_allocation_strategy(target, expr);

    bind_arguments(materialization_params(target), expr,
                   "materializing '" + std::string(target.name) + "'", expr.loc);
    return types_.class_type(&target);
}

const Type* Checker::check_strategy_factory(ast::CallExpr& expr, const ast::MemberExpr& callee) {
    ExprInfo& info = info_for(expr);
    const auto* type_ref = callee.object->as<ast::TypeRefExpr>();
    const ast::TypeNode* node = type_ref ? type_ref->type : nullptr;

    bool is_memory_type = node && node->kind == ast::TypeNode::Kind::Builtin &&
                          node->builtin == ast::BuiltinType::MemoryAllocationTypeObject;
    if (!is_memory_type) {
        diagnostics_.error(callee.name_loc, "type " +
                                                (node ? std::string(node->name) : "<error>") +
                                                " has no member '" + std::string(callee.name) +
                                                "'");
        for (ast::Expr* argument : expr.args) check_expr(argument);
        return types_.error();
    }

    if (callee.name != "setStandard" && callee.name != "setManual") {
        diagnostics_.error(callee.name_loc,
                           "MemoryAllocationTypeObject has no member '" +
                               std::string(callee.name) +
                               "'; the factories are setStandard() and setManual()");
        for (ast::Expr* argument : expr.args) check_expr(argument);
        return types_.error();
    }

    if (!expr.args.empty()) {
        diagnostics_.error(expr.loc, "MemoryAllocationTypeObject." + std::string(callee.name) +
                                         "() takes no arguments");
        for (ast::Expr* argument : expr.args) check_expr(argument);
    }

    info.is_strategy_factory = true;
    info.alloc_strategy = callee.name == "setManual" ? Strategy::Manual : Strategy::Gc;
    return types_.memory_type();
}

const Type* Checker::check_convert_intrinsic(ast::CallExpr& expr) {
    ExprInfo& info = info_for(expr);

    if (expr.args.size() != 2) {
        diagnostics_
            .error(expr.loc, "khuStdMath.convertTo expects a target type and a value")
            .note(expr.loc, "for example: khuStdMath.convertTo(int64, y)");
        for (ast::Expr* argument : expr.args) check_expr(argument);
        return types_.error();
    }

    auto* type_ref = expr.args[0]->as<ast::TypeRefExpr>();
    if (!type_ref) {
        diagnostics_.error(expr.args[0]->loc,
                           "the first argument of khuStdMath.convertTo must be a type");
        check_expr(expr.args[1]);
        return types_.error();
    }

    const Type* target = resolve_type(type_ref->type);
    info_for(*expr.args[0]).type = target;

    const Type* source = check_expr(expr.args[1]);
    if (target->is_error() || (source && source->is_error())) return types_.error();

    if (!target->is_numeric()) {
        diagnostics_.error(expr.args[0]->loc, "khuStdMath.convertTo can only target a numeric "
                                              "type, found " +
                                                  target->display());
        return types_.error();
    }
    if (!source || !source->is_numeric()) {
        diagnostics_.error(expr.args[1]->loc, "khuStdMath.convertTo can only convert a numeric "
                                              "value, found " +
                                                  (source ? source->display() : "<error>"));
        return types_.error();
    }

    info.convert_target = target;
    return target;
}

// ---------------------------------------------------------------------------
// Operators
// ---------------------------------------------------------------------------

const Type* Checker::check_index(ast::IndexExpr& expr) {
    const Type* object = check_expr(expr.object);
    const Type* index = check_expr(expr.index);

    if (index && !index->is_integer() && !index->is_error()) {
        diagnostics_.error(expr.index->loc,
                           "an index must be an integer, found " + index->display());
    }
    if (!object || object->is_error()) return types_.error();

    if (object->kind == TypeKind::Pointer) return object->pointee ? object->pointee : types_.error();
    if (object->kind == TypeKind::String) return types_.unsigned_int(8);
    if (object->kind == TypeKind::Array) {
        diagnostics_
            .error(expr.loc, "'Array' has no element type, so it cannot be indexed yet")
            .note(expr.loc, "typed containers arrive with generics (see docs/roadmap.md); use "
                            "*byte for a raw buffer in the meantime");
        return types_.error();
    }

    diagnostics_.error(expr.loc, "type " + object->display() + " cannot be indexed");
    return types_.error();
}

const Type* Checker::check_unary(ast::UnaryExpr& expr, const Type* expected) {
    const Type* propagate = expected && expected->is_numeric() ? expected : nullptr;

    switch (expr.op) {
        case ast::UnaryOp::Not: {
            const Type* operand = check_expr(expr.operand, types_.bool_type());
            if (operand && !operand->is_bool() && !operand->is_error()) {
                diagnostics_.error(expr.loc,
                                   "'!' expects bool, found " + operand->display());
                return types_.error();
            }
            return types_.bool_type();
        }
        case ast::UnaryOp::BitNot: {
            const Type* operand = check_expr(expr.operand, propagate);
            if (operand && !operand->is_integer() && !operand->is_error()) {
                diagnostics_.error(expr.loc, "'~' expects an integer, found " +
                                                 operand->display());
                return types_.error();
            }
            return operand;
        }
        case ast::UnaryOp::Plus:
        case ast::UnaryOp::Negate: {
            const Type* operand = check_expr(expr.operand, propagate);
            if (operand && !operand->is_numeric() && !operand->is_error()) {
                diagnostics_.error(expr.loc, std::string("'") +
                                                 ast::unary_op_spelling(expr.op) +
                                                 "' expects a numeric value, found " +
                                                 operand->display());
                return types_.error();
            }
            return operand;
        }
    }
    return types_.error();
}

const Type* Checker::check_binary(ast::BinaryExpr& expr, const Type* expected) {
    switch (expr.op) {
        case ast::BinaryOp::LogicalAnd:
        case ast::BinaryOp::LogicalOr: {
            const Type* left = check_expr(expr.left, types_.bool_type());
            const Type* right = check_expr(expr.right, types_.bool_type());
            bool ok = true;
            if (left && !left->is_bool() && !left->is_error()) {
                diagnostics_.error(expr.left->loc, std::string("'") +
                                                       ast::binary_op_spelling(expr.op) +
                                                       "' expects bool, found " +
                                                       left->display());
                ok = false;
            }
            if (right && !right->is_bool() && !right->is_error()) {
                diagnostics_.error(expr.right->loc, std::string("'") +
                                                        ast::binary_op_spelling(expr.op) +
                                                        "' expects bool, found " +
                                                        right->display());
                ok = false;
            }
            (void)ok;
            return types_.bool_type();
        }

        case ast::BinaryOp::ShiftLeft:
        case ast::BinaryOp::ShiftRight: {
            // The shift count is not an arithmetic operand, so it does not have
            // to share the value's width.
            const Type* value =
                check_expr(expr.left, expected && expected->is_integer() ? expected : nullptr);
            const Type* count = check_expr(expr.right);
            if (value && !value->is_integer() && !value->is_error()) {
                diagnostics_.error(expr.left->loc, "a shift expects an integer, found " +
                                                       value->display());
                return types_.error();
            }
            if (count && !count->is_integer() && !count->is_error()) {
                diagnostics_.error(expr.right->loc, "a shift count must be an integer, found " +
                                                        count->display());
            }
            return value;
        }

        case ast::BinaryOp::Equal:
        case ast::BinaryOp::NotEqual: {
            // Compare against a literal in either order without forcing a width
            // on the other side.
            const Type* left;
            const Type* right;
            if (is_numeric_literal(expr.left) && !is_numeric_literal(expr.right)) {
                right = check_expr(expr.right);
                left = check_expr(expr.left, right);
            } else {
                left = check_expr(expr.left);
                right = check_expr(expr.right, left);
            }
            if (!left || !right || left->is_error() || right->is_error()) {
                return types_.bool_type();
            }
            if (left == right || (left->is_null() && right->is_reference()) ||
                (right->is_null() && left->is_reference()) ||
                (left->is_class() && right->is_class() &&
                 (assignable(left, right) || assignable(right, left)))) {
                return types_.bool_type();
            }
            report_mismatch(expr.loc, right, left,
                            std::string("in a '") + ast::binary_op_spelling(expr.op) +
                                "' comparison");
            return types_.bool_type();
        }

        case ast::BinaryOp::Less:
        case ast::BinaryOp::LessEqual:
        case ast::BinaryOp::Greater:
        case ast::BinaryOp::GreaterEqual: {
            const Type* left;
            const Type* right;
            if (is_numeric_literal(expr.left) && !is_numeric_literal(expr.right)) {
                right = check_expr(expr.right);
                left = check_expr(expr.left, right);
            } else {
                left = check_expr(expr.left);
                right = check_expr(expr.right, left);
            }
            if (!left || !right || left->is_error() || right->is_error()) {
                return types_.bool_type();
            }
            if (!left->is_numeric() || !right->is_numeric()) {
                diagnostics_.error(expr.loc, std::string("'") +
                                                 ast::binary_op_spelling(expr.op) +
                                                 "' expects numeric operands, found " +
                                                 left->display() + " and " + right->display());
                return types_.bool_type();
            }
            if (left != right) {
                report_mismatch(expr.right->loc, right, left,
                                std::string("in a '") + ast::binary_op_spelling(expr.op) +
                                    "' comparison");
            }
            return types_.bool_type();
        }

        default:
            break;
    }

    // Arithmetic and bitwise: both operands share one type, and the context's
    // expected type propagates into them so literals take the right width.
    bool bitwise = expr.op == ast::BinaryOp::BitAnd || expr.op == ast::BinaryOp::BitOr ||
                   expr.op == ast::BinaryOp::BitXor;
    const Type* propagate = expected && expected->is_numeric() ? expected : nullptr;

    const Type* left;
    const Type* right;
    if (is_numeric_literal(expr.left) && !is_numeric_literal(expr.right)) {
        right = check_expr(expr.right, propagate);
        left = check_expr(expr.left, right);
    } else {
        left = check_expr(expr.left, propagate);
        right = check_expr(expr.right, left ? left : propagate);
    }

    if (!left || !right || left->is_error() || right->is_error()) return types_.error();

    if (bitwise || expr.op == ast::BinaryOp::Rem) {
        if (!left->is_integer() || !right->is_integer()) {
            diagnostics_.error(expr.loc, std::string("'") + ast::binary_op_spelling(expr.op) +
                                             "' expects integer operands, found " +
                                             left->display() + " and " + right->display());
            return types_.error();
        }
    } else if (!left->is_numeric() || !right->is_numeric()) {
        diagnostics_.error(expr.loc, std::string("'") + ast::binary_op_spelling(expr.op) +
                                         "' expects numeric operands, found " + left->display() +
                                         " and " + right->display());
        return types_.error();
    }

    if (left != right) {
        report_mismatch(expr.right->loc, right, left,
                        std::string("in a '") + ast::binary_op_spelling(expr.op) +
                            "' expression");
        return types_.error();
    }
    return left;
}

const Type* Checker::check_assign(ast::AssignExpr& expr) {
    const Type* target = nullptr;
    if (auto* member = expr.target->as<ast::MemberExpr>()) {
        target = info_for(*expr.target).type = check_member(*member, true);
    } else {
        target = check_expr(expr.target);
    }

    ExprInfo* target_info = expr.target->info;
    if (target_info && target_info->var && target_info->var->is_strategy_field) {
        diagnostics_
            .error(expr.loc, "the memory strategy is fixed at materialization and cannot be "
                             "reassigned")
            .note(target_info->var->loc,
                  "'" + std::string(target_info->var->name) +
                      "' is read once, when the object is allocated");
        check_expr(expr.value, target);
        return target ? target : types_.error();
    }

    if (target && !target->is_error() && (!target_info || !target_info->is_lvalue)) {
        diagnostics_.error(expr.target->loc, "this expression cannot be assigned to");
        check_expr(expr.value);
        return types_.error();
    }

    const Type* value = check_expr(expr.value, target);
    if (!assignable(value, target)) {
        report_mismatch(expr.value->loc, value, target, "in an assignment");
        return types_.error();
    }

    note_assignment_target(*expr.target, expr.value, expr.loc);
    return target;
}

// Managed slot <- manual object is the write barrier that pins (Phase 5). The
// checker records it so an obviously wrong `free` can be caught here rather
// than at runtime.
void Checker::note_assignment_target(ast::Expr& target, ast::Expr* value,
                                     diag::SourceLocation loc) {
    ExprInfo* target_info = target.info;
    if (!target_info || !target_info->var || !target_info->var->is_field()) return;

    ClassSymbol* owner = target_info->var->owner;
    if (!owner || owner->strategy != Strategy::Gc) return;

    std::string slot = describe_slot(&target);
    if (target_info->var->ref_kind != RefKind::ManualRef) {
        pins_.release_slot(slot);
        return;
    }

    if (value && value->kind == ast::ExprKind::NullLiteral) {
        pins_.release_slot(slot);
        return;
    }

    VarSymbol* held = value && value->info ? value->info->var : nullptr;
    if (held) {
        pins_.pin(held, slot, loc);
    } else {
        pins_.release_slot(slot);
    }
}

}  // namespace khu::sema
