#include <algorithm>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>

#include "core/ast_walk.hpp"
#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "semantic/allocation_checker.hpp"
#include "semantic/compile_time_value.hpp"
#include "semantic/generic_expander.hpp"
#include "semantic/semantic_analyzer.hpp"

namespace flecha::semantic {
namespace {
using namespace core;
using diagnostics::DiagnosticCode;
using Range = source::SourceRange;
struct ExpansionToken {
    Token token;
    const StructureField* field = nullptr;
};
using Tokens = std::vector<ExpansionToken>;
bool Inside(Range outer, Range inner) {
    return outer.start.file == inner.start.file &&
           outer.start.offset <= inner.start.offset &&
           inner.end.offset <= outer.end.offset;
}
Range SyntaxRange(const AstNode& node) {
    auto range = node.Range();
    // NameExpression keeps an identifier range for navigation. Its explicit
    // type arguments still belong to the rewrite, before the caller's `>`.
    if (node.Kind() == AstNodeKind::Type) {
        const auto& type = static_cast<const TypeNode&>(node);
        for (const auto& argument : type.typeArguments)
            if (SyntaxRange(*argument).end.offset > range.end.offset)
                range.end = SyntaxRange(*argument).end;
    }
    if (node.Kind() == AstNodeKind::NameExpression) {
        const auto& name = static_cast<const NameExpression&>(node);
        for (const auto& argument : name.typeArguments)
            if (SyntaxRange(*argument).end.offset > range.end.offset)
                range.end = SyntaxRange(*argument).end;
    }
    return range;
}
std::vector<const AstNode*> Children(const AstNode& n) {
    std::vector<const AstNode*> children;
    unsigned depth = 0;
    WalkAst(n, [&](AstNode& child, bool enter) {
        if (enter) {
            if (depth == 1) children.push_back(&child);
            ++depth;
        } else
            --depth;
    });
    return children;
}
const MemberCallExpression* Operation(const AstNode& n) {
    if (n.Kind() != AstNodeKind::MemberCallExpression) return nullptr;
    auto& call = static_cast<const MemberCallExpression&>(n);
    return call.Access() == TokenType::Scope &&
                   call.Receiver().Kind() == AstNodeKind::NameExpression
               ? &call
               : nullptr;
}
std::string Receiver(const MemberCallExpression& call) {
    return static_cast<const NameExpression&>(call.Receiver()).Name();
}
struct Context {
    const FieldIterationStatement* loop = nullptr;
    const BehaviorFieldDescriptor* field = nullptr;
    bool restricted = false;
    std::string fieldTypeAlias;
    bool runtimeConditional = false;
    struct TypePatternBinding {
        SemanticType type{SemanticTypeKind::Error};
        std::string alias;
        Range declaration;
    };
    std::map<std::string, TypePatternBinding> typeBindings;
    unsigned runtimeLoopDepth = 0;
    std::string breakFlag;

    struct UniqueKey {
        const StructureField* field;
        Range range;
    };
    std::map<std::string, UniqueKey>* uniqueKeys = nullptr;
};
}  // namespace

// Materialize accepted plans in the analyzer transaction. Nothing is returned
// to callers until the complete generated surface passes ordinary analysis.
class BehaviorPlanner {
   public:
    explicit BehaviorPlanner(SemanticAnalyzer& analyzer)
        : _host(analyzer), _model(analyzer._model) {}
    void Run(CompilationUnit& unit) {
        _model._behaviorPlans.reserve(_model._behaviorActivations.size());
        for (const auto& activation : _model._behaviorActivations) {
            if (!activation.valid) continue;
            _currentBehavior = activation.behavior;
            const auto* target = _model.DeclaredSymbol(*activation.target);
            if (!target) continue;
            _model._behaviorPlans.push_back({&activation, target->Type()});
            _plan = &_model._behaviorPlans.back();

            for (auto* metadata : activation.applications)
                if (metadata->target == activation.target)
                    _plan->metadata = Record(*metadata);
            if (_plan->target.Kind() == SemanticTypeKind::Structure) {
                const auto* owner = _plan->target.Structure();
                for (auto& field : owner->fields) {
                    BehaviorFieldDescriptor descriptor{
                        owner,
                        &field,
                        field.type,
                        field.syntax->Type().Contract(),
                        {}};
                    for (auto* metadata : activation.applications)
                        if (metadata->target == field.syntax)
                            descriptor.metadata = Record(*metadata);
                    _plan->pack.fields.push_back(std::move(descriptor));
                }
            }
            for (auto& method : _currentBehavior->methods) Build(method, unit);
            _plan = nullptr;
        }
        // All ordinary method symbols and inherited contracts are now visible.
        // Validate the complete selected surface before touching any body.
        _host.ValidateInterfaceConformance();
        if (_host._diagnostics.HasErrors()) return;
        for (auto& pending : _pending) {
            _plan = pending.plan;
            _currentBehavior = _plan->activation->behavior;
            const auto before = unit.Declarations().size();
            Build(*pending.method, unit, &pending);
            if (unit.Declarations().size() != before) return;
        }
        _plan = nullptr;
    }

   private:
    struct Pending {
        BehaviorExpansionPlan* plan;
        const BehaviorMethodContract* method;
        FunctionDeclaration* function;
        Scope* scope;
        std::size_t bodyIndex;
    };
    std::vector<Pending> _pending;
    SemanticAnalyzer& _host;
    SemanticModel& _model;
    const BehaviorContract* _currentBehavior = nullptr;
    BehaviorExpansionPlan* _plan = nullptr;
    const std::vector<Token>* _tokens = nullptr;
    Scope* _scope = nullptr;
    std::size_t _generatedTokens = 0, _generatedBodies = 0, _aliasId = 0;

    std::set<std::pair<std::string, std::size_t>> _ambiguousNavigation;
    std::vector<std::pair<Range, const StructureField*>> _projections;

