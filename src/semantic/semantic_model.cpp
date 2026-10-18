#include "semantic/semantic_model.hpp"

namespace flecha::semantic {

const Symbol* SemanticModel::SymbolFor(
    const core::NameExpression& expression) const {
    const std::unordered_map<const core::NameExpression*,
                             const Symbol*>::const_iterator found =
        _bindings.find(&expression);
    return found == _bindings.end() ? nullptr : found->second;
}

const Symbol* SemanticModel::DeclaredSymbol(
    const core::AstNode& declaration) const {
    const std::unordered_map<const core::AstNode*,
                             const Symbol*>::const_iterator found =
        _declarations.find(&declaration);
    return found == _declarations.end() ? nullptr : found->second;
}

const std::vector<const Symbol*>& SemanticModel::DeferredCaptures(
    const core::DeferStatement& statement) const {
    static const std::vector<const Symbol*> EMPTY;
    const auto found = _deferredCaptures.find(&statement);
    return found == _deferredCaptures.end() ? EMPTY : found->second;
}

const std::vector<ClosureCapture>& SemanticModel::Captures(
    const core::FunctionDeclaration& function) const {
    static const std::vector<ClosureCapture> EMPTY;
    const auto found = _closureCaptures.find(&function);
    return found == _closureCaptures.end() ? EMPTY : found->second;
}

const Symbol* SemanticModel::CaptureSource(
    const core::AnonymousFunctionExpression::Capture& capture) const {
    const auto found = _captureSources.find(&capture);
    return found == _captureSources.end() ? nullptr : found->second;
}

const Symbol* SemanticModel::DestructureTarget(
    const core::DestructureBinding& binding) const {
    const auto found = _destructureTargets.find(&binding);
    return found == _destructureTargets.end() ? nullptr : found->second;
}

SemanticType SemanticModel::TypeOf(const core::Expression& expression) const {
    const std::unordered_map<const core::Expression*,
                             SemanticType>::const_iterator found =
        _types.find(&expression);
    return found == _types.end() ? SemanticType::Error() : found->second;
}

TransferKind SemanticModel::TransferOf(
    const core::Expression& expression) const {
    const auto found = _transfers.find(&expression);
    return found == _transfers.end() ? TransferKind::Copy : found->second;
}

}  // namespace flecha::semantic
