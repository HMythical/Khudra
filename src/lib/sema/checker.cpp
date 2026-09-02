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

ClassSymbol* Checker::lookup_class(std::string_view name) {
    // The standard library resolves its own names first, so a program that
    // declares its own `List` keeps the library's `Stack` working.
    if (current_class_ && current_class_->from_stdlib) {
        return program_->find_stdlib_class(name);
    }
    return program_->find_class(name);
}

void Checker::declare_classes(ast::CompilationUnit& unit) {
    for (ast::ClassDecl* decl : unit.classes) {
        // A name collides only within its own side. A program's class may share
        // a name with one of the standard library's -- it shadows it, which is
        // what keeps a program that already had a `List` compiling.
        util::StringMap<ClassSymbol*>& index =
            unit.is_stdlib && !decl->is_namespace ? program_->stdlib_class_index
                                                  : program_->class_index;
        ClassSymbol** existing_slot = index.find(decl->name);
        ClassSymbol* existing = existing_slot ? *existing_slot : nullptr;
        if (!existing && !unit.is_stdlib) {
            // A namespace name is global whichever side declared it: there is
            // no receiver to disambiguate `io` by.
            if (ClassSymbol** space = program_->class_index.find(decl->name)) {
                if ((*space)->is_namespace) existing = *space;
            }
        }
        if (existing) {
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
        symbol->from_stdlib = unit.is_stdlib;
        decl->symbol = symbol;

        // `class List<T>`. A parameter is a type only inside this body, so it
        // is created here rather than looked up anywhere.
        for (std::size_t i = 0; i < decl->type_params.size(); ++i) {
            std::string_view name = decl->type_params[i];
            bool duplicate = false;
            for (std::string_view seen : symbol->type_params) {
                if (seen == name) duplicate = true;
            }
            if (duplicate) {
                diagnostics_.error(decl->type_param_locs[i],
                                   "type parameter '" + std::string(name) +
                                       "' is already declared");
                continue;
            }
            symbol->type_params.push(name);
            symbol->type_param_types.push(
                types_.type_param(symbol, static_cast<std::uint32_t>(symbol->type_params.size() - 1)));
        }

        // A namespace is never materialized, so it gets no class id and never
        // reaches the layout, vtable or codegen passes.
        if (symbol->is_namespace) {
            program_->namespaces.push(symbol);
        } else {
            symbol->class_id = static_cast<std::uint32_t>(program_->classes.size());
            program_->classes.push(symbol);
        }
        index.insert(symbol->name, symbol);
    }
}

void Checker::resolve_inheritance() {
    for (ClassSymbol* symbol : program_->classes) {
        if (symbol->base_name.empty()) continue;
        // Generics are erased, so a generic base would need its arguments
        // substituted through a vtable that no longer records them. Inheritance
        // and type parameters are kept apart until that is worked out
        // (docs/roadmap.md).
        if (symbol->is_generic()) {
            diagnostics_
                .error(symbol->decl->base_loc, "a class with type parameters cannot extend "
                                               "another class")
                .note(symbol->loc, "generics and inheritance are not combined yet (see "
                                   "docs/roadmap.md)");
            continue;
        }
        ClassSymbol* base = symbol->from_stdlib
                                ? program_->find_stdlib_class(symbol->base_name)
                                : program_->find_class(symbol->base_name);
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
    for (ClassSymbol* symbol : program_->classes) {
        // A type parameter is only a type inside the body that declares it, and
        // a member's declaration is inside that body -- so the class has to be
        // current while its field and signature types are resolved.
        current_class_ = symbol;
        declare_class_members(*symbol);
        current_class_ = nullptr;
    }
    // A namespace's signatures resolve in the scope of the file that declared
    // them, exactly the way a class's do. That matters for the standard
    // library's own namespaces: `khuStdSystem.argv() -> List<string>` has to
    // mean the library's `List` even in a program that declares one of its own,
    // which is the same rule that keeps the library's `Stack` working
    // (lookup_class, above).
    for (ClassSymbol* symbol : program_->namespaces) {
        current_class_ = symbol;
        declare_namespace_members(*symbol);
        current_class_ = nullptr;
    }
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
        // Entry dispatch materializes the class with nothing written at the
        // site, so there is nowhere for its type arguments to come from.
        if (owner.is_generic()) {
            diagnostics_
                .error(program_->main_function->loc,
                       "class '" + std::string(owner.name) +
                           "' declares 'main' but takes type arguments, so it cannot be the "
                           "entry point")
                .note(owner.loc, "entry dispatch materializes the class with nothing written at "
                                 "the site, so there is nowhere to write them");
        }
    }

    // Without a main, materializing the first top-level class fires its
    // Procedures block -- the second supported entry form.
    if (!unit.classes.empty() && unit.classes[0]->symbol) {
        ClassSymbol* root = unit.classes[0]->symbol;
        if (!program_->main_function && root->is_generic()) {
            diagnostics_
                .error(root->loc, "the root class cannot take type arguments")
                .note(root->loc, "without a 'main', the first class is materialized with nothing "
                                 "written at the site, so there is nowhere to write them");
        }
        program_->root_class = root;
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
                case ast::BuiltinType::Array: {
                    if (node->arguments.empty()) return types_.array_type();
                    if (node->arguments.size() != 1) {
                        diagnostics_.error(node->loc, "Array takes one type argument, not " +
                                                          std::to_string(node->arguments.size()));
                        return types_.error();
                    }
                    const Type* element = resolve_type(node->arguments[0]);
                    if (!element || element->is_error()) return types_.error();
                    if (element->is_void()) {
                        diagnostics_.error(node->arguments[0]->loc,
                                           "'void' is not an element type");
                        return types_.error();
                    }
                    return types_.array_of(element);
                }
                case ast::BuiltinType::MemoryAllocationTypeObject: return types_.memory_type();
                case ast::BuiltinType::Void: return types_.void_type();
            }
            return types_.error();

        case ast::TypeNode::Kind::Named: {
            // A type parameter shadows a class of the same name inside the body
            // that declares it, which is the only place it is visible at all.
            if (current_class_ && node->arguments.empty()) {
                for (std::size_t i = 0; i < current_class_->type_params.size(); ++i) {
                    if (current_class_->type_params[i] == node->name) {
                        return current_class_->type_param_types[i];
                    }
                }
            }

            ClassSymbol* symbol = lookup_class(node->name);
            if (!symbol) {
                diagnostics_.error(node->loc, "unknown type '" + std::string(node->name) + "'");
                return types_.error();
            }
            if (symbol->is_namespace) {
                diagnostics_.error(node->loc, "'" + std::string(node->name) +
                                                  "' is a namespace, not a type");
                return types_.error();
            }

            if (symbol->type_params.size() != node->arguments.size()) {
                if (symbol->type_params.empty()) {
                    diagnostics_.error(node->loc, "'" + std::string(node->name) +
                                                      "' takes no type arguments");
                } else {
                    diagnostics_
                        .error(node->loc, "'" + std::string(node->name) + "' takes " +
                                              std::to_string(symbol->type_params.size()) +
                                              " type argument" +
                                              (symbol->type_params.size() == 1 ? "" : "s") +
                                              ", not " + std::to_string(node->arguments.size()))
                        .note(symbol->loc, "declared here");
                }
                return types_.error();
            }
            if (symbol->type_params.empty()) return types_.class_type(symbol);

            util::Array<const Type*> arguments;
            for (ast::TypeNode* argument : node->arguments) {
                const Type* resolved = resolve_type(argument);
                if (!resolved || resolved->is_error()) return types_.error();
                if (resolved->is_void()) {
                    diagnostics_.error(argument->loc, "'void' is not a type argument");
                    return types_.error();
                }
                arguments.push(resolved);
            }
            return types_.class_type(symbol, arguments);
        }
    }
    return types_.error();
}

