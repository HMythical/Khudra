// Khudra lexer.
//
// Scans a whole source buffer into a token vector. Files are small enough that
// buffering the stream up front is simpler than a pull lexer, and it gives the
// parser unrestricted lookahead for free.
#ifndef KHU_LEXER_LEXER_H
#define KHU_LEXER_LEXER_H

#include <cstdint>
#include <string_view>

#include "diag/diagnostic.h"
#include "diag/source_manager.h"
#include "lexer/token.h"
#include "util/arena.h"
#include "util/array.h"

namespace khu::lexer {

class Lexer {
public:
    Lexer(const diag::SourceManager& sources, std::uint32_t file_id,
          diag::DiagnosticEngine& diagnostics, util::Arena& arena);

    // Scans the whole file. The result always ends with an EndOfFile token,
    // even when errors were reported.
    util::Array<Token> tokenize();

private:
    char peek(std::size_t ahead = 0) const;
    bool at_end() const;
    char advance();
    bool match(char expected);
    diag::SourceLocation here() const;

    void skip_trivia();
    Token scan_token();
    Token scan_identifier();
    Token scan_number();
    Token scan_string();
    Token make(TokenKind kind, diag::SourceLocation start, std::size_t start_offset);

    diag::DiagnosticEngine& diagnostics_;
    util::Arena& arena_;
    std::uint32_t file_id_;
    std::string_view source_;
    std::size_t offset_ = 0;
    std::uint32_t line_ = 1;
    std::uint32_t column_ = 1;
};

}  // namespace khu::lexer

#endif  // KHU_LEXER_LEXER_H
