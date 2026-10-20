#ifndef FLECHA_SEMANTIC_SCOPE_HPP
#define FLECHA_SEMANTIC_SCOPE_HPP

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "symbol.hpp"

namespace flecha::semantic {

class Scope final {
   public:
    explicit Scope(const Scope* parent = nullptr) : _parent(parent) {}

    [[nodiscard]] const Scope* Parent() const { return _parent; }
    [[nodiscard]] bool Declare(const Symbol& symbol);
    bool DeclareAs(std::string name, const Symbol& symbol);
    [[nodiscard]] const Symbol* LookupLocal(std::string_view name) const;
    [[nodiscard]] const Symbol* Lookup(std::string_view name) const;
    [[nodiscard]] std::optional<std::string> NameFor(
        const Symbol& symbol) const;

   private:
    const Scope* _parent;
    std::unordered_map<std::string, const Symbol*> _symbols;
};

}  // namespace flecha::semantic

#endif  // FLECHA_SEMANTIC_SCOPE_HPP
