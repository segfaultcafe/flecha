#include "semantic/allocation_checker.hpp"

#include <algorithm>
#include <charconv>
#include <deque>
#include <map>
#include <set>
#include <utility>

#include "semantic/control_flow_graph.hpp"

namespace flecha::semantic {
namespace {
using core::AstNodeKind;
using core::OwnershipContract;
using diagnostics::DiagnosticCode;
using Identity = const core::AstNode*;

bool IsHeapHandle(SemanticType type) {
    return type.Kind() == SemanticTypeKind::Pointer ||
           type.Kind() == SemanticTypeKind::OwnedBuffer;
}

enum class ValueKind {
    Unknown,
    Null,
    True,
    False,
    Error,
    Integer,
    Pointer,
    Interior,
    Dangling,
    Transferred,
    Address,
    Aggregate
};
struct Value final {
    ValueKind kind = ValueKind::Unknown;
    Identity identity = nullptr;
    const Symbol* address = nullptr;
    std::uint64_t integer = 0;
    std::vector<Value> elements;
    bool operator==(const Value&) const = default;
};
struct Resource final {
    bool live = true;
    bool owned = true;
    bool lent = false;
    bool nullable = false;
    bool operator==(const Resource&) const = default;
};
struct Deferred final {
    const core::BlockStatement* scope = nullptr;
    const core::Expression* call = nullptr;
    const FunctionSymbol* function = nullptr;
    std::vector<Value> arguments;
    bool operator==(const Deferred&) const = default;
};
struct State final {
    std::map<const Symbol*, Value> bindings;
    std::map<const core::Expression*, Value> values;
    std::map<Identity, Resource> resources;
    std::vector<Deferred> deferred;
    std::set<std::pair<const Symbol*, const core::StructureField*>>
        initializedOwners;
    std::set<Identity> nullPointers;
    std::map<const core::StructureField*, Value> fieldValues;
    bool operator==(const State&) const = default;
};
struct Outcome final {
    State state;
    Value result;
};

const core::Expression& Unwrap(const core::Expression& expression) {
    if (expression.Kind() == AstNodeKind::ParenthesizedExpression)
        return Unwrap(
            static_cast<const core::ParenthesizedExpression&>(expression)
                .Inner());
    return expression;
}

// Keep paths separate: merging a successful new with its failure loses the
// relationship between its pointer and error, and invents leaks at error exits.
// The finite budget fails closed instead of silently widening away obligations.
class AllocationChecker final {
   public:
    AllocationChecker(const core::CompilationUnit& unit,
                      const SemanticModel& model,
                      diagnostics::DiagnosticBag& diagnostics)
        : _model(model), _diagnostics(diagnostics) {
        for (const auto& declaration : unit.Declarations())
            if (declaration->Kind() == AstNodeKind::FunctionDeclaration)
                Register(static_cast<const core::FunctionDeclaration&>(
                    *declaration));
        for (const auto& function : model.GeneratedFunctions())
            Register(*function);
    }

    void Analyze() {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& [function, graph] : _graphs) {
                for (const FlowNode& node : graph.nodes) {
                    const FunctionSymbol* callee =
                        node.memoryExpression ? Callee(*node.memoryExpression)
                        : node.memoryDefer    ? Callee(node.memoryDefer->Call())
                                              : nullptr;
                    const auto found = _functions.find(callee);
                    const bool heapIndex =
                        node.memoryExpression &&
                        node.memoryExpression->Kind() ==
                            AstNodeKind::IndexExpression &&
                        IsHeapHandle(_model.TypeOf(
                            static_cast<const core::IndexExpression&>(
                                *node.memoryExpression)
                                .Receiver()));
                    const bool heapSlice =
                        node.memoryExpression &&
                        node.memoryExpression->Kind() ==
                            AstNodeKind::SliceExpression &&
                        IsHeapHandle(_model.TypeOf(
                            static_cast<const core::SliceExpression&>(
                                *node.memoryExpression)
                                .Receiver()));
                    if (node.allocation || heapIndex || heapSlice ||
                        (callee &&
                         callee->BuiltIn() == BuiltInFunction::Free) ||
                        (found != _functions.end() &&
                         _resourceFunctions.contains(found->second)))
                        changed |= _resourceFunctions.insert(function).second;
                }
            }
        }
        for (const core::FunctionDeclaration* function : _ordered) {
            // Test harnesses are compiler-generated orchestration. Verifying
            // each user test independently still checks its ownership, while
            // expanding every pass/fail combination in the harness grows as
            // 2^N and rejects otherwise valid test modules at modest sizes.
            if (function->IsTestHarness()) continue;
            if (!_resourceFunctions.contains(function) &&
                !HasContracts(*function) && !HasOwningFields(*function))
                continue;
            Verify(*function);
        }
    }

   private:
    void VerifyNullCall(const core::FunctionDeclaration& function,
                        const std::vector<Value>& supplied) {
        std::vector<bool> mask;
        bool any = false;
        for (const auto& value : supplied) {
            mask.push_back(value.kind == ValueKind::Null);
            any |= mask.back();
        }
        if (!any || !_nullCalls.emplace(&function, mask).second) return;
        if (_nullCalls.size() > 128) {
            Report(DiagnosticCode::UnverifiedOwnership, function,
                   "ownership analysis exceeded its nullable-call budget");
            return;
        }
        State state;
        std::vector<Value> arguments;
        for (std::size_t i = 0; i < function.Parameters().size(); ++i) {
            const auto& parameter = function.Parameters()[i];
            Value value;
            if (i < mask.size() && mask[i])
                value = {ValueKind::Null};
            else if (parameter->Type().TypeForm() == core::TypeKind::Pointer) {
                value = {ValueKind::Pointer, parameter.get()};
                const auto contract = parameter->Type().Contract();
                state.resources[parameter.get()] = {
                    true, contract == OwnershipContract::Own,
                    contract == OwnershipContract::Lend, true};
            }
            arguments.push_back(value);
        }
        SeedFields(function, state);
        const auto savedSteps = _steps;
        _steps = 0;
        for (auto& outcome : Run(function, std::move(state), arguments))
            _nullShapes[{&function, mask}].push_back(std::move(outcome.result));
        _steps = savedSteps;
    }
    void Verify(const core::FunctionDeclaration& function) {
        if (_verified.contains(&function) || _verifying.contains(&function))
            return;
        _verifying.insert(&function);
        const auto savedSteps = _steps;
        _steps = 0;
        State state;
        std::vector<Value> arguments;
        for (const auto& parameter : function.Parameters()) {
            const Symbol* symbol = _model.DeclaredSymbol(*parameter);
            Value argument;
            if (symbol && IsHeapHandle(symbol->Type())) {
                argument = {ValueKind::Pointer, parameter.get()};
                const auto contract = parameter->Type().Contract();
                state.resources[parameter.get()] = {
                    true, contract == OwnershipContract::Own,
                    contract == OwnershipContract::Lend, true};
            }
            arguments.push_back(argument);
        }
        SeedFields(function, state);

        auto outcomes = Run(function, std::move(state), arguments);
        for (auto& outcome : outcomes)
            _returnShapes[&function].push_back(std::move(outcome.result));
        _steps = savedSteps;
        _verifying.erase(&function);
        _verified.insert(&function);
    }

