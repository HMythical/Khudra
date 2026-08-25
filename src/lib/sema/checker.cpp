#include "sema/checker.h"

#include <string>

#include "sema/builtins.h"

namespace khu::sema {
namespace {

const char* member_kind_name(ast::MethodForm form) {
    switch (form) {
        case ast::MethodForm::Func: return "function";
        case ast::MethodForm::Method: return "method";
        case ast::MethodForm::Constructor: return "constructor";
    }
    return "member";
}

}  // namespace

Checker::Checker(TypeContext& types, diag::DiagnosticEngine& diagnostics, util::Arena& arena)
    : types_(types),
      diagnostics_(diagnostics),
      arena_(arena),
      access_(diagnostics),
      memory_(types, diagnostics) {}

ExprInfo& Checker::info_for(ast::Expr& expr) {
    if (!expr.info) expr.info = arena_.create<ExprInfo>();
    return *expr.info;
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

Program* Checker::check(ast::CompilationUnit& unit) {
    util::Array<ast::CompilationUnit*> units;
    units.push(&unit);
    return check(std::move(units), unit);
}

Program* Checker::check(util::Array<ast::CompilationUnit*> units,
                        ast::CompilationUnit& entry_unit) {
    program_ = arena_.create<Program>();

    // Declare every class and namespace across every unit first, so the
    // standard library and the user's program can refer to each other's names
    // regardless of the order they were loaded in.
    for (ast::CompilationUnit* unit : units) declare_classes(*unit);
    resolve_inheritance();
    declare_members();
    resolve_strategies();
    build_layouts();
    validate_materialization_signatures();
    resolve_entry_point(entry_unit);
    check_bodies();
    return program_;
}

void Checker::declare_classes(ast::CompilationUnit& unit) {
    for (ast::ClassDecl* decl : unit.classes) {
        if (ClassSymbol* existing = program_->find_class(decl->name)) {
            if (existing->is_namespace) {
                diagnostics_.error(decl->name_loc, "'" + std::string(decl->name) +
                                                       "' is a built-in namespace and cannot be "
                                                       "declared as a class");
            } else {
                diagnostics_
                    .error(decl->name_loc, "class '" + std::string(decl->name) +
                                               "' is already declared")
                    .note(existing->loc, "the first declaration is here");
            }
            continue;
        }

        auto* symbol = arena_.create<ClassSymbol>();
        symbol->name = decl->name;
        symbol->loc = decl->name_loc;
        symbol->visibility = decl->visibility;
        symbol->decl = decl;
        symbol->base_name = decl->base_name;
        symbol->is_namespace = decl->is_namespace;
        decl->symbol = symbol;

        // A namespace is never materialized, so it gets no class id and never
        // reaches the layout, vtable or codegen passes.
        if (symbol->is_namespace) {
            program_->namespaces.push(symbol);
        } else {
            symbol->class_id = static_cast<std::uint32_t>(program_->classes.size());
            program_->classes.push(symbol);
        }
        program_->class_index.insert(symbol->name, symbol);
    }
}

void Checker::resolve_inheritance() {
    for (ClassSymbol* symbol : program_->classes) {
        if (symbol->base_name.empty()) continue;
        ClassSymbol* base = program_->find_class(symbol->base_name);
        if (!base) {
            diagnostics_.error(symbol->decl->base_loc,
                               "unknown base class '" + std::string(symbol->base_name) + "'");
            continue;
        }
        if (base->is_namespace) {
            diagnostics_.error(symbol->decl->base_loc,
                               "'" + std::string(base->name) + "' is a namespace, not a class");
            continue;
        }
        if (base == symbol) {
            diagnostics_.error(symbol->decl->base_loc,
                               "class '" + std::string(symbol->name) + "' cannot extend itself");
            continue;
        }
        symbol->base = base;
    }

    // Break inheritance cycles: without this, every later walk of the chain
    // would spin forever.
    for (ClassSymbol* symbol : program_->classes) {
        ClassSymbol* slow = symbol;
        ClassSymbol* fast = symbol;
        while (fast && fast->base) {
            slow = slow->base;
            fast = fast->base->base;
            if (slow && slow == fast) {
                diagnostics_.error(symbol->decl->base_loc,
                                   "inheritance cycle involving class '" +
                                       std::string(symbol->name) + "'");
                symbol->base = nullptr;
                break;
            }
        }
    }
}

void Checker::declare_members() {
    for (ClassSymbol* symbol : program_->classes) declare_class_members(*symbol);
    for (ClassSymbol* symbol : program_->namespaces) declare_namespace_members(*symbol);
}

// A namespace holds nothing but `native` declarations: it is a place for the
// toolchain's own operations to live with real Khudra signatures, so overload
// resolution and error messages work the same way they do for user code.
void Checker::declare_namespace_members(ClassSymbol& symbol) {
    if (!symbol.decl) return;

    for (ast::Decl* member : symbol.decl->members) {
        if (member->kind != ast::DeclKind::Method) {
            diagnostics_
                .error(member->loc, "a namespace can only declare native functions")
                .note(symbol.loc, "'" + std::string(symbol.name) +
                                      "' is a namespace, so it has no instances to hold fields, "
                                      "a constructor or a Procedures block");
            continue;
        }

        auto& decl = *static_cast<ast::MethodDecl*>(member);
        if (decl.is_constructor()) {
            diagnostics_.error(decl.name_loc, "a namespace cannot declare a constructor");
            continue;
        }
        if (!decl.is_native) {
            diagnostics_
                .error(decl.name_loc, "'" + std::string(decl.name) +
                                          "' must be declared native")
                .note(decl.name_loc,
                      "a namespace member has no receiver, so its implementation comes from the "
                      "toolchain");
            continue;
        }

        auto* method = arena_.create<MethodSymbol>();
        method->name = decl.name;
        method->form = decl.form;
        method->visibility = decl.visibility;
        method->loc = decl.name_loc;
        method->owner = &symbol;
        method->decl = &decl;
        method->is_static = true;
        method->return_type =
            decl.return_type ? resolve_type(decl.return_type) : types_.void_type();
        declare_params(decl.params, method->params);
        decl.symbol = method;

        NativeBinding binding =
            resolve_native_binding(symbol.name, decl.name, method->params.size());
        if (!binding.valid()) {
            diagnostics_
                .error(decl.name_loc, "no runtime binding for '" + std::string(symbol.name) +
                                          "." + std::string(decl.name) + "' with " +
                                          std::to_string(method->params.size()) + " parameter" +
                                          (method->params.size() == 1 ? "" : "s"))
                .note(decl.name_loc,
                      "native declarations are bound in src/lib/sema/builtins.cpp");
            continue;
        }
        if (binding.intrinsic != Intrinsic::None) {
            method->is_intrinsic = true;
            method->intrinsic_id = static_cast<std::uint32_t>(binding.intrinsic);
        } else {
            method->is_native = true;
            method->native_id = static_cast<std::uint32_t>(binding.native);
        }

        symbol.methods.push(method);
        if (util::Array<MethodSymbol*>* bucket = symbol.method_index.find(decl.name)) {
            for (MethodSymbol* existing : *bucket) {
                if (existing->signature_matches(*method)) {
                    diagnostics_
                        .error(decl.name_loc, "'" + std::string(symbol.name) + "." +
                                                  std::string(decl.name) +
                                                  "' is already declared with the same "
                                                  "parameter types")
                        .note(existing->loc, "the first declaration is here");
                }
            }
            bucket->push(method);
        } else {
            util::Array<MethodSymbol*> fresh;
            fresh.push(method);
            symbol.method_index.insert(decl.name, std::move(fresh));
        }
    }
}

void Checker::declare_params(const util::Array<ast::ParamDecl*>& decls,
                             util::Array<VarSymbol*>& out) {
    for (ast::ParamDecl* decl : decls) {
        auto* param = arena_.create<VarSymbol>();
        param->name = decl->name;
        param->type = resolve_type(decl->type);
        param->role = VarRole::Parameter;
        param->loc = decl->loc;
        decl->symbol = param;
        out.push(param);
    }
}

void Checker::declare_class_members(ClassSymbol& symbol) {
    if (!symbol.decl) return;

    for (ast::Decl* member : symbol.decl->members) {
        switch (member->kind) {
            case ast::DeclKind::Field: {
                auto& decl = *static_cast<ast::FieldDecl*>(member);
                if (VarSymbol** existing = symbol.field_index.find(decl.name)) {
                    diagnostics_
                        .error(decl.name_loc, "field '" + std::string(decl.name) +
                                                  "' is already declared in class '" +
                                                  std::string(symbol.name) + "'")
                        .note((*existing)->loc, "the first declaration is here");
                    continue;
                }
                auto* field = arena_.create<VarSymbol>();
                field->name = decl.name;
                field->type = resolve_type(decl.type);
                field->role = VarRole::Field;
                field->visibility = decl.visibility;
                field->explicit_visibility = decl.explicit_visibility;
                field->loc = decl.name_loc;
                field->owner = &symbol;
                field->field_decl = &decl;
                field->has_initializer = decl.init != nullptr;
                decl.symbol = field;
                symbol.fields.push(field);
                symbol.field_index.insert(field->name, field);
                break;
            }

            case ast::DeclKind::Method: {
                auto& decl = *static_cast<ast::MethodDecl*>(member);
                if (decl.is_native) {
                    diagnostics_
                        .error(decl.name_loc,
                               "only a namespace member can be declared native")
                        .note(symbol.loc, "'" + std::string(symbol.name) +
                                              "' is a class, and its instances need a body to "
                                              "dispatch to");
                    break;
                }
                auto* method = arena_.create<MethodSymbol>();
                method->name = decl.name;
                method->form = decl.form;
                method->visibility = decl.visibility;
                method->loc = decl.name_loc;
                method->owner = &symbol;
                method->decl = &decl;
                method->return_type =
                    decl.return_type ? resolve_type(decl.return_type) : types_.void_type();
                declare_params(decl.params, method->params);
                decl.symbol = method;

                if (decl.is_constructor()) {
                    if (symbol.constructor) {
                        diagnostics_
                            .error(decl.name_loc, "class '" + std::string(symbol.name) +
                                                      "' already declares a constructor")
                            .note(symbol.constructor->loc, "the first one is here");
                        break;
                    }
                    symbol.constructor = method;
                    break;
                }

                if (util::Array<MethodSymbol*>* bucket = symbol.method_index.find(decl.name)) {
                    for (MethodSymbol* existing : *bucket) {
                        if (existing->signature_matches(*method)) {
                            diagnostics_
                                .error(decl.name_loc,
                                       std::string(member_kind_name(decl.form)) + " '" +
                                           std::string(decl.name) +
                                           "' is already declared with the same parameter types")
                                .note(existing->loc, "the first declaration is here");
                        }
                    }
                    bucket->push(method);
                } else {
                    util::Array<MethodSymbol*> fresh;
                    fresh.push(method);
                    symbol.method_index.insert(decl.name, std::move(fresh));
                }
                symbol.methods.push(method);
                break;
            }

            case ast::DeclKind::Procedures: {
                auto& decl = *static_cast<ast::ProceduresDecl*>(member);
                if (symbol.procedures) {
                    diagnostics_
                        .error(decl.loc, "class '" + std::string(symbol.name) +
                                             "' already declares a Procedures block")
                        .note(symbol.procedures->loc, "the first one is here")
                        .note(decl.loc, "a class materializes once, so it runs one block");
                    break;
                }
                auto* procedures = arena_.create<ProcedureSymbol>();
                procedures->visibility = decl.visibility;
                procedures->loc = decl.loc;
                procedures->has_param_list = decl.has_param_list;
                procedures->owner = &symbol;
                procedures->decl = &decl;
                declare_params(decl.params, procedures->params);
                decl.symbol = procedures;
                symbol.procedures = procedures;
                break;
            }

            default:
                break;
        }
    }

    // A field must not shadow one inherited from a base class: the layout puts
    // base fields first, and two slots with one name would make `this.x`
    // ambiguous.
    for (VarSymbol* field : symbol.fields) {
        for (ClassSymbol* base = symbol.base; base; base = base->base) {
            if (VarSymbol** inherited = base->field_index.find(field->name)) {
                diagnostics_
                    .error(field->loc, "field '" + std::string(field->name) +
                                           "' shadows a field inherited from '" +
                                           std::string(base->name) + "'")
                    .note((*inherited)->loc, "the inherited field is declared here");
                break;
            }
        }
    }
}

void Checker::resolve_strategies() {
    for (ClassSymbol* symbol : program_->classes) memory_.resolve_class_strategy(*symbol);
}

void Checker::ensure_layout(ClassSymbol& symbol) {
    if (!symbol.layout.empty() || symbol.object_size != 0) return;
    if (symbol.base) ensure_layout(*symbol.base);

    std::uint32_t offset = 0;
    if (symbol.base) {
        for (VarSymbol* inherited : symbol.base->layout) {
            symbol.layout.push(inherited);
        }
        offset = symbol.base->object_size;
    }
    for (VarSymbol* field : symbol.fields) {
        field->slot = static_cast<std::uint32_t>(symbol.layout.size());
        field->offset = offset;
        offset += field->type ? field->type->slot_size() : 0;
        symbol.layout.push(field);
    }
    symbol.object_size = offset;

    // Child vtable copies the parent's, then overrides replace entries.
    if (symbol.base) {
        for (MethodSymbol* inherited : symbol.base->vtable) symbol.vtable.push(inherited);
    }
    for (MethodSymbol* method : symbol.methods) {
        bool overrode = false;
        for (std::size_t i = 0; i < symbol.vtable.size(); ++i) {
            if (!symbol.vtable[i]->signature_matches(*method)) continue;
            if (symbol.vtable[i]->return_type != method->return_type) {
                diagnostics_
                    .error(method->loc, "override of '" + std::string(method->name) +
                                            "' changes the return type")
                    .note(symbol.vtable[i]->loc, "the base declaration is here");
            }
            method->vtable_slot = static_cast<std::uint32_t>(i);
            symbol.vtable[i] = method;
            overrode = true;
            break;
        }
        if (!overrode) {
            method->vtable_slot = static_cast<std::uint32_t>(symbol.vtable.size());
            symbol.vtable.push(method);
        }
    }

    memory_.build_reference_map(symbol);
}

void Checker::build_layouts() {
    for (ClassSymbol* symbol : program_->classes) ensure_layout(*symbol);

    std::uint32_t next_method_id = 0;
    for (ClassSymbol* symbol : program_->classes) {
        for (MethodSymbol* method : symbol->methods) method->method_id = next_method_id++;
        if (symbol->constructor) symbol->constructor->method_id = next_method_id++;
    }
}

namespace {

// The parameter list a class itself declares for materialization: its
// Procedures block's when it has any, otherwise its constructor's.
const util::Array<VarSymbol*>* declared_materialization_params(const ClassSymbol& symbol) {
    if (symbol.procedures && !symbol.procedures->params.empty()) return &symbol.procedures->params;
    if (symbol.constructor && !symbol.constructor->params.empty()) {
        return &symbol.constructor->params;
    }
    return nullptr;
}

bool same_parameter_types(const util::Array<VarSymbol*>& left,
                          const util::Array<VarSymbol*>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i]->type != right[i]->type) return false;
    }
    return true;
}

