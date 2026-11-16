#include "analyzer/semantic_queries.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

#include "core/ast.hpp"
#include "semantic/semantic_model.hpp"

namespace flecha::analyzer {
namespace {

std::filesystem::path NormalizePath(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : normalized;
}

bool SameFile(const source::SourceRange& range,
              const std::filesystem::path& file) {
    if (range.start.file.empty()) return false;
    return NormalizePath(range.start.file) == file;
}

bool Contains(const source::SourceRange& range, std::size_t offset) {
    return range.start.offset <= offset && offset < range.end.offset;
}

SemanticItemKind ItemKind(semantic::SymbolKind kind) {
    using semantic::SymbolKind;
    switch (kind) {
        case SymbolKind::Function:
            return SemanticItemKind::Function;
        case SymbolKind::Parameter:
            return SemanticItemKind::Parameter;
        case SymbolKind::LocalVariable:
            return SemanticItemKind::LocalVariable;
        case SymbolKind::ModuleConstant:
            return SemanticItemKind::ModuleConstant;
        case SymbolKind::ErrorType:
            return SemanticItemKind::ErrorType;
        case SymbolKind::EnumType:
            return SemanticItemKind::EnumType;
        case SymbolKind::StructureType:
            return SemanticItemKind::StructureType;
        case SymbolKind::InterfaceType:
            return SemanticItemKind::InterfaceType;
        case SymbolKind::CallableType:
            return SemanticItemKind::CallableType;
        case SymbolKind::TypeBinding:
            return SemanticItemKind::TypeParameter;
    }
    return SemanticItemKind::Expression;
}

std::string DisplayName(const semantic::Symbol& symbol) {
    if (symbol.Kind() == semantic::SymbolKind::Function) {
        const auto& function =
            static_cast<const semantic::FunctionSymbol&>(symbol);
        if (function.Declaration()) {
            const auto& declaration = *function.Declaration();
            if (!declaration.specializationName.empty()) {
                const std::size_t arguments =
                    declaration.specializationName.find('<');
                return declaration.specializationName.substr(0, arguments);
            }
            return declaration.Name();
        }
    }
    const std::size_t moduleSeparator = symbol.Name().rfind('$');
    return moduleSeparator == std::string::npos
               ? symbol.Name()
               : symbol.Name().substr(moduleSeparator + 1);
}

std::string_view TerminalName(std::string_view name) {
    const std::size_t separator = name.rfind("::");
    return separator == std::string_view::npos ? name
                                               : name.substr(separator + 2);
}

source::SourceRange IdentifierRange(source::SourceRange range,
                                    std::string_view name) {
    const std::string_view identifier = TerminalName(name);
    if (identifier.empty() || range.Length() < identifier.size()) return range;
    source::SourceLocation start = range.end;
    start.offset -= identifier.size();
    if (start.column > identifier.size())
        start.column -= static_cast<std::uint32_t>(identifier.size());
    else
        start.column = 1;
    return {std::move(start), std::move(range.end)};
}

bool Queryable(const semantic::Symbol& symbol) {
    if (symbol.Name().starts_with('$') && !symbol.Name().starts_with("$m"))
        return false;
    if (symbol.Kind() != semantic::SymbolKind::Function) return true;
    const auto& function = static_cast<const semantic::FunctionSymbol&>(symbol);
    return function.BuiltIn() == semantic::BuiltInFunction::None;
}

bool SameSymbolKey(const semantic::Symbol& left,
                   const semantic::Symbol& right) {
    return left.Kind() == right.Kind() &&
           left.DeclarationRange() == right.DeclarationRange();
}

bool Queryable(const semantic::StructureField& field) {
    return !field.name.starts_with('$');
}

bool SameFieldKey(const semantic::StructureField& left,
                  const semantic::StructureField& right) {
    return left.syntax->NameRange() == right.syntax->NameRange();
}

std::string Lower(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value)
        result.push_back(static_cast<char>(std::tolower(character)));
    return result;
}

std::vector<SemanticOccurrence> BehaviorOccurrences(
    const semantic::SemanticModel& model) {
    std::vector<SemanticOccurrence> result;
    const auto add = [&](source::SourceRange range, std::string name,
                         std::string type, SemanticItemKind kind,
                         source::SourceRange definition = {}) {
        result.push_back({range, std::move(name), std::move(type), kind,
                          definition, range == definition});
    };
    for (const auto& contract : model.Behaviors()) {
        const auto& syntax = *contract.declaration;
        std::string description =
            "behavior " + syntax.decoration.Name() + "<" +
            syntax.targetParameter + "> => " +
            (syntax.target == core::DecorationTarget::Enum ? "enum"
                                                           : "structure") +
            " << ";
        for (std::size_t i = 0; i < syntax.roots.size(); ++i) {
            if (i) description += ", ";
            description += syntax.roots[i].name;
        }
        add(syntax.keywordRange, "behavior", "", SemanticItemKind::Keyword);
        add(syntax.targetRange, "", "", SemanticItemKind::Keyword);
        add(syntax.decoration.pathRanges.back(), syntax.decoration.Name(),
            description, SemanticItemKind::Behavior,
            contract.decoration ? contract.decoration->NameRange()
                                : source::SourceRange{});
        add(syntax.parameterRange, syntax.targetParameter,
            "behavior target parameter", SemanticItemKind::TypeParameter,
            syntax.parameterRange);
        for (const auto& root : contract.roots)
            add(IdentifierRange(root.range, root.type->displayName),
                root.type->displayName, "interface",
                SemanticItemKind::InterfaceType,
                root.type->syntax->NameRange());
        for (const auto& method : contract.methods) {
            const auto& function = *method.declaration;
            add(function.NameRange(), function.Name(),
                method.signature.result.Name(), SemanticItemKind::Function,
                method.requirement
                    ? method.requirement->syntax->Signature().NameRange()
                    : function.NameRange());
            if (function.ownerTokenIndex < function.templateTokens.size()) {
                const auto& owner =
                    function.templateTokens[function.ownerTokenIndex];
                if (owner.value == syntax.targetParameter)
                    add(owner.range, owner.value, "behavior target parameter",
                        SemanticItemKind::TypeParameter, syntax.parameterRange);
            }
            for (std::size_t i = 0; i < function.Parameters().size(); ++i) {
                const auto& parameter = *function.Parameters()[i];
                std::string type =
                    (method.signature.key.receiver ==
                             semantic::MethodReceiverKind::ConstReference
                         ? "&const "
                         : "&") +
                    syntax.targetParameter;
                if (i > 0 && i - 1 < method.signature.key.parameters.size())
                    type = method.signature.key.parameters[i - 1].Name();
                add(parameter.NameRange(), parameter.Name(), type,
                    SemanticItemKind::Parameter, parameter.NameRange());
            }
        }
    }
    for (const auto& item : model.ReflectionOccurrences())
        add(item.range, item.name, item.description,
            item.type            ? SemanticItemKind::TypeParameter
            : item.name == "for" ? SemanticItemKind::Keyword
            : item.operation     ? SemanticItemKind::Function
                                 : SemanticItemKind::Field,
            item.definition);
    return result;
}

}  // namespace