    bool HasContract(const core::TypeNode& type) const {
        if (type.Contract() != OwnershipContract::None) return true;
        for (const auto& element : type.Elements())
            if (HasContract(*element)) return true;
        return false;
    }
    bool HasContracts(const core::FunctionDeclaration& function) const {
        if (HasContract(function.ReturnType())) return true;
        for (const auto& parameter : function.Parameters())
            if (HasContract(parameter->Type())) return true;
        return false;
    }
    const FunctionSymbol* Function(
        const core::FunctionDeclaration& function) const {
        return static_cast<const FunctionSymbol*>(
            _model.DeclaredSymbol(function));
    }
    bool HasOwningFields(const core::FunctionDeclaration& function) const {
        const auto* symbol = Function(function);
        if (!symbol || !symbol->Owner()) return false;
        for (const auto& field : symbol->Owner()->fields)
            if (field.syntax->Type().Contract() == OwnershipContract::Own)
                return true;
        return false;
    }
    void SeedFields(const core::FunctionDeclaration& function, State& state) {
        if (!HasOwningFields(function) || !Function(function)->HasReceiver())
            return;
        for (const auto& field : Function(function)->Owner()->fields) {
            if (field.syntax->Type().Contract() != OwnershipContract::Own)
                continue;
            const bool tear = function.Name() == "tear";
            state.resources[field.syntax] = {true, tear, !tear, true};
            state.fieldValues[field.syntax] = {ValueKind::Pointer,
                                               field.syntax};
        }
    }
    bool Lent(const State& state, const Value& value) const {
        const auto found = state.resources.find(value.identity);
        if (found != state.resources.end() && found->second.lent) return true;
        for (const auto& child : value.elements)
            if (Lent(state, child)) return true;
        return false;
    }
    void Invalidate(Value& value, Identity identity) {
        if (value.identity == identity) value.kind = ValueKind::Transferred;
        for (auto& child : value.elements) Invalidate(child, identity);
    }
    bool Transfer(State& state, const Value& value, const core::AstNode& at) {
        if (value.kind == ValueKind::Null) return true;
        CheckUse(state, value, at);
        const auto found = state.resources.find(value.identity);
        if (value.kind != ValueKind::Pointer ||
            found == state.resources.end() || !found->second.live ||
            !found->second.owned || found->second.lent) {
            Report(DiagnosticCode::UnverifiedOwnership, at,
                   "own requires a live allocation with cleanup "
                   "responsibility, or null",
                   value.identity);
            return false;
        }
        for (const auto& deferred : state.deferred)
            for (const auto& argument : deferred.arguments)
                if (RefersTo(argument, value.identity)) {
                    Report(DiagnosticCode::UnverifiedOwnership, at,
                           "ownership transfer conflicts with a pending "
                           "deferred use or cleanup",
                           value.identity);
                    return false;
                }
        found->second.live = false;
        for (auto& [symbol, alias] : state.bindings)
            Invalidate(alias, value.identity);
        for (auto& [expression, alias] : state.values)
            Invalidate(alias, value.identity);
        return true;
    }

    void Register(const core::FunctionDeclaration& function) {
        if (function.IsExtern()) return;
        const auto* symbol =
            static_cast<const FunctionSymbol*>(_model.DeclaredSymbol(function));
        if (!symbol) return;
        _functions[symbol] = &function;
        _graphs.emplace(&function, BuildControlFlowGraph(function, _model));
        _ordered.push_back(&function);
        if (HasContracts(function) || HasOwningFields(function))
            _resourceFunctions.insert(&function);
    }

    void Report(DiagnosticCode code, const core::AstNode& at,
                const std::string& message, Identity origin = nullptr) {
        if (!_reported.emplace(code, at.Range().start).second) return;
        source::SourceRange range = at.Range();
        if (at.Kind() == AstNodeKind::BlockStatement && !range.Empty()) {
            range.start = range.end;
            --range.start.offset;
            if (range.start.column > 1) --range.start.column;
        }
        _diagnostics.Report(code, range, message);
        if (origin && origin != &at)
            _diagnostics.Report(code, origin->Range(),
                                "allocation or pointer originates here",
                                diagnostics::DiagnosticSeverity::Note);
    }

    Value Get(const State& state, const core::Expression& expression) const {
        const auto found = state.values.find(&expression);
        return found == state.values.end() ? Value{} : found->second;
    }
    Value Binding(const State& state, const Symbol* symbol) const {
        const auto found = state.bindings.find(symbol);
        return found == state.bindings.end() ? Value{} : found->second;
    }
    void Put(State& state, const core::Expression& expression, Value value) {
        if (value.kind == ValueKind::Unknown)
            state.values.erase(&expression);
        else
            state.values[&expression] = std::move(value);
    }
    void Bind(State& state, const Symbol* symbol, Value value) {
        if (!symbol) return;
        if (value.kind == ValueKind::Unknown)
            state.bindings.erase(symbol);
        else
            state.bindings[symbol] = std::move(value);
    }
    bool Contains(const Value& value, Identity identity) const {
        // An interior address can observe storage but cannot discharge its
        // allocation obligation. Only the original pointer can be returned as
        // an owner or kept as a future free argument.
        if (value.identity == identity && value.kind == ValueKind::Pointer)
            return true;
        return std::any_of(
            value.elements.begin(), value.elements.end(),
            [&](const Value& child) { return Contains(child, identity); });
    }
    bool RefersTo(const Value& value, Identity identity) const {
        if (value.identity == identity) return true;
        for (const auto& child : value.elements)
            if (RefersTo(child, identity)) return true;
        return false;
    }
    bool Tracks(const Value& value) const {
        if (value.identity) return true;
        return std::any_of(value.elements.begin(), value.elements.end(),
                           [&](const Value& child) { return Tracks(child); });
    }
    void CheckUse(const State& state, const Value& value,
                  const core::AstNode& at, bool dereference = false) {
        const auto found = state.resources.find(value.identity);
        if (value.kind == ValueKind::Transferred) {
            Report(DiagnosticCode::UseAfterFree, at,
                   "pointer is unavailable after ownership transfer",
                   value.identity);
            return;
        }
        if (value.kind == ValueKind::Dangling ||
            (found != state.resources.end() && !found->second.live))
            Report(DiagnosticCode::UseAfterFree, at,
                   "pointer may be used after its allocation was freed",
                   value.identity);
        if (dereference && value.kind == ValueKind::Null)
            Report(DiagnosticCode::NullDereference, at,
                   "pointer can be null on this path; check the allocation "
                   "error or pointer first");
        for (const Value& child : value.elements) CheckUse(state, child, at);
    }
    void Free(State& state, const Value& value, const core::AstNode& at) {
        if (value.kind == ValueKind::Null) return;
        if (Lent(state, value)) {
            Report(DiagnosticCode::UnverifiedOwnership, at,
                   "a lent pointer cannot be freed", value.identity);
            return;
        }
        if (value.kind == ValueKind::Transferred) {
            CheckUse(state, value, at);
            return;
        }
        const auto found = state.resources.find(value.identity);
        if (value.kind == ValueKind::Dangling ||
            (found != state.resources.end() && !found->second.live)) {
            Report(DiagnosticCode::DoubleFree, at,
                   "allocation may be freed more than once on this path",
                   value.identity);
        } else if (value.kind == ValueKind::Interior ||
                   value.kind == ValueKind::Address) {
            Report(DiagnosticCode::InvalidFree, at,
                   "free requires the original allocation pointer, not stack "
                   "or interior storage",
                   value.identity);
        } else if (value.kind == ValueKind::Pointer &&
                   found != state.resources.end()) {
            found->second.live = false;
        } else {
            Report(DiagnosticCode::UnverifiedOwnership, at,
                   "cannot verify the allocation provenance of this free");
        }
    }

    // Reusing an allocation site must not resurrect aliases of its previous
    // freed instance. Dangling values keep their original diagnostic location.
    void Retire(Value& value, Identity identity) {
        if (value.identity == identity) value.kind = ValueKind::Dangling;
        for (Value& child : value.elements) Retire(child, identity);
    }
    bool Allocate(State& state, const core::CallExpression& call) {
        const auto previous = state.resources.find(&call);
        if (previous != state.resources.end() && previous->second.live) {
            Report(DiagnosticCode::UnverifiedOwnership, call,
                   "allocation site is revisited while its previous allocation "
                   "is still live; "
                   "release it in an inner scope before repeating");
            return false;
        }
        for (auto& [symbol, value] : state.bindings) Retire(value, &call);
        for (auto& [expression, value] : state.values) Retire(value, &call);
        for (Deferred& deferred : state.deferred)
            for (Value& argument : deferred.arguments) Retire(argument, &call);
        state.resources[&call] = {};
        Value result{ValueKind::Aggregate};
        result.elements = {{ValueKind::Pointer, &call}, {ValueKind::Null}};
        Put(state, call, std::move(result));
        return true;
    }

    void CheckLost(State& state, const core::AstNode& at,
                   const Value& returned = {}) {
        for (auto& [identity, resource] : state.resources) {
            if (!resource.live || !resource.owned ||
                identity->Kind() == AstNodeKind::StructureField ||
                Contains(returned, identity))
                continue;
            bool held = false;
            for (const auto& [field, value] : state.fieldValues)
                held |= Contains(value, identity);
            for (const auto& [symbol, value] : state.bindings)
                held |= Contains(value, identity);
            for (const auto& [expression, value] : state.values)
                held |= Contains(value, identity);
            for (const Deferred& deferred : state.deferred)
                for (const Value& value : deferred.arguments)
                    held |= Contains(value, identity);
            if (!held) {
                Report(DiagnosticCode::MemoryLeak, at,
                       "allocation can be lost without free on this path",
                       identity);
                // This obligation is already diagnosed. Avoid a cascade and
                // let the worklist converge for loops with a lost allocation.
                resource.live = false;
            }
        }
    }

