#ifndef FLECHA_SEMANTIC_SEMANTIC_MODEL_HPP
#define FLECHA_SEMANTIC_SEMANTIC_MODEL_HPP

#include <memory>
#include <unordered_map>
#include <vector>

#include "core/ast.hpp"
#include "scope.hpp"

namespace flecha::semantic {

class SemanticAnalyzer;

enum class TransferKind { Copy, Move, Direct };

struct ClosureCapture final {
    const core::AnonymousFunctionExpression::Capture* syntax;
    const Symbol* source;
    const VariableSymbol* variable;
};

class SemanticModel final {
   public:
    SemanticModel() = default;
    SemanticModel(const SemanticModel&) = delete;
    SemanticModel& operator=(const SemanticModel&) = delete;
    SemanticModel(SemanticModel&&) = default;
    SemanticModel& operator=(SemanticModel&&) = default;

    [[nodiscard]] const Symbol* SymbolFor(
        const core::NameExpression& expression) const;
    [[nodiscard]] const Symbol* DeclaredSymbol(
        const core::AstNode& declaration) const;
    [[nodiscard]] const std::vector<const Symbol*>& DeferredCaptures(
        const core::DeferStatement& statement) const;
    [[nodiscard]] const Symbol* DestructureTarget(
        const core::DestructureBinding& binding) const;
    [[nodiscard]] SemanticType TypeOf(const core::Expression& expression) const;
    [[nodiscard]] TransferKind TransferOf(
        const core::Expression& expression) const;
    [[nodiscard]] const core::ModuleConstantDeclaration* ModuleConstantFor(
        const Symbol& symbol) const;
    const StructureField* FieldFor(
        const core::MemberAccessExpression& expression) const {
        const auto found = _fields.find(&expression);
        return found == _fields.end() ? nullptr : found->second;
    }
    const FunctionSymbol* MethodFor(
        const core::MemberCallExpression& expression) const {
        const auto found = _methods.find(&expression);
        return found == _methods.end() ? nullptr : found->second;
    }
    const auto& GeneratedFunctions() const { return _generatedFunctions; }
    const auto& AnonymousFunctions() const { return _anonymousFunctions; }
    [[nodiscard]] const std::vector<ClosureCapture>& Captures(
        const core::FunctionDeclaration& function) const;
    [[nodiscard]] const Symbol* CaptureSource(
        const core::AnonymousFunctionExpression::Capture& capture) const;
    [[nodiscard]] const core::AnonymousFunctionExpression::Capture* CaptureFor(
        const Symbol& variable) const;
    const auto& Structures() const { return _structures; }
    const auto& Enums() const { return _enums; }
    [[nodiscard]] const Scope& GlobalScope() const { return *_scopes.front(); }
    [[nodiscard]] const std::vector<std::unique_ptr<Symbol>>& Symbols() const {
        return _symbols;
    }
    [[nodiscard]] const std::vector<std::unique_ptr<Scope>>& Scopes() const {
        return _scopes;
    }

   private:
    friend class SemanticAnalyzer;
    std::vector<std::unique_ptr<StructureType>> _structures;
    std::vector<std::unique_ptr<EnumType>> _enums;
    std::vector<std::unique_ptr<core::FunctionDeclaration>> _generatedFunctions;
    std::vector<const core::FunctionDeclaration*> _anonymousFunctions;
    std::unordered_map<const core::FunctionDeclaration*,
                       std::vector<ClosureCapture>>
        _closureCaptures;
    std::unordered_map<const core::AnonymousFunctionExpression::Capture*,
                       const Symbol*>
        _captureSources;
    std::unordered_map<const Symbol*,
                       const core::AnonymousFunctionExpression::Capture*>
        _captureVariables;
    std::unordered_map<const core::MemberAccessExpression*,
                       const StructureField*>
        _fields;
    std::unordered_map<const core::MemberCallExpression*, const FunctionSymbol*>
        _methods;
    std::vector<std::unique_ptr<Symbol>> _symbols;
    std::vector<std::unique_ptr<Scope>> _scopes;
    std::unordered_map<const core::NameExpression*, const Symbol*> _bindings;
    std::unordered_map<const core::AstNode*, const Symbol*> _declarations;
    std::unordered_map<const core::Expression*, SemanticType> _types;
    std::unordered_map<const core::Expression*, TransferKind> _transfers;
    std::unordered_map<const Symbol*, const core::ModuleConstantDeclaration*>
        _moduleConstants;
    std::unordered_map<const core::DestructureBinding*, const Symbol*>
        _destructureTargets;
    std::unordered_map<const core::DeferStatement*, std::vector<const Symbol*>>
        _deferredCaptures;
};

}  // namespace flecha::semantic

#endif  // FLECHA_SEMANTIC_SEMANTIC_MODEL_HPP
