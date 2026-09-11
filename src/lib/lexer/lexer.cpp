#include "lexer/lexer.h"

#include <cerrno>
#include <cstdlib>
#include <string>

namespace khu::lexer {
namespace {

bool is_identifier_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_identifier_part(char c) { return is_identifier_start(c) || (c >= '0' && c <= '9'); }

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

}  // namespace

Lexer::Lexer(const diag::SourceManager& sources, std::uint32_t file_id,
             diag::DiagnosticEngine& diagnostics, util::Arena& arena)
    : diagnostics_(diagnostics),
      arena_(arena),
      file_id_(file_id),
      source_(sources.contents(file_id)) {}

char Lexer::peek(std::size_t ahead) const {
    std::size_t index = offset_ + ahead;
    return index < source_.size() ? source_[index] : '\0';
}

bool Lexer::at_end() const { return offset_ >= source_.size(); }

char Lexer::advance() {
    char c = source_[offset_++];
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return c;
}

bool Lexer::match(char expected) {
    if (at_end() || source_[offset_] != expected) return false;
    advance();
    return true;
}

diag::SourceLocation Lexer::here() const {
    return diag::SourceLocation{file_id_, line_, column_, static_cast<std::uint32_t>(offset_)};
}

Token Lexer::make(TokenKind kind, diag::SourceLocation start, std::size_t start_offset) {
    Token token;
    token.kind = kind;
    token.loc = start;
    token.text = source_.substr(start_offset, offset_ - start_offset);
    return token;
}

void Lexer::skip_trivia() {
    while (!at_end()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
            continue;
        }
        if (c == '/' && peek(1) == '/') {
            while (!at_end() && peek() != '\n') advance();
            continue;
        }
        if (c == '/' && peek(1) == '*') {
            diag::SourceLocation start = here();
            advance();
            advance();
            bool closed = false;
            while (!at_end()) {
                if (peek() == '*' && peek(1) == '/') {
                    advance();
                    advance();
                    closed = true;
                    break;
                }
                advance();
            }
            if (!closed) diagnostics_.error(start, "unterminated block comment");
            continue;
        }
        return;
    }
}

Token Lexer::scan_identifier() {
    diag::SourceLocation start = here();
    std::size_t start_offset = offset_;
    while (!at_end() && is_identifier_part(peek())) advance();

    Token token = make(TokenKind::Identifier, start, start_offset);
    token.kind = keyword_kind(token.text);
    if (token.kind == TokenKind::KwInlineC || token.kind == TokenKind::KwInlineAsm) {
        // The next token is the raw block, so its braces belong to the C or
        // asm text, not to statement-block punctuation.
        inline_block_pending_ = true;
    }
    return token;
}