    const Symbol* DirectTarget(const core::Expression& expression) const {
        const core::Expression& target = Unwrap(expression);
        return target.Kind() == AstNodeKind::NameExpression
                   ? _model.SymbolFor(
                         static_cast<const core::NameExpression&>(target))
                   : nullptr;
    }
    Value Address(const State& state,
                  const core::Expression& expression) const {
        const core::Expression& target = Unwrap(expression);
        if (const Symbol* symbol = DirectTarget(target))
            return {ValueKind::Address, nullptr, symbol};
        if (target.Kind() == AstNodeKind::UnaryExpression) {
            const auto& unary =
                static_cast<const core::UnaryExpression&>(target);
            if (unary.OperatorType() == TokenType::Mul)
                return Get(state, unary.Operand());
        }
        if (target.Kind() == AstNodeKind::MemberAccessExpression) {
            const auto& field =
                static_cast<const core::MemberAccessExpression&>(target);
            Value receiver = field.Access() == TokenType::Arrow
                                 ? Get(state, field.Receiver())
                                 : Address(state, field.Receiver());
            if (receiver.identity) {
                receiver.kind = ValueKind::Interior;
                return receiver;
            }
            return {ValueKind::Address};
        }
        if (target.Kind() == AstNodeKind::IndexExpression) {
            const auto& index =
                static_cast<const core::IndexExpression&>(target);
            Value receiver = IsHeapHandle(_model.TypeOf(index.Receiver()))
                                 ? Get(state, index.Receiver())
                                 : Address(state, index.Receiver());
            if (receiver.identity) receiver.kind = ValueKind::Interior;
            receiver.address = nullptr;
            return receiver;
        }
        return {};
    }
    void Assign(State& state, const core::Expression& target, Value value) {
        if (const Symbol* symbol = DirectTarget(target)) {
            Bind(state, symbol, std::move(value));
            return;
        }
        const core::Expression& unwrapped = Unwrap(target);
        if (unwrapped.Kind() == AstNodeKind::IndexExpression) {
            const auto& index =
                static_cast<const core::IndexExpression&>(unwrapped);
            if (!IsHeapHandle(_model.TypeOf(index.Receiver())) &&
                Owns(state, Get(state, index.Receiver())))
                Report(DiagnosticCode::UnverifiedOwnership, target,
                       "cannot verify mutation of an aggregate containing "
                       "allocation handles");
        }
        if (unwrapped.Kind() == AstNodeKind::MemberAccessExpression) {
            const auto& member =
                static_cast<const core::MemberAccessExpression&>(unwrapped);
            const auto* field = _model.FieldFor(member);
            if (field &&
                field->syntax->Type().Contract() == OwnershipContract::Own) {
                const Symbol* receiver = DirectTarget(member.Receiver());
                const bool building =
                    receiver && receiver->Name() == "self" &&
                    receiver->Kind() == SymbolKind::LocalVariable;
                if (!building) {
                    if (!receiver || receiver->Name() != "self") {
                        Report(DiagnosticCode::UnverifiedOwnership, target,
                               "replace owning fields only through self",
                               field->syntax);
                        return;
                    }
                    const Value previous = Get(state, member);
                    if (previous.kind != ValueKind::Null) {
                        Report(
                            DiagnosticCode::MemoryLeak, target,
                            "take the current owning field before replacing it",
                            field->syntax);
                        return;
                    }
                    if (!Transfer(state, value, target)) return;
                    if (value.kind == ValueKind::Null) {
                        state.fieldValues[field->syntax] = {ValueKind::Null};
                        return;
                    }
                    const bool tear =
                        !_active.empty() && _active.back()->Name() == "tear";
                    state.resources[&target] = {true, tear, !tear, true};
                    state.nullPointers.erase(&target);
                    state.fieldValues[field->syntax] = {ValueKind::Pointer,
                                                        &target};
                    return;
                }
                const auto slot = std::make_pair(receiver, field->syntax);
                if (state.initializedOwners.contains(slot))
                    Report(DiagnosticCode::MemoryLeak, target,
                           "overwriting an initialized owning field loses its "
                           "allocation",
                           field->syntax);
                if (Transfer(state, value, target) &&
                    value.kind != ValueKind::Null)
                    state.initializedOwners.insert(slot);
                return;
            }
        }
        const Value address = Address(state, target);
        if (address.kind == ValueKind::Address && address.address) {
            Bind(state, address.address, std::move(value));
        } else if (Owns(state, value) || Lent(state, value)) {
            Report(DiagnosticCode::UnverifiedOwnership, target,
                   "cannot verify ownership stored through a field, index, or "
                   "indirect pointer; "
                   "keep the allocation in a local binding");
        }
    }

    const FunctionSymbol* Callee(const core::Expression& expression) const {
        if (expression.Kind() == AstNodeKind::CallExpression) {
            const auto& call =
                static_cast<const core::CallExpression&>(expression);
            const Symbol* symbol = DirectTarget(call.Callee());
            return symbol && symbol->Kind() == SymbolKind::Function
                       ? static_cast<const FunctionSymbol*>(symbol)
                       : nullptr;
        }
        if (expression.Kind() == AstNodeKind::MemberCallExpression)
            return _model.MethodFor(
                static_cast<const core::MemberCallExpression&>(expression));
        return nullptr;
    }
    std::vector<Value> Arguments(const State& state,
                                 const core::Expression& expression) const {
        std::vector<Value> result;
        if (expression.Kind() == AstNodeKind::CallExpression) {
            for (const auto& argument :
                 static_cast<const core::CallExpression&>(expression)
                     .Arguments())
                result.push_back(Get(state, *argument));
        } else if (expression.Kind() == AstNodeKind::MemberCallExpression) {
            const auto& call =
                static_cast<const core::MemberCallExpression&>(expression);
            const FunctionSymbol* method = _model.MethodFor(call);
            if (method && method->HasReceiver())
                result.push_back(Get(state, call.Receiver()));
            for (const auto& argument : call.Arguments())
                result.push_back(Get(state, *argument));
        }
        return result;
    }

    bool Relevant(const FunctionSymbol* function,
                  const std::vector<Value>& arguments) const {
        for (const Value& argument : arguments)
            if (Tracks(argument) || argument.address) return true;
        if (!function) return false;
        if (function->BuiltIn() == BuiltInFunction::Free) return true;
        for (const SemanticType& type : function->ParameterTypes())
            if (IsHeapHandle(type)) return true;
        const auto found = _functions.find(function);
        if (found == _functions.end()) return false;
        // Calls that allocate are interpreted even without pointer arguments,
        // including factories that return an allocation in a tuple.
        return _resourceFunctions.contains(found->second);
    }

