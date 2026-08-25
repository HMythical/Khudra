#include "sema/symbol.h"

namespace khu::sema {

const char* strategy_name(Strategy strategy) {
    return strategy == Strategy::Manual ? "manual" : "standard";
}

bool MethodSymbol::signature_matches(const MethodSymbol& other) const {
    if (name != other.name) return false;
    if (params.size() != other.params.size()) return false;
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (params[i]->type != other.params[i]->type) return false;
    }
    return true;
}

VarSymbol* ClassSymbol::find_field(std::string_view field_name) {
    for (ClassSymbol* current = this; current; current = current->base) {
        if (VarSymbol** found = current->field_index.find(field_name)) return *found;
    }
    return nullptr;
}

const VarSymbol* ClassSymbol::find_field(std::string_view field_name) const {
    for (const ClassSymbol* current = this; current; current = current->base) {
        if (const VarSymbol* const* found = current->field_index.find(field_name)) return *found;
    }
    return nullptr;
}

void ClassSymbol::find_methods(std::string_view method_name, util::Array<MethodSymbol*>& out) {
    for (ClassSymbol* current = this; current; current = current->base) {
        if (util::Array<MethodSymbol*>* found = current->method_index.find(method_name)) {
            for (MethodSymbol* method : *found) {
                // A derived override shadows the base declaration.
                bool shadowed = false;
                for (MethodSymbol* existing : out) {
                    if (existing->signature_matches(*method)) {
                        shadowed = true;
                        break;
                    }
                }
                if (!shadowed) out.push(method);
            }
        }
    }
}

bool ClassSymbol::derives_from(const ClassSymbol* other) const {
    for (const ClassSymbol* current = this; current; current = current->base) {
        if (current == other) return true;
    }
    return false;
}

VarSymbol* Scope::declare(VarSymbol* symbol) {
    if (names_.find(symbol->name)) return nullptr;
    names_.insert(symbol->name, symbol);
    return symbol;
}

VarSymbol* Scope::lookup(std::string_view name) {
    for (Scope* scope = this; scope; scope = scope->parent_) {
        if (VarSymbol** found = scope->names_.find(name)) return *found;
    }
    return nullptr;
}

VarSymbol* Scope::lookup_local(std::string_view name) {
    VarSymbol** found = names_.find(name);
    return found ? *found : nullptr;
}

}  // namespace khu::sema