// Replaces `owner`'s type parameters with `arguments` throughout `type`.
//
// This is what erasure costs: the runtime is handed one class whatever the
// arguments were, so the checker has to do the whole job of remembering them.
// A field declared `T` on `List<T>` reads back as `int32` through a
// `List<int32>`, and that substitution happens here, at every use.
const Type* Checker::substitute(const Type* type, const ClassSymbol* owner,
                                const util::Array<const Type*>& arguments) {
    if (!type || arguments.empty()) return type;

    switch (type->kind) {
        case TypeKind::TypeParam:
            if (type->class_symbol != owner || type->width >= arguments.size()) return type;
            return arguments[type->width];

        case TypeKind::Array: {
            if (!type->element) return type;
            const Type* element = substitute(type->element, owner, arguments);
            return element == type->element ? type : types_.array_of(element);
        }

        case TypeKind::Pointer: {
            const Type* pointee = substitute(type->pointee, owner, arguments);
            return pointee == type->pointee ? type : types_.pointer_to(pointee);
        }

        case TypeKind::Class: {
            if (type->arguments.empty()) return type;
            util::Array<const Type*> replaced;
            bool changed = false;
            for (const Type* argument : type->arguments) {
                const Type* fresh = substitute(argument, owner, arguments);
                changed = changed || fresh != argument;
                replaced.push(fresh);
            }
            return changed ? types_.class_type(type->class_symbol, replaced) : type;
        }

        default:
            return type;
    }
}