    void InvalidateMutableFields(State& state, const FunctionSymbol* function,
                                 const core::Expression& at,
                                 const std::vector<Value>& arguments) {
        if (!function) return;
        for (const auto& parameter : function->ParameterTypes()) {
            if (parameter.Kind() != SemanticTypeKind::Reference ||
                parameter.Target()->Kind() != SemanticTypeKind::Structure ||
                parameter.Target()->IsConst())
                continue;
            for (const auto& field : parameter.Target()->Structure()->fields) {
                auto slot = state.fieldValues.find(field.syntax);
                if (slot == state.fieldValues.end()) continue;
                const auto identity = slot->second.identity;
                if (identity) {
                    for (const auto& argument : arguments)
                        if (RefersTo(argument, identity))
                            Report(DiagnosticCode::UnverifiedOwnership, at,
                                   "a mutable owner call cannot also receive "
                                   "an alias of its owning field",
                                   identity);
                    for (const auto& deferred : state.deferred)
                        for (const auto& argument : deferred.arguments)
                            if (RefersTo(argument, identity))
                                Report(DiagnosticCode::UnverifiedOwnership, at,
                                       "mutable owner call conflicts with a "
                                       "deferred field alias",
                                       identity);
                    state.resources[identity].live = false;
                    for (auto& [symbol, alias] : state.bindings)
                        Invalidate(alias, identity);
                    for (auto& [node, alias] : state.values)
                        Invalidate(alias, identity);
                    state.nullPointers.erase(identity);
                }
                // A mutable call may replace any owning field. Keep a fresh
                // abstract field capability, but never revive earlier aliases.
                const bool tear =
                    !_active.empty() && _active.back()->Name() == "tear";
                state.resources[field.syntax] = {true, tear, !tear, true};
                state.nullPointers.erase(field.syntax);
                slot->second = {ValueKind::Pointer, field.syntax};
            }
        }
    }
    std::vector<Outcome> Invoke(const core::Expression& expression,
                                const FunctionSymbol* function,
                                const std::vector<Value>& arguments,
                                State state) {
        if (function && function->BuiltIn() == BuiltInFunction::Take) {
            const auto& call =
                static_cast<const core::CallExpression&>(expression);
            const auto& member =
                static_cast<const core::MemberAccessExpression&>(
                    Unwrap(*call.Arguments()[0]));
            const auto* field = _model.FieldFor(member);
            Value value = arguments[0];
            if (value.kind == ValueKind::Null) {
                state.fieldValues[field->syntax] = {ValueKind::Null};
                return {{std::move(state), value}};
            }
            CheckUse(state, value, expression);
            auto& resource = state.resources[value.identity];
            resource.owned = true;
            resource.lent = false;
            if (!Transfer(state, value, expression))
                return {{std::move(state), {}}};
            state.fieldValues[field->syntax] = {ValueKind::Null};
            if (state.resources.contains(&expression) &&
                state.resources[&expression].live) {
                Report(DiagnosticCode::UnverifiedOwnership, expression,
                       "take site is revisited while its previous owner is "
                       "still live");
                return {{std::move(state), {}}};
            }
            for (auto& [symbol, alias] : state.bindings)
                Retire(alias, &expression);
            for (auto& [node, alias] : state.values) Retire(alias, &expression);
            state.resources[&expression] = {true, true, false,
                                            resource.nullable};
            return {{std::move(state), {ValueKind::Pointer, &expression}}};
        }
        if (function && function->BuiltIn() == BuiltInFunction::Free) {
            if (!arguments.empty()) Free(state, arguments[0], expression);
            return {{std::move(state), {}}};
        }
        for (const Value& argument : arguments)
            CheckUse(state, argument, expression);
        InvalidateMutableFields(state, function, expression, arguments);
        if (function == nullptr &&
            expression.Kind() == AstNodeKind::CallExpression) {
            const auto& call =
                static_cast<const core::CallExpression&>(expression);
            const SemanticType callable = _model.TypeOf(call.Callee());
            if (callable.Kind() == SemanticTypeKind::Function) {
                const auto& signature = callable.Signature();
                std::set<Identity> consumed;
                for (std::size_t i = 0;
                     i < arguments.size() && i < signature.parameters.size();
                     ++i) {
                    const auto contract =
                        i < signature.parameterContracts.size()
                            ? signature.parameterContracts[i]
                            : OwnershipContract::None;
                    if (contract == OwnershipContract::Own &&
                        arguments[i].kind != ValueKind::Null) {
                        if (!consumed.insert(arguments[i].identity).second)
                            Report(DiagnosticCode::UnverifiedOwnership,
                                   expression,
                                   "the same allocation cannot satisfy two "
                                   "consuming callable parameters",
                                   arguments[i].identity);
                        Transfer(state, arguments[i], expression);
                    } else if (contract == OwnershipContract::None &&
                               Tracks(arguments[i]) &&
                               signature.parameters[i].Kind() ==
                                   SemanticTypeKind::Pointer) {
                        Report(DiagnosticCode::UnverifiedOwnership, expression,
                               "an indirect callable needs own or lend on raw "
                               "pointer parameters receiving tracked storage",
                               arguments[i].identity);
                    }
                }
                Value result =
                    IndirectContractResult(state, expression, signature);
                return {{std::move(state), std::move(result)}};
            }
        }
        if (function && function->IsExtern()) {
            for (const Value& argument : arguments)
                if (Owns(state, argument) || Lent(state, argument))
                    Report(DiagnosticCode::UnverifiedOwnership, expression,
                           "extern calls cannot receive tracked allocations "
                           "until external ownership contracts are supported",
                           argument.identity);
            return {{std::move(state), {}}};
        }
        if (!Relevant(function, arguments)) return {{std::move(state), {}}};
        const auto found = _functions.find(function);
        if (found == _functions.end()) {
            if (function && function->BuiltIn() != BuiltInFunction::None)
                return {{std::move(state), {}}};
            Report(DiagnosticCode::UnverifiedOwnership, expression,
                   "cannot verify ownership effects of this call");
            return {{std::move(state), {}}};
        }
        if (HasContracts(*found->second) || HasOwningFields(*found->second)) {
            const auto& syntax = *found->second;
            std::set<Identity> consumed;
            for (std::size_t i = 0;
                 i < arguments.size() && i < syntax.Parameters().size(); ++i) {
                const auto contract = syntax.Parameters()[i]->Type().Contract();
                if (arguments[i].address &&
                    (Owns(state, arguments[i]) ||
                     Lent(state, Binding(state, arguments[i].address))))
                    Report(DiagnosticCode::UnverifiedOwnership, expression,
                           "a contract cannot yet summarize mutation through "
                           "an address of a tracked pointer slot");
                if (contract == OwnershipContract::Own &&
                    arguments[i].kind != ValueKind::Null) {
                    if (!consumed.insert(arguments[i].identity).second)
                        Report(DiagnosticCode::UnverifiedOwnership, expression,
                               "the same allocation cannot satisfy two "
                               "consuming parameters",
                               arguments[i].identity);
                    for (std::size_t j = 0; j < arguments.size(); ++j)
                        if (i != j &&
                            RefersTo(arguments[j], arguments[i].identity))
                            Report(DiagnosticCode::UnverifiedOwnership,
                                   expression,
                                   "a consumed allocation cannot also be "
                                   "passed through an alias",
                                   arguments[i].identity);
                    Transfer(state, arguments[i], expression);
                } else if (contract == OwnershipContract::None &&
                           ((Tracks(arguments[i]) &&
                             syntax.Parameters()[i]->Type().TypeForm() !=
                                 core::TypeKind::Reference) ||
                            (arguments[i].address &&
                             Owns(state, arguments[i])))) {
                    Report(DiagnosticCode::UnverifiedOwnership, expression,
                           "an annotated function needs own or lend on every "
                           "raw pointer receiving tracked storage",
                           arguments[i].identity);
                }
            }
            Verify(syntax);
            VerifyNullCall(syntax, arguments);
            std::vector<Outcome> outcomes;
            std::vector<bool> nullMask;
            for (const auto& argument : arguments)
                nullMask.push_back(argument.kind == ValueKind::Null);
            const auto nullableShapes = _nullShapes.find({&syntax, nullMask});
            const auto shapes = _returnShapes.find(&syntax);
            const std::vector<Value>* alternatives =
                nullableShapes != _nullShapes.end() ? &nullableShapes->second
                : shapes != _returnShapes.end()     ? &shapes->second
                                                    : nullptr;
            if (alternatives && !alternatives->empty()) {
                for (const auto& shape : *alternatives) {
                    State next = state;
                    Value result = ContractResult(next, expression,
                                                  syntax.ReturnType(), &shape);
                    outcomes.push_back({std::move(next), std::move(result)});
                }
            } else {
                auto result =
                    ContractResult(state, expression, syntax.ReturnType());
                outcomes.push_back({std::move(state), std::move(result)});
            }
            return outcomes;
        }
        if (std::find(_active.begin(), _active.end(), found->second) !=
            _active.end()) {
            Report(DiagnosticCode::UnverifiedOwnership, expression,
                   "recursive pointer ownership requires a contract and cannot "
                   "yet be verified");
            return {};
        }
        return Run(*found->second, std::move(state), arguments);
    }

    Value ContractResult(State& state, const core::Expression& call,
                         const core::TypeNode& type,
                         const Value* shape = nullptr) {
        if (type.Contract() == OwnershipContract::Own) {
            if (shape && shape->kind == ValueKind::Null)
                return {ValueKind::Null};
            const auto key = std::make_pair(&call, &type);
            auto& origin = _returnOrigins[key];
            if (!origin)
                origin = std::make_unique<core::TypeNode>(
                    call.Range(), core::PrimitiveType::Void);
            const Identity identity = origin.get();
            const auto previous = state.resources.find(identity);
            if (previous != state.resources.end() && previous->second.live)
                Report(DiagnosticCode::UnverifiedOwnership, call,
                       "owning return site is revisited while its previous "
                       "result remains live",
                       identity);
            for (auto& [symbol, value] : state.bindings)
                Retire(value, identity);
            for (auto& [expression, value] : state.values)
                Retire(value, identity);
            state.resources[identity] = {true, true, false, true};
            return {ValueKind::Pointer, identity};
        }
        if (type.TypeForm() == core::TypeKind::Tuple) {
            Value result{ValueKind::Aggregate};
            for (std::size_t i = 0; i < type.Elements().size(); ++i)
                result.elements.push_back(ContractResult(
                    state, call, *type.Elements()[i],
                    shape && i < shape->elements.size() ? &shape->elements[i]
                                                        : nullptr));
            return result;
        }
        if (shape &&
            (shape->kind == ValueKind::Null || shape->kind == ValueKind::Error))
            return *shape;
        return {};
    }

