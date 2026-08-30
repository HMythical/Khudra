// Khudra abstract syntax tree.
//
// Nodes are arena-allocated and referenced by raw pointer: the whole tree has
// one lifetime, so ownership bookkeeping would be pure overhead. Every node
// carries a SourceLocation.
//
// Dispatch is a `kind` tag plus a switch rather than virtual methods -- the
// same tree is walked by the pretty printer, the checker and codegen, and a
// switch keeps each walk in one readable place.
#ifndef KHU_PARSER_AST_H
#define KHU_PARSER_AST_H

#include <cstdint>
#include <string_view>

#include "diag/source_location.h"
#include "util/array.h"

// Sema annotates the tree in place. Only opaque pointers appear here, so the
// AST stays independent of the checker.
namespace khu::sema {
struct ExprInfo;
struct VarSymbol;
struct MethodSymbol;
struct ProcedureSymbol;
struct ClassSymbol;
}  // namespace khu::sema

namespace khu::ast {

using diag::SourceLocation;

enum class Visibility : std::uint8_t {
    Public,
    Private,
};

const char* visibility_name(Visibility visibility);

// Memory strategy selected at an allocation site (docs/memory-model.md).
enum class Strategy : std::uint8_t {
    ClassDefault,  // no token: use the class's `type` field
    Gc,            // `standard`
    Manual,        // `manual`
};

const char* strategy_name(Strategy strategy);

// ---------------------------------------------------------------------------
// Types (syntactic)
// ---------------------------------------------------------------------------

enum class BuiltinType : std::uint8_t {
    Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64,
    Float32, Float64,
    Bool,
    String,
    Array,
    MemoryAllocationTypeObject,
    Void,
};

const char* builtin_canonical_name(BuiltinType type);

struct TypeNode {
    enum class Kind : std::uint8_t {
        Builtin,  // int32, i32, byte, bool, string, Array, ...
        Named,    // a class reference
        Pointer,  // *byte
    };

    Kind kind = Kind::Builtin;
    SourceLocation loc;
    BuiltinType builtin = BuiltinType::Void;
    // Spelling exactly as written, so the pretty printer preserves `i32` vs
    // `int32`. Canonicalization happens in Sema, not here.
    std::string_view name;
    TypeNode* pointee = nullptr;
    // Type arguments: the `int32` of `Array<int32>`, the `K, V` of `Map<K, V>`.
    // Empty for a bare `Array`, which stays legal and stays unindexable -- the
    // form that predates generics -- and for a class written without them.
    util::Array<TypeNode*> arguments;
};

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

enum class ExprKind : std::uint8_t {
    IntLiteral,
    FloatLiteral,
    BoolLiteral,
    StringLiteral,
    NullLiteral,
    Identifier,
    This,
    TypeRef,    // a type used as an argument: khuStdMath.convertTo(int64, y)
    Strategy,   // `manual` / `standard` in an allocation site
    Member,     // a.b
    Call,       // f(args) -- also an allocation site once Sema resolves the callee
    Index,      // a[i]
    Unary,
    Binary,
    Assign,
};

enum class UnaryOp : std::uint8_t {
    Plus,
    Negate,
    Not,
    BitNot,
};

enum class BinaryOp : std::uint8_t {
    Add, Sub, Mul, Div, Rem,
    Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
    LogicalAnd, LogicalOr,
    BitAnd, BitOr, BitXor, ShiftLeft, ShiftRight,
};

const char* unary_op_spelling(UnaryOp op);
const char* binary_op_spelling(BinaryOp op);

struct Expr {
    ExprKind kind;
    SourceLocation loc;
    // Filled by Sema: resolved type and target. Null before checking.
    sema::ExprInfo* info = nullptr;

    explicit Expr(ExprKind k, SourceLocation location) : kind(k), loc(location) {}

    template <typename T>
    const T* as() const {
        return kind == T::kKind ? static_cast<const T*>(this) : nullptr;
    }
    template <typename T>
    T* as() {
        return kind == T::kKind ? static_cast<T*>(this) : nullptr;
    }
};

struct IntLiteralExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::IntLiteral;
    std::uint64_t value = 0;
    IntLiteralExpr(SourceLocation loc, std::uint64_t v) : Expr(kKind, loc), value(v) {}
};

struct FloatLiteralExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::FloatLiteral;
    double value = 0.0;
    FloatLiteralExpr(SourceLocation loc, double v) : Expr(kKind, loc), value(v) {}
};

struct BoolLiteralExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::BoolLiteral;
    bool value = false;
    BoolLiteralExpr(SourceLocation loc, bool v) : Expr(kKind, loc), value(v) {}
};

struct StringLiteralExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::StringLiteral;
    std::string_view value;  // decoded contents
    StringLiteralExpr(SourceLocation loc, std::string_view v) : Expr(kKind, loc), value(v) {}
};

struct NullLiteralExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::NullLiteral;
    explicit NullLiteralExpr(SourceLocation loc) : Expr(kKind, loc) {}
};

struct IdentifierExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::Identifier;
    std::string_view name;
    IdentifierExpr(SourceLocation loc, std::string_view n) : Expr(kKind, loc), name(n) {}
};

