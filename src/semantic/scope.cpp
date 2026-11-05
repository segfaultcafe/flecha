#include "semantic/scope.hpp"

#include <string>

namespace flecha::semantic {

bool Scope::Declare(const Symbol& symbol) {
    return _symbols.emplace(symbol.Name(),
                            std::vector<const Symbol*>{&symbol})
        .second;
}

bool Scope::DeclareAs(std::string name, const Symbol& symbol) {
    return _symbols.emplace(std::move(name),
                            std::vector<const Symbol*>{&symbol})
        .second;
}

bool Scope::DeclareOverloadAs(std::string name,
                              const FunctionSymbol& symbol) {
    auto found = _symbols.find(name);
    if (found == _symbols.end()) {
        _symbols.emplace(std::move(name),
                         std::vector<const Symbol*>{&symbol});
        return true;
    }
    for (const Symbol* candidate : found->second)
        if (candidate->Kind() != SymbolKind::Function) return false;
    found->second.push_back(&symbol);
    return true;
}

const Symbol* Scope::LookupLocal(std::string_view name) const {
    const auto found = _symbols.find(std::string(name));
    return found == _symbols.end() || found->second.empty()
               ? nullptr
               : found->second.front();
}

const Symbol* Scope::Lookup(std::string_view name) const {
    const Symbol* symbol = LookupLocal(name);
    if (symbol != nullptr) return symbol;
    return _parent == nullptr ? nullptr : _parent->Lookup(name);
}

const std::vector<const Symbol*>& Scope::LookupAllLocal(
    std::string_view name) const {
    static const std::vector<const Symbol*> EMPTY;
    const auto found = _symbols.find(std::string(name));
    return found == _symbols.end() ? EMPTY : found->second;
}

const std::vector<const Symbol*>& Scope::LookupAll(
    std::string_view name) const {
    const auto& symbols = LookupAllLocal(name);
    if (!symbols.empty()) return symbols;
    if (_parent != nullptr) return _parent->LookupAll(name);
    static const std::vector<const Symbol*> EMPTY;
    return EMPTY;
}

std::optional<std::string> Scope::NameFor(const Symbol& symbol) const {
    for (const auto& [name, candidates] : _symbols)
        for (const Symbol* candidate : candidates)
            if (candidate == &symbol) return name;
    return _parent == nullptr ? std::nullopt : _parent->NameFor(symbol);
}

}  // namespace flecha::semantic
