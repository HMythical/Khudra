// Access-control pass.
//
// Khudra's rule (KHU-PLAN.md, Visibility) is object-level, not class-level:
// "private members/locals are not readable or modifiable from other loaded
// objects". That is stricter than Java -- a private field is unreachable even
// from another instance of the same class -- so the pass tracks whether an
// access goes through the *current* object or some *other* loaded object.
#ifndef KHU_SEMA_ACCESS_H
#define KHU_SEMA_ACCESS_H

#include "diag/diagnostic.h"
#include "sema/symbol.h"

namespace khu::sema {

enum class AccessContext {
    CurrentObject,  // `this.x`, or a bare `x` inside the owning object
    OtherObject,    // `other.x`, a namespace member, a call result
};

class AccessChecker {
public:
    explicit AccessChecker(diag::DiagnosticEngine& diagnostics) : diagnostics_(diagnostics) {}

    // Returns false and reports when the access is not permitted.
    bool check_field(const VarSymbol& field, AccessContext context, bool writing,
                     diag::SourceLocation loc);
    bool check_method(const MethodSymbol& method, AccessContext context,
                      diag::SourceLocation loc);
    // Locals live in the current frame, so every access to one is a
    // current-object access. Kept explicit so the rule is visible where it is
    // applied rather than only in a comment.
    bool check_local(const VarSymbol& local, AccessContext context, bool writing,
                     diag::SourceLocation loc);

private:
    diag::DiagnosticEngine& diagnostics_;
};

}  // namespace khu::sema

#endif  // KHU_SEMA_ACCESS_H
