#include "semantic/scope.hpp"

#include <string>

namespace flecha::semantic {

bool Scope::Declare(const Symbol& symbol) {
    return _symbols.emplace(symbol.Name(), &symbol).second;
}

bool Scope::DeclareAs(std::string name, const Symbol& symbol) {
    return _symbols.emplace(std::move(name), &symbol).second;
}

const Symbol* Scope::LookupLocal(std::string_view name) const {
    const std::unordered_map<std::string, const Symbol*>::const_iterator found =
        _symbols.find(std::string(name));
    return found == _symbols.end() ? nullptr : found->second;
}

const Symbol* Scope::Lookup(std::string_view name) const {
    const Symbol* symbol = LookupLocal(name);
    if (symbol != nullptr) return symbol;
    return _parent == nullptr ? nullptr : _parent->Lookup(name);
}

std::optional<std::string> Scope::NameFor(const Symbol& symbol) const {
    for (const auto& [name, candidate] : _symbols)
        if (candidate == &symbol) return name;
    return _parent == nullptr ? std::nullopt : _parent->NameFor(symbol);
}

}  // namespace flecha::semantic
