#include "semantic/semantic_model.hpp"

#include <algorithm>

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

const core::AnonymousFunctionExpression::Capture* SemanticModel::CaptureFor(
    const Symbol& variable) const {
    const auto found = _captureVariables.find(&variable);
    return found == _captureVariables.end() ? nullptr : found->second;
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

std::vector<const core::Expression*> SemanticModel::ArgumentsFor(
    const core::CallExpression& expression) const {
    if (const auto found = _callArguments.find(&expression);
        found != _callArguments.end())
        return found->second;
    std::vector<const core::Expression*> arguments;
    arguments.reserve(expression.Arguments().size());
    for (const auto& argument : expression.Arguments())
        arguments.push_back(argument.get());
    return arguments;
}

std::vector<const core::Expression*> SemanticModel::ArgumentsFor(
    const core::MemberCallExpression& expression) const {
    if (const auto found = _callArguments.find(&expression);
        found != _callArguments.end())
        return found->second;
    std::vector<const core::Expression*> arguments;
    arguments.reserve(expression.Arguments().size());
    for (const auto& argument : expression.Arguments())
        arguments.push_back(argument.get());
    return arguments;
}

const FunctionSymbol* SemanticModel::ForwardedFunction(
    const core::CallExpression& expression) const {
    const auto found = _forwardedFunctions.find(&expression);
    return found == _forwardedFunctions.end() ? nullptr : found->second;
}

const core::ModuleConstantDeclaration* SemanticModel::ModuleConstantFor(
    const Symbol& symbol) const {
    const auto found = _moduleConstants.find(&symbol);
    return found == _moduleConstants.end() ? nullptr : found->second;
}

std::vector<const InterfaceType*> InterfaceConformanceClosure(
    const std::vector<InterfaceReference>& roots) {
    std::vector<const InterfaceType*> result;
    const auto add = [&](const InterfaceType* type) {
        if (std::find(result.begin(), result.end(), type) == result.end())
            result.push_back(type);
    };
    for (const auto& root : roots) {
        add(root.type);
        for (const auto* ancestor : root.type->ancestors) add(ancestor);
    }
    return result;
}

source::SourceRange SemanticModel::SourceOrigin(
    source::SourceRange range) const {
    for (const auto& plan : _behaviorPlans)
        for (const auto& body : plan.bodies)
            for (const auto& origin : body.provenance)
                if (origin.expansion.start.file == range.start.file &&
                    origin.expansion.start.offset <= range.start.offset &&
                    range.start.offset < origin.expansion.end.offset)
                    return origin.original;
    return range;
}

}  // namespace flecha::semantic