struct ThisExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::This;
    explicit ThisExpr(SourceLocation loc) : Expr(kKind, loc) {}
};

struct TypeRefExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::TypeRef;
    TypeNode* type = nullptr;
    TypeRefExpr(SourceLocation loc, TypeNode* t) : Expr(kKind, loc), type(t) {}
};

struct StrategyExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::Strategy;
    Strategy strategy = Strategy::ClassDefault;
    StrategyExpr(SourceLocation loc, Strategy s) : Expr(kKind, loc), strategy(s) {}
};

struct MemberExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::Member;
    Expr* object = nullptr;
    std::string_view name;
    SourceLocation name_loc;
    MemberExpr(SourceLocation loc, Expr* obj, std::string_view n, SourceLocation nloc)
        : Expr(kKind, loc), object(obj), name(n), name_loc(nloc) {}
};

struct CallExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::Call;
    Expr* callee = nullptr;
    util::Array<Expr*> args;
    // Set when the first argument was a `manual` / `standard` token; that
    // argument is removed from `args` and recorded here instead.
    Strategy strategy = Strategy::ClassDefault;
    bool has_strategy_token = false;
    SourceLocation strategy_loc;
    CallExpr(SourceLocation loc, Expr* target) : Expr(kKind, loc), callee(target) {}
};

struct IndexExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::Index;
    Expr* object = nullptr;
    Expr* index = nullptr;
    IndexExpr(SourceLocation loc, Expr* obj, Expr* idx)
        : Expr(kKind, loc), object(obj), index(idx) {}
};

struct UnaryExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::Unary;
    UnaryOp op = UnaryOp::Plus;
    Expr* operand = nullptr;
    UnaryExpr(SourceLocation loc, UnaryOp o, Expr* value)
        : Expr(kKind, loc), op(o), operand(value) {}
};

struct BinaryExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::Binary;
    BinaryOp op = BinaryOp::Add;
    Expr* left = nullptr;
    Expr* right = nullptr;
    BinaryExpr(SourceLocation loc, BinaryOp o, Expr* l, Expr* r)
        : Expr(kKind, loc), op(o), left(l), right(r) {}
};

struct AssignExpr : Expr {
    static constexpr ExprKind kKind = ExprKind::Assign;
    Expr* target = nullptr;
    Expr* value = nullptr;
    AssignExpr(SourceLocation loc, Expr* t, Expr* v) : Expr(kKind, loc), target(t), value(v) {}
};

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

enum class StmtKind : std::uint8_t {
    Block,
    VarDecl,
    Expr,
    Return,
    If,
    While,
    Free,
    Empty,  // a stray ';'
};

struct Stmt {
    StmtKind kind;
    SourceLocation loc;

    explicit Stmt(StmtKind k, SourceLocation location) : kind(k), loc(location) {}

    template <typename T>
    const T* as() const {
        return kind == T::kKind ? static_cast<const T*>(this) : nullptr;
    }
    template <typename T>
    T* as() {
        return kind == T::kKind ? static_cast<T*>(this) : nullptr;
    }
};

struct BlockStmt : Stmt {
    static constexpr StmtKind kKind = StmtKind::Block;
    util::Array<Stmt*> statements;
    explicit BlockStmt(SourceLocation loc) : Stmt(kKind, loc) {}
};

// A local declaration. Locals carry visibility just as fields do -- Khudra
// applies JVM-style access control to both (KHU-PLAN.md, Visibility).
struct VarDeclStmt : Stmt {
    static constexpr StmtKind kKind = StmtKind::VarDecl;
    Visibility visibility = Visibility::Private;
    bool explicit_visibility = false;
    TypeNode* type = nullptr;
    std::string_view name;
    SourceLocation name_loc;
    Expr* init = nullptr;
    sema::VarSymbol* symbol = nullptr;
    explicit VarDeclStmt(SourceLocation loc) : Stmt(kKind, loc) {}
};

struct ExprStmt : Stmt {
    static constexpr StmtKind kKind = StmtKind::Expr;
    Expr* expr = nullptr;
    ExprStmt(SourceLocation loc, Expr* e) : Stmt(kKind, loc), expr(e) {}
};

struct ReturnStmt : Stmt {
    static constexpr StmtKind kKind = StmtKind::Return;
    Expr* value = nullptr;  // null for a bare `return;`
    ReturnStmt(SourceLocation loc, Expr* v) : Stmt(kKind, loc), value(v) {}
};

struct IfStmt : Stmt {
    static constexpr StmtKind kKind = StmtKind::If;
    Expr* condition = nullptr;
    Stmt* then_branch = nullptr;
    Stmt* else_branch = nullptr;  // may be null
    explicit IfStmt(SourceLocation loc) : Stmt(kKind, loc) {}
};

struct WhileStmt : Stmt {
    static constexpr StmtKind kKind = StmtKind::While;
    Expr* condition = nullptr;
    Stmt* body = nullptr;
    explicit WhileStmt(SourceLocation loc) : Stmt(kKind, loc) {}
};

