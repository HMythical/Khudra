// Phase 1: the lexer must handle the real syntax in example.khu.
#include "test_harness.h"

#include <string>

#include "compiler.h"
#include "lexer/lexer.h"

using khu::Compiler;
using khu::lexer::Token;
using khu::lexer::TokenKind;

namespace {

khu::util::Array<Token> scan(Compiler& compiler, std::string source) {
    std::uint32_t file = compiler.add_buffer("test.khu", std::move(source));
    return compiler.tokenize(file);
}

}  // namespace

KHU_TEST(lexer, scans_keywords_and_type_aliases) {
    Compiler compiler;
    auto tokens = scan(compiler, "bring class public private Procedures func method i32 int32 "
                                 "uint8 byte dfloat MemoryAllocationTypeObject manual standard");

    KHU_CHECK(tokens[0].is(TokenKind::KwBring));
    KHU_CHECK(tokens[1].is(TokenKind::KwClass));
    KHU_CHECK(tokens[2].is(TokenKind::KwPublic));
    KHU_CHECK(tokens[3].is(TokenKind::KwPrivate));
    KHU_CHECK(tokens[4].is(TokenKind::KwProcedures));
    KHU_CHECK(tokens[5].is(TokenKind::KwFunc));
    KHU_CHECK(tokens[6].is(TokenKind::KwMethod));
    KHU_CHECK(tokens[7].is(TokenKind::KwI32));
    KHU_CHECK(tokens[8].is(TokenKind::KwInt32));
    KHU_CHECK(tokens[9].is(TokenKind::KwUInt8));
    KHU_CHECK(tokens[10].is(TokenKind::KwByte));
    KHU_CHECK(tokens[11].is(TokenKind::KwDFloat));
    KHU_CHECK(tokens[12].is(TokenKind::KwMemoryAllocationTypeObject));
    KHU_CHECK(tokens[13].is(TokenKind::KwManual));
    KHU_CHECK(tokens[14].is(TokenKind::KwStandard));
    KHU_CHECK(tokens[15].is(TokenKind::EndOfFile));
    KHU_CHECK(!compiler.diagnostics().has_errors());
}

KHU_TEST(lexer, scans_operators_with_maximal_munch) {
    Compiler compiler;
    auto tokens = scan(compiler, ":: -> == != <= >= << >> && || = < > ! & | ^ ~ + - * / %");

    const TokenKind expected[] = {
        TokenKind::ColonColon, TokenKind::Arrow,     TokenKind::EqualEqual,
        TokenKind::BangEqual,  TokenKind::LessEqual, TokenKind::GreaterEqual,
        TokenKind::LessLess,   TokenKind::GreaterGreater, TokenKind::AmpAmp,
        TokenKind::PipePipe,   TokenKind::Assign,    TokenKind::Less,
        TokenKind::Greater,    TokenKind::Bang,      TokenKind::Amp,
        TokenKind::Pipe,       TokenKind::Caret,     TokenKind::Tilde,
        TokenKind::Plus,       TokenKind::Minus,     TokenKind::Star,
        TokenKind::Slash,      TokenKind::Percent,   TokenKind::EndOfFile,
    };
    KHU_CHECK_EQ(tokens.size(), sizeof(expected) / sizeof(expected[0]));
    for (std::size_t i = 0; i < tokens.size() && i < sizeof(expected) / sizeof(expected[0]); ++i) {
        KHU_CHECK(tokens[i].is(expected[i]));
    }
    KHU_CHECK(!compiler.diagnostics().has_errors());
}