diag::SourceLocation materialization_blame(const ClassSymbol& symbol) {
    if (symbol.procedures && !symbol.procedures->params.empty()) return symbol.procedures->loc;
    if (symbol.constructor) return symbol.constructor->loc;
    return symbol.loc;
}

}  // namespace

// One allocation site supplies the arguments for the whole materialization:
// every field initializer, Procedures block and constructor in the inheritance
// chain. So every parameter list involved has to be the same list.
void Checker::validate_materialization_signatures() {
    for (ClassSymbol* symbol : program_->classes) {
        // Within one class: the Procedures block and the constructor.
        ProcedureSymbol* procedures = symbol->procedures;
        MethodSymbol* constructor = symbol->constructor;
        if (procedures && constructor && !procedures->params.empty() &&
            !constructor->params.empty() &&
            !same_parameter_types(procedures->params, constructor->params)) {
            diagnostics_
                .error(procedures->loc,
                       "the Procedures block and the constructor of '" +
                           std::string(symbol->name) +
                           "' declare different parameters, but both are bound to the same "
                           "allocation-site arguments")
                .note(constructor->loc, "the constructor is declared here");
        }

        // Across the chain: this class against the nearest ancestor that
        // declares parameters.
        const util::Array<VarSymbol*>* own = declared_materialization_params(*symbol);
        if (!own) continue;
        for (ClassSymbol* ancestor = symbol->base; ancestor; ancestor = ancestor->base) {
            const util::Array<VarSymbol*>* inherited = declared_materialization_params(*ancestor);
            if (!inherited) continue;
            if (!same_parameter_types(*own, *inherited)) {
                diagnostics_
                    .error(materialization_blame(*symbol),
                           "class '" + std::string(symbol->name) +
                               "' and its base class '" + std::string(ancestor->name) +
                               "' declare different materialization parameters")
                    .note(materialization_blame(*ancestor),
                          "the base class declares them here")
                    .note(symbol->loc,
                          "one allocation site supplies the arguments for the whole chain, so "
                          "every class in it must take the same ones");
            }
            break;
        }
    }
}

