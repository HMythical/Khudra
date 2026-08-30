#include "compiler.h"

#include "codegen/emitter.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "sema/stdlib_sources.h"

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
    unit_ = parser.parse_unit();
    return unit_;
}

bool Compiler::load_stdlib() {
    if (stdlib_loaded_) return true;
    stdlib_loaded_ = true;

    const sema::StdlibFile* files = sema::stdlib_files();
    for (std::size_t i = 0; i < sema::stdlib_file_count(); ++i) {
        std::uint32_t file_id = add_buffer(files[i].path, files[i].contents);
        parser::Parser parser(tokenize(file_id), file_id, diagnostics_, arena_);
        ast::CompilationUnit* library = parser.parse_unit();
        if (library) library->is_stdlib = true;
        stdlib_units_.push(library);
    }
    // A broken standard library is a toolchain bug, not a user error, so it is
    // worth saying so plainly rather than reporting it as part of their file.
    return !diagnostics_.has_errors();
}

sema::Program* Compiler::analyze(std::uint32_t file_id) {
    if (!load_stdlib()) return nullptr;

    ast::CompilationUnit* unit = parse(file_id);
    if (!unit) return nullptr;

    util::Array<ast::CompilationUnit*> units;
    for (ast::CompilationUnit* library : stdlib_units_) units.push(library);
    units.push(unit);

    // Sema still runs after parse errors: recovery leaves a usable tree, and
    // reporting only the first syntax error would hide everything behind it.
    sema::Checker checker(types_, diagnostics_, arena_);
    return checker.check(std::move(units), *unit);
}

}  // namespace khu

namespace khu {

bool Compiler::compile(std::uint32_t file_id, bytecode::Module& out) {
    sema::Program* program = analyze(file_id);
    if (!program || diagnostics_.has_errors()) return false;

    codegen::Emitter emitter(*program, diagnostics_);
    if (!emitter.emit(*unit_, sources_.path(file_id), out, &sources_)) return false;
    return !diagnostics_.has_errors();
}

}  // namespace khu
