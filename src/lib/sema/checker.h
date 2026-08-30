// Semantic analysis.
//
// One driver runs the passes the plan calls for: name resolution and the class
// hierarchy, the width-aware type checker, the access-control pass, the
// memory-model checker and Procedures validation. They share a walk because
// they ask overlapping questions about the same expression -- what does this
// name resolve to, whose object is it, and which heap does it live on.
#ifndef KHU_SEMA_CHECKER_H
#define KHU_SEMA_CHECKER_H

#include <cstdint>
#include <string_view>

#include "diag/diagnostic.h"
#include "parser/ast.h"
#include "bytecode/native.h"
#include "sema/access.h"
#include "sema/memory_model.h"
#include "sema/symbol.h"
#include "sema/type.h"
#include "util/arena.h"
#include "util/array.h"

namespace khu::sema {

// What Sema learned about one expression. Codegen reads this back.
struct ExprInfo {
    const Type* type = nullptr;
    VarSymbol* var = nullptr;             // resolved local, parameter or field
    MethodSymbol* method = nullptr;       // resolved call target
    ClassSymbol* alloc_class = nullptr;   // allocation site target
    ClassSymbol* namespace_ref = nullptr; // `khuStdMath` in `khuStdMath.add(..)`
    Strategy alloc_strategy = Strategy::Gc;
    const Type* convert_target = nullptr; // khuStdMath.convertTo(<type>, ..)
    // The element type of khuStdCollection.arrayCreate(<type>, n), and of an
    // index expression's array. Codegen needs the first to know what to fill a
    // fresh array with; the second is what an element read produces.
    const Type* array_element = nullptr;
    bool is_allocation = false;
    bool through_this = false;            // reached via the current object
    bool is_lvalue = false;
    bool is_strategy_factory = false;     // MemoryAllocationTypeObject.setX()
};

class Checker {
public:
    Checker(TypeContext& types, diag::DiagnosticEngine& diagnostics, util::Arena& arena);

    // Runs every pass over all `units`. `entry_unit` is the user's program --
    // the standard library units around it never supply an entry point.
    // Always returns a Program; inspect diagnostics for errors.
    Program* check(util::Array<ast::CompilationUnit*> units, ast::CompilationUnit& entry_unit);
    // Single-unit convenience used by the tests.
    Program* check(ast::CompilationUnit& unit);

private:
    // --- passes ---
    void declare_classes(ast::CompilationUnit& unit);
    // Resolves a class name from the side the code being checked is on.
    ClassSymbol* lookup_class(std::string_view name);
    void declare_namespace_members(ClassSymbol& symbol);
    void resolve_inheritance();
    void declare_members();
    void resolve_strategies();
    void build_layouts();
    void ensure_layout(ClassSymbol& symbol);
    void validate_materialization_signatures();
    void check_bodies();
    void resolve_entry_point(ast::CompilationUnit& unit);

    // --- declarations ---
    void declare_class_members(ClassSymbol& symbol);
    void declare_params(const util::Array<ast::ParamDecl*>& decls,
                        util::Array<VarSymbol*>& out);
    void check_class_bodies(ClassSymbol& symbol);
    void check_method_body(MethodSymbol& method);
    void check_procedures_body(ProcedureSymbol& procedures);

    // --- types ---
    const Type* resolve_type(const ast::TypeNode* node);
    // Generic substitution: replaces a class's type parameters with the
    // arguments a use site wrote.
    const Type* substitute(const Type* type, const ClassSymbol* owner,
                           const util::Array<const Type*>& arguments);
    const Type* substitute_through(const Type* type, const Type* receiver);

    // The synthesized symbols for the erased-argument natives, created once.
    util::StringMap<MethodSymbol*> erased_natives_;

    bool assignable(const Type* from, const Type* to) const;
    // Reports "cannot convert X to Y", naming convertTo when the two are
    // numeric -- the only sanctioned way across widths.
    void report_mismatch(diag::SourceLocation loc, const Type* from, const Type* to,
                         std::string_view context);