    void Error(DiagnosticCode code, Range range, std::string message,
               Context context = {},
               std::vector<diagnostics::RelatedLocation> extra = {}) {
        if (_plan) _plan->valid = false;
        std::vector<diagnostics::RelatedLocation> related{
            {_currentBehavior->declaration->decoration.range,
             "behavior declared here"}};
        if (_plan) {
            related.push_back(
                {_plan->activation->applications.front()->application->range,
                 "behavior activated here"});
            related.push_back({_plan->activation->target->Range(),
                               "concrete T = " + _plan->target.Name()});
        }
        if (context.field)
            related.push_back({context.field->field->syntax->NameRange(),
                               "field '" + context.field->field->name +
                                   "', F = " + context.field->type.Name()});
        related.insert(related.end(), extra.begin(), extra.end());
        _host._diagnostics.Report(code, range, std::move(message),
                                  diagnostics::DiagnosticSeverity::Error,
                                  std::move(related));
    }
    void Occurrence(Range range, std::string name, std::string description,
                    bool type = false, Range definition = {},
                    bool operation = false) {
        if (range.Empty()) return;
        auto& out = _model._reflectionOccurrences;
        auto it = std::find_if(out.begin(), out.end(), [&](auto& v) {
            return v.range.start.file == range.start.file &&
                   v.range.start.offset == range.start.offset;
        });
        if (it == out.end())
            out.push_back({range, definition, std::move(name),
                           std::move(description), type, operation});
        else {
            if (it->description.find(description) == std::string::npos)
                it->description += "; " + description;
            const auto key =
                std::make_pair(range.start.file, range.start.offset);
            if (!definition.Empty() && !_ambiguousNavigation.contains(key)) {
                if (it->definition.Empty())
                    it->definition = definition;
                else if (it->definition != definition) {
                    it->definition = {};
                    _ambiguousNavigation.insert(key);
                }
            }
        }
    }
    Tokens Raw(Range range) const {
        Tokens result;
        for (auto& token : *_tokens)
            if (Inside(range, token.range)) result.push_back({token});
        return result;
    }
    Tokens One(TokenType type, std::string value, Range range) {
        return {{Token(type, std::move(value), range)}};
    }
    void Append(Tokens& into, Tokens from) {
        into.insert(into.end(), std::make_move_iterator(from.begin()),
                    std::make_move_iterator(from.end()));
    }
    bool IsContextual(const MemberCallExpression& call, Context c) const {
        return Receiver(call) ==
                   _currentBehavior->declaration->targetParameter ||
               (c.loop && Receiver(call) == c.loop->descriptor);
    }
    void Validate(const AstNode& n, Context c, bool statement = false) {
        if (n.Kind() == AstNodeKind::IsOfExpression) {
            const auto& query = static_cast<const IsOfExpression&>(n);
            if (query.Value().Kind() == AstNodeKind::NameExpression) {
                const auto& name =
                    static_cast<const NameExpression&>(query.Value());
                if (name.Name() ==
                        _currentBehavior->declaration->targetParameter ||
                    (c.loop && name.Name() == c.loop->binder)) {
                    if (c.restricted)
                        Error(DiagnosticCode::InvalidReflectionContext,
                              query.Range(),
                              "type reflection cannot be used in a closure", c);
                    Occurrence(
                        name.Range(), name.Name(), "compile-time type query",
                        true,
                        c.loop && name.Name() == c.loop->binder
                            ? c.loop->binderRange
                            : _currentBehavior->declaration->parameterRange);
                }
            }
        }
        if (n.Kind() == AstNodeKind::ExpressionStatement) {
            Validate(static_cast<const ExpressionStatement&>(n).Value(), c,
                     true);
            return;
        }
        if (n.Kind() == AstNodeKind::FieldIterationStatement) {
            auto& loop = static_cast<const FieldIterationStatement&>(n);
            if (_currentBehavior->declaration->target !=
                    DecorationTarget::Structure ||
                c.restricted || c.loop)
                Error(DiagnosticCode::InvalidReflectionContext,
                      loop.keywordRange,
                      "field iteration requires a structure behavior method "
                      "outside closures and other field iterations",
                      c);
            if (loop.binder == _currentBehavior->declaration->targetParameter ||
                loop.target != _currentBehavior->declaration->targetParameter ||
                loop.valueType != loop.binder ||
                loop.descriptor == loop.binder ||
                loop.descriptor == loop.target)
                Error(DiagnosticCode::InvalidFieldIteration, loop.binderRange,
                      "Field<T, F> must use the behavior target and one "
                      "distinct field-type binder",
                      c);
            const auto* pack = Operation(*loop.pack);
            if (!pack ||
                Receiver(*pack) !=
                    _currentBehavior->declaration->targetParameter ||
                pack->Member() != "fields" || !pack->Arguments().empty() ||
                !pack->typeArguments.empty())
                Error(DiagnosticCode::InvalidFieldIteration, loop.packRange,
                      "field iteration requires exactly T::fields()", c);
            Occurrence(loop.keywordRange, "for",
                       "compile-time field iteration");
            Occurrence(loop.binderRange, loop.binder,
                       "exact field type (bound per iteration)", true,
                       loop.binderRange);
            Occurrence(loop.valueTypeRange, loop.valueType,
                       "exact field type (bound per iteration)", true,
                       loop.binderRange);
            Occurrence(loop.targetRange, loop.target, "behavior target type",
                       true, _currentBehavior->declaration->parameterRange);
            Occurrence(loop.fieldRange, "Field",
                       "compiler-only field descriptor", true);
            Occurrence(loop.descriptorRange, loop.descriptor,
                       "unique direct-field descriptor", true,
                       loop.descriptorRange);
            if (pack) {
                Occurrence(pack->MemberRange(), "fields",
                           "compile-time direct field pack in source order",
                           false, {}, true);
                Occurrence(pack->Receiver().Range(), loop.target,
                           "behavior target type", true,
                           _currentBehavior->declaration->parameterRange);
            }
            c.loop = &loop;
            Validate(*loop.body, c);
            return;
        }
        if (n.Kind() == AstNodeKind::AnonymousFunctionExpression)
            c.restricted = true;
        if (c.loop && n.Kind() == AstNodeKind::JumpToStatement)
            Error(DiagnosticCode::InvalidReflectionContext, n.Range(),
                  "jumpto is invalid in compile-time field iteration", c);
        if (auto* call = Operation(n); call && IsContextual(*call, c)) {
            const bool field = c.loop && Receiver(*call) == c.loop->descriptor;
            if (c.restricted)
                Error(DiagnosticCode::InvalidReflectionContext,
                      call->MemberRange(),
                      "reflection cannot be used in a nested closure", c);
            const auto& op = call->Member();
            bool valid =
                op == "decorated" || op == "decorator" || op == "require" ||
                (field && (op == "name" || op == "get" || op == "isPrivate" ||
                           op == "requireUnique"));
            if (op == "fields")
                Error(DiagnosticCode::ReflectionEscape, call->Range(),
                      "a field pack can only initialize a bounded field "
                      "iteration",
                      c);
            else if (!valid && field)
                Error(DiagnosticCode::InvalidReflectionOperation,
                      call->MemberRange(),
                      "unknown field descriptor operation '" + op + "'", c);
            if (valid && (call->Arguments().size() !=
                              (op == "require"                        ? 2u
                               : op == "get" || op == "requireUnique" ? 1u
                                                                      : 0u) ||
                          !call->typeArguments.empty() || call->IsCrashable()))
                Error(DiagnosticCode::InvalidReflectionOperation, call->Range(),
                      "invalid reflection arguments; operations query only the "
                      "owning decoration",
                      c);
            if (valid && (op == "requireUnique" || op == "require") &&
                !statement)
                Error(DiagnosticCode::InvalidReflectionOperation, call->Range(),
                      op + "() is a compile-time validation statement, not a "
                           "value",
                      c);
            if (valid) {
                Occurrence(call->Receiver().Range(), Receiver(*call),
                           field ? "compiler-only field descriptor"
                                 : "behavior target type",
                           true,
                           field
                               ? c.loop->descriptorRange
                               : _currentBehavior->declaration->parameterRange);
                Occurrence(call->MemberRange(), op,
                           op == "name" ? "stringv field name"
                           : op == "decorated" || op == "isPrivate"
                               ? "compile-time bool"
                           : op == "requireUnique"
                               ? "compile-time unique stringv key in this "
                                 "field iteration"
                           : op == "require"
                               ? "compile-time requirement (bool, stringv)"
                           : op == "get" ? "exact field place"
                                         : "typed decoration argument record",
                           false, {}, true);
                for (auto& arg : call->Arguments()) Validate(*arg, c);
                return;
            }
        }
        if (n.Kind() == AstNodeKind::NameExpression && c.loop &&
            static_cast<const NameExpression&>(n).Name() == c.loop->descriptor)
            Error(DiagnosticCode::ReflectionEscape, n.Range(),
                  "a field descriptor cannot be used as a runtime value", c);
        if (n.Kind() == AstNodeKind::Type) {
            auto& t = static_cast<const TypeNode&>(n);
            if (t.Name() == "Field")
                Error(DiagnosticCode::ReflectionEscape, t.Range(),
                      "Field<T,F> is only valid in a bounded field iteration",
                      c);
            if (c.loop && t.Name() == c.loop->binder) {
                if (c.restricted)
                    Error(DiagnosticCode::InvalidReflectionContext, t.Range(),
                          "a field-type binding cannot escape into a closure",
                          c);
                Occurrence(t.Range(), c.loop->binder,
                           "exact field type (bound per iteration)", true,
                           c.loop->binderRange);
            }
        }
        if (n.Kind() == AstNodeKind::AnonymousFunctionExpression && c.loop) {
            auto& closure = static_cast<const AnonymousFunctionExpression&>(n);
            for (auto& capture : closure.Captures())
                if (capture->Name() == c.loop->descriptor ||
                    capture->Name() == c.loop->binder)
                    Error(DiagnosticCode::ReflectionEscape,
                          capture->NameRange(),
                          "compile-time descriptors cannot be captured", c);
        }
        for (auto* child : Children(n)) Validate(*child, c);
    }
    std::optional<TypedDecorationRecord> Record(
        const DecorationMetadata& metadata) {
        TypedDecorationRecord record{&metadata, {}};
        const auto* contract = _model.ContractFor(*metadata.declaration);
        if (!contract) return {};
        for (auto& signature : contract->applications)
            if (signature.syntax == metadata.signature) {
                for (std::size_t i = 0; i < signature.parameters.size(); ++i) {
                    const auto& parameter = signature.parameters[i];
                    if (!parameter.syntax->marker.empty()) continue;
                    if (i >= metadata.application->arguments.size()) return {};
                    auto& argument = metadata.application->arguments[i];
                    auto value = argument.expression
                                     ? EvaluateCompileTimeValue(
                                           *argument.expression, _model)
                                     : std::nullopt;
                    if (!value) {
                        Error(DiagnosticCode::InvalidBehaviorExpansion,
                              argument.range,
                              "cannot evaluate typed decoration argument");
                        return {};
                    }
                    // Decoration validation already proved assignability, but
                    // an error parameter may hold a value from a named error
                    // family.
                    value = ConvertCompileTimeValue(
                        *value, parameter.type.WithoutConst());
                    if (!value) {
                        Error(DiagnosticCode::InvalidBehaviorExpansion,
                              argument.range,
                              "cannot convert typed decoration argument");
                        return {};
                    }
                    record.members.push_back({parameter.syntax, *value});
                }
            }
        return record;
    }
    const TypedDecorationRecord* Metadata(const MemberCallExpression& call,
                                          Context c) const {
        if (c.loop && Receiver(call) == c.loop->descriptor)
            return c.field && c.field->metadata ? &*c.field->metadata : nullptr;
        return _plan->metadata ? &*_plan->metadata : nullptr;
    }
    std::optional<CompileTimeValue> Reflection(const Expression& e, Context c,
                                               Context* bindings = nullptr) {
        if (e.Kind() == AstNodeKind::IsOfExpression) {
            const auto& query = static_cast<const IsOfExpression&>(e);
            if (query.Value().Kind() == AstNodeKind::NameExpression) {
                const auto& name =
                    static_cast<const NameExpression&>(query.Value()).Name();
                const bool field = c.field && name == c.loop->binder;
                if (field || c.typeBindings.contains(name) ||
                    name == _currentBehavior->declaration->targetParameter) {
                    const auto value = field ? c.field->type
                                       : c.typeBindings.contains(name)
                                           ? c.typeBindings.at(name).type
                                           : _plan->target;
                    const auto& pattern = query.Target();
                    const auto* element = pattern.Target();
                    if (pattern.TypeForm() == TypeKind::Slice &&
                        pattern.Contract() == OwnershipContract::Own &&
                        element && element->TypeForm() == TypeKind::Named &&
                        element->typeArguments.empty() && !element->IsConst() &&
                        element->Contract() == OwnershipContract::None &&
                        element->Name().find("::") == std::string::npos &&
                        !c.typeBindings.contains(element->Name()) &&
                        element->Name() !=
                            _currentBehavior->declaration->targetParameter &&
                        (!c.loop || element->Name() != c.loop->binder) &&
                        !_scope->Lookup(element->Name())) {
                        const bool matches =
                            value.Kind() == SemanticTypeKind::OwnedSlice;
                        if (matches && bindings) {
                            auto alias =
                                "$behavior$type$" + std::to_string(_aliasId++);
                            auto symbol = std::make_unique<Symbol>(
                                SymbolKind::TypeBinding, alias, *value.Target(),
                                element->Range(), false);
                            (void)_scope->Declare(*symbol);
                            _model._symbols.push_back(std::move(symbol));
                            bindings->typeBindings[element->Name()] = {
                                *value.Target(), alias, element->Range()};
                            Occurrence(
                                element->Range(), element->Name(),
                                "element type = " + value.Target()->Name(),
                                true, element->Range());
                        }
                        return CompileTimeValue{
                            SemanticType(SemanticTypeKind::Bool), matches};
                    }
                    Scope* saved = _host._currentScope;
                    _host._currentScope = &_host.AddScope(_scope);
                    if (c.field) {
                        auto binder = std::make_unique<Symbol>(
                            SymbolKind::TypeBinding, c.loop->binder,
                            c.field->type, c.loop->binderRange, false);
                        (void)_host._currentScope->Declare(*binder);
                        _model._symbols.push_back(std::move(binder));
                    }
                    for (const auto& [boundName, binding] : c.typeBindings) {
                        auto symbol = std::make_unique<Symbol>(
                            SymbolKind::TypeBinding, boundName, binding.type,
                            binding.declaration, false);
                        (void)_host._currentScope->Declare(*symbol);
                        _model._symbols.push_back(std::move(symbol));
                    }
                    bool result = false;
                    if (query.Candidates().empty())
                        result = MatchesTypeQuery(
                            value, _host.ResolveType(query.Target()), false);
                    else
                        for (const auto& candidate : query.Candidates())
                            result |= MatchesTypeQuery(
                                value, _host.ResolveType(*candidate),
                                query.IsTypeSetQuery());
                    _host._currentScope = saved;
                    return CompileTimeValue{
                        SemanticType(SemanticTypeKind::Bool), result};
                }
            }
        }
        if (auto* call = Operation(e); call && IsContextual(*call, c) &&
                                       call->Arguments().empty() &&
                                       call->typeArguments.empty()) {
            if (call->Member() == "decorated")
                return CompileTimeValue{SemanticType(SemanticTypeKind::Bool),
                                        Metadata(*call, c) != nullptr};
            if (call->Member() == "name" && c.field &&
                Receiver(*call) == c.loop->descriptor)
                return CompileTimeValue{
                    SemanticType(SemanticTypeKind::StringView),
                    c.field->field->name};
            if (call->Member() == "isPrivate" && c.field &&
                Receiver(*call) == c.loop->descriptor)
                return CompileTimeValue{SemanticType(SemanticTypeKind::Bool),
                                        c.field->field->syntax->IsPrivate()};
        }
        if (e.Kind() == AstNodeKind::MemberAccessExpression) {
            auto& member = static_cast<const MemberAccessExpression&>(e);
            if (auto* call = Operation(member.Receiver());
                call && IsContextual(*call, c) &&
                call->Member() == "decorator" && call->Arguments().empty() &&
                call->typeArguments.empty()) {
                if (auto* record = Metadata(*call, c))
                    for (auto& entry : record->members)
                        if (entry.parameter->name == member.Member())
                            return entry.value;
            }
        }
        return {};
    }
    Tokens Constant(const CompileTimeValue& value, Range range, Context c) {
        const std::string name =
            "$behavior$value$" + std::to_string(_aliasId++);
        TokenType kind = TokenType::NumberLiteral;
        std::string spelling;
        SemanticType literalType = value.type;
        if (const auto* integer = std::get_if<std::uint64_t>(&value.value))
            spelling = std::to_string(*integer);
        else if (const auto* floating = std::get_if<double>(&value.value)) {
            kind = TokenType::FloatLiteral;
            std::ostringstream text;
            text << std::setprecision(std::numeric_limits<double>::max_digits10)
                 << *floating;
            spelling = text.str();
        } else if (const auto* boolean = std::get_if<bool>(&value.value)) {
            kind = *boolean ? TokenType::True : TokenType::False;
            spelling = *boolean ? "true" : "false";
        } else if (const auto* character = std::get_if<char>(&value.value)) {
            kind = TokenType::CharLiteral;
            spelling.assign(1, *character);
        } else if (const auto* string =
                       std::get_if<std::string>(&value.value)) {
            kind = TokenType::StringLiteral;
            spelling = *string;
        } else if (const auto* enumeration =
                       std::get_if<EnumMetadataValue>(&value.value))
            spelling = std::to_string(enumeration->value);
        else if (const auto* error =
                     std::get_if<ErrorMetadataValue>(&value.value)) {
            if (!error->code) {
                kind = TokenType::None;
                spelling = "none";
            } else {
                spelling = std::to_string(error->code);
                for (const auto& symbol : _model.Symbols())
                    if (symbol->Kind() == SymbolKind::ErrorType &&
                        symbol->Type().Family().name == error->family) {
                        literalType = symbol->Type();
                        break;
                    }
            }
        }
        auto initializer =
            std::make_unique<LiteralExpression>(range, kind, spelling);
        // These values were evaluated from an already validated scalar
        // contract. Preserve their exact type/bits, including signed integers
        // and errors.
        _model._types.insert_or_assign(initializer.get(), literalType);
        auto declaration = std::make_unique<ModuleConstantDeclaration>(
            range, _host.TypeSyntax(value.type.WithConst(), range), name, range,
            std::move(initializer));
        auto symbol = std::make_unique<VariableSymbol>(
            SymbolKind::ModuleConstant, name, value.type.WithConst(), range,
            false);
        (void)_scope->Declare(*symbol);
        _model._moduleConstants[symbol.get()] = declaration.get();
        _model._declarations[declaration.get()] = symbol.get();
        _model._symbols.push_back(std::move(symbol));
        _model._detachedConstants.push_back(std::move(declaration));
        _plan->substitutions.push_back(
            {range, c.field ? c.field->field : nullptr, value});
        return One(TokenType::Identifier, name, range);
    }
    Tokens Rewrite(const AstNode& n, Context c) {
        if (_generatedTokens > 1000000) return {};
        // Reflection expands once, even inside runtime loops. Requirements
        // still cannot depend on runtime selection or execution counts.
        if (n.Kind() == AstNodeKind::WhileStatement ||
            n.Kind() == AstNodeKind::RangeStatement) {
            c.runtimeConditional = true;
            ++c.runtimeLoopDepth;
        }
        if (c.field && c.runtimeLoopDepth == 0 &&
            (n.Kind() == AstNodeKind::BreakStatement ||
             n.Kind() == AstNodeKind::ProceedStatement)) {
            Tokens result;
            if (n.Kind() == AstNodeKind::BreakStatement) {
                Append(result,
                       One(TokenType::Identifier, c.breakFlag, n.Range()));
                Append(result, One(TokenType::Assign, "=", n.Range()));
                Append(result, One(TokenType::True, "true", n.Range()));
                Append(result, One(TokenType::SemiColon, ";", n.Range()));
            }
            Append(result, One(TokenType::Break, "break", n.Range()));
            Append(result, One(TokenType::SemiColon, ";", n.Range()));
            return result;
        }
        if (n.Kind() == AstNodeKind::ExpressionStatement) {
            const auto* call =
                Operation(static_cast<const ExpressionStatement&>(n).Value());
            if (call && IsContextual(*call, c) && call->Member() == "require") {
                if (c.runtimeConditional) {
                    Error(
                        DiagnosticCode::InvalidReflectionContext, call->Range(),
                        "require() selection must be known at compile time", c);
                    return {};
                }
                const auto evaluate = [&](const auto& expression) {
                    return EvaluateCompileTimeValue(
                        expression, _model, [&](const auto& value) {
                            return Reflection(value, c);
                        });
                };
                const auto condition = evaluate(*call->Arguments()[0]);
                const auto message = evaluate(*call->Arguments()[1]);
                if (!condition ||
                    condition->type.Kind() != SemanticTypeKind::Bool ||
                    !std::holds_alternative<bool>(condition->value) ||
                    !message ||
                    message->type.Kind() != SemanticTypeKind::StringView ||
                    !std::holds_alternative<std::string>(message->value)) {
                    Error(DiagnosticCode::InvalidReflectionOperation,
                          call->Range(),
                          "require() requires a compile-time bool and stringv "
                          "message",
                          c);
                    return {};
                }
                if (!std::get<bool>(condition->value))
                    Error(DiagnosticCode::BehaviorRequirementFailed,
                          call->Range(), std::get<std::string>(message->value),
                          c);
                _plan->substitutions.push_back(
                    {call->Range(), c.field ? c.field->field : nullptr,
                     condition});
                return {};
            }
            if (call && c.field && Receiver(*call) == c.loop->descriptor &&
                call->Member() == "requireUnique") {
                if (c.runtimeConditional) {
                    Error(DiagnosticCode::InvalidReflectionContext,
                          call->Range(),
                          "requireUnique() selection must be known at compile "
                          "time",
                          c);
                    return {};
                }
                auto key = EvaluateCompileTimeValue(
                    *call->Arguments()[0], _model,
                    [&](const auto& value) { return Reflection(value, c); });
                if (!key || key->type.Kind() != SemanticTypeKind::StringView ||
                    !std::holds_alternative<std::string>(key->value)) {
                    Error(DiagnosticCode::InvalidReflectionOperation,
                          call->Arguments()[0]->Range(),
                          "requireUnique() requires a compile-time stringv key",
                          c);
                    return {};
                }
                const auto& name = std::get<std::string>(key->value);
                const auto [previous, inserted] = c.uniqueKeys->emplace(
                    name, Context::UniqueKey{c.field->field, call->Range()});
                if (!inserted)
                    Error(DiagnosticCode::DuplicateBehaviorFieldKey,
                          call->Range(),
                          "duplicate reflected field key '" + name + "'", c,
                          {{previous->second.field->syntax->NameRange(),
                            "first used by field '" +
                                previous->second.field->name + "'"},
                           {previous->second.range,
                            "first key validation here"}});
                _plan->substitutions.push_back(
                    {call->Range(), c.field->field, key});
                return {};
            }
        }
        if (n.Kind() == AstNodeKind::FieldIterationStatement) {
            auto& loop = static_cast<const FieldIterationStatement&>(n);
            Tokens result;
            bool transfers = false;
            WalkAst(*loop.body, [&](AstNode& node, bool enter) {
                if (enter && (node.Kind() == AstNodeKind::BreakStatement ||
                              node.Kind() == AstNodeKind::ProceedStatement))
                    transfers = true;
            });
            const auto breakFlag = "$loop$break$" + std::to_string(_aliasId++);
            const auto syntax = [&](std::string text) {
                for (auto pos = text.find(breakFlag); pos != std::string::npos;
                     pos = text.find(breakFlag))
                    text.replace(pos, breakFlag.size(), "fieldBreak");
                diagnostics::DiagnosticBag ignored;
                auto tokens = Tokenizer(text, ignored).Tokenize();
                Tokens result;
                for (auto& token : tokens) {
                    if (token.type == TokenType::EOF_TOKEN) continue;
                    if (token.value == "fieldBreak") token.value = breakFlag;
                    token.range = loop.Range();
                    result.push_back({std::move(token)});
                }
                return result;
            };
            if (transfers)
                Append(result, syntax("{ bool " + breakFlag +
                                      " = false; while true {"));
            std::map<std::string, Context::UniqueKey> uniqueKeys;
            for (auto& field : _plan->pack.fields) {
                Context iteration{
                    &loop, &field, false,
                    "$behavior$type$" + std::to_string(_aliasId++),
                    c.runtimeConditional};
                iteration.uniqueKeys = &uniqueKeys;
                iteration.typeBindings = c.typeBindings;
                iteration.breakFlag = breakFlag;
                auto alias = std::make_unique<Symbol>(
                    field.type.Kind() == SemanticTypeKind::Structure
                        ? SymbolKind::StructureType
                    : field.type.Kind() == SemanticTypeKind::Enum
                        ? SymbolKind::EnumType
                        : SymbolKind::TypeBinding,
                    iteration.fieldTypeAlias, field.type, loop.binderRange,
                    false);
                (void)_scope->Declare(*alias);
                _model._symbols.push_back(std::move(alias));
                Occurrence(loop.binderRange, loop.binder,
                           "F = " + field.type.Name(), true, loop.binderRange);
                auto body = Rewrite(*loop.body, iteration);
                for (auto& token : body) token.field = field.field;
                _generatedTokens += body.size();
                if (_generatedTokens > 1000000) {
                    Error(
                        DiagnosticCode::InvalidBehaviorExpansion, loop.Range(),
                        "behavior expansion token budget exceeded", iteration);
                    break;
                }
                if (transfers) Append(result, syntax("while true {"));
                Append(result, std::move(body));
                if (transfers)
                    Append(result,
                           syntax("break; } if " + breakFlag + " { break; }"));
            }
            if (transfers) Append(result, syntax("break; } }"));
            return result;
        }
        if (n.Kind() == AstNodeKind::IfStatement) {
            const auto& statement = static_cast<const IfStatement&>(n);
            const auto fold = [&](const Expression& expression,
                                  Context* bindings = nullptr) {
                return EvaluateCompileTimeValue(
                    expression, _model, [&](const auto& value) {
                        return Reflection(
                            value, c,
                            &value == &expression ? bindings : nullptr);
                    });
            };
            std::vector<const ConditionalBranch*> branches{
                &statement.PrimaryBranch()};
            for (const auto& branch : statement.ElseIfBranches())
                branches.push_back(branch.get());
            const auto emit = [&](const auto& self, std::size_t index,
                                  Context path) -> Tokens {
                if (index == branches.size())
                    return statement.ElseBody()
                               ? Rewrite(*statement.ElseBody(), path)
                               : Tokens{};
                const auto& branch = *branches[index];
                Context selected = path;
                const auto condition = fold(branch.Condition(), &selected);
                const bool constant =
                    condition && std::holds_alternative<bool>(condition->value);
                if (constant) {
                    _plan->substitutions.push_back(
                        {branch.Condition().Range(),
                         c.field ? c.field->field : nullptr, condition});
                    if (!std::get<bool>(condition->value))
                        return self(self, index + 1, path);
                    if (!branch.Also()) return Rewrite(branch.Body(), selected);
                }
                if (!constant) path.runtimeConditional = true;
                Tokens result =
                    One(TokenType::If, "if", branch.Condition().Range());
                Append(result, constant ? One(TokenType::True, "true",
                                              branch.Condition().Range())
                                        : Rewrite(branch.Condition(), path));
                selected.runtimeConditional = path.runtimeConditional;
                Append(result, Rewrite(branch.Body(), selected));
                if (const auto* also = branch.Also()) {
                    const auto value = fold(also->Condition());
                    const bool known =
                        value && std::holds_alternative<bool>(value->value);
                    if (!known || std::get<bool>(value->value)) {
                        Append(result,
                               One(TokenType::Also, "also", also->Range()));
                        Append(result, known ? One(TokenType::True, "true",
                                                   also->Condition().Range())
                                             : Rewrite(also->Condition(), c));
                        Context alsoPath = path;
                        if (!known) alsoPath.runtimeConditional = true;
                        Append(result, Rewrite(also->Body(), alsoPath));
                    }
                }
                if (!constant) {
                    auto alternative = self(self, index + 1, path);
                    if (!alternative.empty()) {
                        Append(result,
                               One(TokenType::Else, "else", branch.Range()));
                        Append(result, std::move(alternative));
                    }
                }
                return result;
            };
            return emit(emit, 0, c);
        }
        if (auto* expression = dynamic_cast<const Expression*>(&n)) {
            if (auto value = Reflection(*expression, c)) {
                if (n.Kind() == AstNodeKind::MemberAccessExpression) {
                    auto& member =
                        static_cast<const MemberAccessExpression&>(n);
                    auto* record = Metadata(*Operation(member.Receiver()), c);
                    for (auto& entry : record->members)
                        if (entry.parameter->name == member.Member())
                            Occurrence(member.MemberRange(), member.Member(),
                                       entry.value.type.Name() +
                                           " decoration parameter",
                                       false, entry.parameter->nameRange);
                }
                return Constant(*value, n.Range(), c);
            }
        }
        if (n.Kind() == AstNodeKind::MemberAccessExpression) {
            auto& member = static_cast<const MemberAccessExpression&>(n);
            if (auto* call = Operation(member.Receiver());
                call && IsContextual(*call, c) &&
                call->Member() == "decorator") {
                if (!Metadata(*call, c))
                    Error(DiagnosticCode::MissingDecorationProof, call->Range(),
                          "decorator() requires compile-time proof of a direct "
                          "application",
                          c);
                else
                    Error(DiagnosticCode::InvalidMetadataMember,
                          member.MemberRange(),
                          "no decoration parameter named '" + member.Member() +
                              "'",
                          c);
                return One(TokenType::None, "none", n.Range());
            }
        }
        if (auto* call = Operation(n); call && IsContextual(*call, c)) {
            if (call->Member() == "get" && c.field &&
                call->Arguments().size() == 1) {
                Tokens result = One(TokenType::LParen, "(", call->Range());
                Append(result, Rewrite(*call->Arguments()[0], c));
                Append(result, One(TokenType::RParen, ")", call->Range()));
                Append(result,
                       One(TokenType::Arrow, "->", call->MemberRange()));
                Append(result, One(TokenType::Identifier, c.field->field->name,
                                   call->MemberRange()));
                _projections.push_back({call->MemberRange(), c.field->field});
                Occurrence(call->MemberRange(), "get",
                           "field place: " + c.field->type.Name(), false,
                           c.field->field->syntax->NameRange(), true);
                _plan->substitutions.push_back(
                    {call->Range(), c.field->field, {}});
                return result;
            }
            if (call->Member() == "decorator") {
                Error(Metadata(*call, c)
                          ? DiagnosticCode::ReflectionEscape
                          : DiagnosticCode::MissingDecorationProof,
                      call->Range(),
                      "decorator records must be consumed through a typed "
                      "metadata member",
                      c);
                return One(TokenType::None, "none", n.Range());
            }
        }
        if (n.Kind() == AstNodeKind::Type ||
            n.Kind() == AstNodeKind::NameExpression) {
            const auto name =
                n.Kind() == AstNodeKind::Type
                    ? static_cast<const TypeNode&>(n).Name()
                    : static_cast<const NameExpression&>(n).Name();
            if (auto binding = c.typeBindings.find(name);
                binding != c.typeBindings.end()) {
                Occurrence(n.Range(), name,
                           "element type = " + binding->second.type.Name(),
                           true, binding->second.declaration);
                Tokens result;
                if (n.Kind() == AstNodeKind::Type &&
                    static_cast<const TypeNode&>(n).IsConst())
                    Append(result, One(TokenType::Const, "const", n.Range()));
                Append(result, One(TokenType::Identifier, binding->second.alias,
                                   n.Range()));
                return result;
            }
        }
        if (n.Kind() == AstNodeKind::Type && c.field) {
            auto& type = static_cast<const TypeNode&>(n);
            if (type.TypeForm() == TypeKind::Named &&
                type.Name() == c.loop->binder) {
                const std::string& name = c.fieldTypeAlias;
                Tokens out;
                const auto ownership =
                    type.Contract() == OwnershipContract::None
                        ? c.field->ownership
                        : type.Contract();
                if (ownership != OwnershipContract::None)
                    Append(out,
                           One(ownership == OwnershipContract::Own
                                   ? TokenType::Own
                                   : TokenType::Lend,
                               ownership == OwnershipContract::Own ? "own"
                                                                   : "lend",
                               type.Range()));
                if (type.IsConst())
                    Append(out, One(TokenType::Const, "const", type.Range()));
                Append(out, One(TokenType::Identifier, name, type.Range()));
                return out;
            }
        }
        if (n.Kind() == AstNodeKind::NameExpression && c.field &&
            static_cast<const NameExpression&>(n).Name() == c.loop->binder)
            return One(TokenType::Identifier, c.fieldTypeAlias, n.Range());
        // Closures cannot capture reflection. Range loops retain their source
        // children so the parser's synthetic cursor declarations are not
        // mistaken for source expressions during token substitution.
        if (n.Kind() == AstNodeKind::AnonymousFunctionExpression)
            return Raw(n.Range());
        auto children =
            n.Kind() == AstNodeKind::RangeStatement
                ? static_cast<const RangeStatement&>(n).SyntaxChildren()
                : Children(n);
        std::sort(children.begin(), children.end(), [](auto* a, auto* b) {
            return a->Range().start.offset < b->Range().start.offset;
        });
        Tokens out;
        std::size_t cursor = SyntaxRange(n).start.offset;
        for (auto* child : children) {
            if (!Inside(SyntaxRange(n), SyntaxRange(*child)) ||
                SyntaxRange(*child).start.offset < cursor)
                continue;
            for (auto& token : *_tokens)
                if (Inside(SyntaxRange(n), token.range) &&
                    token.range.start.offset >= cursor &&
                    token.range.end.offset <= SyntaxRange(*child).start.offset)
                    out.push_back({token});
            Append(out, Rewrite(*child, c));
            cursor = SyntaxRange(*child).end.offset;
        }
        for (auto& token : *_tokens)
            if (Inside(SyntaxRange(n), token.range) &&
                token.range.start.offset >= cursor)
                out.push_back({token});
        return out;
    }
    void Build(const BehaviorMethodContract& method, CompilationUnit& unit,
               Pending* pending = nullptr) {
        if (!_plan->valid) return;
        if (!pending) {
            // Exact handwritten providers suppress the entire body, including
            // its static reflection validation.
            const auto& methods = _plan->target.Structure()
                                      ? _plan->target.Structure()->methods
                                      : _plan->target.Enum()->methods;
            for (const auto& [name, overloads] : methods)
                for (const auto* provider : overloads)
                    if (!provider->IsPrivate() &&
                        MethodContractOf(*provider,
                                         provider->IsProtected()
                                             ? MethodVisibility::Protected
                                             : MethodVisibility::Public)
                            .SameContract(method.signature))
                        return;
            if (++_generatedBodies > 2048) {
                Error(DiagnosticCode::InvalidBehaviorExpansion,
                      method.declaration->Range(),
                      "behavior declaration budget exceeded");
                return;
            }
        }
        _tokens = &method.declaration->templateTokens;
        if (pending) {
            Validate(method.declaration->Body(), {});
            if (!_plan->valid) return;
            const auto beforeQueries = unit.Declarations().size();
            PrepareBehaviorTypeQueries(unit, *_currentBehavior->declaration,
                                       *method.declaration, _host._diagnostics);
            // Closed query targets may discover generic specializations. The
            // analyzer's existing fixed point registers them before folding.
            if (unit.Declarations().size() != beforeQueries) return;
            for (const auto& [alias, declaration] : unit.genericAliases)
                if (const auto* symbol = _model.DeclaredSymbol(*declaration))
                    (void)_model._scopes.front()->DeclareAs(alias, *symbol);
        }
        if (!_plan->valid) return;
        auto& diagnostics = _host._diagnostics;
        auto& analyzer = _host;
        if (pending) {
            _scope = pending->scope;
            analyzer.SetSource(pending->function->sourceId);
        } else {
            _host.SetSource(_currentBehavior->declaration->sourceId);
            _scope = &analyzer.AddScope(_host._currentScope);
            analyzer._currentScope = _scope;
            auto binder = std::make_unique<Symbol>(
                _plan->target.Structure() ? SymbolKind::StructureType
                                          : SymbolKind::EnumType,
                _currentBehavior->declaration->targetParameter, _plan->target,
                _currentBehavior->declaration->parameterRange, false);
            (void)_scope->Declare(*binder);
            analyzer._model._symbols.push_back(std::move(binder));
        }
        _projections.clear();
        Tokens expanded;
        if (pending)
            expanded = Rewrite(*method.declaration, {});
        else {
            auto signature = method.declaration->Range();
            signature.end = method.declaration->Body().Range().start;
            expanded = Raw(signature);
            Append(expanded, One(TokenType::LCurly, "{", signature));
            Append(expanded, One(TokenType::RCurly, "}", signature));
        }
        if (!_plan->valid) return;
        // Give repeated expansions distinct coordinates inside the detached
        // AST. Every coordinate maps back to its exact original token and F.
        std::vector<BehaviorSourceProvenance> provenance;
        std::vector<Token> materialized;
        std::size_t offset = 0;
        const std::string expansionFile =
            pending ? pending->function->sourceId
                    : "$behavior$" + std::to_string(_generatedBodies);
        for (auto& item : expanded) {
            auto original = item.token.range;
            auto range = original;
            range.start.file = range.end.file = expansionFile;
            range.start.offset = offset;
            offset += std::max<std::size_t>(1, original.Length());
            range.end.offset = offset++;
            provenance.push_back({range, original, item.field});
            item.token.range = range;
            materialized.push_back(std::move(item.token));
        }
        materialized.emplace_back(TokenType::EOF_TOKEN, "",
                                  materialized.empty()
                                      ? method.declaration->Range()
                                      : materialized.back().range);
        Parser parser(std::move(materialized), diagnostics,
                      "$behavior$" + std::to_string(_generatedBodies) + "$");
        for (auto& module : unit.modules)
            if (module.sourceId == _currentBehavior->declaration->sourceId)
                parser.SetImports(module.imports);
        auto parsed = parser.Parse()->TakeDeclarations();
        if (parsed.size() != 1 ||
            parsed.front()->Kind() != AstNodeKind::FunctionDeclaration ||
            diagnostics.HasErrors()) {
            Error(DiagnosticCode::InvalidBehaviorExpansion,
                  method.declaration->Range(),
                  "cannot materialize behavior method");
        } else {
            auto function = std::unique_ptr<FunctionDeclaration>(
                static_cast<FunctionDeclaration*>(parsed.front().release()));
            if (pending) {
                // Reuse the generic expander after binding T/F to concrete
                // syntax. Canonical aliases preserve nominal identity across
                // modules; primitives keep their ordinary syntax.
                const auto alias = [&](const Declaration* declaration) {
                    for (const auto& [name, existing] : unit.genericAliases)
                        if (existing == declaration) return name;
                    std::size_t id = unit.genericAliases.size();
                    while (unit.genericAliases.contains("$generic_alias" +
                                                        std::to_string(id)))
                        ++id;
                    const auto name = "$generic_alias" + std::to_string(id);
                    unit.genericAliases[name] = declaration;
                    return name;
                };
                const auto nominal =
                    [&](SemanticType type) -> const Declaration* {
                    if (type.Structure()) return type.Structure()->syntax;
                    if (type.Enum()) return type.Enum()->syntax;
                    if (type.Interface()) return type.Interface()->syntax;
                    if (type.Kind() == SemanticTypeKind::NamedError ||
                        type.Kind() == SemanticTypeKind::Function)
                        for (const auto& declaration : unit.Declarations()) {
                            const auto* symbol =
                                _model.DeclaredSymbol(*declaration);
                            if (symbol &&
                                (symbol->Kind() == SymbolKind::ErrorType ||
                                 symbol->Kind() == SymbolKind::CallableType) &&
                                symbol->Type() == type.WithoutConst())
                                return declaration.get();
                        }
                    return nullptr;
                };
                const auto canonicalize = [&](auto&& recurse, TypeNode& syntax,
                                              SemanticType type) -> void {
                    if (const auto* declaration = nominal(type))
                        syntax.Rename(alias(declaration));
                    else if (type.Kind() == SemanticTypeKind::Tuple) {
                        for (std::size_t i = 0; i < type.Elements().size(); ++i)
                            recurse(recurse, *syntax.Elements()[i],
                                    type.Elements()[i]);
                    } else if (type.Target()) {
                        auto* target = syntax.Target();
                        if (type.Kind() == SemanticTypeKind::OwnedBuffer)
                            target = target->Target();
                        recurse(recurse, *target, *type.Target());
                    }
                };
                WalkAst(*function, [&](AstNode& node, bool enter) {
                    if (!enter) return;
                    if (node.Kind() == AstNodeKind::Type) {
                        auto& type = static_cast<TypeNode&>(node);
                        if (type.TypeForm() != TypeKind::Named ||
                            !type.typeArguments.empty())
                            return;
                        const auto* symbol = _scope->Lookup(type.Name());
                        if (!symbol) return;
                        if (const auto* declaration = nominal(symbol->Type()))
                            type.Rename(alias(declaration));
                        else if (symbol->Kind() == SymbolKind::TypeBinding) {
                            auto actual = analyzer.TypeSyntax(
                                type.IsConst() ? symbol->Type().WithConst()
                                               : symbol->Type(),
                                type.Range(), type.Contract());
                            canonicalize(canonicalize, *actual, symbol->Type());
                            type = std::move(*actual);
                        }
                    } else if (node.Kind() == AstNodeKind::NameExpression) {
                        auto& name = static_cast<NameExpression&>(node);
                        const auto* symbol = _scope->Lookup(name.Name());
                        if ((name.Name() == _currentBehavior->declaration
                                                ->targetParameter ||
                             name.Name().starts_with("$behavior$type$")) &&
                            symbol &&
                            (symbol->Kind() == SymbolKind::StructureType ||
                             symbol->Kind() == SymbolKind::EnumType))
                            if (const auto* declaration =
                                    nominal(symbol->Type()))
                                name.Rename(alias(declaration));
                    }
                });
                function->sourceId = _currentBehavior->declaration->sourceId;
                function->moduleId = _currentBehavior->declaration->moduleId;
                function->quiverId = _currentBehavior->declaration->quiverId;
                ExpandGeneratedGenerics(unit, *function, diagnostics);
                pending->function->ReplaceBody(*function);
                _plan->bodies[pending->bodyIndex].provenance =
                    std::move(provenance);
                _plan->bodies[pending->bodyIndex].projections = _projections;
                return;
            }
            function->sourceId = expansionFile;
            function->moduleId = _plan->activation->target->moduleId;
            function->quiverId = _plan->activation->target->quiverId;
            analyzer._sourceScopes[expansionFile] = _scope;
            analyzer._sourceModules[expansionFile] = function->moduleId;
            analyzer._inaccessibleImports[expansionFile] =
                analyzer._inaccessibleImports[_currentBehavior->declaration
                                                  ->sourceId];
            analyzer._ambiguousImports[expansionFile] =
                analyzer
                    ._ambiguousImports[_currentBehavior->declaration->sourceId];
            analyzer.RegisterFunction(*function, true);
            _plan->bodies.push_back({std::move(provenance), function.get()});
            _pending.push_back({_plan, &method, function.get(), _scope,
                                _plan->bodies.size() - 1});
            _model._generatedFunctions.push_back(std::move(function));
        }
    }
};
void SemanticAnalyzer::ValidateBehaviorProjections() {
    for (const auto& plan : _model._behaviorPlans)
        for (const auto& body : plan.bodies)
            WalkAst(body.declaration->Body(), [&](AstNode& node, bool enter) {
                if (!enter ||
                    node.Kind() != AstNodeKind::MemberAccessExpression)
                    return;
                const auto& access =
                    static_cast<const MemberAccessExpression&>(node);
                for (const auto& origin : body.provenance) {
                    if (!Inside(origin.expansion, access.MemberRange()))
                        continue;
                    for (const auto& [range, field] : body.projections)
                        if (range == origin.original &&
                            access.Member() == field->name &&
                            _model.FieldFor(access) != field)
                            Report(DiagnosticCode::InvalidReflectionOperation,
                                   access.Range(),
                                   "get() receiver must refer to the concrete "
                                   "behavior target");
                }
            });
}
void SemanticAnalyzer::PlanBehaviors(core::CompilationUnit& unit) {
    BehaviorPlanner(*this).Run(unit);
}
}  // namespace flecha::semantic