std::optional<SemanticQueries::Match> SemanticQueries::Find(
    const std::filesystem::path& inputFile, std::size_t offset,
    bool includeExpressions) const {
    const std::filesystem::path file = NormalizePath(inputFile);
    std::optional<Match> best;
    const auto consider = [&](Match match) {
        if (!SameFile(match.range, file) || !Contains(match.range, offset))
            return;
        if (!best || match.range.Length() < best->range.Length())
            best = std::move(match);
    };
    const auto symbolMatch = [&](source::SourceRange range, std::string name,
                                 const semantic::Symbol& symbol) {
        if (!Queryable(symbol)) return;
        const std::string displayName(TerminalName(name));
        consider({IdentifierRange(std::move(range), name),
                  _model.SourceOrigin(symbol.DeclarationRange()), displayName,
                  symbol.Type().Name(), ItemKind(symbol.Kind()),
                  TargetKind::Symbol, &symbol});
    };

    for (const auto& item : BehaviorOccurrences(_model)) {
        if (item.kind == SemanticItemKind::Keyword) continue;
        TargetKind targetKind = TargetKind::None;
        const void* target = nullptr;
        if (item.kind == SemanticItemKind::Behavior) {
            for (const auto& behavior : _model.Behaviors())
                if (behavior.declaration->decoration.pathRanges.back() ==
                        item.range &&
                    behavior.decoration) {
                    targetKind = TargetKind::Decoration;
                    target = behavior.decoration;
                }
        } else {
            for (const auto& symbol : _model.Symbols())
                if (!item.declarationRange.Empty() && Queryable(*symbol) &&
                    symbol->DeclarationRange() == item.declarationRange) {
                    targetKind = TargetKind::Symbol;
                    target = symbol.get();
                    break;
                }
        }
        consider({item.range, item.declarationRange, item.name, item.type,
                  item.kind, targetKind, target});
    }

    for (const auto& [declaration, contract] : _model._decorationContracts) {
        consider({declaration->NameRange(), declaration->NameRange(),
                  declaration->Name(), "decoration",
                  SemanticItemKind::Decoration, TargetKind::Decoration,
                  declaration});
        for (const auto& signature : contract.applications)
            for (const auto& parameter : signature.parameters)
                if (parameter.syntax->type)
                    consider({parameter.syntax->nameRange,
                              parameter.syntax->nameRange,
                              parameter.syntax->name, parameter.type.Name(),
                              SemanticItemKind::Parameter});
    }
    for (const auto& [application, declaration] : _model._decorationBindings)
        consider({application->pathRanges.back(), declaration->NameRange(),
                  declaration->Name(), "decoration",
                  SemanticItemKind::Decoration, TargetKind::Decoration,
                  declaration});

    for (const auto& [expression, symbol] : _model._bindings)
        symbolMatch(expression->Range(), expression->Name(), *symbol);

    for (const auto& [type, symbol] : _model._typeBindings)
        symbolMatch(type->Range(), type->Name(), *symbol);

    for (const auto& [declaration, symbol] : _model._declarations)
        if (Queryable(*symbol))
            consider({symbol->DeclarationRange(), symbol->DeclarationRange(),
                      DisplayName(*symbol), symbol->Type().Name(),
                      ItemKind(symbol->Kind()), TargetKind::Symbol, symbol});

    for (const auto& [expression, field] : _model._fields)
        if (Queryable(*field))
            consider({expression->MemberRange(), field->syntax->NameRange(),
                      expression->Member(), field->type.Name(),
                      SemanticItemKind::Field, TargetKind::Field, field});

    for (const auto& structure : _model._structures)
        for (const auto& field : structure->fields)
            if (Queryable(field))
                consider({field.syntax->NameRange(), field.syntax->NameRange(),
                          field.name, field.type.Name(),
                          SemanticItemKind::Field, TargetKind::Field, &field});

    for (const auto& [expression, method] : _model._methods)
        symbolMatch(expression->MemberRange(), expression->Member(), *method);

    if (best || !includeExpressions) return best;

    for (const auto& [expression, type] : _model._types) {
        if (type.IsError()) continue;
        std::string displayType = type.Name();
        if (expression->Kind() == core::AstNodeKind::TryExpression)
            displayType += " (propagates returned error)";
        if (expression->Kind() == core::AstNodeKind::AwaitExpression) {
            const auto& await =
                static_cast<const core::AwaitExpression&>(*expression);
            if (const auto* result = _model.AwaitResultFor(await))
                displayType += result->forwardsFailure
                                   ? " (forwards execution failure)"
                                   : " (none means success)";
        }
        consider({expression->Range(),
                  {},
                  {},
                  std::move(displayType),
                  SemanticItemKind::Expression,
                  TargetKind::None,
                  nullptr});
    }
    return best;
}