Token Lexer::scan_number() {
    diag::SourceLocation start = here();
    std::size_t start_offset = offset_;

    // Radix prefixes. Underscores are permitted as digit separators anywhere
    // after the first digit.
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        advance();
        advance();
        std::uint64_t value = 0;
        bool any = false;
        while (!at_end() && (is_hex_digit(peek()) || peek() == '_')) {
            char c = advance();
            if (c == '_') continue;
            value = value * 16 + static_cast<std::uint64_t>(hex_value(c));
            any = true;
        }
        Token token = make(TokenKind::IntLiteral, start, start_offset);
        if (!any) {
            diagnostics_.error(start, "hexadecimal literal has no digits");
            token.kind = TokenKind::Invalid;
        }
        token.int_value = value;
        return token;
    }

    if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
        advance();
        advance();
        std::uint64_t value = 0;
        bool any = false;
        while (!at_end() && (peek() == '0' || peek() == '1' || peek() == '_')) {
            char c = advance();
            if (c == '_') continue;
            value = value * 2 + static_cast<std::uint64_t>(c - '0');
            any = true;
        }
        Token token = make(TokenKind::IntLiteral, start, start_offset);
        if (!any) {
            diagnostics_.error(start, "binary literal has no digits");
            token.kind = TokenKind::Invalid;
        }
        token.int_value = value;
        return token;
    }

    bool is_float = false;
    while (!at_end() && (is_digit(peek()) || peek() == '_')) advance();

    // A '.' only starts a fraction when a digit follows, so `1.foo()` still
    // lexes as an integer followed by a member access.
    if (peek() == '.' && is_digit(peek(1))) {
        is_float = true;
        advance();
        while (!at_end() && (is_digit(peek()) || peek() == '_')) advance();
    }

    if (peek() == 'e' || peek() == 'E') {
        std::size_t ahead = 1;
        if (peek(1) == '+' || peek(1) == '-') ahead = 2;
        if (is_digit(peek(ahead))) {
            is_float = true;
            advance();
            if (peek() == '+' || peek() == '-') advance();
            while (!at_end() && is_digit(peek())) advance();
        }
    }

    Token token = make(is_float ? TokenKind::FloatLiteral : TokenKind::IntLiteral, start,
                       start_offset);

    std::string digits;
    digits.reserve(token.text.size());
    for (char c : token.text) {
        if (c != '_') digits.push_back(c);
    }

    if (is_float) {
        token.float_value = std::strtod(digits.c_str(), nullptr);
    } else {
        errno = 0;
        token.int_value = std::strtoull(digits.c_str(), nullptr, 10);
        if (errno == ERANGE) {
            diagnostics_.error(start, "integer literal does not fit in 64 bits");
        }
    }
    return token;
}

Token Lexer::scan_string() {
    diag::SourceLocation start = here();
    std::size_t start_offset = offset_;
    advance();  // opening quote

    std::string decoded;
    bool terminated = false;
    while (!at_end()) {
        char c = peek();
        if (c == '"') {
            advance();
            terminated = true;
            break;
        }
        if (c == '\n') break;  // strings do not span lines
        advance();
        if (c != '\\') {
            decoded.push_back(c);
            continue;
        }
        if (at_end()) break;
        diag::SourceLocation escape = here();
        char escaped = advance();
        switch (escaped) {
            case 'n': decoded.push_back('\n'); break;
            case 't': decoded.push_back('\t'); break;
            case 'r': decoded.push_back('\r'); break;
            case '0': decoded.push_back('\0'); break;
            case '\\': decoded.push_back('\\'); break;
            case '"': decoded.push_back('"'); break;
            case '\'': decoded.push_back('\''); break;
            default:
                diagnostics_.error(escape, std::string("unknown escape sequence '\\") + escaped +
                                               "' in string literal");
                decoded.push_back(escaped);
                break;
        }
    }

    Token token = make(TokenKind::StringLiteral, start, start_offset);
    if (!terminated) {
        diagnostics_.error(start, "unterminated string literal");
        token.kind = TokenKind::Invalid;
    }
    token.string_value = arena_.copy_string(decoded);
    return token;
}

Token Lexer::scan_raw_block(diag::SourceLocation start) {
    std::size_t start_offset = offset_;
    advance();  // opening '{'

    // The C text is spliced verbatim into the emitted translation unit, so the
    // lexer only has to find the matching close brace. Braces inside string and
    // character literals, and inside // and /* */ comments, do not count.
    int depth = 1;
    while (!at_end() && depth > 0) {
        char c = peek();
        if (c == '"' || c == '\'') {
            advance();
            const char quote = c;
            while (!at_end()) {
                char inner = peek();
                if (inner == '\\') {
                    advance();
                    if (!at_end()) advance();
                    continue;
                }
                advance();
                if (inner == quote) break;
                if (inner == '\n') break;  // an unterminated literal (illegal C)
            }
            continue;
        }
        if (c == '/' && peek(1) == '/') {
            while (!at_end() && peek() != '\n') advance();
            continue;
        }
        if (c == '/' && peek(1) == '*') {
            advance();
            advance();
            while (!at_end() && !(peek() == '*' && peek(1) == '/')) advance();
            if (!at_end()) {
                advance();
                advance();
            }
            continue;
        }
        if (c == '{') {
            ++depth;
            advance();
            continue;
        }
        if (c == '}') {
            --depth;
            advance();
            continue;
        }
        advance();
    }

    Token token = make(TokenKind::RawBlock, start, start_offset);
    if (depth > 0) {
        diagnostics_.error(start, "unterminated inline block: expected '}' to close it");
        token.kind = TokenKind::Invalid;
    }
    return token;
}

