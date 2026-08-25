// Phase 1: parsing the real Khudra syntax into an AST.
#include "test_harness.h"

#include <string>

#include "compiler.h"
#include "parser/ast.h"
#include "parser/pretty_printer.h"

using khu::Compiler;
namespace ast = khu::ast;

namespace {

ast::CompilationUnit* parse(Compiler& compiler, std::string source) {
    std::uint32_t file = compiler.add_buffer("test.khu", std::move(source));
    return compiler.parse(file);
}

const ast::Decl* member_at(const ast::ClassDecl& decl, std::size_t index) {
    return index < decl.members.size() ? decl.members[index] : nullptr;
}

}  // namespace

KHU_TEST(parser, parses_imports_and_class_shell) {
    Compiler compiler;
    auto* unit = parse(compiler, "bring khu::stdlib;\nbring khu::io;\npublic class Example { }\n");

    KHU_CHECK(!compiler.diagnostics().has_errors());
    KHU_CHECK_EQ(unit->imports.size(), static_cast<std::size_t>(2));
    KHU_CHECK_EQ(unit->imports[0]->path.size(), static_cast<std::size_t>(2));
    KHU_CHECK_EQ(std::string(unit->imports[0]->path[0]), std::string("khu"));
    KHU_CHECK_EQ(std::string(unit->imports[0]->path[1]), std::string("stdlib"));
    KHU_CHECK_EQ(unit->classes.size(), static_cast<std::size_t>(1));
    KHU_CHECK_EQ(std::string(unit->classes[0]->name), std::string("Example"));
    KHU_CHECK(unit->classes[0]->visibility == ast::Visibility::Public);
}

KHU_TEST(parser, parses_inheritance) {
    Compiler compiler;
    auto* unit = parse(compiler, "public class Sprite extends Node { }\n");
    KHU_CHECK(!compiler.diagnostics().has_errors());
    KHU_CHECK_EQ(std::string(unit->classes[0]->base_name), std::string("Node"));
}

KHU_TEST(parser, parses_fields_with_visibility_and_initializers) {
    Compiler compiler;
    auto* unit = parse(compiler,
                       "public class A {\n"
                       "  public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setStandard();\n"
                       "  public int32 x = 0;\n"
                       "  private int64 y = 0;\n"
                       "  public bool yes = false;\n"
                       "  private Array char = null;\n"
                       "}\n");
    KHU_CHECK(!compiler.diagnostics().has_errors());

    const ast::ClassDecl& decl = *unit->classes[0];
    KHU_CHECK_EQ(decl.members.size(), static_cast<std::size_t>(5));

    const auto* type_field = member_at(decl, 0)->as<ast::FieldDecl>();
    KHU_CHECK(type_field != nullptr);
    KHU_CHECK_EQ(std::string(type_field->name), std::string("type"));
    KHU_CHECK(type_field->visibility == ast::Visibility::Public);
    KHU_CHECK(type_field->type->builtin == ast::BuiltinType::MemoryAllocationTypeObject);
    KHU_CHECK(type_field->init != nullptr);
    KHU_CHECK(type_field->init->kind == ast::ExprKind::Call);

    const auto* y = member_at(decl, 2)->as<ast::FieldDecl>();
    KHU_CHECK(y->visibility == ast::Visibility::Private);
    KHU_CHECK(y->type->builtin == ast::BuiltinType::Int64);

    // `null` is uninstantiated, and `char` is an ordinary identifier.
    const auto* chars = member_at(decl, 4)->as<ast::FieldDecl>();
    KHU_CHECK_EQ(std::string(chars->name), std::string("char"));
    KHU_CHECK(chars->init->kind == ast::ExprKind::NullLiteral);
}

KHU_TEST(parser, applies_func_and_method_visibility_defaults) {
    Compiler compiler;
    auto* unit = parse(compiler,
                       "public class A {\n"
                       "  func a() { }\n"
                       "  method b() { }\n"
                       "  private func c() { }\n"
                       "  public method d() { }\n"
                       "}\n");
    KHU_CHECK(!compiler.diagnostics().has_errors());

    const ast::ClassDecl& decl = *unit->classes[0];
    // func defaults to public, method defaults to private...
    KHU_CHECK(member_at(decl, 0)->as<ast::MethodDecl>()->visibility == ast::Visibility::Public);
    KHU_CHECK(member_at(decl, 1)->as<ast::MethodDecl>()->visibility == ast::Visibility::Private);
    // ...and an explicit modifier overrides the default.
    KHU_CHECK(member_at(decl, 2)->as<ast::MethodDecl>()->visibility == ast::Visibility::Private);
    KHU_CHECK(member_at(decl, 3)->as<ast::MethodDecl>()->visibility == ast::Visibility::Public);
}