std::optional<SemanticHover> SemanticQueries::Hover(
    const std::filesystem::path& file, std::size_t offset) const {
    const std::optional<Match> match = Find(file, offset, true);
    if (!match) return std::nullopt;
    std::string type = match->type;
    if (match->targetKind == TargetKind::Symbol)
        for (const auto& [statement, checked] : _model._checkedAsyncResults)
            if (checked.value == match->target && checked.error) {
                type +=
                    " (initialized when " + checked.error->Name() + " == none)";
                break;
            }
    return SemanticHover{match->range, match->name, std::move(type),
                         match->kind};
}

std::optional<SemanticOccurrence> SemanticQueries::OccurrenceAt(
    const std::filesystem::path& file, std::size_t offset) const {
    const std::optional<Match> match = Find(file, offset, false);
    if (!match) return std::nullopt;
    return SemanticOccurrence{
        match->range,
        match->name,
        match->type,
        match->kind,
        match->declarationRange,
        match->range == match->declarationRange,
    };
}

std::vector<SemanticOccurrence> SemanticQueries::Occurrences(
    const std::filesystem::path& inputFile) const {
    const std::filesystem::path file = NormalizePath(inputFile);
    std::vector<SemanticOccurrence> result;
    const auto add = [&](source::SourceRange range, std::string name,
                         std::string type, SemanticItemKind kind,
                         source::SourceRange declarationRange) {
        if (!SameFile(range, file) || range.Empty()) return;
        SemanticOccurrence occurrence{
            range, std::move(name),  std::move(type),
            kind,  declarationRange, range == declarationRange};
        const auto duplicate = std::find_if(
            result.begin(), result.end(), [&](const auto& existing) {
                return existing.range == occurrence.range &&
                       existing.kind == occurrence.kind &&
                       existing.declarationRange == occurrence.declarationRange;
            });
        if (duplicate == result.end()) result.push_back(std::move(occurrence));
    };
    const auto addSymbol = [&](source::SourceRange range, std::string name,
                               const semantic::Symbol& symbol) {
        if (!Queryable(symbol)) return;
        const std::string displayName(TerminalName(name));
        add(IdentifierRange(std::move(range), name), displayName,
            symbol.Type().Name(), ItemKind(symbol.Kind()),
            _model.SourceOrigin(symbol.DeclarationRange()));
    };

    for (const auto& item : BehaviorOccurrences(_model))
        add(item.range, item.name, item.type, item.kind, item.declarationRange);

    for (const auto& [declaration, contract] : _model._decorationContracts) {
        add(declaration->NameRange(), declaration->Name(), "decoration",
            SemanticItemKind::Decoration, declaration->NameRange());
        for (const auto& signature : contract.applications) {
            add(signature.syntax->applicationRange, "application", "",
                SemanticItemKind::Keyword, {});
            add(signature.syntax->targetKeywordRange, "target", "",
                SemanticItemKind::Keyword, {});
            for (const auto& range : signature.syntax->targetPathRanges)
                add(range, "", "", SemanticItemKind::Keyword, {});
            for (const auto& parameter : signature.parameters)
                if (parameter.syntax->type)
                    add(parameter.syntax->nameRange, parameter.syntax->name,
                        parameter.type.Name(), SemanticItemKind::Parameter,
                        parameter.syntax->nameRange);
        }
    }
    for (const auto& [application, declaration] : _model._decorationBindings) {
        const bool behaviorName = std::any_of(
            _model.Behaviors().begin(), _model.Behaviors().end(),
            [&](const auto& behavior) {
                return &behavior.declaration->decoration == application;
            });
        if (!behaviorName)
            add(application->pathRanges.back(), declaration->Name(),
                "decoration", SemanticItemKind::Decoration,
                declaration->NameRange());
    }

    for (const auto& [expression, symbol] : _model._bindings)
        addSymbol(expression->Range(), expression->Name(), *symbol);
    for (const auto& [type, symbol] : _model._typeBindings)
        addSymbol(type->Range(), type->Name(), *symbol);
    for (const auto& [declaration, symbol] : _model._declarations)
        if (Queryable(*symbol))
            add(symbol->DeclarationRange(), DisplayName(*symbol),
                symbol->Type().Name(), ItemKind(symbol->Kind()),
                symbol->DeclarationRange());
    for (const auto& [expression, field] : _model._fields)
        if (Queryable(*field))
            add(expression->MemberRange(), expression->Member(),
                field->type.Name(), SemanticItemKind::Field,
                field->syntax->NameRange());
    for (const auto& structure : _model._structures)
        for (const auto& field : structure->fields)
            if (Queryable(field))
                add(field.syntax->NameRange(), field.name, field.type.Name(),
                    SemanticItemKind::Field, field.syntax->NameRange());
    for (const auto& [expression, method] : _model._methods)
        addSymbol(expression->MemberRange(), expression->Member(), *method);

    std::sort(result.begin(), result.end(),
              [](const auto& left, const auto& right) {
                  if (left.range.start.offset != right.range.start.offset)
                      return left.range.start.offset < right.range.start.offset;
                  return left.range.end.offset < right.range.end.offset;
              });
    return result;
}

