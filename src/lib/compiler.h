// Compilation facade.
//
// Owns the pieces every stage needs -- the source manager, the diagnostic
// engine and the AST arena -- so the CLI and the tests drive the front end the
// same way.
#ifndef KHU_COMPILER_H
#define KHU_COMPILER_H

#include <cstdint>
#include <string>
#include <string_view>

#include "diag/diagnostic.h"
#include "diag/source_manager.h"
#include "lexer/token.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "util/array.h"

namespace khu {

class Compiler {
public:
    Compiler() : diagnostics_(sources_) {}

    // Registers a source. Returns diag::kInvalidFileId and fills `error` when
    // the file cannot be read.
    std::uint32_t add_file(std::string_view path, std::string& error);
    std::uint32_t add_buffer(std::string_view path, std::string contents);

    util::Array<lexer::Token> tokenize(std::uint32_t file_id);

    // Lexes and parses. Returns a unit even when errors were reported; check
    // diagnostics().has_errors().
    ast::CompilationUnit* parse(std::uint32_t file_id);

    diag::SourceManager& sources() { return sources_; }
    const diag::SourceManager& sources() const { return sources_; }
    diag::DiagnosticEngine& diagnostics() { return diagnostics_; }
    const diag::DiagnosticEngine& diagnostics() const { return diagnostics_; }
    util::Arena& arena() { return arena_; }

private:
    diag::SourceManager sources_;
    diag::DiagnosticEngine diagnostics_;
    util::Arena arena_;
};

}  // namespace khu

#endif  // KHU_COMPILER_H
