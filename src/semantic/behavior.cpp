#include <algorithm>
#include <set>

#include "semantic/semantic_analyzer.hpp"

namespace flecha::semantic {
namespace {

// The same root rules apply to a behavior alone and to its prospective union
// with explicit conformance. This never writes a concrete type's graph.
bool ConflictingRoots(const InterfaceType& left, const InterfaceType& right) {
    if (&left == &right) return true;
    const auto contains = [](const auto& values, const auto* value) {
        return std::find(values.begin(), values.end(), value) != values.end();
    };
    if (contains(left.ancestors, &right) || contains(right.ancestors, &left))
        return true;
    for (const auto* ancestor : left.ancestors)
        if (contains(right.ancestors, ancestor)) return true;
    for (const auto* first : left.methods)
        for (const auto* second : right.methods)
            if (first->key == second->key) return true;
    return false;
}

bool ContainsTargetParameter(const core::TypeNode& type,
                             const std::string& name) {
    if (type.TypeForm() == core::TypeKind::Named && type.Name() == name)
        return true;
    if (type.Target() && ContainsTargetParameter(*type.Target(), name))
        return true;
    for (const auto& element : type.Elements())
        if (ContainsTargetParameter(*element, name)) return true;
    for (const auto& argument : type.typeArguments)
        if (ContainsTargetParameter(*argument, name)) return true;
    return false;
}

std::vector<core::OwnershipContract> BehaviorResultContracts(
    const core::TypeNode& type) {
    std::vector<core::OwnershipContract> result;
    if (type.TypeForm() == core::TypeKind::Tuple) {
        for (const auto& element : type.Elements())
            result.push_back(element->Contract());
    } else if (type.TypeForm() != core::TypeKind::Primitive ||
               type.Type() != core::PrimitiveType::Void)
        result.push_back(type.Contract());
    return result;
}

}  // namespace

void SemanticAnalyzer::RegisterBehaviors(const core::CompilationUnit& unit) {
    using core::AstNodeKind;
    using core::DecorationTarget;
    using diagnostics::DiagnosticCode;
    // Traverse source order, never unordered semantic tables, for diagnostics
    // and stable activation/contract ordering.
    const auto findDecoration = [&](const std::string& module,
                                    const std::string& name) {
        const core::DecorationDeclaration* result = nullptr;
        for (const auto& declaration : unit.Declarations())
            if (declaration->moduleId == module &&
                declaration->Kind() == AstNodeKind::DecorationDeclaration) {
                const auto& decoration =
                    static_cast<const core::DecorationDeclaration&>(
                        *declaration);
                if (decoration.Name() == name) {
                    result = &decoration;
                    break;
                }
            }
        return result;
    };
    const auto conflict = [&](const InterfaceReference& left,
                              const InterfaceReference& right) {
        if (!ConflictingRoots(*left.type, *right.type)) return false;
        _diagnostics.Report(
            DiagnosticCode::ConflictingBehaviorRoots, right.range,
            "conflicting behavior roots '" + left.type->displayName +
                "' and '" + right.type->displayName + "'",
            diagnostics::DiagnosticSeverity::Error,
            {{left.range, "previous interface root"}});
        return true;
    };
    for (const auto& declaration : unit.Declarations()) {
        if (declaration->Kind() != AstNodeKind::BehaviorDeclaration) continue;
        const auto& syntax =
            static_cast<const core::BehaviorDeclaration&>(*declaration);
        SetSource(syntax.sourceId);
        BehaviorContract contract{&syntax};
        contract.valid =
            syntax.validParameter && syntax.target != DecorationTarget::Invalid;
        const auto invalid = [&](DiagnosticCode code, source::SourceRange range,
                                 std::string message) {
            Report(code, range, std::move(message));
            contract.valid = false;
        };
        const auto& application = syntax.decoration;
        if (application.path.size() == 1)
            contract.decoration =
                findDecoration(syntax.moduleId, application.Name());
        if (!contract.decoration && application.path.size() <= 2) {
            for (const auto& module : unit.modules) {
                if (module.sourceId != syntax.sourceId) continue;
                for (const auto& import : module.imports) {
                    if (import.alias != application.path.front()) continue;
                    const auto* candidate = findDecoration(
                        import.resolvedModule, application.Name());
                    if (!candidate) continue;
                    if (contract.decoration &&
                        contract.decoration != candidate) {
                        contract.decoration = nullptr;
                        break;
                    }
                    contract.decoration = candidate;
                }
            }
        }
        if (!contract.decoration)
            invalid(DiagnosticCode::InvalidBehavior, application.range,
                    "unknown owning decoration '" + application.Name() + "'");
        else {
            _model._decorationBindings.emplace(&application,
                                               contract.decoration);
            if (contract.decoration->moduleId != syntax.moduleId)
                invalid(DiagnosticCode::InvalidBehavior, application.range,
                        "behavior and owning decoration must share a logical "
                        "module");
            if (!contract.decoration->HasBody())
                invalid(DiagnosticCode::InvalidBehavior, application.range,
                        "behavior requires a typed decoration contract");
            bool applicable = false;
            const DecorationContract* owner =
                _model.ContractFor(*contract.decoration);
            if (owner)
                for (const auto& signature : owner->applications)
                    applicable =
                        applicable ||
                        (signature.valid &&
                         (signature.syntax->target == syntax.target ||
                          (syntax.target == DecorationTarget::Structure &&
                           signature.syntax->target ==
                               DecorationTarget::StructureField)));
            if (!applicable)
                invalid(DiagnosticCode::InvalidBehavior, syntax.targetRange,
                        "owning decoration has no application that can "
                        "activate this target kind");
        }
        if (syntax.roots.empty())
            invalid(DiagnosticCode::InvalidBehavior, syntax.targetRange,
                    "behavior must promise at least one interface root");
        for (const auto& root : syntax.roots) {
            if (ReportAmbiguousImport(root.name, root.range)) {
                contract.valid = false;
                continue;
            }
            const Symbol* symbol = _currentScope->Lookup(root.name);
            if (!symbol || symbol->Kind() != SymbolKind::InterfaceType) {
                invalid(
                    DiagnosticCode::InvalidBehavior, root.range,
                    "unknown, inaccessible or non-interface behavior root '" +
                        root.name + "'");
                continue;
            }
            InterfaceReference reference{symbol->Type().Interface(),
                                         root.range};
            for (const auto& prior : contract.roots)
                if (conflict(prior, reference)) contract.valid = false;
            contract.roots.push_back(reference);
        }
        for (const auto& method : syntax.methods) {
            const MethodContract* requirement = nullptr;
            const auto invalidMethod = [&](source::SourceRange range,
                                           std::string message) {
                std::vector<diagnostics::RelatedLocation> related{
                    {syntax.decoration.range, "behavior declared here"}};
                if (requirement)
                    related.push_back(
                        {requirement->syntax->Signature().NameRange(),
                         "interface requirement declared here"});
                _diagnostics.Report(DiagnosticCode::InvalidBehaviorMethod,
                                    range, std::move(message),
                                    diagnostics::DiagnosticSeverity::Error,
                                    std::move(related));
                contract.valid = false;
            };
            const std::size_t before = _diagnostics.Diagnostics().size();
            if (method->Owner() != syntax.targetParameter ||
                !method->ownerParameters.empty() ||
                !method->genericParameters.empty() || method->IsExtern() ||
                method->HasModifier(core::FunctionModifier::Private))
                invalidMethod(method->NameRange(),
                              "behavior methods require the target owner and "
                              "cannot be generic, private or extern");
            std::vector<SemanticType> parameters;
            std::vector<core::OwnershipContract> ownership;
            std::set<std::string> names;
            bool receiver = false;
            for (std::size_t index = 0; index < method->Parameters().size();
                 ++index) {
                const auto& parameter = *method->Parameters()[index];
                const auto& type = parameter.Type();
                SemanticType resolved = SemanticType::Error();
                if (index == 0 && parameter.Name() == "self") {
                    receiver =
                        type.TypeForm() == core::TypeKind::Reference &&
                        type.Target()->TypeForm() == core::TypeKind::Named &&
                        type.Target()->Name() == syntax.targetParameter &&
                        type.Target()->typeArguments.empty() &&
                        type.Contract() == core::OwnershipContract::None;
                    if (receiver)
                        // Normalization erases the receiver identity. No
                        // nominal target or runtime symbol is manufactured for
                        // the binder.
                        resolved =
                            SemanticType(SemanticTypeKind::Reference,
                                         SemanticType(SemanticTypeKind::Void,
                                                      type.Target()->IsConst()),
                                         type.IsConst());
                } else if (ContainsTargetParameter(type,
                                                   syntax.targetParameter)) {
                    invalidMethod(type.Range(),
                                  "target-dependent parameter cannot match a "
                                  "declared interface signature");
                } else
                    resolved = ResolveType(type);
                if (!names.insert(parameter.Name()).second)
                    invalidMethod(parameter.NameRange(),
                                  "duplicate behavior method parameter");
                if (parameter.DefaultValue())
                    invalidMethod(
                        parameter.NameRange(),
                        "behavior method parameters cannot have defaults");
                parameters.push_back(resolved);
                ownership.push_back(type.Contract());
            }
            if (!receiver)
                invalidMethod(method->NameRange(),
                              "behavior methods require a first '&self' or "
                              "'&const self' receiver");
            SemanticType result = SemanticType::Error();
            if (ContainsTargetParameter(method->ReturnType(),
                                        syntax.targetParameter))
                invalidMethod(method->ReturnType().Range(),
                              "target-dependent result cannot match a declared "
                              "interface signature");
            else
                result = ResolveType(method->ReturnType(), true);
            std::vector<std::size_t> sources;
            for (const auto& source : method->BorrowSources()) {
                const auto found = std::find_if(
                    method->Parameters().begin(), method->Parameters().end(),
                    [&](const auto& parameter) {
                        return parameter->Name() == source;
                    });
                if (found == method->Parameters().end())
                    invalidMethod(
                        method->NameRange(),
                        "unknown behavior borrow source '" + source + "'");
                else
                    sources.push_back(static_cast<std::size_t>(
                        found - method->Parameters().begin()));
            }
            CallableSignature signature{
                result,
                std::move(parameters),
                std::move(ownership),
                BehaviorResultContracts(method->ReturnType()),
                std::move(sources),
                method->IsUnsafe(),
                method->HasModifier(core::FunctionModifier::Once),
                method->IsCrashable()};
            MethodContract normalized = NormalizeMethodContract(
                method->Name(), std::move(signature), receiver,
                method->HasVariadicPack(), method->BorrowsVariadicPack(),
                method->HasModifier(core::FunctionModifier::Protected)
                    ? MethodVisibility::Protected
                    : MethodVisibility::Public);
            for (const auto& root : contract.roots)
                for (const auto* candidate : root.type->methods)
                    if (candidate->key == normalized.key && !requirement)
                        requirement = candidate;
            if (!requirement)
                invalidMethod(method->NameRange(),
                              "behavior method is unrelated to its declared "
                              "interface graph");
            else {
                if (!normalized.SameContract(*requirement))
                    invalidMethod(method->NameRange(),
                                  "behavior method does not match the complete "
                                  "interface contract");
                if (requirement->category == core::MethodCategory::Final)
                    invalidMethod(
                        method->NameRange(),
                        "behavior cannot replace a final interface method");
                if (!method->HasModifier(core::FunctionModifier::Override))
                    invalidMethod(
                        method->NameRange(),
                        "behavior interface implementations require override");
            }
            for (const auto& prior : contract.methods)
                if (prior.signature.key == normalized.key)
                    invalidMethod(method->NameRange(),
                                  "duplicate behavior method signature");
            if (_diagnostics.Diagnostics().size() != before)
                contract.valid = false;
            contract.methods.push_back(
                {method.get(), std::move(normalized), requirement});
        }
        for (auto& prior : _model._behaviors)
            if (contract.decoration &&
                prior.decoration == contract.decoration &&
                prior.declaration->target == syntax.target) {
                _diagnostics.Report(DiagnosticCode::DuplicateBehavior,
                                    application.range,
                                    "only one behavior per decoration and "
                                    "concrete target kind is allowed",
                                    diagnostics::DiagnosticSeverity::Error,
                                    {{prior.declaration->decoration.range,
                                      "previous behavior"}});
                prior.valid = false;
                contract.valid = false;
            }
        _model._behaviors.push_back(std::move(contract));
    }

    if (_model._behaviors.empty()) return;

    // All contracts and metadata are stable before storing references to them.
    for (const auto& metadata : _model._decorations) {
        if (metadata.signature->target == DecorationTarget::Interface) continue;
        const DecorationTarget target =
            metadata.owner->Kind() == AstNodeKind::StructureDeclaration
                ? DecorationTarget::Structure
                : DecorationTarget::Enum;
        for (const auto& behavior : _model._behaviors) {
            if (!behavior.valid ||
                behavior.decoration != metadata.declaration ||
                behavior.declaration->target != target)
                continue;
            const auto prior = std::find_if(
                _model._behaviorActivations.begin(),
                _model._behaviorActivations.end(), [&](const auto& candidate) {
                    return candidate.behavior == &behavior &&
                           candidate.target == metadata.owner;
                });
            if (prior == _model._behaviorActivations.end())
                _model._behaviorActivations.push_back(
                    {&behavior, metadata.owner, {&metadata}});
            else
                prior->applications.push_back(&metadata);
        }
    }
    if (_model._behaviorActivations.empty()) return;

    for (const auto& declaration : unit.Declarations()) {
        std::vector<InterfaceReference> roots;
        for (const auto& structure : _model._structures)
            if (structure->syntax == declaration.get())
                roots = structure->conformances;
        for (const auto& enumeration : _model._enums)
            if (enumeration->syntax == declaration.get())
                roots = enumeration->conformances;
        bool valid = true;
        for (auto& candidate : _model._behaviorActivations) {
            if (candidate.target != declaration.get()) continue;
            for (const auto& promised : candidate.behavior->roots) {
                const InterfaceReference activated{
                    promised.type,
                    candidate.applications.front()->application->range};
                for (const auto& prior : roots)
                    if (conflict(prior, activated)) valid = false;
                roots.push_back(activated);
            }
        }
        if (!valid)
            for (auto& candidate : _model._behaviorActivations)
                if (candidate.target == declaration.get())
                    candidate.valid = false;
    }
}

}  // namespace flecha::semantic
