#include "sema/access.h"

#include <string>

namespace khu::sema {

bool AccessChecker::check_field(const VarSymbol& field, AccessContext context, bool writing,
                                diag::SourceLocation loc) {
    if (context == AccessContext::CurrentObject) return true;
    if (field.visibility == ast::Visibility::Public) return true;

    std::string owner = field.owner ? std::string(field.owner->name) : std::string("<class>");
    diagnostics_
        .error(loc, std::string(writing ? "cannot modify" : "cannot read") + " private field '" +
                        owner + "." + std::string(field.name) + "' from another object")
        .note(field.loc, "declared private here; mark it public to allow access from other "
                         "loaded objects");
    return false;
}

bool AccessChecker::check_method(const MethodSymbol& method, AccessContext context,
                                 diag::SourceLocation loc) {
    if (context == AccessContext::CurrentObject) return true;
    if (method.visibility == ast::Visibility::Public) return true;

    std::string owner = method.owner ? std::string(method.owner->name) : std::string("<class>");
    const char* form = method.form == ast::MethodForm::Method ? "method" : "function";
    diagnostics_
        .error(loc, "cannot call private " + std::string(form) + " '" + owner + "." +
                        std::string(method.name) + "' from another object")
        .note(method.loc, method.form == ast::MethodForm::Method
                              ? "'method' is private by default; use 'func' or an explicit "
                                "'public' modifier"
                              : "declared private here");
    return false;
}

bool AccessChecker::check_local(const VarSymbol& local, AccessContext context, bool writing,
                                diag::SourceLocation loc) {
    // A local is only ever nameable from inside the frame that declared it, so
    // this always holds. The check exists so that the invariant is enforced
    // rather than assumed, and so a future feature that hands a frame to
    // another object trips over it here.
    if (context == AccessContext::CurrentObject) return true;

    diagnostics_.error(loc, std::string(writing ? "cannot modify" : "cannot read") + " local '" +
                                std::string(local.name) + "' from another object");
    return false;
}

}  // namespace khu::sema