    Value IndirectContractResult(State& state, const core::Expression& call,
                                 const CallableSignature& signature) {
        const auto owned = [&](std::size_t index) {
            return index < signature.resultContracts.size() &&
                   signature.resultContracts[index] == OwnershipContract::Own;
        };
        const auto makeOwned = [&](std::size_t index) {
            const auto key = std::make_pair(&call, index);
            auto& origin = _indirectReturnOrigins[key];
            if (!origin)
                origin = std::make_unique<core::TypeNode>(
                    call.Range(), core::PrimitiveType::Void);
            const Identity identity = origin.get();
            const auto previous = state.resources.find(identity);
            if (previous != state.resources.end() && previous->second.live)
                Report(DiagnosticCode::UnverifiedOwnership, call,
                       "owning indirect-call result is revisited while its "
                       "previous allocation is still live",
                       identity);
            for (auto& [symbol, value] : state.bindings)
                Retire(value, identity);
            for (auto& [node, value] : state.values) Retire(value, identity);
            state.resources[identity] = {true, true, false, true};
            return Value{ValueKind::Pointer, identity};
        };
        if (signature.result.Kind() != SemanticTypeKind::Tuple)
            return owned(0) ? makeOwned(0) : Value{};
        Value result{ValueKind::Aggregate};
        for (std::size_t i = 0; i < signature.result.Elements().size(); ++i)
            result.elements.push_back(owned(i) ? makeOwned(i) : Value{});
        return result;
    }
    void CheckReturn(State& state, const Value& result,
                     const core::TypeNode& type, const core::AstNode& at,
                     bool explicitContract,
                     std::set<Identity>& returnedOwners) {
        if (type.TypeForm() == core::TypeKind::Tuple) {
            for (std::size_t i = 0; i < type.Elements().size(); ++i)
                CheckReturn(
                    state,
                    i < result.elements.size() ? result.elements[i] : Value{},
                    *type.Elements()[i], at, explicitContract, returnedOwners);
            return;
        }
        if (Lent(state, result))
            Report(DiagnosticCode::UnverifiedOwnership, at,
                   "a lent pointer or its derived aliases cannot escape in a "
                   "return",
                   result.identity);
        if (type.Contract() == OwnershipContract::Own) {
            if (result.kind == ValueKind::Pointer &&
                !returnedOwners.insert(result.identity).second)
                Report(DiagnosticCode::UnverifiedOwnership, at,
                       "one allocation cannot be returned as multiple owners",
                       result.identity);
            const auto found = state.resources.find(result.identity);
            if (result.kind != ValueKind::Null &&
                (result.kind != ValueKind::Pointer ||
                 found == state.resources.end() || !found->second.owned ||
                 !found->second.live || found->second.lent))
                Report(DiagnosticCode::UnverifiedOwnership, at,
                       "owning return requires a live owned allocation or null",
                       result.identity);
        } else if (explicitContract && Owns(state, result)) {
            Report(DiagnosticCode::UnverifiedOwnership, at,
                   "returning cleanup responsibility requires an own return "
                   "contract",
                   result.identity);
        }
    }

    // Consume child temporaries only after their parent has used them. Address
    // formation needs the lvalue's children, so keep those until the address is
    // evaluated; the statement boundary then drops all remaining temporaries.
    void ForgetChildren(State& state, const core::Expression& expression) {
        switch (expression.Kind()) {
            case AstNodeKind::ParenthesizedExpression:
                state.values.erase(
                    &static_cast<const core::ParenthesizedExpression&>(
                         expression)
                         .Inner());
                break;
            case AstNodeKind::CastExpression:
                state.values.erase(
                    &static_cast<const core::CastExpression&>(expression)
                         .Value());
                break;
            case AstNodeKind::BinaryExpression: {
                const auto& binary =
                    static_cast<const core::BinaryExpression&>(expression);
                state.values.erase(&binary.Left());
                state.values.erase(&binary.Right());
                break;
            }
            case AstNodeKind::AssignmentExpression: {
                const auto& assignment =
                    static_cast<const core::AssignmentExpression&>(expression);
                state.values.erase(&assignment.Target());
                state.values.erase(&assignment.Value());
                break;
            }
            case AstNodeKind::CallExpression: {
                const auto& call =
                    static_cast<const core::CallExpression&>(expression);
                state.values.erase(&call.Callee());
                for (const auto& argument : call.Arguments())
                    state.values.erase(argument.get());
                break;
            }
            case AstNodeKind::MemberCallExpression: {
                const auto& call =
                    static_cast<const core::MemberCallExpression&>(expression);
                state.values.erase(&call.Receiver());
                for (const auto& argument : call.Arguments())
                    state.values.erase(argument.get());
                break;
            }
            case AstNodeKind::TupleExpression:
                for (const auto& element :
                     static_cast<const core::TupleExpression&>(expression)
                         .Elements())
                    state.values.erase(element.get());
                break;
            case AstNodeKind::ArrayExpression:
                for (const auto& element :
                     static_cast<const core::ArrayExpression&>(expression)
                         .Elements())
                    state.values.erase(element.get());
                break;
            case AstNodeKind::MatchExpression: {
                const auto& match =
                    static_cast<const core::MatchExpression&>(expression);
                state.values.erase(&match.Value());
                for (const auto& arm : match.Arms())
                    state.values.erase(&arm->Value());
                break;
            }
            default:
                break;
        }
    }