std::optional<source::SourceRange> SemanticQueries::Definition(
    const std::filesystem::path& file, std::size_t offset) const {
    const std::optional<Match> match = Find(file, offset, false);
    if (!match || match->declarationRange.Empty() ||
        match->declarationRange.start.file.empty())
        return std::nullopt;
    return match->declarationRange;
}

std::vector<source::SourceRange> SemanticQueries::ReferenceRanges(
    const Match& match, bool includeDeclaration) const {
    std::vector<source::SourceRange> result;
    const auto add = [&](const source::SourceRange& range) {
        if (range.Empty() || range.start.file.empty()) return;
        if (std::find(result.begin(), result.end(), range) == result.end())
            result.push_back(range);
    };

    if (match.targetKind == TargetKind::Decoration) {
        const auto* declaration =
            static_cast<const core::DecorationDeclaration*>(match.target);
        if (includeDeclaration) add(declaration->NameRange());
        for (const auto& [application, target] : _model._decorationBindings)
            if (target == declaration) add(application->pathRanges.back());
    }
    if (match.targetKind == TargetKind::Symbol) {
        const auto* target = static_cast<const semantic::Symbol*>(match.target);
        for (const auto& [expression, symbol] : _model._bindings)
            if (Queryable(*symbol) && SameSymbolKey(*symbol, *target))
                add(IdentifierRange(expression->Range(), expression->Name()));
        for (const auto& [type, symbol] : _model._typeBindings)
            if (Queryable(*symbol) && SameSymbolKey(*symbol, *target))
                add(IdentifierRange(type->Range(), type->Name()));
        for (const auto& [expression, method] : _model._methods)
            if (Queryable(*method) && SameSymbolKey(*method, *target))
                add(expression->MemberRange());
        for (const auto& item : BehaviorOccurrences(_model))
            if (item.declarationRange == target->DeclarationRange() &&
                (includeDeclaration || item.range != item.declarationRange))
                add(item.range);
        if (includeDeclaration) add(target->DeclarationRange());
    } else if (match.targetKind == TargetKind::Field) {
        const auto* target =
            static_cast<const semantic::StructureField*>(match.target);
        for (const auto& [expression, field] : _model._fields)
            if (Queryable(*field) && SameFieldKey(*field, *target))
                add(expression->MemberRange());
        if (includeDeclaration) add(target->syntax->NameRange());
    }

    std::sort(result.begin(), result.end(),
              [](const auto& left, const auto& right) {
                  if (left.start.file != right.start.file)
                      return left.start.file < right.start.file;
                  return left.start.offset < right.start.offset;
              });
    return result;
}

