// Memory-model checker (docs/memory-model.md).
//
// Owns everything about how a class chooses its allocation strategy, what its
// slots mean to the collector, and which `free`s are provably wrong.
#ifndef KHU_SEMA_MEMORY_MODEL_H
#define KHU_SEMA_MEMORY_MODEL_H

#include <string>
#include <string_view>

#include "diag/diagnostic.h"
#include "parser/ast.h"
#include "sema/symbol.h"
#include "sema/type.h"
#include "util/array.h"

namespace khu::sema {

class MemoryModel {
public:
    MemoryModel(TypeContext& types, diag::DiagnosticEngine& diagnostics)
        : types_(types), diagnostics_(diagnostics) {}

    // Validates the class's `MemoryAllocationTypeObject type` field and sets
    // ClassSymbol::strategy. A class with no such field is collected.
    void resolve_class_strategy(ClassSymbol& symbol);

    // Tags every slot in the class layout with its reference kind. Must run
    // after every class's strategy is resolved, because a slot holding a manual
    // class is a MANUAL_REF while the same slot holding a GC class is a
    // MANAGED_REF.
    void build_reference_map(ClassSymbol& symbol);

    // Resolves an allocation site's strategy from the class default and the
    // optional `manual` / `standard` override token.
    Strategy resolve_allocation_strategy(const ClassSymbol& target,
                                         const ast::CallExpr& site) const;

private:
    // True when `init` is exactly MemoryAllocationTypeObject.setStandard() or
    // .setManual(); reports and returns false otherwise.
    bool read_strategy_initializer(const ast::Expr* init, Strategy& out,
                                   diag::SourceLocation loc);

    TypeContext& types_;
    diag::DiagnosticEngine& diagnostics_;
};

// Intra-method pin analysis for `free` / `dispose`.
//
// Full pin tracking is a runtime property (Phase 5 counts pins in the object
// header). What is decidable here is the common, obviously-wrong case: a local
// that was stored into a managed object's slot and then freed while that slot
// still holds it.
class PinTracker {
public:
    struct PinSite {
        std::string holder;  // e.g. "this.head" -- named in the diagnostic
        diag::SourceLocation loc;
    };

    // Records that `manual_local` was assigned into the managed slot `holder`.
    void pin(const VarSymbol* manual_local, std::string holder, diag::SourceLocation loc);
    // Records that `holder` no longer refers to whatever it held.
    void release_slot(std::string_view holder);

    const PinSite* find_pin(const VarSymbol* manual_local) const;

    // Returns the location of an earlier free of the same local, or nullptr.
    const diag::SourceLocation* find_free(const VarSymbol* local) const;
    void record_free(const VarSymbol* local, diag::SourceLocation loc);

    void clear();

private:
    struct Entry {
        const VarSymbol* local;
        PinSite site;
    };
    struct FreeEntry {
        const VarSymbol* local;
        diag::SourceLocation loc;
    };

    util::Array<Entry> pins_;
    util::Array<FreeEntry> frees_;
};

// Renders a stable textual name for an assignment target, used both as the
// PinTracker key and in diagnostics ("this.head", "node.next").
std::string describe_slot(const ast::Expr* target);

}  // namespace khu::sema

#endif  // KHU_SEMA_MEMORY_MODEL_H