KHU_TEST(parser, treats_a_missing_arrow_as_void) {
    Compiler compiler;
    auto* unit = parse(compiler,
                       "public class A {\n"
                       "  func a() { }\n"
                       "  func b() -> void { }\n"
                       "  func c() -> int32 { return 0; }\n"
                       "}\n");
    KHU_CHECK(!compiler.diagnostics().has_errors());

    const ast::ClassDecl& decl = *unit->classes[0];
    const auto* a = member_at(decl, 0)->as<ast::MethodDecl>();
    KHU_CHECK(a->return_type == nullptr);
    KHU_CHECK(!a->explicit_void);

    const auto* b = member_at(decl, 1)->as<ast::MethodDecl>();
    KHU_CHECK(b->return_type == nullptr);
    KHU_CHECK(b->explicit_void);

    const auto* c = member_at(decl, 2)->as<ast::MethodDecl>();
    KHU_CHECK(c->return_type != nullptr);
    KHU_CHECK(c->return_type->builtin == ast::BuiltinType::Int32);
}

KHU_TEST(parser, parses_constructors_and_procedures) {
    Compiler compiler;
    auto* unit = parse(compiler,
                       "public class Example {\n"
                       "  private Procedures {\n"
                       "    khu.stdlibLoadObject();\n"
                       "  }\n"
                       "  public Procedures(int32 w, int32 h) { }\n"
                       "  public Example() {\n"
                       "    this.x = 1;\n"
                       "  }\n"
                       "}\n");
    KHU_CHECK(!compiler.diagnostics().has_errors());

    const ast::ClassDecl& decl = *unit->classes[0];
    const auto* bare = member_at(decl, 0)->as<ast::ProceduresDecl>();
    KHU_CHECK(bare != nullptr);
    KHU_CHECK(!bare->has_param_list);
    KHU_CHECK(bare->visibility == ast::Visibility::Private);
    KHU_CHECK_EQ(bare->body->statements.size(), static_cast<std::size_t>(1));

    const auto* with_params = member_at(decl, 1)->as<ast::ProceduresDecl>();
    KHU_CHECK(with_params->has_param_list);
    KHU_CHECK_EQ(with_params->params.size(), static_cast<std::size_t>(2));

    const auto* ctor = member_at(decl, 2)->as<ast::MethodDecl>();
    KHU_CHECK(ctor != nullptr);
    KHU_CHECK(ctor->is_constructor());
    KHU_CHECK_EQ(std::string(ctor->name), std::string("Example"));
}

KHU_TEST(parser, parses_allocation_sites_with_strategy_tokens) {
    Compiler compiler;
    auto* unit = parse(compiler,
                       "public class A {\n"
                       "  func go() {\n"
                       "    Sprite s = Sprite(x, y);\n"
                       "    FrameBuffer f = FrameBuffer(manual);\n"
                       "    Sprite t = Sprite(standard, x, y);\n"
                       "  }\n"
                       "}\n");
    KHU_CHECK(!compiler.diagnostics().has_errors());

    const auto* body = unit->classes[0]->members[0]->as<ast::MethodDecl>()->body;
    auto strategy_of = [&](std::size_t index) {
        const auto* local = body->statements[index]->as<ast::VarDeclStmt>();
        return local->init->as<ast::CallExpr>();
    };

    const ast::CallExpr* plain = strategy_of(0);
    KHU_CHECK(!plain->has_strategy_token);
    KHU_CHECK_EQ(plain->args.size(), static_cast<std::size_t>(2));

    const ast::CallExpr* manual = strategy_of(1);
    KHU_CHECK(manual->has_strategy_token);
    KHU_CHECK(manual->strategy == ast::Strategy::Manual);
    // The strategy token is consumed by the allocator, not passed on.
    KHU_CHECK_EQ(manual->args.size(), static_cast<std::size_t>(0));

    const ast::CallExpr* standard = strategy_of(2);
    KHU_CHECK(standard->has_strategy_token);
    KHU_CHECK(standard->strategy == ast::Strategy::Gc);
    KHU_CHECK_EQ(standard->args.size(), static_cast<std::size_t>(2));
}

KHU_TEST(parser, parses_a_type_used_as_an_argument) {
    Compiler compiler;
    auto* unit = parse(compiler,
                       "public class A {\n"
                       "  method foo(int x, float y, dfloat z) -> int64 {\n"
                       "    khuStdMath.convertTo(int64, y);\n"
                       "  }\n"
                       "}\n");
    KHU_CHECK(!compiler.diagnostics().has_errors());

    const auto* body = unit->classes[0]->members[0]->as<ast::MethodDecl>()->body;
    const auto* call = body->statements[0]->as<ast::ExprStmt>()->expr->as<ast::CallExpr>();
    KHU_CHECK(call != nullptr);
    KHU_CHECK_EQ(call->args.size(), static_cast<std::size_t>(2));
    const auto* type_arg = call->args[0]->as<ast::TypeRefExpr>();
    KHU_CHECK(type_arg != nullptr);
    KHU_CHECK(type_arg->type->builtin == ast::BuiltinType::Int64);
}