    // --- statements ---
    void check_stmt(ast::Stmt* statement);
    void check_block(ast::BlockStmt* block, bool open_scope);
    void check_local(ast::VarDeclStmt& decl);
    void check_free(ast::FreeStmt& statement);

    // --- expressions ---
    const Type* check_expr(ast::Expr* expr, const Type* expected = nullptr);
    const Type* check_identifier(ast::IdentifierExpr& expr);
    const Type* check_member(ast::MemberExpr& expr, bool writing);
    const Type* check_call(ast::CallExpr& expr, const Type* expected);
    const Type* check_allocation(ast::CallExpr& expr, ClassSymbol& target,
                                 const Type* instantiated = nullptr);
    const Type* check_binary(ast::BinaryExpr& expr, const Type* expected);
    const Type* check_unary(ast::UnaryExpr& expr, const Type* expected);
    const Type* check_assign(ast::AssignExpr& expr);
    const Type* check_index(ast::IndexExpr& expr);
    const Type* check_convert_intrinsic(ast::CallExpr& expr);
    const Type* check_array_create_intrinsic(ast::CallExpr& expr);
    // khuStdCollection.hash / .sameValue: natives that take a value of *any*
    // type, which is the one shape a Khudra signature cannot express.
    const Type* check_value_intrinsic(ast::CallExpr& expr, std::string_view member_name);
    MethodSymbol* erased_native(std::string_view name, bytecode::NativeId id,
                                const Type* return_type, std::size_t arity);
    const Type* check_strategy_factory(ast::CallExpr& expr, const ast::MemberExpr& callee);
    // Non-null when `object` names a built-in namespace rather than a value.
    ClassSymbol* namespace_receiver(ast::Expr* object);
    void report_stray_strategy(const ast::CallExpr& expr);

    // Resolves an overload set against already-typed arguments.
    // `receiver` is the type the call is made through, so a generic class's
    // parameters can be substituted before the arguments are compared. Null
    // for a namespace member, a constructor or a call on `this`.
    MethodSymbol* resolve_overload(util::Array<MethodSymbol*>& candidates, ast::CallExpr& call,
                                   std::string_view display_name,
                                   const Type* expected = nullptr,
                                   const Type* receiver = nullptr);
    bool bind_arguments(const util::Array<VarSymbol*>& params, ast::CallExpr& call,
                        std::string_view what, diag::SourceLocation loc,
                        const Type* receiver = nullptr);
    void note_candidates(diag::DiagnosticEngine::Builder& builder,
                         const util::Array<MethodSymbol*>& candidates,
                         diag::SourceLocation loc);

    ExprInfo& info_for(ast::Expr& expr);
    void note_assignment_target(ast::Expr& target, ast::Expr* value, diag::SourceLocation loc);
    void collect_constructor_assignments(ClassSymbol& symbol);
    bool is_constructor_assigned(std::string_view field_name) const;

    // Scope helpers.
    void push_scope();
    void pop_scope();
    VarSymbol* declare_variable(VarSymbol* symbol, diag::SourceLocation loc);

    TypeContext& types_;
    diag::DiagnosticEngine& diagnostics_;
    util::Arena& arena_;
    AccessChecker access_;
    MemoryModel memory_;
    PinTracker pins_;

    Program* program_ = nullptr;
    ClassSymbol* current_class_ = nullptr;
    MethodSymbol* current_method_ = nullptr;
    ProcedureSymbol* current_procedure_ = nullptr;
    util::Array<Scope*> scopes_;
    // Fields the current class's constructor assigns; the Procedures block runs
    // before the constructor, so reading one of these there is suspicious.
    util::Array<std::string_view> constructor_assigned_;
    std::uint32_t next_frame_index_ = 0;
};

}  // namespace khu::sema

#endif  // KHU_SEMA_CHECKER_H
