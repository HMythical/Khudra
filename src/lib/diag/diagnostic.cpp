#include "diag/diagnostic.h"

#include "util/string_builder.h"

namespace khu::diag {

const char* severity_name(Severity severity) {
    switch (severity) {
        case Severity::Note: return "note";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
    }
    return "diagnostic";
}

DiagnosticEngine::Builder& DiagnosticEngine::Builder::note(SourceLocation location,
                                                           std::string message) {
    Diagnostic child;
    child.severity = Severity::Note;
    child.location = location;
    child.message = std::move(message);
    engine_.diagnostics_[index_].notes.push(std::move(child));
    return *this;
}

DiagnosticEngine::Builder DiagnosticEngine::report(Severity severity, SourceLocation location,
                                                   std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.location = location;
    diagnostic.message = std::move(message);
    diagnostics_.push(std::move(diagnostic));
    if (severity == Severity::Error) ++error_count_;
    if (severity == Severity::Warning) ++warning_count_;
    return Builder(*this, diagnostics_.size() - 1);
}

DiagnosticEngine::Builder DiagnosticEngine::error(SourceLocation location, std::string message) {
    return report(Severity::Error, location, std::move(message));
}

DiagnosticEngine::Builder DiagnosticEngine::warning(SourceLocation location, std::string message) {
    return report(Severity::Warning, location, std::move(message));
}

DiagnosticEngine::Builder DiagnosticEngine::note(SourceLocation location, std::string message) {
    return report(Severity::Note, location, std::move(message));
}

void DiagnosticEngine::render_one(const Diagnostic& diagnostic, int depth,
                                  const SourceLocation* parent, std::string& out) const {
    khu::util::StringBuilder builder;
    builder.set_indent_unit("  ");
    builder.indent(depth);

    if (diagnostic.location.valid()) {
        builder.append(sources_.format(diagnostic.location)).append(": ");
    } else {
        builder.append("khudra: ");
    }
    builder.append(severity_name(diagnostic.severity)).append(": ").append(diagnostic.message);
    builder.append('\n');

    bool repeats_parent = parent && parent->file_id == diagnostic.location.file_id &&
                          parent->line == diagnostic.location.line &&
                          parent->column == diagnostic.location.column;
    std::string_view line = repeats_parent ? std::string_view() 
                                           : sources_.line_text(diagnostic.location);
    if (diagnostic.location.valid() && !line.empty()) {
        builder.indent(depth).append("    ").append(line).append('\n');
        builder.indent(depth).append("    ");
        // Copy tabs across so the caret lines up in tab-indented sources.
        for (std::uint32_t i = 1; i < diagnostic.location.column && i <= line.size(); ++i) {
            builder.append(line[i - 1] == '\t' ? '\t' : ' ');
        }
        builder.append("^\n");
    }

    out += builder.str();
    for (const Diagnostic& child : diagnostic.notes) {
        render_one(child, depth + 1, &diagnostic.location, out);
    }
}

std::string DiagnosticEngine::render() const {
    std::string out;
    for (const Diagnostic& diagnostic : diagnostics_) render_one(diagnostic, 0, nullptr, out);
    return out;
}

void DiagnosticEngine::print(std::FILE* stream) const {
    std::string text = render();
    if (!text.empty()) std::fwrite(text.data(), 1, text.size(), stream);
}

void DiagnosticEngine::clear() {
    diagnostics_.clear();
    error_count_ = 0;
    warning_count_ = 0;
}

}  // namespace khu::diag