struct FreeStmt : Stmt {
    static constexpr StmtKind kKind = StmtKind::Free;
    Expr* target = nullptr;
    bool is_dispose = false;  // spelled `dispose` rather than `free`
    explicit FreeStmt(SourceLocation loc) : Stmt(kKind, loc) {}
};

struct EmptyStmt : Stmt {
    static constexpr StmtKind kKind = StmtKind::Empty;
    explicit EmptyStmt(SourceLocation loc) : Stmt(kKind, loc) {}
};

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

enum class DeclKind : std::uint8_t {
    Import,
    Class,
    Field,
    Method,
    Procedures,
    Param,
};

// `func` is public by default, `method` is private by default; a constructor is
// named after its class.
enum class MethodForm : std::uint8_t {
    Func,
    Method,
    Constructor,
};

struct Decl {
    DeclKind kind;
    SourceLocation loc;

    explicit Decl(DeclKind k, SourceLocation location) : kind(k), loc(location) {}

    template <typename T>
    const T* as() const {
        return kind == T::kKind ? static_cast<const T*>(this) : nullptr;
    }
    template <typename T>
    T* as() {
        return kind == T::kKind ? static_cast<T*>(this) : nullptr;
    }
};

// `bring khu::stdlib;`
struct ImportDecl : Decl {
    static constexpr DeclKind kKind = DeclKind::Import;
    util::Array<std::string_view> path;
    explicit ImportDecl(SourceLocation loc) : Decl(kKind, loc) {}
};

struct ParamDecl : Decl {
    static constexpr DeclKind kKind = DeclKind::Param;
    TypeNode* type = nullptr;
    std::string_view name;
    sema::VarSymbol* symbol = nullptr;
    explicit ParamDecl(SourceLocation loc) : Decl(kKind, loc) {}
};

struct FieldDecl : Decl {
    static constexpr DeclKind kKind = DeclKind::Field;
    Visibility visibility = Visibility::Private;
    bool explicit_visibility = false;
    TypeNode* type = nullptr;
    std::string_view name;
    SourceLocation name_loc;
    Expr* init = nullptr;
    sema::VarSymbol* symbol = nullptr;
    explicit FieldDecl(SourceLocation loc) : Decl(kKind, loc) {}
};

struct MethodDecl : Decl {
    static constexpr DeclKind kKind = DeclKind::Method;
    MethodForm form = MethodForm::Func;
    Visibility visibility = Visibility::Public;
    bool explicit_visibility = false;
    std::string_view name;
    SourceLocation name_loc;
    util::Array<ParamDecl*> params;
    // null means void: "no arrow = void" (KHU-PLAN.md, Returns).
    TypeNode* return_type = nullptr;
    bool explicit_void = false;  // written as `-> void`
    // A `native` member has no body: the implementation is provided by the
    // toolchain, either as a bytecode instruction or as a runtime call.
    bool is_native = false;
    BlockStmt* body = nullptr;
    sema::MethodSymbol* symbol = nullptr;
    explicit MethodDecl(SourceLocation loc) : Decl(kKind, loc) {}

    bool is_constructor() const { return form == MethodForm::Constructor; }
};

// `[visibility] Procedures [(params)] { ... }` -- see docs/procedures.md.
struct ProceduresDecl : Decl {
    static constexpr DeclKind kKind = DeclKind::Procedures;
    Visibility visibility = Visibility::Private;
    bool explicit_visibility = false;
    bool has_param_list = false;  // `Procedures {` vs `Procedures() {`
    util::Array<ParamDecl*> params;
    BlockStmt* body = nullptr;
    sema::ProcedureSymbol* symbol = nullptr;
    explicit ProceduresDecl(SourceLocation loc) : Decl(kKind, loc) {}
};

struct ClassDecl : Decl {
    static constexpr DeclKind kKind = DeclKind::Class;
    Visibility visibility = Visibility::Public;
    bool explicit_visibility = false;
    std::string_view name;
    SourceLocation name_loc;
    // `class List<T>`: the names bound inside the body. Empty for an ordinary
    // class.
    util::Array<std::string_view> type_params;
    util::Array<SourceLocation> type_param_locs;
    std::string_view base_name;  // empty when there is no `extends`
    SourceLocation base_loc;
    // `namespace Name { ... }`: a container of static members that is never
    // materialized. Its members are reached as `Name.member(...)`.
    bool is_namespace = false;
    // Members in source order, so the pretty printer round-trips the layout.
    util::Array<Decl*> members;
    sema::ClassSymbol* symbol = nullptr;
    explicit ClassDecl(SourceLocation loc) : Decl(kKind, loc) {}
};

struct CompilationUnit {
    std::uint32_t file_id = diag::kInvalidFileId;
    // True for the embedded standard library. Its class names are resolved
    // ahead of the program's inside its own code, and behind them everywhere
    // else, so a program that declares its own `List` gets its own.
    bool is_stdlib = false;
    util::Array<ImportDecl*> imports;
    util::Array<ClassDecl*> classes;
};

}  // namespace khu::ast

#endif  // KHU_PARSER_AST_H
