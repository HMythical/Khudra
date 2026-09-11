// Khudra tokens.
//
// The kinds are generated from X-macro lists so that the enum, the spelling
// table and the keyword map can never drift apart.
#ifndef KHU_LEXER_TOKEN_H
#define KHU_LEXER_TOKEN_H

#include <cstdint>
#include <string_view>

#include "diag/source_location.h"

namespace khu::lexer {

// Kinds that carry their own text rather than a fixed spelling.
#define KHU_TOKEN_SPECIALS(X)             \
    X(EndOfFile, "end of file")           \
    X(Invalid, "invalid token")           \
    X(Identifier, "identifier")           \
    X(IntLiteral, "integer literal")      \
    X(FloatLiteral, "float literal")      \
    X(StringLiteral, "string literal")    \
    X(RawBlock, "raw block")

// Reserved type names. Aliases (i32, u8, byte, int) canonicalize in Sema.
#define KHU_TOKEN_TYPE_KEYWORDS(X)                                  \
    X(KwInt8, "int8")     X(KwI8, "i8")                             \
    X(KwInt16, "int16")   X(KwI16, "i16")                           \
    X(KwInt32, "int32")   X(KwI32, "i32")                           \
    X(KwInt64, "int64")   X(KwI64, "i64")                           \
    X(KwInt, "int")                                                 \
    X(KwUInt8, "uint8")   X(KwU8, "u8")                             \
    X(KwUInt16, "uint16") X(KwU16, "u16")                           \
    X(KwUInt32, "uint32") X(KwU32, "u32")                           \
    X(KwUInt64, "uint64") X(KwU64, "u64")                           \
    X(KwByte, "byte")                                               \
    X(KwFloat, "float")   X(KwDFloat, "dfloat")                     \
    X(KwBool, "bool")                                               \
    X(KwString, "string")                                           \
    X(KwArray, "Array")                                             \
    X(KwMemoryAllocationTypeObject, "MemoryAllocationTypeObject")   \
    X(KwVoid, "void")

#define KHU_TOKEN_KEYWORDS(X)                                       \
    X(KwBring, "bring")                                             \
    X(KwClass, "class")                                             \
    X(KwNamespace, "namespace")                                     \
    X(KwNative, "native")                                           \
    X(KwExtends, "extends")                                         \
    X(KwPublic, "public")                                           \
    X(KwPrivate, "private")                                         \
    X(KwProcedures, "Procedures")                                   \
    X(KwFunc, "func")                                               \
    X(KwMethod, "method")                                           \
    X(KwReturn, "return")                                           \
    X(KwThis, "this")                                               \
    X(KwNull, "null")                                               \
    X(KwTrue, "true")                                               \
    X(KwFalse, "false")                                             \
    X(KwIf, "if")                                                   \
    X(KwElse, "else")                                               \
    X(KwWhile, "while")                                             \
    X(KwFree, "free")                                               \
    X(KwDispose, "dispose")                                         \
    X(KwManual, "manual")                                           \
    X(KwStandard, "standard")                                       \
    X(KwInlineC, "inline_c")                                        \
    X(KwInlineAsm, "inline_asm")                                    \
    KHU_TOKEN_TYPE_KEYWORDS(X)

#define KHU_TOKEN_PUNCTUATION(X)                                    \
    X(LBrace, "{")        X(RBrace, "}")                            \
    X(LParen, "(")        X(RParen, ")")                            \
    X(LBracket, "[")      X(RBracket, "]")                          \
    X(Semicolon, ";")     X(Comma, ",")     X(Dot, ".")             \
    X(ColonColon, "::")   X(Arrow, "->")                            \
    X(Assign, "=")        X(EqualEqual, "==") X(BangEqual, "!=")    \
    X(Less, "<")          X(LessEqual, "<=")                        \
    X(Greater, ">")       X(GreaterEqual, ">=")                     \
    X(Plus, "+")          X(Minus, "-")                             \
    X(Star, "*")          X(Slash, "/")     X(Percent, "%")         \
    X(AmpAmp, "&&")       X(PipePipe, "||") X(Bang, "!")            \
    X(Amp, "&")           X(Pipe, "|")      X(Caret, "^")           \
    X(Tilde, "~")         X(LessLess, "<<") X(GreaterGreater, ">>")

enum class TokenKind : std::uint16_t {
#define KHU_ENUM_ENTRY(name, spelling) name,
    KHU_TOKEN_SPECIALS(KHU_ENUM_ENTRY)

    // Keyword range markers bracket every reserved word.
    KeywordFirst,
    KHU_TOKEN_KEYWORDS(KHU_ENUM_ENTRY)
    KeywordLast,

    KHU_TOKEN_PUNCTUATION(KHU_ENUM_ENTRY)
#undef KHU_ENUM_ENTRY
    Count,
};

// Human-readable name used in diagnostics: the exact spelling for keywords and
// punctuation, a category name ("identifier") otherwise.
const char* token_name(TokenKind kind);

// The fixed spelling of a keyword or punctuator, or an empty view.
std::string_view token_spelling(TokenKind kind);

bool is_keyword(TokenKind kind);
bool is_type_keyword(TokenKind kind);

// Maps an identifier spelling to its keyword kind, or Identifier.
TokenKind keyword_kind(std::string_view text);

struct Token {
    TokenKind kind = TokenKind::Invalid;
    diag::SourceLocation loc;
    // Raw spelling as it appears in the source buffer.
    std::string_view text;
    // Literal payloads; only the one matching `kind` is meaningful.
    std::uint64_t int_value = 0;
    double float_value = 0.0;
    // Decoded string-literal contents, owned by the lexer's arena.
    std::string_view string_value;

    bool is(TokenKind expected) const { return kind == expected; }
};

}  // namespace khu::lexer

#endif  // KHU_LEXER_TOKEN_H