KHU_TEST(lexer, scans_numeric_literals) {
    Compiler compiler;
    auto tokens = scan(compiler, "0 42 0xff 0b1010 1_000 1.5 2.0e-3 1e5 7.foo");

    KHU_CHECK(tokens[0].is(TokenKind::IntLiteral));
    KHU_CHECK_EQ(tokens[0].int_value, static_cast<std::uint64_t>(0));
    KHU_CHECK_EQ(tokens[1].int_value, static_cast<std::uint64_t>(42));
    KHU_CHECK_EQ(tokens[2].int_value, static_cast<std::uint64_t>(255));
    KHU_CHECK_EQ(tokens[3].int_value, static_cast<std::uint64_t>(10));
    KHU_CHECK_EQ(tokens[4].int_value, static_cast<std::uint64_t>(1000));
    KHU_CHECK(tokens[5].is(TokenKind::FloatLiteral));
    KHU_CHECK_EQ(tokens[5].float_value, 1.5);
    KHU_CHECK(tokens[6].is(TokenKind::FloatLiteral));
    KHU_CHECK_EQ(tokens[6].float_value, 2.0e-3);
    KHU_CHECK(tokens[7].is(TokenKind::FloatLiteral));
    KHU_CHECK_EQ(tokens[7].float_value, 1e5);
    // A '.' only starts a fraction when a digit follows.
    KHU_CHECK(tokens[8].is(TokenKind::IntLiteral));
    KHU_CHECK(tokens[9].is(TokenKind::Dot));
    KHU_CHECK(tokens[10].is(TokenKind::Identifier));
    KHU_CHECK(!compiler.diagnostics().has_errors());
}

KHU_TEST(lexer, decodes_string_escapes) {
    Compiler compiler;
    auto tokens = scan(compiler, "\"a\\nb\\t\\\"c\\\\\"");
    KHU_CHECK(tokens[0].is(TokenKind::StringLiteral));
    KHU_CHECK_EQ(std::string(tokens[0].string_value), std::string("a\nb\t\"c\\"));
    KHU_CHECK(!compiler.diagnostics().has_errors());
}

KHU_TEST(lexer, skips_comments) {
    Compiler compiler;
    auto tokens = scan(compiler,
                       "// leading\n"
                       "public /* inline */ class /* multi\nline */ A\n"
                       "// trailing");
    KHU_CHECK(tokens[0].is(TokenKind::KwPublic));
    KHU_CHECK(tokens[1].is(TokenKind::KwClass));
    KHU_CHECK(tokens[2].is(TokenKind::Identifier));
    KHU_CHECK(tokens[3].is(TokenKind::EndOfFile));
    KHU_CHECK(!compiler.diagnostics().has_errors());
}

KHU_TEST(lexer, tracks_line_and_column) {
    Compiler compiler;
    auto tokens = scan(compiler, "bring khu::stdlib;\n\npublic class Example{\n");

    KHU_CHECK_EQ(tokens[0].loc.line, static_cast<std::uint32_t>(1));
    KHU_CHECK_EQ(tokens[0].loc.column, static_cast<std::uint32_t>(1));
    // `public` on line 3.
    KHU_CHECK_EQ(tokens[5].loc.line, static_cast<std::uint32_t>(3));
    KHU_CHECK_EQ(tokens[5].loc.column, static_cast<std::uint32_t>(1));
    // `class` follows it.
    KHU_CHECK_EQ(tokens[6].loc.column, static_cast<std::uint32_t>(8));
}

KHU_TEST(lexer, reports_lexical_errors_with_locations) {
    Compiler compiler;
    scan(compiler, "public class A {\n  int32 x = #;\n}\n");
    KHU_CHECK(compiler.diagnostics().has_errors());
    KHU_CHECK_CONTAINS(compiler.diagnostics().render(), "test.khu:2:13: error: unexpected character '#'");
}

KHU_TEST(lexer, reports_unterminated_constructs) {
    {
        Compiler compiler;
        scan(compiler, "\"no end\n");
        KHU_CHECK_CONTAINS(compiler.diagnostics().render(), "unterminated string literal");
    }
    {
        Compiler compiler;
        scan(compiler, "/* no end\n");
        KHU_CHECK_CONTAINS(compiler.diagnostics().render(), "unterminated block comment");
    }
}

KHU_TEST(lexer, collects_multiple_errors_in_one_pass) {
    Compiler compiler;
    scan(compiler, "# @ $");
    KHU_CHECK_EQ(compiler.diagnostics().error_count(), static_cast<std::size_t>(3));
}