    Value Evaluate(State& state, const core::Expression& expression) {
        switch (expression.Kind()) {
            case AstNodeKind::LiteralExpression: {
                const auto& literal =
                    static_cast<const core::LiteralExpression&>(expression);
                switch (literal.LiteralType()) {
                    case TokenType::Null:
                    case TokenType::None:
                        return {ValueKind::Null};
                    case TokenType::True:
                        return {ValueKind::True};
                    case TokenType::False:
                        return {ValueKind::False};
                    case TokenType::NumberLiteral: {
                        Value result{ValueKind::Integer};
                        const std::string& text = literal.Value();
                        const auto parsed = std::from_chars(
                            text.data(), text.data() + text.size(),
                            result.integer);
                        return parsed.ec == std::errc{} &&
                                       parsed.ptr == text.data() + text.size()
                                   ? result
                                   : Value{};
                    }
                    default:
                        return {};
                }
            }
            case AstNodeKind::NameExpression: {
                Value value = Binding(
                    state,
                    _model.SymbolFor(
                        static_cast<const core::NameExpression&>(expression)));
                CheckUse(state, value, expression);
                return value;
            }
            case AstNodeKind::ParenthesizedExpression:
                return Get(state,
                           static_cast<const core::ParenthesizedExpression&>(
                               expression)
                               .Inner());
            case AstNodeKind::CastExpression:
                return Get(state,
                           static_cast<const core::CastExpression&>(expression)
                               .Value());
            case AstNodeKind::AssignmentExpression: {
                const auto& assignment =
                    static_cast<const core::AssignmentExpression&>(expression);
                Value value = Get(state, assignment.Value());
                CheckUse(state, value, expression);
                Assign(state, assignment.Target(), value);
                return value;
            }
            case AstNodeKind::UnaryExpression: {
                const auto& unary =
                    static_cast<const core::UnaryExpression&>(expression);
                Value operand = Get(state, unary.Operand());
                if (unary.OperatorType() == TokenType::RawAddress ||
                    unary.OperatorType() == TokenType::Ampersand) {
                    const core::Expression& target = Unwrap(unary.Operand());
                    if (target.Kind() == AstNodeKind::MemberAccessExpression) {
                        const auto* field = _model.FieldFor(
                            static_cast<const core::MemberAccessExpression&>(
                                target));
                        if (field && field->syntax->Type().Contract() ==
                                         OwnershipContract::Own)
                            Report(DiagnosticCode::UnverifiedOwnership,
                                   expression,
                                   "taking an address of an owning field would "
                                   "bypass its ownership contract",
                                   field->syntax);
                    }

                    if (target.Kind() == AstNodeKind::IndexExpression &&
                        Owns(state,
                             Get(state,
                                 static_cast<const core::IndexExpression&>(
                                     target)
                                     .Receiver())))
                        Report(DiagnosticCode::UnverifiedOwnership, expression,
                               "cannot track an address into an aggregate "
                               "containing allocation handles");
                    return Address(state, unary.Operand());
                }
                if (unary.OperatorType() == TokenType::Mul) {
                    CheckUse(state, operand, expression, true);
                    if (operand.kind == ValueKind::Address)
                        return Binding(state, operand.address);
                    return {};
                }
                if (unary.OperatorType() == TokenType::Increment)
                    Assign(state, unary.Operand(), {});
                if (unary.OperatorType() == TokenType::Bang) {
                    if (operand.kind == ValueKind::True)
                        return {ValueKind::False};
                    if (operand.kind == ValueKind::False)
                        return {ValueKind::True};
                }
                return {};
            }
            case AstNodeKind::BinaryExpression: {
                const auto& binary =
                    static_cast<const core::BinaryExpression&>(expression);
                const Value left = Get(state, binary.Left()),
                            right = Get(state, binary.Right());
                CheckUse(state, left, expression);
                CheckUse(state, right, expression);
                const TokenType op = binary.OperatorType();
                if (op == TokenType::And || op == TokenType::Or)
                    return Get(state, expression);
                if (op == TokenType::Equal || op == TokenType::NotEqual ||
                    op == TokenType::StrictEqual ||
                    op == TokenType::StrictNotEqual) {
                    // Distinct external pointers can alias. Only new identities
                    // are known non-null; parameter nullability is a contract.
                    const auto comparable = [&](const Value& value) {
                        if (value.kind == ValueKind::Pointer) {
                            const auto resource =
                                state.resources.find(value.identity);
                            return resource != state.resources.end() &&
                                   resource->second.owned &&
                                   !resource->second.nullable;
                        }
                        return value.kind == ValueKind::Null ||
                               value.kind == ValueKind::Error ||
                               value.kind == ValueKind::True ||
                               value.kind == ValueKind::False ||
                               value.kind == ValueKind::Integer;
                    };
                    if (comparable(left) && comparable(right)) {
                        bool equal = left == right;
                        if (op == TokenType::NotEqual ||
                            op == TokenType::StrictNotEqual)
                            equal = !equal;
                        return {equal ? ValueKind::True : ValueKind::False};
                    }
                }
                return {};
            }
            case AstNodeKind::TupleExpression: {
                Value value{ValueKind::Aggregate};
                for (const auto& element :
                     static_cast<const core::TupleExpression&>(expression)
                         .Elements())
                    value.elements.push_back(Get(state, *element));
                return value;
            }
            case AstNodeKind::ArrayExpression: {
                Value value{ValueKind::Aggregate};
                for (const auto& element :
                     static_cast<const core::ArrayExpression&>(expression)
                         .Elements())
                    value.elements.push_back(Get(state, *element));
                return value;
            }
            case AstNodeKind::MatchExpression: {
                const auto& match =
                    static_cast<const core::MatchExpression&>(expression);
                for (const auto& arm : match.Arms()) {
                    const auto found = state.values.find(&arm->Value());
                    if (found != state.values.end()) return found->second;
                }
                return {};
            }
            case AstNodeKind::MemberAccessExpression: {
                const auto& member =
                    static_cast<const core::MemberAccessExpression&>(
                        expression);
                if (!_model.FieldFor(member)) {
                    // Only the built-in allocation error is used to refine an
                    // allocation result; unrelated error families stay unknown.
                    if (_model.TypeOf(expression).Name() == "AllocationError")
                        return {ValueKind::Error};
                    return {};
                }
                CheckUse(state, Get(state, member.Receiver()), expression,
                         member.Access() == TokenType::Arrow);
                const auto* field = _model.FieldFor(member);
                if (field->syntax->Type().Contract() ==
                    OwnershipContract::Own) {
                    const Symbol* receiver = DirectTarget(member.Receiver());
                    if (!receiver || receiver->Name() != "self") {
                        Report(DiagnosticCode::UnverifiedOwnership, expression,
                               "access owning fields through the structure's "
                               "self methods",
                               field->syntax);
                        return {};
                    }
                    if (receiver->Kind() == SymbolKind::LocalVariable &&
                        !state.initializedOwners.contains(
                            {receiver, field->syntax}))
                        return {ValueKind::Null};
                    if (const auto stored =
                            state.fieldValues.find(field->syntax);
                        stored != state.fieldValues.end()) {
                        Value value = stored->second;
                        if (state.nullPointers.contains(value.identity))
                            return {ValueKind::Null};
                        CheckUse(state, value, expression);
                        return value;
                    }
                    if (state.nullPointers.contains(field->syntax))
                        return {ValueKind::Null};
                    if (!state.resources.contains(field->syntax))
                        state.resources[field->syntax] = {true, false, true,
                                                          true};
                    Value result{ValueKind::Pointer, field->syntax};
                    CheckUse(state, result, expression);
                    return result;
                }
                return {};
            }
            case AstNodeKind::SliceExpression: {
                const auto& slice =
                    static_cast<const core::SliceExpression&>(expression);
                const Value receiver = Get(state, slice.Receiver());
                if (IsHeapHandle(_model.TypeOf(slice.Receiver()))) {
                    CheckUse(state, receiver, expression, true);
                    const auto resource =
                        state.resources.find(receiver.identity);
                    const core::Expression& base = Unwrap(slice.Receiver());
                    const auto* field =
                        base.Kind() == AstNodeKind::MemberAccessExpression
                            ? _model.FieldFor(
                                  static_cast<
                                      const core::MemberAccessExpression&>(
                                      base))
                            : nullptr;
                    const bool owningField =
                        field && field->syntax->Type().Contract() ==
                                     OwnershipContract::Own;
                    if (receiver.kind != ValueKind::Pointer ||
                        resource == state.resources.end() ||
                        !resource->second.live ||
                        (!resource->second.owned &&
                         !(resource->second.lent && owningField)))
                        Report(DiagnosticCode::UnverifiedOwnership, expression,
                               "raw slicing requires a live owning allocation "
                               "or owning field",
                               receiver.identity);
                } else if (Owns(state, receiver))
                    Report(DiagnosticCode::UnverifiedOwnership, expression,
                           "cannot track allocation handles through a slice");
                return {};
            }
            case AstNodeKind::IndexExpression: {
                const auto& index =
                    static_cast<const core::IndexExpression&>(expression);
                Value receiver = Get(state, index.Receiver());
                const Value subscript = Get(state, index.Index());
                if (receiver.kind == ValueKind::Address &&
                    !IsHeapHandle(_model.TypeOf(index.Receiver())))
                    receiver = Binding(state, receiver.address);
                const bool heap = IsHeapHandle(_model.TypeOf(index.Receiver()));
                CheckUse(state, receiver, expression, heap);
                if (heap && (receiver.kind == ValueKind::Address ||
                             receiver.kind == ValueKind::Interior))
                    Report(DiagnosticCode::UnverifiedOwnership, expression,
                           "raw indexing requires a runtime allocation base",
                           receiver.identity);
                if (receiver.kind == ValueKind::Aggregate) {
                    if (subscript.kind == ValueKind::Integer &&
                        subscript.integer < receiver.elements.size())
                        return receiver.elements[subscript.integer];
                    if (Tracks(receiver))
                        Report(DiagnosticCode::UnverifiedOwnership, expression,
                               "cannot verify ownership through a dynamic "
                               "aggregate index");
                }
                return {};
            }
            default:
                return {};
        }
    }

    void Drop(State& state, const core::Expression& expression) {
        state.values.erase(&expression);
        switch (expression.Kind()) {
            case AstNodeKind::ParenthesizedExpression:
                Drop(state, static_cast<const core::ParenthesizedExpression&>(
                                expression)
                                .Inner());
                break;
            case AstNodeKind::CastExpression:
                Drop(state, static_cast<const core::CastExpression&>(expression)
                                .Value());
                break;
            case AstNodeKind::UnaryExpression:
                Drop(state,
                     static_cast<const core::UnaryExpression&>(expression)
                         .Operand());
                break;
            case AstNodeKind::BinaryExpression: {
                const auto& binary =
                    static_cast<const core::BinaryExpression&>(expression);
                Drop(state, binary.Left());
                Drop(state, binary.Right());
                break;
            }
            case AstNodeKind::AssignmentExpression: {
                const auto& assignment =
                    static_cast<const core::AssignmentExpression&>(expression);
                Drop(state, assignment.Target());
                Drop(state, assignment.Value());
                break;
            }
            case AstNodeKind::CallExpression: {
                const auto& call =
                    static_cast<const core::CallExpression&>(expression);
                Drop(state, call.Callee());
                for (const auto& argument : call.Arguments())
                    Drop(state, *argument);
                break;
            }
            case AstNodeKind::MemberCallExpression: {
                const auto& call =
                    static_cast<const core::MemberCallExpression&>(expression);
                Drop(state, call.Receiver());
                for (const auto& argument : call.Arguments())
                    Drop(state, *argument);
                break;
            }
            case AstNodeKind::MemberAccessExpression:
                Drop(state, static_cast<const core::MemberAccessExpression&>(
                                expression)
                                .Receiver());
                break;
            case AstNodeKind::IndexExpression: {
                const auto& index =
                    static_cast<const core::IndexExpression&>(expression);
                Drop(state, index.Receiver());
                Drop(state, index.Index());
                break;
            }
            case AstNodeKind::TupleExpression:
                for (const auto& element :
                     static_cast<const core::TupleExpression&>(expression)
                         .Elements())
                    Drop(state, *element);
                break;
            case AstNodeKind::ArrayExpression:
                for (const auto& element :
                     static_cast<const core::ArrayExpression&>(expression)
                         .Elements())
                    Drop(state, *element);
                break;
            case AstNodeKind::MatchExpression: {
                const auto& match =
                    static_cast<const core::MatchExpression&>(expression);
                Drop(state, match.Value());
                for (const auto& arm : match.Arms()) Drop(state, arm->Value());
                break;
            }
            default:
                break;
        }
    }

