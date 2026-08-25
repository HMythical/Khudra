#include "compiler.h"

#include "lexer/lexer.h"
#include "parser/parser.h"

namespace khu {

std::uint32_t Compiler::add_file(std::string_view path, std::string& error) {
    return sources_.load_file(path, error);
}

std::uint32_t Compiler::add_buffer(std::string_view path, std::string contents) {
    return sources_.add_buffer(path, std::move(contents));
}

util::Array<lexer::Token> Compiler::tokenize(std::uint32_t file_id) {
    lexer::Lexer lexer(sources_, file_id, diagnostics_, arena_);
    return lexer.tokenize();
}

ast::CompilationUnit* Compiler::parse(std::uint32_t file_id) {
    parser::Parser parser(tokenize(file_id), file_id, diagnostics_, arena_);
    return parser.parse_unit();
}

}  // namespace khu