Token Lexer::scan_token() {
    diag::SourceLocation start = here();

    // `inline_c` was scanned a moment ago; its `{` opens a raw C block rather
    // than a statement block, so the whole thing is one token.
    if (inline_block_pending_ && peek() == '{') {
        inline_block_pending_ = false;
        return scan_raw_block(start);
    }
    inline_block_pending_ = false;

    std::size_t start_offset = offset_;
    char c = advance();

    switch (c) {
        case '{': return make(TokenKind::LBrace, start, start_offset);
        case '}': return make(TokenKind::RBrace, start, start_offset);
        case '(': return make(TokenKind::LParen, start, start_offset);
        case ')': return make(TokenKind::RParen, start, start_offset);
        case '[': return make(TokenKind::LBracket, start, start_offset);
        case ']': return make(TokenKind::RBracket, start, start_offset);
        case ';': return make(TokenKind::Semicolon, start, start_offset);
        case ',': return make(TokenKind::Comma, start, start_offset);
        case '.': return make(TokenKind::Dot, start, start_offset);
        case '~': return make(TokenKind::Tilde, start, start_offset);
        case '+': return make(TokenKind::Plus, start, start_offset);
        case '*': return make(TokenKind::Star, start, start_offset);
        case '/': return make(TokenKind::Slash, start, start_offset);
        case '%': return make(TokenKind::Percent, start, start_offset);
        case '^': return make(TokenKind::Caret, start, start_offset);
        case '-':
            if (match('>')) return make(TokenKind::Arrow, start, start_offset);
            return make(TokenKind::Minus, start, start_offset);
        case '=':
            if (match('=')) return make(TokenKind::EqualEqual, start, start_offset);
            return make(TokenKind::Assign, start, start_offset);
        case '!':
            if (match('=')) return make(TokenKind::BangEqual, start, start_offset);
            return make(TokenKind::Bang, start, start_offset);
        case '<':
            if (match('=')) return make(TokenKind::LessEqual, start, start_offset);
            if (match('<')) return make(TokenKind::LessLess, start, start_offset);
            return make(TokenKind::Less, start, start_offset);
        case '>':
            if (match('=')) return make(TokenKind::GreaterEqual, start, start_offset);
            if (match('>')) return make(TokenKind::GreaterGreater, start, start_offset);
            return make(TokenKind::Greater, start, start_offset);
        case '&':
            if (match('&')) return make(TokenKind::AmpAmp, start, start_offset);
            return make(TokenKind::Amp, start, start_offset);
        case '|':
            if (match('|')) return make(TokenKind::PipePipe, start, start_offset);
            return make(TokenKind::Pipe, start, start_offset);
        case ':':
            if (match(':')) return make(TokenKind::ColonColon, start, start_offset);
            diagnostics_.error(start, "expected '::' -- Khudra uses '::' for namespaces");
            return make(TokenKind::Invalid, start, start_offset);
        default:
            break;
    }

    // Rewind one character: the scanners below want to see the first character.
    offset_ = start_offset;
    line_ = start.line;
    column_ = start.column;

    if (is_identifier_start(c)) return scan_identifier();
    if (is_digit(c)) return scan_number();
    if (c == '"') return scan_string();

    advance();
    Token token = make(TokenKind::Invalid, start, start_offset);
    diagnostics_.error(start, std::string("unexpected character '") + c + "'");
    return token;
}

util::Array<Token> Lexer::tokenize() {
    util::Array<Token> tokens;
    while (true) {
        skip_trivia();
        if (at_end()) break;
        tokens.push(scan_token());
    }

    Token end;
    end.kind = TokenKind::EndOfFile;
    end.loc = here();
    end.text = {};
    tokens.push(end);
    return tokens;
}

}  // namespace khu::lexer