    std::vector<State> Leave(
        State state, const FlowNode& from,
        const std::vector<const core::BlockStatement*>& scopes) {
        const auto leaving = [&](const core::BlockStatement* scope) {
            return std::find(from.scopes.begin(), from.scopes.end(), scope) !=
                       from.scopes.end() &&
                   std::find(scopes.begin(), scopes.end(), scope) ==
                       scopes.end();
        };
        std::vector<State> states{std::move(state)};
        // A registration is removed before invocation. Nested calls may append
        // their own defers, but Run executes those before returning here.
        while (!states.empty()) {
            const auto found = std::find_if(
                states.front().deferred.rbegin(),
                states.front().deferred.rend(),
                [&](const Deferred& item) { return leaving(item.scope); });
            if (found == states.front().deferred.rend()) break;
            const std::size_t index =
                states.front().deferred.size() - 1 -
                static_cast<std::size_t>(found -
                                         states.front().deferred.rbegin());
            std::vector<State> next;
            for (State& current : states) {
                const Deferred deferred = current.deferred[index];
                current.deferred.erase(current.deferred.begin() +
                                       static_cast<std::ptrdiff_t>(index));
                for (Outcome& outcome :
                     Invoke(*deferred.call, deferred.function,
                            deferred.arguments, std::move(current)))
                    next.push_back(std::move(outcome.state));
            }
            states = std::move(next);
        }
        return states;
    }