std::optional<std::vector<source::SourceRange>> SemanticQueries::References(
    const std::filesystem::path& file, std::size_t offset,
    bool includeDeclaration) const {
    const std::optional<Match> match = Find(file, offset, false);
    if (!match || match->targetKind == TargetKind::None) return std::nullopt;
    return ReferenceRanges(*match, includeDeclaration);
}

std::optional<SemanticRename> SemanticQueries::Rename(
    const std::filesystem::path& file, std::size_t offset,
    bool includeDeclaration) const {
    const std::optional<Match> match = Find(file, offset, false);
    if (!match || match->targetKind == TargetKind::None ||
        match->declarationRange.Empty() ||
        match->declarationRange.start.file.empty())
        return std::nullopt;
    std::vector<source::SourceRange> ranges =
        ReferenceRanges(*match, includeDeclaration);
    if (ranges.empty()) return std::nullopt;
    return SemanticRename{match->name, std::move(ranges)};
}

std::vector<SemanticSymbol> SemanticQueries::Symbols(
    std::string_view query) const {
    const std::string needle = Lower(query);
    std::vector<SemanticSymbol> result;
    for (const auto& behavior : _model.Behaviors()) {
        const auto& syntax = *behavior.declaration;
        const std::string name = syntax.decoration.Name();
        if (needle.empty() || Lower(name).contains(needle))
            result.push_back({name, "behavior", SemanticItemKind::Behavior,
                              syntax.decoration.pathRanges.back()});
        for (const auto& method : behavior.methods)
            if (needle.empty() ||
                Lower(method.declaration->Name()).contains(needle))
                result.push_back({method.declaration->Name(),
                                  method.signature.result.Name(),
                                  SemanticItemKind::Function,
                                  method.declaration->NameRange()});
    }
    for (const auto& [declaration, contract] : _model._decorationContracts)
        if (needle.empty() || Lower(declaration->Name()).contains(needle))
            result.push_back({declaration->Name(), "decoration",
                              SemanticItemKind::Decoration,
                              declaration->NameRange()});
    for (const auto& symbol : _model._symbols) {
        if (symbol->DeclarationRange().Empty() ||
            symbol->DeclarationRange().start.file.empty() ||
            !Queryable(*symbol))
            continue;
        const std::string name = DisplayName(*symbol);
        if (!needle.empty() && !Lower(name).contains(needle)) continue;
        const SemanticItemKind kind = ItemKind(symbol->Kind());
        const auto duplicate = std::find_if(
            result.begin(), result.end(), [&](const auto& existing) {
                return existing.kind == kind &&
                       existing.declarationRange == symbol->DeclarationRange();
            });
        if (duplicate != result.end()) continue;
        result.push_back({name, symbol->Type().Name(), ItemKind(symbol->Kind()),
                          symbol->DeclarationRange()});
    }
    for (const auto& structure : _model._structures) {
        for (const auto& field : structure->fields) {
            if (!Queryable(field)) continue;
            if (!needle.empty() && !Lower(field.name).contains(needle))
                continue;
            const auto duplicate = std::find_if(
                result.begin(), result.end(), [&](const auto& existing) {
                    return existing.kind == SemanticItemKind::Field &&
                           existing.declarationRange ==
                               field.syntax->NameRange();
                });
            if (duplicate != result.end()) continue;
            result.push_back({field.name, field.type.Name(),
                              SemanticItemKind::Field,
                              field.syntax->NameRange()});
        }
    }
    std::sort(result.begin(), result.end(),
              [](const auto& left, const auto& right) {
                  if (left.name != right.name) return left.name < right.name;
                  if (left.declarationRange.start.file !=
                      right.declarationRange.start.file)
                      return left.declarationRange.start.file <
                             right.declarationRange.start.file;
                  return left.declarationRange.start.offset <
                         right.declarationRange.start.offset;
              });
    return result;
}

}  // namespace flecha::analyzer