void Checker::resolve_entry_point(ast::CompilationUnit& unit) {
    for (ClassSymbol* symbol : program_->classes) {
        util::Array<MethodSymbol*> candidates;
        if (util::Array<MethodSymbol*>* bucket = symbol->method_index.find("main")) {
            for (MethodSymbol* method : *bucket) candidates.push(method);
        }
        for (MethodSymbol* method : candidates) {
            if (!method->params.empty()) continue;
            if (program_->main_function) {
                diagnostics_
                    .error(method->loc, "more than one 'main' entry point")
                    .note(program_->main_function->loc, "the first one is here");
                continue;
            }
            program_->main_function = method;
        }
    }

    // `main` is an ordinary member, so running it means materializing the class
    // that declares it -- which only works when materialization needs no
    // arguments.
    if (program_->main_function && program_->main_function->owner) {
        ClassSymbol& owner = *program_->main_function->owner;
        std::size_t required = 0;
        diag::SourceLocation blame = owner.loc;
        if (owner.procedures && !owner.procedures->params.empty()) {
            required = owner.procedures->params.size();
            blame = owner.procedures->loc;
        } else if (owner.constructor && !owner.constructor->params.empty()) {
            required = owner.constructor->params.size();
            blame = owner.constructor->loc;
        }
        if (required != 0) {
            diagnostics_
                .error(program_->main_function->loc,
                       "class '" + std::string(owner.name) +
                           "' declares 'main' but needs " + std::to_string(required) +
                           " allocation argument" + (required == 1 ? "" : "s") +
                           ", so it cannot be materialized as the entry point")
                .note(blame, "the allocation-site arguments are bound here")
                .note(program_->main_function->loc,
                      "move 'main' to a class that materializes with no arguments");
        }
    }

    // Without a main, materializing the first top-level class fires its
    // Procedures block -- the second supported entry form.
    if (!unit.classes.empty() && unit.classes[0]->symbol) {
        program_->root_class = unit.classes[0]->symbol;
    }
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

const Type* Checker::resolve_type(const ast::TypeNode* node) {
    if (!node) return types_.error();

    switch (node->kind) {
        case ast::TypeNode::Kind::Pointer:
            return types_.pointer_to(resolve_type(node->pointee));

        case ast::TypeNode::Kind::Builtin:
            switch (node->builtin) {
                case ast::BuiltinType::Int8: return types_.signed_int(8);
                case ast::BuiltinType::Int16: return types_.signed_int(16);
                case ast::BuiltinType::Int32: return types_.signed_int(32);
                case ast::BuiltinType::Int64: return types_.signed_int(64);
                case ast::BuiltinType::UInt8: return types_.unsigned_int(8);
                case ast::BuiltinType::UInt16: return types_.unsigned_int(16);
                case ast::BuiltinType::UInt32: return types_.unsigned_int(32);
                case ast::BuiltinType::UInt64: return types_.unsigned_int(64);
                case ast::BuiltinType::Float32: return types_.float_type(32);
                case ast::BuiltinType::Float64: return types_.float_type(64);
                case ast::BuiltinType::Bool: return types_.bool_type();
                case ast::BuiltinType::String: return types_.string_type();
                case ast::BuiltinType::Array: return types_.array_type();
                case ast::BuiltinType::MemoryAllocationTypeObject: return types_.memory_type();
                case ast::BuiltinType::Void: return types_.void_type();
            }
            return types_.error();

        case ast::TypeNode::Kind::Named: {
            ClassSymbol* symbol = program_->find_class(node->name);
            if (!symbol) {
                diagnostics_.error(node->loc, "unknown type '" + std::string(node->name) + "'");
                return types_.error();
            }
            if (symbol->is_namespace) {
                diagnostics_.error(node->loc, "'" + std::string(node->name) +
                                                  "' is a namespace, not a type");
                return types_.error();
            }
            return types_.class_type(symbol);
        }
    }
    return types_.error();
}

bool Checker::assignable(const Type* from, const Type* to) const {
    if (!from || !to) return true;  // an error type suppresses cascades
    if (from->is_error() || to->is_error()) return true;
    if (from == to) return true;
    // `null` means "not instantiated yet" and fits any reference slot.
    if (from->is_null() && to->is_reference()) return true;
    // A derived class reference fits a base slot.
    if (from->is_class() && to->is_class() && from->class_symbol && to->class_symbol) {
        return from->class_symbol->derives_from(to->class_symbol);
    }
    return false;
}

void Checker::report_mismatch(diag::SourceLocation loc, const Type* from, const Type* to,
                              std::string_view context) {
    auto builder = diagnostics_.error(loc, "cannot convert " + from->display() + " to " +
                                               to->display() + " " + std::string(context));
    if (from->is_numeric() && to->is_numeric()) {
        // The only sanctioned way across widths.
        builder.note(loc, "Khudra has no implicit widening or narrowing; use "
                          "khuStdMath.convertTo(" +
                              to->display() + ", <expr>)");
    }
}

// ---------------------------------------------------------------------------
// Scopes
// ---------------------------------------------------------------------------

void Checker::push_scope() {
    Scope* parent = scopes_.empty() ? nullptr : scopes_.back();
    scopes_.push(arena_.create<Scope>(parent));
}

void Checker::pop_scope() { scopes_.pop(); }

VarSymbol* Checker::declare_variable(VarSymbol* symbol, diag::SourceLocation loc) {
    if (scopes_.empty()) return symbol;
    if (VarSymbol* existing = scopes_.back()->lookup_local(symbol->name)) {
        diagnostics_
            .error(loc, "'" + std::string(symbol->name) + "' is already declared in this scope")
            .note(existing->loc, "the first declaration is here");
        return existing;
    }
    symbol->frame_index = next_frame_index_++;
    scopes_.back()->declare(symbol);
    return symbol;
}

}  // namespace khu::sema
