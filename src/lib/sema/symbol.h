// Symbols and scopes.
//
// One symbol per declared entity, arena-allocated alongside the AST. Symbols
// carry everything later phases need: field slots for the object layout,
// vtable slots for dispatch, and the class's resolved memory strategy.
#ifndef KHU_SEMA_SYMBOL_H
#define KHU_SEMA_SYMBOL_H

#include <cstdint>
#include <string_view>

#include "diag/source_location.h"
#include "parser/ast.h"
#include "sema/type.h"
#include "util/array.h"
#include "util/hashmap.h"

namespace khu::sema {

struct ClassSymbol;

// A class's resolved memory strategy. Unlike ast::Strategy there is no
// "class default" here -- by this point it has been resolved.
enum class Strategy : std::uint8_t {
    Gc,
    Manual,
};

const char* strategy_name(Strategy strategy);

// What a VarSymbol names.
enum class VarRole : std::uint8_t {
    Field,
    Parameter,
    Local,
};

struct VarSymbol {
    std::string_view name;
    const Type* type = nullptr;
    VarRole role = VarRole::Local;
    ast::Visibility visibility = ast::Visibility::Private;
    bool explicit_visibility = false;
    diag::SourceLocation loc;

    // Fields
    ClassSymbol* owner = nullptr;
    std::uint32_t slot = 0;    // index within the class layout
    std::uint32_t offset = 0;  // byte offset within the object (Phase 4)
    RefKind ref_kind = RefKind::Raw;
    bool is_strategy_field = false;  // the MemoryAllocationTypeObject field
    const ast::FieldDecl* field_decl = nullptr;

    // Parameters and locals
    std::uint32_t frame_index = 0;
    bool has_initializer = false;
    // Set when the declaration's initializer was an allocation site with an
    // explicit `manual` / `standard` token: that override beats the class
    // default, so `free` has to judge by it.
    bool has_site_strategy = false;
    Strategy site_strategy = Strategy::Gc;

    bool is_field() const { return role == VarRole::Field; }
};

struct MethodSymbol {
    std::string_view name;
    ast::MethodForm form = ast::MethodForm::Func;
    ast::Visibility visibility = ast::Visibility::Public;
    diag::SourceLocation loc;

    const Type* return_type = nullptr;  // never null; void for no arrow
    util::Array<VarSymbol*> params;
    ClassSymbol* owner = nullptr;
    const ast::MethodDecl* decl = nullptr;

    std::uint32_t vtable_slot = 0;
    std::uint32_t method_id = 0;
    // Frame slots needed at run time: parameters plus every local. Filled by
    // the checker as it declares them.
    std::uint32_t frame_size = 0;
    // Builtins have no body; the VM binds them by native id (Phase 7).
    bool is_native = false;
    std::uint32_t native_id = 0;
    // Lowered directly by codegen rather than called (khuStdMath.convertTo).
    bool is_intrinsic = false;
    std::uint32_t intrinsic_id = 0;
    // Namespace members are called as `Namespace.member(...)` without a
    // receiver object.
    bool is_static = false;

    bool is_constructor() const { return form == ast::MethodForm::Constructor; }
    bool signature_matches(const MethodSymbol& other) const;
};

// The Procedures block: at most one per class, never invocable by name.
struct ProcedureSymbol {
    ast::Visibility visibility = ast::Visibility::Private;
    diag::SourceLocation loc;
    bool has_param_list = false;
    util::Array<VarSymbol*> params;
    ClassSymbol* owner = nullptr;
    const ast::ProceduresDecl* decl = nullptr;
    std::uint32_t method_id = 0;
    std::uint32_t frame_size = 0;
};

struct ClassSymbol {
    std::string_view name;
    diag::SourceLocation loc;
    ast::Visibility visibility = ast::Visibility::Public;
    const ast::ClassDecl* decl = nullptr;

    std::string_view base_name;
    ClassSymbol* base = nullptr;
    std::uint32_t class_id = 0;

    // Declared in this class, in source order.
    util::Array<VarSymbol*> fields;
    util::Array<MethodSymbol*> methods;
    MethodSymbol* constructor = nullptr;
    ProcedureSymbol* procedures = nullptr;
    VarSymbol* strategy_field = nullptr;
    Strategy strategy = Strategy::Gc;

    // Full layout: base class fields first, then this class's own. Filled once
    // inheritance is resolved; index into it is VarSymbol::slot.
    util::Array<VarSymbol*> layout;
    // Full dispatch table: a copy of the base's, with overrides replaced.
    util::Array<MethodSymbol*> vtable;
    std::uint32_t object_size = 0;

    // Builtin namespaces (khu, khuStdMath, io) are classes that are never
    // materialized; their members are static.
    bool is_namespace = false;

    // Lookup that walks the inheritance chain.
    VarSymbol* find_field(std::string_view field_name);
    const VarSymbol* find_field(std::string_view field_name) const;
    // Collects every overload of `method_name` visible on this class.
    void find_methods(std::string_view method_name, util::Array<MethodSymbol*>& out);

    bool derives_from(const ClassSymbol* other) const;

    util::StringMap<VarSymbol*> field_index;
    util::StringMap<util::Array<MethodSymbol*>> method_index;
};

// Everything the later phases need from the front end.
struct Program {
    // User classes in class_id order, followed by nothing else; builtin
    // namespaces live in `namespaces`.
    util::Array<ClassSymbol*> classes;
    util::Array<ClassSymbol*> namespaces;
    util::StringMap<ClassSymbol*> class_index;

    // Entry dispatch (KHU-PLAN.md, Entry point): `main` when one exists,
    // otherwise the first top-level class is materialized and its Procedures
    // block fires.
    MethodSymbol* main_function = nullptr;
    ClassSymbol* root_class = nullptr;

    ClassSymbol* find_class(std::string_view name) {
        ClassSymbol** found = class_index.find(name);
        return found ? *found : nullptr;
    }
};

// A lexical scope for parameters and locals.
class Scope {
public:
    explicit Scope(Scope* parent = nullptr) : parent_(parent) {}

    // Returns nullptr when the name is already declared in *this* scope.
    VarSymbol* declare(VarSymbol* symbol);
    // Walks outward through enclosing scopes.
    VarSymbol* lookup(std::string_view name);
    // Only this scope.
    VarSymbol* lookup_local(std::string_view name);

    Scope* parent() const { return parent_; }

private:
    Scope* parent_;
    util::StringMap<VarSymbol*> names_;
};

}  // namespace khu::sema

#endif  // KHU_SEMA_SYMBOL_H
