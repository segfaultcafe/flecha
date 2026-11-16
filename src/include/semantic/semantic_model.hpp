#ifndef FLECHA_SEMANTIC_SEMANTIC_MODEL_HPP
#define FLECHA_SEMANTIC_SEMANTIC_MODEL_HPP

#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "core/ast.hpp"
#include "scope.hpp"

namespace flecha::analyzer {
class SemanticQueries;
}

namespace flecha::semantic {

class SemanticAnalyzer;
class BehaviorPlanner;
// Internal success-only binding contract, never a source-level tuple/ABI.
struct CheckedAsyncResult final {
    const Symbol* value = nullptr;
    const Symbol* error = nullptr;
    bool creation = false;
};

class SemanticModel;

enum class TransferKind { Copy, Move, Direct };

struct ClosureCapture final {
    const core::AnonymousFunctionExpression::Capture* syntax;
    const Symbol* source;
    const VariableSymbol* variable;
};

// Declaration-driven metadata. No runtime data or derived methods are created.
struct DecorationParameterContract final {
    const core::DecorationParameter* syntax;
    SemanticType type;
};

struct DecorationApplicationContract final {
    const core::DecorationSignature* syntax;
    std::vector<DecorationParameterContract> parameters;
    bool valid = true;
};

struct DecorationContract final {
    const core::DecorationDeclaration* declaration;
    std::vector<DecorationApplicationContract> applications;
};

struct DecorationMetadata final {
    const core::DecorationDeclaration* declaration;
    const core::DecorationSignature* signature;
    const core::DecorationApplication* application;
    // Field identity is preserved even when several fields have the same type.
    const core::AstNode* target;
    const core::Declaration* owner;
};

struct BehaviorMethodContract final {
    const core::FunctionDeclaration* declaration;
    MethodContract signature;
    const MethodContract* requirement = nullptr;
};

struct BehaviorContract final {
    const core::BehaviorDeclaration* declaration;
    const core::DecorationDeclaration* decoration = nullptr;
    std::vector<InterfaceReference> roots;
    std::vector<BehaviorMethodContract> methods;
    bool valid = true;
};

struct BehaviorActivationCandidate final {
    const BehaviorContract* behavior;
    const core::Declaration* target;
    // Multiple direct fields and a declaration application activate once.
    std::vector<const DecorationMetadata*> applications;
    bool valid = true;
};

// Typed compiler values use the same closed scalar domain as module constants.
struct EnumMetadataValue final {
    const EnumType* type;
    std::uint64_t value;
    bool operator==(const EnumMetadataValue&) const = default;
};
struct ErrorMetadataValue final {
    std::string family;
    std::uint64_t code;
    bool operator==(const ErrorMetadataValue&) const = default;
};
struct CompileTimeValue final {
    SemanticType type = SemanticType::Error();
    std::variant<std::uint64_t, double, bool, char, std::string,
                 EnumMetadataValue, ErrorMetadataValue>
        value;
};
struct DecorationRecordMember final {
    const core::DecorationParameter* parameter;
    CompileTimeValue value;
};
struct TypedDecorationRecord final {
    const DecorationMetadata* application;
    std::vector<DecorationRecordMember> members;
};
struct BehaviorFieldDescriptor final {
    // Identity includes the closed owner and declaration, never merely F.
    const StructureType* owner;
    const StructureField* field;
    SemanticType type;
    core::OwnershipContract ownership;
    std::optional<TypedDecorationRecord> metadata;
};
struct BehaviorFieldPack final {
    std::vector<BehaviorFieldDescriptor> fields;
};
struct BehaviorReflectionOccurrence final {
    source::SourceRange range, definition;
    std::string name, description;
    bool type = false;
    bool operation = false;
};
struct BehaviorSubstitution final {
    source::SourceRange range;
    const StructureField* field = nullptr;
    std::optional<CompileTimeValue> value;
};
struct BehaviorSourceProvenance final {
    source::SourceRange expansion, original;
    const StructureField* field = nullptr;
};
struct BehaviorBodyCandidate final {
    std::vector<BehaviorSourceProvenance> provenance;
    // Owned by GeneratedFunctions after materialization. The plan retains the
    // exact source/field provenance used by ordinary analysis and tooling.
    const core::FunctionDeclaration* declaration = nullptr;
    std::vector<std::pair<source::SourceRange, const StructureField*>>
        projections;
};
struct BehaviorExpansionPlan final {
    const BehaviorActivationCandidate* activation;
    SemanticType target;
    std::optional<TypedDecorationRecord> metadata;
    BehaviorFieldPack pack;
    std::vector<BehaviorSubstitution> substitutions;
    std::vector<BehaviorBodyCandidate> bodies;
    bool valid = true;
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
    [[nodiscard]] SemanticType ImplicitInterfaceConversionFor(
        const core::Expression& expression) const {
        const auto found = _implicitInterfaceConversions.find(&expression);
        return found == _implicitInterfaceConversions.end()
                   ? SemanticType::Error()
                   : found->second;
    }
    [[nodiscard]] bool IsBuildInitializer(
        const core::AssignmentExpression& expression) const {
        return _buildInitializers.contains(&expression);
    }
    [[nodiscard]] std::vector<const core::Expression*> ArgumentsFor(
        const core::CallExpression& expression) const;
    [[nodiscard]] std::vector<const core::Expression*> ArgumentsFor(
        const core::MemberCallExpression& expression) const;
    [[nodiscard]] const FunctionSymbol* ForwardedFunction(
        const core::CallExpression& expression) const;
    [[nodiscard]] const core::ModuleConstantDeclaration* ModuleConstantFor(
        const Symbol& symbol) const;
    const StructureField* FieldFor(
        const core::MemberAccessExpression& expression) const {
        const auto found = _fields.find(&expression);
        return found == _fields.end() ? nullptr : found->second;
    }
    const Symbol* CheckedErrorFor(const Symbol& value) const {
        for (const auto& [statement, checked] : _checkedAsyncResults)
            if (checked.value == &value) return checked.error;
        return nullptr;
    }
    const CheckedAsyncResult* CheckedResultFor(
        const core::DestructureStatement& statement) const {
        const auto found = _checkedAsyncResults.find(&statement);
        return found == _checkedAsyncResults.end() ? nullptr : &found->second;
    }
    const VariableSymbol* TaskFor(const core::SpawnExpression& expression) const {
        const auto found = _asyncTasks.find(&expression);
        return found == _asyncTasks.end() ? nullptr : found->second;
    }
    const FunctionSymbol* MethodFor(
        const core::MemberCallExpression& expression) const {
        const auto found = _methods.find(&expression);
        return found == _methods.end() ? nullptr : found->second;
    }
    [[nodiscard]] bool ConditionallyTransfersAppend(
        const core::MemberCallExpression& expression) const {
        return _conditionalAppendTransfers.contains(&expression);
    }
    const MethodContract* InterfaceMethodFor(
        const core::MemberCallExpression& expression) const {
        const auto found = _interfaceMethods.find(&expression);
        return found == _interfaceMethods.end() ? nullptr : found->second;
    }
    const VariadicShape* InterfaceVariadicShapeFor(
        const core::MemberCallExpression& expression) const {
        const auto found = _interfaceVariadicCallShapes.find(&expression);
        return found == _interfaceVariadicCallShapes.end() ? nullptr
                                                           : &found->second;
    }
    const auto& InterfaceVariadicShapes() const {
        return _interfaceVariadicShapes;
    }
    const auto& VariadicTargets() const { return _variadicTargets; }
    const auto& InterfaceVariadicShapeTargets() const {
        return _interfaceVariadicShapeTargets;
    }
    [[nodiscard]] bool IsQualifiedInterfaceProviderCall(
        const core::MemberCallExpression& expression) const {
        return _qualifiedInterfaceProviderCalls.contains(&expression);
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
    const auto& Interfaces() const { return _interfaces; }
    const auto& BehaviorPlans() const { return _behaviorPlans; }
    const auto& ReflectionOccurrences() const { return _reflectionOccurrences; }
    const auto& Behaviors() const { return _behaviors; }
    const auto& BehaviorActivations() const { return _behaviorActivations; }
    const auto& DecorationContracts() const { return _decorationContracts; }
    const auto& Decorations() const { return _decorations; }
    [[nodiscard]] const DecorationContract* ContractFor(
        const core::DecorationDeclaration& declaration) const {
        const auto found = _decorationContracts.find(&declaration);
        return found == _decorationContracts.end() ? nullptr : &found->second;
    }
    [[nodiscard]] const Scope& GlobalScope() const { return *_scopes.front(); }
    [[nodiscard]] const std::vector<std::unique_ptr<Symbol>>& Symbols() const {
        return _symbols;
    }
    [[nodiscard]] const std::vector<std::unique_ptr<Scope>>& Scopes() const {
        return _scopes;
    }

    [[nodiscard]] source::SourceRange SourceOrigin(
        source::SourceRange range) const;

   private:
    friend class SemanticAnalyzer;
    friend class BehaviorPlanner;
    // Scalar substitutions owned by detached models, never input-unit
    // declarations.
    std::vector<std::unique_ptr<core::ModuleConstantDeclaration>>
        _detachedConstants;
    std::vector<BehaviorExpansionPlan> _behaviorPlans;
    std::vector<BehaviorReflectionOccurrence> _reflectionOccurrences;
    friend class analyzer::SemanticQueries;
    std::vector<BehaviorContract> _behaviors;
    std::vector<BehaviorActivationCandidate> _behaviorActivations;
    std::unordered_map<const core::DecorationDeclaration*, DecorationContract>
        _decorationContracts;
    std::vector<DecorationMetadata> _decorations;
    std::unordered_map<const core::DecorationApplication*,
                       const core::DecorationDeclaration*>
        _decorationBindings;
    std::vector<std::unique_ptr<StructureType>> _structures;
    std::vector<std::unique_ptr<EnumType>> _enums;
    std::vector<std::unique_ptr<InterfaceType>> _interfaces;
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
    std::unordered_map<const core::DestructureStatement*, CheckedAsyncResult>
        _checkedAsyncResults;
    std::unordered_map<const core::SpawnExpression*, const VariableSymbol*> _asyncTasks;
    std::unordered_map<const core::MemberCallExpression*, const FunctionSymbol*>
        _methods;
    std::unordered_set<const core::MemberCallExpression*>
        _conditionalAppendTransfers;
    std::unordered_map<const core::MemberCallExpression*, const MethodContract*>
        _interfaceMethods;
    std::unordered_map<const core::MemberCallExpression*, VariadicShape>
        _interfaceVariadicCallShapes;
    std::unordered_map<const MethodContract*, std::vector<VariadicShape>>
        _interfaceVariadicShapes;
    std::unordered_set<const core::MemberCallExpression*>
        _qualifiedInterfaceProviderCalls;
    std::vector<std::unique_ptr<Symbol>> _symbols;
    std::vector<std::unique_ptr<Scope>> _scopes;
    std::unordered_map<const core::NameExpression*, const Symbol*> _bindings;
    std::unordered_map<const core::TypeNode*, const Symbol*> _typeBindings;
    std::unordered_map<const core::AstNode*, const Symbol*> _declarations;
    std::unordered_map<const core::Expression*, SemanticType> _types;
    std::unordered_map<const core::Expression*, TransferKind> _transfers;
    std::unordered_map<const core::Expression*, SemanticType>
        _implicitInterfaceConversions;
    std::unordered_set<const core::AssignmentExpression*> _buildInitializers;
    std::unordered_map<const core::Expression*,
                       std::vector<const core::Expression*>>
        _callArguments;
    std::unordered_map<const FunctionSymbol*, const FunctionSymbol*>
        _variadicTargets;
    std::unordered_map<const FunctionSymbol*,
                       std::vector<const FunctionSymbol*>>
        _variadicTargetCandidates;
    std::unordered_map<const FunctionSymbol*,
                       std::unordered_map<std::string, const FunctionSymbol*>>
        _interfaceVariadicShapeTargets;
    std::unordered_map<const core::CallExpression*, const FunctionSymbol*>
        _forwardedFunctions;
    std::unordered_map<const Symbol*, const core::ModuleConstantDeclaration*>
        _moduleConstants;
    std::unordered_map<const core::DestructureBinding*, const Symbol*>
        _destructureTargets;
    std::unordered_map<const core::DeferStatement*, std::vector<const Symbol*>>
        _deferredCaptures;
};

}  // namespace flecha::semantic

#endif  // FLECHA_SEMANTIC_SEMANTIC_MODEL_HPP