    std::vector<Outcome> Run(const core::FunctionDeclaration& function,
                             State initial,
                             const std::vector<Value>& arguments) {
        _active.push_back(&function);
        const ControlFlowGraph& graph = _graphs.at(&function);
        const auto savedValues = initial.values;
        for (std::size_t index = 0; index < function.Parameters().size();
             ++index)
            Bind(initial, _model.DeclaredSymbol(*function.Parameters()[index]),
                 index < arguments.size() ? arguments[index] : Value{});
        std::vector<std::vector<State>> visited(graph.nodes.size());
        std::deque<std::pair<NodeId, State>> pending;
        pending.emplace_back(graph.entry, std::move(initial));
        std::vector<Outcome> results;
        while (!pending.empty()) {
            auto [id, state] = std::move(pending.front());
            pending.pop_front();
            auto& seen = visited[id];
            if (std::find(seen.begin(), seen.end(), state) != seen.end())
                continue;
            if (++_steps > 100000 || seen.size() >= 128) {
                Report(DiagnosticCode::UnverifiedOwnership, function,
                       "ownership analysis exceeded its path budget; simplify "
                       "control flow or split this function");
                break;
            }
            seen.push_back(state);
            const FlowNode& node = graph.nodes[id];
            std::vector<State> outputs;
            if (node.memoryExpression) {
                const core::Expression& expression = *node.memoryExpression;
                const FunctionSymbol* callee = Callee(expression);
                if (expression.Kind() == AstNodeKind::CallExpression &&
                    static_cast<const core::CallExpression&>(expression)
                        .TypeArgument()) {
                    // Allocation happens before its conditional initializer.
                    const Value result = Get(state, expression);
                    // Raw pointer initializers would hide another owned
                    // allocation inside heap storage, whose teardown is not
                    // recursive through raw pointers.
                    const auto& call =
                        static_cast<const core::CallExpression&>(expression);
                    if (!call.Arguments().empty() &&
                        Owns(state, Get(state, *call.Arguments()[0])))
                        Report(DiagnosticCode::UnverifiedOwnership, expression,
                               "cannot verify an owned pointer stored inside "
                               "another allocation");
                    ForgetChildren(state, expression);
                    Put(state, expression, result);
                    outputs.push_back(std::move(state));
                } else if (expression.Kind() == AstNodeKind::CallExpression ||
                           expression.Kind() ==
                               AstNodeKind::MemberCallExpression) {
                    if (expression.Kind() ==
                        AstNodeKind::MemberCallExpression) {
                        const auto& call =
                            static_cast<const core::MemberCallExpression&>(
                                expression);
                        if (!callee && call.Member() != "len" &&
                            Owns(state, Get(state, call.Receiver())))
                            Report(DiagnosticCode::UnverifiedOwnership,
                                   expression,
                                   "cannot track allocation handles through "
                                   "this container operation");
                        if (call.Access() == TokenType::Arrow)
                            CheckUse(state, Get(state, call.Receiver()),
                                     expression, true);
                    }
                    const std::vector<Value> arguments =
                        Arguments(state, expression);
                    for (Outcome& outcome : Invoke(
                             expression, callee, arguments, std::move(state))) {
                        ForgetChildren(outcome.state, expression);
                        Put(outcome.state, expression,
                            std::move(outcome.result));
                        outputs.push_back(std::move(outcome.state));
                    }
                } else {
                    Value result = Evaluate(state, expression);
                    ForgetChildren(state, expression);
                    Put(state, expression, std::move(result));
                    outputs.push_back(std::move(state));
                }
            } else {
                if (node.memoryUntrackedStore) {
                    if (Owns(state, Get(state, *node.memoryUntrackedStore)) ||
                        Lent(state, Get(state, *node.memoryUntrackedStore)))
                        Report(DiagnosticCode::UnverifiedOwnership,
                               *node.memoryUntrackedStore,
                               "cannot verify ownership stored in a structure "
                               "field initializer");
                    Drop(state, *node.memoryUntrackedStore);
                    CheckLost(state, *node.memoryUntrackedStore);
                }
                if (node.memoryDeclaration) {
                    const auto& declaration = *node.memoryDeclaration;
                    Value value;
                    if (declaration.Initializer())
                        value = Get(state, *declaration.Initializer());
                    CheckUse(state, value, declaration);
                    Bind(state, _model.DeclaredSymbol(declaration),
                         std::move(value));
                    if (declaration.Initializer())
                        Drop(state, *declaration.Initializer());
                    CheckLost(state, declaration);
                }
                if (node.memoryDestructure) {
                    const auto& destructure = *node.memoryDestructure;
                    const Value tuple = Get(state, destructure.Value());
                    CheckUse(state, tuple, destructure);
                    for (std::size_t index = 0;
                         index < destructure.Bindings().size(); ++index)
                        Bind(state,
                             _model.DestructureTarget(
                                 *destructure.Bindings()[index]),
                             index < tuple.elements.size()
                                 ? tuple.elements[index]
                                 : Value{});
                    Drop(state, destructure.Value());
                    CheckLost(state, destructure);
                }
                if (node.memoryDefer) {
                    const core::Expression& call = node.memoryDefer->Call();
                    std::vector<Value> arguments = Arguments(state, call);
                    for (const Value& value : arguments)
                        CheckUse(state, value, call);
                    // Even calls without tracked arguments may allocate; they
                    // need execution at cleanup rather than at registration.
                    if (Relevant(Callee(call), arguments)) {
                        if (state.deferred.size() >= 128) {
                            Report(DiagnosticCode::UnverifiedOwnership, call,
                                   "cannot verify an unbounded number of "
                                   "deferred pointer operations");
                            continue;
                        }
                        state.deferred.push_back({node.scopes.back(), &call,
                                                  Callee(call),
                                                  std::move(arguments)});
                    }
                    Drop(state, call);
                    CheckLost(state, *node.memoryDefer);
                }
                if (node.memoryDiscard) {
                    Drop(state, *node.memoryDiscard);
                    CheckLost(state, *node.memoryDiscard);
                }
                if (node.memoryBoolean)
                    Put(state, *node.memoryBoolean,
                        {node.booleanValue ? ValueKind::True
                                           : ValueKind::False});
                outputs.push_back(std::move(state));
            }
            for (State& output : outputs) {
                if (node.successors.empty()) {
                    Value result;
                    if (node.memoryReturn && node.memoryReturn->Value()) {
                        result = Get(output, *node.memoryReturn->Value());
                        Drop(output, *node.memoryReturn->Value());
                    }
                    const core::AstNode& at =
                        node.memoryReturn ? static_cast<const core::AstNode&>(
                                                *node.memoryReturn)
                                          : function.Body();
                    // The synthetic fallthrough exit has no scopes. Usually
                    // cleanup already ran on its incoming edge.
                    for (State& finished : Leave(std::move(output), node, {})) {
                        for (const auto& [symbol, scope] : graph.symbolScopes)
                            finished.bindings.erase(symbol);
                        for (const auto& parameter : function.Parameters())
                            finished.bindings.erase(
                                _model.DeclaredSymbol(*parameter));
                        std::erase_if(finished.values, [&](const auto& entry) {
                            return !savedValues.contains(entry.first);
                        });
                        CheckUse(finished, result, at);
                        std::set<Identity> returnedOwners;
                        CheckReturn(
                            finished, result, function.ReturnType(), at,
                            HasContracts(function) || HasOwningFields(function),
                            returnedOwners);
                        if (function.Name() == "tear" &&
                            HasOwningFields(function))
                            for (const auto& field :
                                 Function(function)->Owner()->fields) {
                                const auto resource = finished.resources.find(
                                    finished.fieldValues.contains(field.syntax)
                                        ? finished.fieldValues.at(field.syntax)
                                              .identity
                                        : field.syntax);
                                if (field.syntax->Type().Contract() ==
                                        OwnershipContract::Own &&
                                    resource != finished.resources.end() &&
                                    resource->second.live)
                                    Report(DiagnosticCode::MemoryLeak, at,
                                           "tear must discharge every live "
                                           "owning field",
                                           field.syntax);
                            }
                        CheckLost(finished, at, result);
                        results.push_back({std::move(finished), result});
                    }
                    continue;
                }
                const Value condition =
                    node.condition ? Get(output, *node.condition) : Value{};
                for (std::size_t edge = 0; edge < node.successors.size();
                     ++edge) {
                    if (node.condition &&
                        ((condition.kind == ValueKind::True && edge != 0) ||
                         (condition.kind == ValueKind::False && edge != 1)))
                        continue;
                    State outgoing = output;
                    if (node.condition) {
                        const auto& conditionExpression =
                            Unwrap(*node.condition);
                        if (conditionExpression.Kind() ==
                            AstNodeKind::BinaryExpression) {
                            const auto& binary =
                                static_cast<const core::BinaryExpression&>(
                                    conditionExpression);
                            const auto op = binary.OperatorType();
                            if (op == TokenType::Equal ||
                                op == TokenType::NotEqual ||
                                op == TokenType::StrictEqual ||
                                op == TokenType::StrictNotEqual) {
                                // Child temporaries have been consumed;
                                // re-evaluate pure operands only.
                                auto operand =
                                    [&](const core::Expression& expression) {
                                        const auto& e = Unwrap(expression);
                                        if (e.Kind() ==
                                            AstNodeKind::NameExpression)
                                            return Binding(outgoing,
                                                           DirectTarget(e));
                                        if (e.Kind() ==
                                            AstNodeKind::LiteralExpression)
                                            return Evaluate(outgoing, e);
                                        if (e.Kind() ==
                                            AstNodeKind::MemberAccessExpression)
                                            return Evaluate(outgoing, e);
                                        return Value{};
                                    };
                                Value left = operand(binary.Left()),
                                      right = operand(binary.Right());
                                Value pointer =
                                    left.kind == ValueKind::Null ? right : left;
                                if ((left.kind == ValueKind::Null ||
                                     right.kind == ValueKind::Null) &&
                                    pointer.kind == ValueKind::Pointer &&
                                    outgoing.resources.contains(
                                        pointer.identity)) {
                                    const bool equal =
                                        op == TokenType::Equal ||
                                        op == TokenType::StrictEqual;
                                    const bool isNull = (edge == 0) == equal;
                                    auto& resource =
                                        outgoing.resources[pointer.identity];
                                    if (isNull && !resource.nullable) continue;
                                    if (isNull) {
                                        resource.live = false;
                                        outgoing.nullPointers.insert(
                                            pointer.identity);
                                        auto clear = [&](auto&& self,
                                                         Value& value) -> void {
                                            if (value.identity ==
                                                    pointer.identity &&
                                                value.kind ==
                                                    ValueKind::Pointer)
                                                value = {ValueKind::Null};
                                            for (auto& child : value.elements)
                                                self(self, child);
                                        };
                                        for (auto& [symbol, value] :
                                             outgoing.bindings)
                                            clear(clear, value);
                                        for (auto& [expression, value] :
                                             outgoing.values)
                                            clear(clear, value);
                                    } else
                                        resource.nullable = false;
                                }
                            }
                        }
                        Drop(outgoing, *node.condition);
                    }
                    if (node.allocation) {
                        if (edge == 0) {
                            if (!Allocate(outgoing, *node.allocation)) continue;
                        } else {
                            Value failure{ValueKind::Aggregate};
                            failure.elements = {{ValueKind::Null},
                                                {ValueKind::Error}};
                            Put(outgoing, *node.allocation, std::move(failure));
                        }
                    }
                    const NodeId successor = node.successors[edge];
                    for (State& next : Leave(std::move(outgoing), node,
                                             graph.nodes[successor].scopes)) {
                        for (const auto& [symbol, scope] : graph.symbolScopes)
                            if (std::find(graph.nodes[successor].scopes.begin(),
                                          graph.nodes[successor].scopes.end(),
                                          scope) ==
                                graph.nodes[successor].scopes.end()) {
                                next.bindings.erase(symbol);
                                std::erase_if(next.initializedOwners,
                                              [&](const auto& slot) {
                                                  return slot.first == symbol;
                                              });
                            }
                        // Do not check here while a return value is in transit:
                        // its expression temporary represents the transferred
                        // owner.
                        const core::AstNode& loss =
                            node.memoryExpression
                                ? static_cast<const core::AstNode&>(
                                      *node.memoryExpression)
                            : node.condition
                                ? static_cast<const core::AstNode&>(
                                      *node.condition)
                                : function.Body();
                        CheckLost(next, loss);
                        pending.emplace_back(successor, std::move(next));
                    }
                }
            }
        }
        _active.pop_back();
        return results;
    }

    bool Owns(const State& state, const Value& value,
              std::size_t depth = 0) const {
        if (depth > 16) return true;
        const auto resource = state.resources.find(value.identity);
        if (resource != state.resources.end() && resource->second.owned)
            return true;
        if (value.address &&
            Owns(state, Binding(state, value.address), depth + 1))
            return true;
        return std::any_of(
            value.elements.begin(), value.elements.end(),
            [&](const Value& child) { return Owns(state, child, depth + 1); });
    }

    std::set<std::pair<const core::FunctionDeclaration*, std::vector<bool>>>
        _nullCalls;
    std::map<std::pair<const core::FunctionDeclaration*, std::vector<bool>>,
             std::vector<Value>>
        _nullShapes;
    std::set<const core::FunctionDeclaration*> _verified, _verifying;
    std::map<const core::FunctionDeclaration*, std::vector<Value>>
        _returnShapes;
    std::map<std::pair<const core::Expression*, const core::TypeNode*>,
             std::unique_ptr<core::TypeNode>>
        _returnOrigins;
    std::map<std::pair<const core::Expression*, std::size_t>,
             std::unique_ptr<core::TypeNode>>
        _indirectReturnOrigins;
    const SemanticModel& _model;
    diagnostics::DiagnosticBag& _diagnostics;
    std::map<const FunctionSymbol*, const core::FunctionDeclaration*>
        _functions;
    std::map<const core::FunctionDeclaration*, ControlFlowGraph> _graphs;
    std::vector<const core::FunctionDeclaration*> _ordered;
    std::set<const core::FunctionDeclaration*> _resourceFunctions;
    std::vector<const core::FunctionDeclaration*> _active;
    std::set<std::pair<DiagnosticCode, source::SourceLocation>> _reported;
    std::size_t _steps = 0;
};
}  // namespace

void AnalyzeAllocations(const core::CompilationUnit& unit,
                        const SemanticModel& model,
                        diagnostics::DiagnosticBag& diagnostics) {
    AllocationChecker(unit, model, diagnostics).Analyze();
}
}  // namespace flecha::semantic
