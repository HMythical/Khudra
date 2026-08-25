#include "sema/memory_model.h"

namespace khu::sema {

namespace {

// The two factories that may initialize a `MemoryAllocationTypeObject` field.
constexpr std::string_view kSetStandard = "setStandard";
constexpr std::string_view kSetManual = "setManual";

}  // namespace

std::string describe_slot(const ast::Expr* target) {
    if (!target) return "<slot>";
    switch (target->kind) {
        case ast::ExprKind::Identifier:
            return std::string(static_cast<const ast::IdentifierExpr*>(target)->name);
        case ast::ExprKind::This:
            return "this";
        case ast::ExprKind::Member: {
            const auto& member = *static_cast<const ast::MemberExpr*>(target);
            return describe_slot(member.object) + "." + std::string(member.name);
        }
        case ast::ExprKind::Index:
            return describe_slot(static_cast<const ast::IndexExpr*>(target)->object) + "[...]";
        default:
            return "<slot>";
    }
}

bool MemoryModel::read_strategy_initializer(const ast::Expr* init, Strategy& out,
                                            diag::SourceLocation loc) {
    const auto* call = init ? init->as<ast::CallExpr>() : nullptr;
    const ast::MemberExpr* member = call ? call->callee->as<ast::MemberExpr>() : nullptr;
    const ast::TypeRefExpr* receiver = member ? member->object->as<ast::TypeRefExpr>() : nullptr;

    bool well_formed = call && member && receiver && receiver->type &&
                       receiver->type->kind == ast::TypeNode::Kind::Builtin &&
                       receiver->type->builtin == ast::BuiltinType::MemoryAllocationTypeObject &&
                       call->args.empty() && !call->has_strategy_token;

    if (well_formed && member->name == kSetStandard) {
        out = Strategy::Gc;
        return true;
    }
    if (well_formed && member->name == kSetManual) {
        out = Strategy::Manual;
        return true;
    }

    diagnostics_
        .error(init ? init->loc : loc,
               "a MemoryAllocationTypeObject field must be initialized with "
               "MemoryAllocationTypeObject.setStandard() or "
               "MemoryAllocationTypeObject.setManual()")
        .note(loc, "the strategy is read at allocation time and fixed at materialization, so it "
                   "must be known from the declaration");
    return false;
}

void MemoryModel::resolve_class_strategy(ClassSymbol& symbol) {
    VarSymbol* strategy_field = nullptr;
    for (VarSymbol* field : symbol.fields) {
        if (!field->type || !field->type->is_memory()) continue;
        if (strategy_field) {
            diagnostics_
                .error(field->loc, "class '" + std::string(symbol.name) +
                                       "' declares more than one MemoryAllocationTypeObject field")
                .note(strategy_field->loc, "the first one is declared here");
            continue;
        }
        strategy_field = field;
    }

    symbol.strategy_field = strategy_field;
    if (!strategy_field) {
        // No declaration means the class default: garbage collected.
        symbol.strategy = Strategy::Gc;
        return;
    }

    strategy_field->is_strategy_field = true;
    Strategy strategy = Strategy::Gc;
    if (read_strategy_initializer(strategy_field->field_decl
                                      ? strategy_field->field_decl->init
                                      : nullptr,
                                  strategy, strategy_field->loc)) {
        symbol.strategy = strategy;
    } else {
        symbol.strategy = Strategy::Gc;
    }
}

void MemoryModel::build_reference_map(ClassSymbol& symbol) {
    for (VarSymbol* field : symbol.layout) {
        field->ref_kind = types_.ref_kind_of(field->type);
    }
}

Strategy MemoryModel::resolve_allocation_strategy(const ClassSymbol& target,
                                                  const ast::CallExpr& site) const {
    if (!site.has_strategy_token) return target.strategy;
    // The site override always wins over the class default.
    return site.strategy == ast::Strategy::Manual ? Strategy::Manual : Strategy::Gc;
}

// ---------------------------------------------------------------------------
// PinTracker
// ---------------------------------------------------------------------------

void PinTracker::pin(const VarSymbol* manual_local, std::string holder,
                     diag::SourceLocation loc) {
    if (!manual_local) return;
    release_slot(holder);
    pins_.push(Entry{manual_local, PinSite{std::move(holder), loc}});
}

void PinTracker::release_slot(std::string_view holder) {
    util::Array<Entry> kept;
    for (const Entry& entry : pins_) {
        if (entry.site.holder != holder) kept.push(entry);
    }
    pins_ = std::move(kept);
}

const PinTracker::PinSite* PinTracker::find_pin(const VarSymbol* manual_local) const {
    for (const Entry& entry : pins_) {
        if (entry.local == manual_local) return &entry.site;
    }
    return nullptr;
}

const diag::SourceLocation* PinTracker::find_free(const VarSymbol* local) const {
    for (const FreeEntry& entry : frees_) {
        if (entry.local == local) return &entry.loc;
    }
    return nullptr;
}

void PinTracker::record_free(const VarSymbol* local, diag::SourceLocation loc) {
    if (!local) return;
    frees_.push(FreeEntry{local, loc});
}

void PinTracker::clear() {
    pins_.clear();
    frees_.clear();
}

}  // namespace khu::sema