KHU_TEST(parser, honours_operator_precedence) {
    Compiler compiler;
    auto* unit = parse(compiler,
                       "public class A {\n"
                       "  func go() {\n"
                       "    int32 r = 1 + 2 * 3 - 4;\n"
                       "    bool b = a < b && c != d || !e;\n"
                       "  }\n"
                       "}\n");
    KHU_CHECK(!compiler.diagnostics().has_errors());

    const auto* body = unit->classes[0]->members[0]->as<ast::MethodDecl>()->body;
    khu::parser::PrettyPrinter printer;
    KHU_CHECK_EQ(printer.print_expr(*body->statements[0]->as<ast::VarDeclStmt>()->init),
                 std::string("1 + 2 * 3 - 4"));
    KHU_CHECK_EQ(printer.print_expr(*body->statements[1]->as<ast::VarDeclStmt>()->init),
                 std::string("a < b && c != d || !e"));

    // Precedence really is encoded in the tree, not just the text.
    const auto* sum = body->statements[0]->as<ast::VarDeclStmt>()->init->as<ast::BinaryExpr>();
    KHU_CHECK(sum->op == ast::BinaryOp::Sub);
    KHU_CHECK(sum->left->as<ast::BinaryExpr>()->op == ast::BinaryOp::Add);
    KHU_CHECK(sum->left->as<ast::BinaryExpr>()->right->as<ast::BinaryExpr>()->op ==
              ast::BinaryOp::Mul);
}

KHU_TEST(parser, parses_control_flow_and_free) {
    Compiler compiler;
    auto* unit = parse(compiler,
                       "public class A {\n"
                       "  func go() {\n"
                       "    if (x < 1) { y = 1; } else if (x < 2) { y = 2; } else { y = 3; }\n"
                       "    while (x > 0) { x = x - 1; }\n"
                       "    free(buffer);\n"
                       "    dispose(other);\n"
                       "    return;\n"
                       "  }\n"
                       "}\n");
    KHU_CHECK(!compiler.diagnostics().has_errors());

    const auto* body = unit->classes[0]->members[0]->as<ast::MethodDecl>()->body;
    KHU_CHECK_EQ(body->statements.size(), static_cast<std::size_t>(5));
    KHU_CHECK(body->statements[0]->kind == ast::StmtKind::If);
    KHU_CHECK(body->statements[1]->kind == ast::StmtKind::While);
    KHU_CHECK(body->statements[2]->as<ast::FreeStmt>()->is_dispose == false);
    KHU_CHECK(body->statements[3]->as<ast::FreeStmt>()->is_dispose == true);
    KHU_CHECK(body->statements[4]->as<ast::ReturnStmt>()->value == nullptr);
}

KHU_TEST(parser, distinguishes_declarations_from_expressions) {
    Compiler compiler;
    auto* unit = parse(compiler,
                       "public class A {\n"
                       "  func go() {\n"
                       "    Sprite s = Sprite();\n"   // Type name -> declaration
                       "    khu.getType();\n"          // ident '.' -> expression
                       "    s = other;\n"              // ident '=' -> expression
                       "  }\n"
                       "}\n");
    KHU_CHECK(!compiler.diagnostics().has_errors());

    const auto* body = unit->classes[0]->members[0]->as<ast::MethodDecl>()->body;
    KHU_CHECK(body->statements[0]->kind == ast::StmtKind::VarDecl);
    KHU_CHECK(body->statements[1]->kind == ast::StmtKind::Expr);
    KHU_CHECK(body->statements[2]->as<ast::ExprStmt>()->expr->kind == ast::ExprKind::Assign);
}

KHU_TEST(parser, reports_errors_with_locations_and_keeps_going) {
    Compiler compiler;
    parse(compiler,
          "public class A {\n"
          "  int32 x = ;\n"
          "  int32 y = 1\n"
          "  func ok() { }\n"
          "}\n");

    std::string text = compiler.diagnostics().render();
    KHU_CHECK(compiler.diagnostics().error_count() >= 2);
    KHU_CHECK_CONTAINS(text, "test.khu:2:13: error: expected an expression");
    KHU_CHECK_CONTAINS(text, "expected ';' after a field declaration");
}

KHU_TEST(parser, recovers_and_still_sees_later_classes) {
    Compiler compiler;
    auto* unit = parse(compiler,
                       "public class Broken {\n"
                       "  ??? nonsense\n"
                       "}\n"
                       "public class Fine { }\n");
    KHU_CHECK(compiler.diagnostics().has_errors());
    KHU_CHECK_EQ(unit->classes.size(), static_cast<std::size_t>(2));
    KHU_CHECK_EQ(std::string(unit->classes[1]->name), std::string("Fine"));
}
