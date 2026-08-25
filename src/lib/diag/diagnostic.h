// Diagnostic collection and rendering.
//
// The engine collects rather than aborts, so a single pass can report many
// problems (Phase 1 acceptance: multi-error collection with warning severity).
#ifndef KHU_DIAG_DIAGNOSTIC_H
#define KHU_DIAG_DIAGNOSTIC_H

#include <cstdio>
#include <string>
#include <string_view>

#include "diag/source_location.h"
#include "diag/source_manager.h"
#include "util/array.h"

namespace khu::diag {

enum class Severity {
    Note,
    Warning,
    Error,
};

const char* severity_name(Severity severity);

struct Diagnostic {
    Severity severity = Severity::Error;
    SourceLocation location;
    std::string message;
    // Optional follow-up notes attached to the primary diagnostic.
    khu::util::Array<Diagnostic> notes;
};

class DiagnosticEngine {
public:
    explicit DiagnosticEngine(const SourceManager& sources) : sources_(sources) {}

    // Each reporter returns a handle so the caller can attach notes:
    //   diags.error(loc, "...").note(other, "first declared here");
    class Builder {
    public:
        Builder(DiagnosticEngine& engine, std::size_t index) : engine_(engine), index_(index) {}
        Builder& note(SourceLocation location, std::string message);

    private:
        DiagnosticEngine& engine_;
        std::size_t index_;
    };

    Builder error(SourceLocation location, std::string message);
    Builder warning(SourceLocation location, std::string message);
    Builder note(SourceLocation location, std::string message);

    bool has_errors() const { return error_count_ > 0; }
    std::size_t error_count() const { return error_count_; }
    std::size_t warning_count() const { return warning_count_; }
    std::size_t size() const { return diagnostics_.size(); }
    const Diagnostic& operator[](std::size_t index) const { return diagnostics_[index]; }

    // Renders every diagnostic as
    //   path:line:col: error: message
    //       <source line>
    //       ^
    void print(std::FILE* stream) const;

    // Same rendering, returned as a string (used by tests).
    std::string render() const;

    void clear();

private:
    Builder report(Severity severity, SourceLocation location, std::string message);
    // `parent` suppresses re-printing the same source line for a note that
    // points at exactly where its parent diagnostic already pointed.
    void render_one(const Diagnostic& diagnostic, int depth, const SourceLocation* parent,
                    std::string& out) const;

    const SourceManager& sources_;
    khu::util::Array<Diagnostic> diagnostics_;
    std::size_t error_count_ = 0;
    std::size_t warning_count_ = 0;
};

}  // namespace khu::diag

#endif  // KHU_DIAG_DIAGNOSTIC_H