// The same, driven by a receiver's type: `list.head` seen through a
// `List<int32>` substitutes int32 for T.
const Type* Checker::substitute_through(const Type* type, const Type* receiver) {
    if (!receiver || receiver->kind != TypeKind::Class || receiver->arguments.empty()) {
        return type;
    }
    return substitute(type, receiver->class_symbol, receiver->arguments);
}

bool Checker::assignable(const Type* from, const Type* to) const {
    if (!from || !to) return true;  // an error type suppresses cascades
    if (from->is_error() || to->is_error()) return true;
    if (from == to) return true;
    // `null` means "not instantiated yet" and fits any reference slot.
    if (from->is_null() && to->is_reference()) return true;
    // `*byte` is the universal buffer pointer, the way `void*` is in C: it
    // converts to and from any other pointer type, and no other pair of
    // pointer types converts. `khuStdMem.alloc` hands out bytes, and a program
    // that wants to read them as something wider says so by declaring the
    // pointer it stores them in. Nothing about the value changes -- a `*T` is
    // an address whatever T is -- only what the checker will let you index it
    // as.
    if (from->kind == TypeKind::Pointer && to->kind == TypeKind::Pointer) {
        const Type* byte_type = types_.unsigned_int(8);
        return from->pointee == byte_type || to->pointee == byte_type;
    }
    // A bare `Array` is the untyped view of any array: it predates generics and
    // still appears in `khu.getType()`, so it accepts and is accepted by every
    // `Array<T>`. What it does not do is let you index it -- reading an element
    // needs an element type, and that is exactly what it has not got.
    if (from->is_array() && to->is_array() && (!from->element || !to->element)) return true;
    // A derived class reference fits a base slot.
    if (from->is_class() && to->is_class() && from->class_symbol && to->class_symbol) {
        // Type arguments are invariant: a Box<int32> is not a Box<string>, and
        // neither is a subtype of the other. There is nothing subtler to do
        // here -- variance needs a place to declare it, and generics and
        // inheritance are not combined yet.
        if (from->arguments.size() != to->arguments.size()) return false;
        for (std::size_t i = 0; i < from->arguments.size(); ++i) {
            if (from->arguments[i] != to->arguments[i]) return false;
        }
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
