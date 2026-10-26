#include "semantic/borrow_checker.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace flecha::semantic {
namespace {
using Symbols = std::set<const Symbol*>;
using Code = diagnostics::DiagnosticCode;
using Kind = core::AstNodeKind;

bool ContainsBorrowed(SemanticType type) {
    if (type.HasBorrowedLifetime()) return true;
    if (type.Kind() == SemanticTypeKind::Tuple)
        return std::any_of(type.Elements().begin(), type.Elements().end(),
                           ContainsBorrowed);
    if (type.Kind() == SemanticTypeKind::Array)
        return ContainsBorrowed(*type.Target());
    if (type.Kind() == SemanticTypeKind::Structure &&
        type.Structure()->validLayout)
        return std::any_of(type.Structure()->fields.begin(),
                           type.Structure()->fields.end(),
                           [](const StructureField& field) {
                               return ContainsBorrowed(field.type);
                           });
    return false;
}

bool CanCarryBorrow(SemanticType type) {
    if (type.HasBorrowedLifetime() || type.Kind() == SemanticTypeKind::Function)
        return true;
    if (type.Kind() == SemanticTypeKind::Tuple)
        return std::any_of(type.Elements().begin(), type.Elements().end(),
                           CanCarryBorrow);
    if (type.Kind() == SemanticTypeKind::Array ||
        type.Kind() == SemanticTypeKind::OwnedBuffer)
        return CanCarryBorrow(*type.Target());
    if (type.Kind() == SemanticTypeKind::Structure &&
        type.Structure()->validLayout)
        return std::any_of(type.Structure()->fields.begin(),
                           type.Structure()->fields.end(),
                           [](const StructureField& field) {
                               return CanCarryBorrow(field.type);
                           });
    return false;
}

bool TracksBorrow(const Symbol* symbol) {
    return symbol != nullptr &&
           (symbol->Kind() == SymbolKind::Parameter ||
            symbol->Kind() == SymbolKind::LocalVariable) &&
           CanCarryBorrow(symbol->Type());
}

bool IsExclusiveBorrow(SemanticType type) {
    if (type.IsBorrowed()) return !type.Target()->IsConst();
    if (type.Kind() == SemanticTypeKind::Array)
        return IsExclusiveBorrow(*type.Target());
    if (type.Kind() == SemanticTypeKind::Tuple)
        return std::any_of(type.Elements().begin(), type.Elements().end(),
                           IsExclusiveBorrow);
    if (type.Kind() == SemanticTypeKind::Structure &&
        type.Structure()->validLayout)
        return std::any_of(type.Structure()->fields.begin(),
                           type.Structure()->fields.end(),
                           [](const StructureField& field) {
                               return IsExclusiveBorrow(field.type);
                           });
    return false;
}

const core::Expression& Unwrap(const core::Expression& expression) {
    if (expression.Kind() == Kind::ParenthesizedExpression)
        return Unwrap(
            static_cast<const core::ParenthesizedExpression&>(expression)
                .Inner());
    return expression;
}

// A reborrow retains its owner and all parent handles. The sets are finite even
// on back edges; joins retain every possible origin rather than choosing one.
struct Projection final {
    const StructureField* field = nullptr;
    const FunctionSymbol* resultFunction = nullptr;
    std::size_t resultIndex = 0;
    auto operator<=>(const Projection&) const = default;
};

struct Origin final {
    const Symbol* owner;
    Symbols parents;
    std::vector<Projection> fields;
    // Array/slice elements continue to borrow their whole containing array.
    // Do not refine a path further after crossing an element boundary.
    bool wholeSequence = false;
    // Closures can carry a mixture of shared and exclusive captures, so the
    // access mode belongs to each origin rather than only to the holder type.
    bool exclusive = false;
    auto operator<=>(const Origin&) const = default;
};
bool Overlaps(const Origin& left, const Origin& right) {
    if (left.owner != right.owner) return false;
    // Equal paths overlap; a prefix denotes an enclosing structure.
    // Differing concrete fields prove disjointness. Result slots prove
    // disjointness only within the same validated multi-result function;
    // abstract result slots conservatively overlap concrete fields.
    const auto length = std::min(left.fields.size(), right.fields.size());
    for (std::size_t index = 0; index < length; ++index) {
        const Projection& a = left.fields[index];
        const Projection& b = right.fields[index];
        if (a == b) continue;
        if (a.field != nullptr && b.field != nullptr) return false;
        if (a.resultFunction != nullptr &&
            a.resultFunction == b.resultFunction && b.resultFunction != nullptr)
            return false;
        return true;
    }
    return true;
}

using Origins = std::set<Origin>;
struct Handle final {
    Origins origins;
    bool moved = false;
    bool operator==(const Handle&) const = default;
};
using State = std::map<const Symbol*, Handle>;
struct Value final {
    Origins origins;
    const Symbol* handle = nullptr;
    std::size_t temporary = 0;
    std::vector<Value> elements;
};
struct Loan final {
    Value value;
    bool exclusive;
    std::size_t id;
};

void AddBorrowSource(Value& destination, const Value& source) {
    if (destination.origins.empty() && destination.handle == nullptr &&
        destination.temporary == 0 && destination.elements.empty()) {
        destination = source;
        return;
    }

    destination.origins.insert(source.origins.begin(), source.origins.end());
    destination.handle = nullptr;
    destination.temporary = 0;
    destination.elements.clear();
}

void Merge(State& destination, const State& source) {
    for (const auto& [symbol, handle] : source) {
        Handle& target = destination[symbol];
        target.origins.insert(handle.origins.begin(), handle.origins.end());
        target.moved = target.moved || handle.moved;
    }
}

class BorrowChecker final {
   public:
    BorrowChecker(const SemanticModel& model,
                  diagnostics::DiagnosticBag& diagnostics,
                  const ControlFlowGraph& graph)
        : _model(model),
          _diagnostics(diagnostics),
          _graph(graph),
          _live(graph.nodes.size()),
          _uses(graph.nodes.size()),
          _defs(graph.nodes.size()) {}

    void Analyze(const core::FunctionDeclaration& function) {
        State seed;
        for (const std::unique_ptr<core::Parameter>& parameter :
             function.Parameters()) {
            const Symbol* symbol = _model.DeclaredSymbol(*parameter);
            if (TracksBorrow(symbol))
                seed[symbol].origins.insert(
                    {symbol,
                     {},
                     {},
                     false,
                     symbol->Type().Kind() == SemanticTypeKind::Function ||
                         IsExclusiveBorrow(symbol->Type())});
        }
        const Symbol* declared = _model.DeclaredSymbol(function);
        if (declared != nullptr && declared->Kind() == SymbolKind::Function) {
            _function = static_cast<const FunctionSymbol*>(declared);
            for (const std::size_t source : _function->BorrowSourceIndices()) {
                if (source >= function.Parameters().size()) continue;
                if (const Symbol* parameter =
                        _model.DeclaredSymbol(*function.Parameters()[source]))
                    _borrowSources.insert(parameter);
            }
        }
        ComputeLiveness();
        std::vector<State> incoming(_graph.nodes.size()),
            outgoing(_graph.nodes.size());
        std::deque<NodeId> pending;
        std::vector<bool> queued(_graph.nodes.size(), false);
        for (NodeId id = 0; id < _graph.nodes.size(); ++id) {
            if (_graph.nodes[id].reachable) {
                pending.push_back(id);
                queued[id] = true;
            }
        }
        while (!pending.empty()) {
            const NodeId id = pending.front();
            pending.pop_front();
            queued[id] = false;
            State next = id == _graph.entry ? seed : State{};
            for (NodeId predecessor : _graph.nodes[id].predecessors)
                if (_graph.nodes[predecessor].reachable)
                    Merge(next, outgoing[predecessor]);
            PruneScope(next, _graph.nodes[id]);
            incoming[id] = next;
            Transfer(id, next);
            if (next == outgoing[id]) continue;
            outgoing[id] = std::move(next);
            for (NodeId successor : _graph.nodes[id].successors) {
                if (!queued[successor]) {
                    pending.push_back(successor);
                    queued[successor] = true;
                }
            }
        }
        // Report only from the fixed point, never transient worklist states.
        _report = true;
        for (NodeId id = 0; id < _graph.nodes.size(); ++id)
            if (_graph.nodes[id].reachable) Transfer(id, incoming[id]);
        for (const auto& [key, message] : _reported) {
            const auto& [range, code] = key;
            _diagnostics.Report(code, range, message);
        }
    }

   private:
    const Symbol* Named(const core::Expression& expression) const {
        const core::Expression& inner = Unwrap(expression);
        return inner.Kind() == Kind::NameExpression
                   ? _model.SymbolFor(
                         static_cast<const core::NameExpression&>(inner))
                   : nullptr;
    }

    const Symbol* Root(const core::Expression& expression) const {
        const core::Expression& inner = Unwrap(expression);
        if (const Symbol* symbol = Named(inner)) return symbol;
        if (inner.Kind() == Kind::MemberAccessExpression)
            return Root(static_cast<const core::MemberAccessExpression&>(inner)
                            .Receiver());
        if (inner.Kind() == Kind::IndexExpression)
            return Root(
                static_cast<const core::IndexExpression&>(inner).Receiver());
        return nullptr;
    }

    void Report(Code code, source::SourceRange range, std::string message) {
        if (_report)
            _reported.try_emplace(std::make_pair(range, code),
                                  std::move(message));
    }

    void CollectUses(const core::Expression& expression, Symbols& uses) const {
        switch (expression.Kind()) {
            case Kind::SliceExpression: {
                const auto& slice =
                    static_cast<const core::SliceExpression&>(expression);
                CollectUses(slice.Receiver(), uses);
                if (slice.Start() != nullptr) CollectUses(*slice.Start(), uses);
                if (slice.End() != nullptr) CollectUses(*slice.End(), uses);
                break;
            }
            case Kind::TupleExpression:
                for (const auto& element :
                     static_cast<const core::TupleExpression&>(expression)
                         .Elements())
                    CollectUses(*element, uses);
                break;
            case Kind::ArrayExpression:
                for (const auto& element :
                     static_cast<const core::ArrayExpression&>(expression)
                         .Elements())
                    CollectUses(*element, uses);
                break;
            case Kind::MatchExpression: {
                const auto& match =
                    static_cast<const core::MatchExpression&>(expression);
                CollectUses(match.Value(), uses);
                for (const auto& arm : match.Arms())
                    CollectUses(arm->Value(), uses);
                break;
            }
            case Kind::NameExpression: {
                const Symbol* symbol = Named(expression);
                if (TracksBorrow(symbol)) uses.insert(symbol);
                break;
            }
            case Kind::AnonymousFunctionExpression:
                for (const auto& capture :
                     static_cast<const core::AnonymousFunctionExpression&>(
                         expression)
                         .Captures())
                    if (capture->Mode() !=
                        core::AnonymousFunctionExpression::CaptureMode::Copy) {
                        const Symbol* source = _model.CaptureSource(*capture);
                        if (TracksBorrow(source)) uses.insert(source);
                    }
                break;
            case Kind::ParenthesizedExpression:
                CollectUses(Unwrap(expression), uses);
                break;
            case Kind::UnaryExpression:
                CollectUses(
                    static_cast<const core::UnaryExpression&>(expression)
                        .Operand(),
                    uses);
                break;
            case Kind::CastExpression:
                CollectUses(static_cast<const core::CastExpression&>(expression)
                                .Value(),
                            uses);
                break;
            case Kind::BinaryExpression: {
                const auto& binary =
                    static_cast<const core::BinaryExpression&>(expression);
                CollectUses(binary.Left(), uses);
                CollectUses(binary.Right(), uses);
                break;
            }
            case Kind::AssignmentExpression: {
                const auto& assignment =
                    static_cast<const core::AssignmentExpression&>(expression);
                if (assignment.OperatorType() != TokenType::Assign ||
                    Named(assignment.Target()) == nullptr)
                    CollectUses(assignment.Target(), uses);
                CollectUses(assignment.Value(), uses);
                break;
            }
            case Kind::CallExpression: {
                const auto& call =
                    static_cast<const core::CallExpression&>(expression);
                CollectUses(call.Callee(), uses);
                for (const auto& argument : call.Arguments())
                    CollectUses(*argument, uses);
                break;
            }
            case Kind::MemberAccessExpression: {
                const auto& field =
                    static_cast<const core::MemberAccessExpression&>(
                        expression);
                if (_model.FieldFor(field)) CollectUses(field.Receiver(), uses);
                break;
            }
            case Kind::MemberCallExpression: {
                const auto& call =
                    static_cast<const core::MemberCallExpression&>(expression);
                CollectUses(call.Receiver(), uses);
                for (const auto& argument : call.Arguments())
                    CollectUses(*argument, uses);
                break;
            }
            case Kind::IndexExpression: {
                const auto& index =
                    static_cast<const core::IndexExpression&>(expression);
                CollectUses(index.Receiver(), uses);
                CollectUses(index.Index(), uses);
                break;
            }
            default:
                break;
        }
    }

    void ComputeLiveness() {
        for (NodeId id = 0; id < _graph.nodes.size(); ++id) {
            const FlowNode& node = _graph.nodes[id];
            if (node.captureRead != nullptr) {
                const Symbol* source = _model.CaptureSource(*node.captureRead);
                if (TracksBorrow(source)) _uses[id].insert(source);
            }
            if (node.declaration != nullptr) {
                const Symbol* symbol = _model.DeclaredSymbol(*node.declaration);
                if (TracksBorrow(symbol)) _defs[id].insert(symbol);
                if (node.declaration->Initializer() != nullptr)
                    CollectUses(*node.declaration->Initializer(), _uses[id]);
            }
            if (node.destructure != nullptr) {
                CollectUses(node.destructure->Value(), _uses[id]);
                for (const auto& binding : node.destructure->Bindings()) {
                    const Symbol* symbol = _model.DestructureTarget(*binding);
                    if (TracksBorrow(symbol)) _defs[id].insert(symbol);
                }
            }
            if (node.deferRegistration != nullptr)
                CollectUses(node.deferRegistration->Call(), _uses[id]);
            if (node.evaluation != nullptr) {
                CollectUses(*node.evaluation, _uses[id]);
                const core::Expression& expression = Unwrap(*node.evaluation);
                if (expression.Kind() == Kind::AssignmentExpression) {
                    const Symbol* symbol =
                        Named(static_cast<const core::AssignmentExpression&>(
                                  expression)
                                  .Target());
                    if (TracksBorrow(symbol)) _defs[id].insert(symbol);
                }
            }
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (NodeId id = 0; id < _graph.nodes.size(); ++id) {
                if (!_graph.nodes[id].reachable) continue;
                Symbols next = _uses[id];
                for (NodeId successor : _graph.nodes[id].successors)
                    for (const Symbol* symbol : _live[successor])
                        if (!_defs[id].contains(symbol)) next.insert(symbol);
                if (next != _live[id]) {
                    _live[id] = std::move(next);
                    changed = true;
                }
            }
        }
    }

    void PruneScope(State& state, const FlowNode& node) const {
        std::erase_if(state, [&](const auto& entry) {
            const auto found = _graph.symbolScopes.find(entry.first);
            return found != _graph.symbolScopes.end() &&
                   std::find(node.scopes.begin(), node.scopes.end(),
                             found->second) == node.scopes.end();
        });
    }

    void Transfer(NodeId id, State& state) {
        _state = &state;
        _active = _live[id];
        // Registered captures remain live until the lexical scope is exited,
        // even if the original handle has no later source-level uses.
        for (const auto& [symbol, handle] : state)
            if (_graph.deferredCaptures.contains(symbol))
                _active.insert(symbol);
        _temporaries.clear();
        _pinned.clear();
        _nextTemporary = 1;
        const FlowNode& node = _graph.nodes[id];
        if (node.declaration != nullptr) {
            const Symbol* symbol = _model.DeclaredSymbol(*node.declaration);
            // A backward jump can execute a declaration again while another
            // handle still borrows its old storage.
            CheckHandleChange(symbol, node.declaration->NameRange());
            if (!TracksBorrow(symbol))
                CheckAccess({{{symbol, {}}}, nullptr, 0}, true,
                            node.declaration->NameRange());
            Value value;
            if (node.declaration->Initializer() != nullptr)
                value = Evaluate(*node.declaration->Initializer());
            if (TracksBorrow(symbol)) {
                if (node.declaration->IsRangeSource())
                    value = Borrow(std::move(value), false,
                                   node.declaration->Range());
                Store(symbol, value,
                      node.declaration->Initializer() != nullptr
                          ? node.declaration->Initializer()->Range()
                          : node.declaration->NameRange());
            }
        }
        if (node.destructure != nullptr) {
            Value value = Evaluate(node.destructure->Value());
            for (std::size_t index = 0;
                 index < node.destructure->Bindings().size(); ++index) {
                const auto& binding = node.destructure->Bindings()[index];
                const Symbol* target = _model.DestructureTarget(*binding);
                if (TracksBorrow(target) && index < value.elements.size())
                    Store(target, std::move(value.elements[index]),
                          binding->Range());
                else if (target != nullptr)
                    CheckAccess({{{target, {}}}, nullptr, 0}, true,
                                binding->Range());
            }
        }
        if (node.deferRegistration != nullptr) {
            const core::Expression& deferred =
                Unwrap(node.deferRegistration->Call());
            std::vector<const core::Expression*> operands;
            if (deferred.Kind() == Kind::CallExpression) {
                const auto& call =
                    static_cast<const core::CallExpression&>(deferred);
                bool direct = false;
                if (call.Callee().Kind() == Kind::NameExpression) {
                    const Symbol* symbol = _model.SymbolFor(
                        static_cast<const core::NameExpression&>(
                            call.Callee()));
                    direct = symbol != nullptr &&
                             symbol->Kind() == SymbolKind::Function;
                }
                if (!direct) operands.push_back(&call.Callee());
                for (const auto& argument : call.Arguments())
                    operands.push_back(argument.get());
            } else if (deferred.Kind() == Kind::MemberCallExpression) {
                const auto& call =
                    static_cast<const core::MemberCallExpression&>(deferred);
                const FunctionSymbol* method = _model.MethodFor(call);
                if (method != nullptr && method->HasReceiver())
                    operands.push_back(&call.Receiver());
                for (const auto& argument : call.Arguments())
                    operands.push_back(argument.get());
            }
            const auto& captures =
                _model.DeferredCaptures(*node.deferRegistration);
            for (std::size_t index = 0;
                 index < operands.size() && index < captures.size(); ++index) {
                const Symbol* capture = captures[index];
                const SemanticType operandType =
                    _model.TypeOf(*operands[index]);
                Value value =
                    capture != nullptr && capture->Type().IsBorrowed() &&
                            (operandType.Kind() ==
                                 SemanticTypeKind::Structure ||
                             operandType.Kind() == SemanticTypeKind::Enum)
                        ? Place(*operands[index])
                        : Evaluate(*operands[index]);
                if (capture == nullptr) continue;
                if (capture->Type().IsBorrowed())
                    value = Borrow(std::move(value),
                                   IsExclusiveBorrow(capture->Type()),
                                   operands[index]->Range());
                const Origins previous = state[capture].origins;
                Store(capture, std::move(value), operands[index]->Range());
                state[capture].origins.insert(previous.begin(), previous.end());
            }
        }
        if (node.evaluation != nullptr) {
            Value value = Evaluate(*node.evaluation);
            if (node.borrowReturn != nullptr)
                ValidateReturn(value, node.evaluation->Range());
        }
    }

    void ValidateReturn(const Value& value, source::SourceRange range) {
        if (_function == nullptr) return;
        if (!ContainsBorrowed(_function->ReturnType()) && value.origins.empty())
            return;
        ValidateReturn(value, _function->ReturnType(), range);
    }

    void ValidateReturn(const Value& value, SemanticType type,
                        source::SourceRange range) {
        if (type.Kind() == SemanticTypeKind::Tuple) {
            if (value.elements.size() != type.Elements().size()) {
                Report(Code::InvalidBorrowContract, range,
                       "borrowed tuple result cannot be traced to its "
                       "declared source");
                return;
            }
            for (std::size_t left = 0; left < type.Elements().size(); ++left) {
                if (!type.Elements()[left].IsBorrowed()) continue;
                for (std::size_t right = left + 1;
                     right < type.Elements().size(); ++right) {
                    if (!type.Elements()[right].IsBorrowed()) continue;
                    const bool exclusive =
                        !type.Elements()[left].Target()->IsConst() ||
                        !type.Elements()[right].Target()->IsConst();
                    if (!exclusive) continue;
                    for (const Origin& a : value.elements[left].origins)
                        for (const Origin& b : value.elements[right].origins)
                            if (Overlaps(a, b))
                                Report(
                                    Code::InvalidBorrowContract, range,
                                    "mutable borrowed results must originate "
                                    "from disjoint places");
                }
            }
            for (std::size_t index = 0; index < type.Elements().size(); ++index)
                if (ContainsBorrowed(type.Elements()[index]))
                    ValidateReturn(value.elements[index],
                                   type.Elements()[index], range);
            return;
        }
        if (!type.HasBorrowedLifetime() && value.origins.empty()) return;
        if (_borrowSources.empty()) {
            if (!value.origins.empty())
                Report(Code::InvalidBorrowContract, range,
                       "borrowed result requires a '<- source>' borrow "
                       "contract");
            return;
        }
        if (value.origins.empty()) {
            if (_borrowSources.size() == 1)
                Report(Code::InvalidBorrowContract, range,
                       "borrowed result does not originate from declared "
                       "source '" +
                           (*_borrowSources.begin())->Name() + "'");
            else
                Report(Code::InvalidBorrowContract, range,
                       "borrowed result does not originate from any declared "
                       "source");
            return;
        }
        for (const Origin& origin : value.origins)
            if (!_borrowSources.contains(origin.owner)) {
                if (_borrowSources.size() == 1)
                    Report(Code::InvalidBorrowContract, range,
                           "borrowed result originates from '" +
                               origin.owner->Name() +
                               "', not declared source '" +
                               (*_borrowSources.begin())->Name() + "'");
                else
                    Report(Code::InvalidBorrowContract, range,
                           "borrowed result originates from '" +
                               origin.owner->Name() +
                               "', not any declared source");
                return;
            }
    }

    Value BorrowResult(SemanticType type, const Value& source,
                       source::SourceRange range,
                       const FunctionSymbol* function, std::size_t& resultIndex,
                       bool multiple) {
        if (type.HasBorrowedLifetime()) {
            Value projected = source;
            if (multiple) {
                Origins origins;
                for (Origin origin : projected.origins) {
                    origin.fields.push_back({nullptr, function, resultIndex});
                    origins.insert(std::move(origin));
                }
                projected.origins = std::move(origins);
            }
            ++resultIndex;
            return Borrow(std::move(projected), IsExclusiveBorrow(type), range);
        }
        Value result;
        if (type.Kind() == SemanticTypeKind::Tuple) {
            for (const SemanticType& element : type.Elements()) {
                Value value = BorrowResult(element, source, range, function,
                                           resultIndex, true);
                result.origins.insert(value.origins.begin(),
                                      value.origins.end());
                result.elements.push_back(std::move(value));
            }
        } else if (CanCarryBorrow(type))
            return Borrow(source, IsExclusiveBorrow(type), range);
        return result;
    }

    void CheckHandleChange(const Symbol* symbol, source::SourceRange range) {
        if (_pinned.contains(symbol)) {
            Report(Code::BorrowConflict, range,
                   "cannot move or rebind a reference used by the pending "
                   "assignment");
        }
        for (const auto& [handle, value] : *_state) {
            if (!_active.contains(handle)) continue;
            for (const Origin& origin : value.origins)
                if (origin.parents.contains(symbol))
                    Report(Code::BorrowConflict, range,
                           "cannot move or rebind '" + symbol->Name() +
                               "' while a child reborrow is live");
        }
        for (const Loan& loan : _temporaries)
            for (const Origin& origin : loan.value.origins)
                if (origin.parents.contains(symbol))
                    Report(Code::BorrowConflict, range,
                           "reference is borrowed for the current expression");
    }

    void CheckAccess(const Value& access, bool write,
                     source::SourceRange range) {
        for (const Origin& origin : access.origins) {
            for (const auto& [symbol, value] : *_state) {
                if (!_active.contains(symbol) || symbol == access.handle ||
                    symbol == origin.owner || origin.parents.contains(symbol))
                    continue;
                for (const Origin& loan : value.origins) {
                    if (Overlaps(loan, origin) && (write || loan.exclusive)) {
                        Report(Code::BorrowConflict, range,
                               "access to '" + origin.owner->Name() +
                                   "' conflicts with live reference '" +
                                   symbol->Name() + "'");
                    }
                }
            }
            for (const Loan& loan : _temporaries) {
                if (loan.id == access.temporary) continue;
                for (const Origin& other : loan.value.origins)
                    if (Overlaps(other, origin) && (write || other.exclusive))
                        Report(Code::BorrowConflict, range,
                               "access conflicts with a borrow in the current "
                               "expression");
            }
            if (write) {
                for (const Value& reservation : _reservations)
                    for (const Origin& other : reservation.origins)
                        if (Overlaps(other, origin))
                            Report(Code::BorrowConflict, range,
                                   "access conflicts with a reserved mutable "
                                   "receiver borrow");
            }
        }
    }

    bool Outlives(const Symbol* owner, const Symbol* holder) const {
        const auto ownerScope = _graph.symbolScopes.find(owner);
        if (ownerScope == _graph.symbolScopes.end()) return true;
        const auto holderScope = _graph.symbolScopes.find(holder);
        if (holderScope == _graph.symbolScopes.end()) return false;
        const core::BlockStatement* scope = holderScope->second;
        while (scope != nullptr) {
            if (scope == ownerScope->second) return true;
            scope = _graph.scopeParents.at(scope);
        }
        return false;
    }

    void RemoveTemporary(std::size_t id) {
        std::erase_if(_temporaries,
                      [id](const Loan& loan) { return loan.id == id; });
    }

    void Store(const Symbol* destination, Value value,
               source::SourceRange range) {
        CheckHandleChange(destination, range);
        for (const Origin& origin : value.origins) {
            if (!Outlives(origin.owner, destination))
                Report(Code::ReferenceEscape, range,
                       "reference would outlive its owner '" +
                           origin.owner->Name() + "'");
            for (const Symbol* parent : origin.parents)
                if (parent == destination || !Outlives(parent, destination))
                    Report(Code::ReferenceEscape, range,
                           "reference would outlive or overwrite its parent "
                           "reborrow");
        }
        if (value.handle != nullptr &&
            IsExclusiveBorrow(value.handle->Type())) {
            CheckHandleChange(value.handle, range);
            CheckAccess(value, true, range);
            if (value.handle->Type().IsConst())
                Report(Code::MovedReference, range,
                       "cannot move from a fixed mutable-reference binding; "
                       "reborrow it instead");
            (*_state)[value.handle] = {{}, true};
        }
        RemoveTemporary(value.temporary);
        (*_state)[destination] = {std::move(value.origins), false};
        _active.insert(destination);
    }

    Value Project(Value value, const StructureField* field) {
        Origins projected;
        for (Origin origin : value.origins) {
            if (!origin.wholeSequence)
                origin.fields.push_back({field, nullptr, 0});
            projected.insert(std::move(origin));
        }
        value.origins = std::move(projected);
        return value;
    }

    Value Sequence(Value value) {
        Origins origins;
        for (Origin origin : value.origins) {
            origin.wholeSequence = true;
            origins.insert(std::move(origin));
        }
        value.origins = std::move(origins);
        return value;
    }

    Value Place(const core::Expression& expression) {
        const core::Expression& inner = Unwrap(expression);
        const Symbol* symbol = Named(inner);
        if (symbol != nullptr) return {{{symbol, {}}}, nullptr, 0};
        if (inner.Kind() == Kind::MemberAccessExpression) {
            const auto& field =
                static_cast<const core::MemberAccessExpression&>(inner);
            if (!_model.FieldFor(field)) return {};
            if (field.Access() == TokenType::Arrow) {
                Value receiver = Evaluate(field.Receiver());
                return _model.TypeOf(field.Receiver()).Kind() ==
                               SemanticTypeKind::Reference
                           ? Project(std::move(receiver),
                                     _model.FieldFor(field))
                           : Value{};
            }
            return Project(Place(field.Receiver()), _model.FieldFor(field));
        }
        if (inner.Kind() == Kind::MemberCallExpression) {
            const auto& call =
                static_cast<const core::MemberCallExpression&>(inner);
            if (call.Member() == "front" || call.Member() == "back")
                return Sequence(_model.TypeOf(call.Receiver()).Kind() ==
                                        SemanticTypeKind::Slice
                                    ? Evaluate(call.Receiver())
                                    : Place(call.Receiver()));
        }
        if (inner.Kind() == Kind::IndexExpression) {
            const auto& index =
                static_cast<const core::IndexExpression&>(inner);
            Value value = _model.TypeOf(index.Receiver()).Kind() ==
                                  SemanticTypeKind::Slice
                              ? Evaluate(index.Receiver())
                              : Place(index.Receiver());
            const Symbols previousPinned = _pinned;
            if (value.handle != nullptr) _pinned.insert(value.handle);
            for (const Origin& origin : value.origins)
                _pinned.insert(origin.parents.begin(), origin.parents.end());
            (void)Evaluate(index.Index());
            _pinned = previousPinned;
            return Sequence(std::move(value));
        }
        if (inner.Kind() == Kind::UnaryExpression) {
            const auto& unary =
                static_cast<const core::UnaryExpression&>(inner);
            Value value = Evaluate(unary.Operand());
            if (_model.TypeOf(unary.Operand()).Kind() ==
                SemanticTypeKind::Reference)
                return value;
        }
        if (inner.Kind() != Kind::UnaryExpression) (void)Evaluate(inner);
        return {};
    }

    Value Borrow(Value value, bool exclusive, source::SourceRange range) {
        CheckAccess(value, exclusive, range);
        Origins origins;
        for (Origin origin : value.origins) {
            if (value.handle != nullptr) origin.parents.insert(value.handle);
            origin.exclusive = exclusive;
            origins.insert(std::move(origin));
        }
        RemoveTemporary(value.temporary);
        Value result{std::move(origins), nullptr, _nextTemporary++};
        _temporaries.push_back({result, exclusive, result.temporary});
        return result;
    }

    void SimulateBuildBlock(const core::BlockStatement& block) {
        for (const auto& statement : block.Statements())
            SimulateBuildStatement(*statement);
    }

    void SimulateBuildStatement(const core::Statement& statement) {
        switch (statement.Kind()) {
            case Kind::VariableDeclarationStatement: {
                const auto& declaration =
                    static_cast<const core::VariableDeclarationStatement&>(
                        statement);
                const Symbol* symbol = _model.DeclaredSymbol(declaration);
                Value value;
                if (declaration.Initializer() != nullptr)
                    value = Evaluate(*declaration.Initializer());
                if (TracksBorrow(symbol))
                    Store(symbol, std::move(value), declaration.Range());
                break;
            }
            case Kind::DestructureStatement: {
                const auto& destructure =
                    static_cast<const core::DestructureStatement&>(statement);
                Value value = Evaluate(destructure.Value());
                for (std::size_t index = 0;
                     index < destructure.Bindings().size(); ++index) {
                    const Symbol* target = _model.DestructureTarget(
                        *destructure.Bindings()[index]);
                    if (TracksBorrow(target) && index < value.elements.size())
                        Store(target, value.elements[index],
                              destructure.Bindings()[index]->Range());
                }
                break;
            }
            case Kind::ExpressionStatement:
                (void)Evaluate(
                    static_cast<const core::ExpressionStatement&>(statement)
                        .Value());
                break;
            case Kind::ReturnStatement: {
                const auto* value =
                    static_cast<const core::ReturnStatement&>(statement)
                        .Value();
                if (value != nullptr) (void)Evaluate(*value);
                break;
            }
            case Kind::BlockStatement:
                SimulateBuildBlock(
                    static_cast<const core::BlockStatement&>(statement));
                break;
            case Kind::RangeStatement:
                SimulateBuildBlock(
                    static_cast<const core::RangeStatement&>(statement)
                        .Expansion());
                break;
            case Kind::IfStatement: {
                const auto& conditional =
                    static_cast<const core::IfStatement&>(statement);
                const State before = *_state;
                State merged = before;
                auto mergeBranch = [&](const core::BlockStatement& body) {
                    *_state = before;
                    SimulateBuildBlock(body);
                    Merge(merged, *_state);
                };
                mergeBranch(conditional.PrimaryBranch().Body());
                for (const auto& branch : conditional.ElseIfBranches())
                    mergeBranch(branch->Body());
                if (conditional.ElseBody() != nullptr)
                    mergeBranch(*conditional.ElseBody());
                *_state = std::move(merged);
                break;
            }
            case Kind::WhileStatement: {
                const auto& loop =
                    static_cast<const core::WhileStatement&>(statement);
                const State before = *_state;
                SimulateBuildBlock(loop.Body());
                Merge(*_state, before);
                if (loop.Also() != nullptr)
                    SimulateBuildBlock(loop.Also()->Body());
                break;
            }
            default:
                break;
        }
    }

    Value EvaluateBuild(const core::BuildExpression& build) {
        State simulated = *_state;
        State* const previousState = _state;
        const Symbols previousActive = _active;
        const Symbols previousPinned = _pinned;
        const std::vector<Loan> previousLoans = _temporaries;
        const std::size_t previousTemporary = _nextTemporary;
        _state = &simulated;
        const Symbol* self = _model.DeclaredSymbol(build);
        if (self != nullptr) simulated[self] = {};
        SimulateBuildBlock(build.Body());
        Value result;
        if (self != nullptr) result.origins = simulated[self].origins;
        _state = previousState;
        _active = previousActive;
        _pinned = previousPinned;
        _temporaries = previousLoans;
        _nextTemporary = previousTemporary;
        return result;
    }

    Value Evaluate(const core::Expression& expression) {
        switch (expression.Kind()) {
            case Kind::BuildExpression:
                return EvaluateBuild(
                    static_cast<const core::BuildExpression&>(expression));
            case Kind::AnonymousFunctionExpression: {
                const auto& function =
                    static_cast<const core::AnonymousFunctionExpression&>(
                        expression);
                Value result;
                std::vector<std::size_t> temporaries;
                for (const auto& capture : function.Captures()) {
                    const auto mode = capture->Mode();
                    if (mode == core::AnonymousFunctionExpression::CaptureMode::
                                    Copy ||
                        mode == core::AnonymousFunctionExpression::CaptureMode::
                                    Lend)
                        continue;
                    const Symbol* source = _model.CaptureSource(*capture);
                    if (source == nullptr) continue;
                    if (mode ==
                        core::AnonymousFunctionExpression::CaptureMode::Own) {
                        if (source->Type().Kind() ==
                                SemanticTypeKind::Pointer ||
                            source->Type().Kind() ==
                                SemanticTypeKind::OwnedBuffer)
                            continue;
                        if (TracksBorrow(source)) {
                            const Handle handle = (*_state)[source];
                            Value moved{handle.origins, source, 0};
                            CheckAccess(moved, true, capture->NameRange());
                            if (source->Type().HasBorrowedLifetime()) {
                                CheckHandleChange(source, capture->NameRange());
                                (*_state)[source] = {{}, true};
                            }
                            result.origins.insert(handle.origins.begin(),
                                                  handle.origins.end());
                        }
                        continue;
                    }
                    Value place;
                    if (source->Type().HasBorrowedLifetime()) {
                        const Handle& handle = (*_state)[source];
                        if (handle.moved)
                            Report(Code::MovedReference, capture->NameRange(),
                                   "capture of possibly moved reference '" +
                                       source->Name() + "'");
                        place = {handle.origins, source, 0};
                    } else {
                        place = {{{source, {}}}, source, 0};
                    }
                    Value borrowed = Borrow(
                        std::move(place),
                        capture->Mode() == core::AnonymousFunctionExpression::
                                               CaptureMode::Reference,
                        capture->Range());
                    temporaries.push_back(borrowed.temporary);
                    result.origins.insert(borrowed.origins.begin(),
                                          borrowed.origins.end());
                }
                for (const std::size_t temporary : temporaries)
                    RemoveTemporary(temporary);
                if (!result.origins.empty()) {
                    result.temporary = _nextTemporary++;
                    _temporaries.push_back({result, false, result.temporary});
                }
                return result;
            }
            case Kind::SliceExpression: {
                const auto& slice =
                    static_cast<const core::SliceExpression&>(expression);
                Value value = _model.TypeOf(slice.Receiver()).Kind() ==
                                      SemanticTypeKind::Slice
                                  ? Evaluate(slice.Receiver())
                                  : Place(slice.Receiver());
                const Symbols previousPinned = _pinned;
                if (value.handle != nullptr) _pinned.insert(value.handle);
                for (const Origin& origin : value.origins)
                    _pinned.insert(origin.parents.begin(),
                                   origin.parents.end());
                if (slice.Start() != nullptr) (void)Evaluate(*slice.Start());
                if (slice.End() != nullptr) (void)Evaluate(*slice.End());
                _pinned = previousPinned;
                return Borrow(Sequence(std::move(value)),
                              !_model.TypeOf(expression).Target()->IsConst(),
                              expression.Range());
            }
            case Kind::TupleExpression: {
                Value result;
                for (const auto& element :
                     static_cast<const core::TupleExpression&>(expression)
                         .Elements()) {
                    Value value = Evaluate(*element);
                    result.origins.insert(value.origins.begin(),
                                          value.origins.end());
                    result.elements.push_back(std::move(value));
                }
                return result;
            }
            case Kind::ArrayExpression: {
                Value result;
                for (const auto& element :
                     static_cast<const core::ArrayExpression&>(expression)
                         .Elements()) {
                    Value value = Evaluate(*element);
                    result.origins.insert(value.origins.begin(),
                                          value.origins.end());
                }
                return result;
            }
            case Kind::MatchExpression: {
                const auto& match =
                    static_cast<const core::MatchExpression&>(expression);
                (void)Evaluate(match.Value());
                const State before = *_state;
                State merged;
                bool first = true;
                for (const auto& arm : match.Arms()) {
                    *_state = before;
                    (void)Evaluate(arm->Value());
                    if (first) {
                        merged = *_state;
                        first = false;
                    } else {
                        Merge(merged, *_state);
                    }
                }
                if (!first) *_state = std::move(merged);
                return {};
            }
            case Kind::ParenthesizedExpression:
                return Evaluate(Unwrap(expression));
            case Kind::NameExpression: {
                const Symbol* symbol = Named(expression);
                if (TracksBorrow(symbol)) {
                    const Handle& handle = (*_state)[symbol];
                    if (handle.moved && symbol->Type().HasBorrowedLifetime())
                        Report(Code::MovedReference, expression.Range(),
                               "use of possibly moved reference '" +
                                   symbol->Name() + "'");
                    if (!symbol->Type().HasBorrowedLifetime())
                        CheckAccess(
                            {{{symbol, {}}}, symbol, 0},
                            _model.TransferOf(expression) == TransferKind::Move,
                            expression.Range());
                    return {handle.origins, symbol, 0};
                }
                if (symbol != nullptr && symbol->Kind() != SymbolKind::Function)
                    CheckAccess(
                        {{{symbol, {}}}, nullptr, 0},
                        _model.TransferOf(expression) == TransferKind::Move,
                        expression.Range());
                return {};
            }
            case Kind::UnaryExpression: {
                const auto& unary =
                    static_cast<const core::UnaryExpression&>(expression);
                if (unary.OperatorType() == TokenType::Increment ||
                    unary.OperatorType() == TokenType::Decrement) {
                    CheckAccess(Place(unary.Operand()), true,
                                expression.Range());
                    return {};
                }
                if (unary.OperatorType() == TokenType::Ampersand)
                    return Borrow(
                        Place(unary.Operand()),
                        !_model.TypeOf(expression).Target()->IsConst(),
                        expression.Range());
                if (unary.OperatorType() == TokenType::RawAddress) {
                    CheckAccess(Place(unary.Operand()), true,
                                expression.Range());
                    return {};
                }
                if (unary.OperatorType() == TokenType::Mul) {
                    CheckAccess(Place(expression), false, expression.Range());
                    return {};
                }
                (void)Evaluate(unary.Operand());
                return {};
            }
            case Kind::AssignmentExpression: {
                const auto& assignment =
                    static_cast<const core::AssignmentExpression&>(expression);
                const Symbol* symbol = Named(assignment.Target());
                if (TracksBorrow(symbol)) {
                    Value value = Evaluate(assignment.Value());
                    Store(symbol, std::move(value), assignment.Value().Range());
                    return {(*_state)[symbol].origins, symbol, 0};
                }
                Value place = Place(assignment.Target());
                const Symbols previousPinned = _pinned;
                if (place.handle != nullptr) _pinned.insert(place.handle);
                for (const Origin& origin : place.origins)
                    _pinned.insert(origin.parents.begin(),
                                   origin.parents.end());
                Value value = Evaluate(assignment.Value());
                CheckAccess(place, true, assignment.Target().Range());
                _pinned = previousPinned;
                const Symbol* root = Root(assignment.Target());
                if (TracksBorrow(root) && !root->Type().HasBorrowedLifetime()) {
                    value.origins.insert((*_state)[root].origins.begin(),
                                         (*_state)[root].origins.end());
                    Store(root, std::move(value), assignment.Value().Range());
                }
                return {};
            }
            case Kind::BinaryExpression: {
                const auto& binary =
                    static_cast<const core::BinaryExpression&>(expression);
                (void)Evaluate(binary.Left());
                const bool conditional =
                    _model.TypeOf(expression).Kind() ==
                        SemanticTypeKind::Bool &&
                    (binary.OperatorType() == TokenType::And ||
                     binary.OperatorType() == TokenType::Or);
                const State skipped = conditional ? *_state : State{};
                (void)Evaluate(binary.Right());
                if (conditional) Merge(*_state, skipped);
                return {};
            }
            case Kind::CallExpression: {
                const auto& call =
                    static_cast<const core::CallExpression&>(expression);
                const Symbol* symbol = Named(call.Callee());
                const auto* function =
                    symbol && symbol->Kind() == SymbolKind::Function
                        ? static_cast<const FunctionSymbol*>(symbol)
                        : nullptr;
                const SemanticType callable = _model.TypeOf(call.Callee());
                const CallableSignature* signature =
                    callable.Kind() == SemanticTypeKind::Function
                        ? &callable.Signature()
                        : nullptr;
                const bool allocation =
                    function &&
                    (function->BuiltIn() == BuiltInFunction::New ||
                     function->BuiltIn() == BuiltInFunction::StorageNew);
                const State skipped = allocation ? *_state : State{};
                const std::vector<Loan> previousLoans = _temporaries;
                if (function == nullptr) {
                    Value callee = Evaluate(call.Callee());
                    for (const Origin& origin : callee.origins)
                        CheckAccess({{origin}, callee.handle, callee.temporary},
                                    origin.exclusive, call.Callee().Range());
                }
                Value borrowSource;
                for (std::size_t index = 0; index < call.Arguments().size();
                     ++index) {
                    Value value = Evaluate(*call.Arguments()[index]);
                    const bool destroys =
                        signature != nullptr &&
                        ((index == 0 && function != nullptr &&
                          (function->BuiltIn() == BuiltInFunction::Free ||
                           function->BuiltIn() ==
                               BuiltInFunction::StorageFree ||
                           function->BuiltIn() == BuiltInFunction::Take)) ||
                         (index < signature->parameterContracts.size() &&
                          signature->parameterContracts[index] ==
                              core::OwnershipContract::Own));
                    if (destroys)
                        CheckAccess(Place(*call.Arguments()[index]), true,
                                    call.Arguments()[index]->Range());
                    if (signature != nullptr &&
                        std::find(signature->borrowSourceIndices.begin(),
                                  signature->borrowSourceIndices.end(),
                                  index) !=
                            signature->borrowSourceIndices.end())
                        AddBorrowSource(borrowSource, value);
                    if (signature != nullptr &&
                        index < signature->parameters.size() &&
                        signature->parameters[index].HasBorrowedLifetime()) {
                        (void)Borrow(
                            std::move(value),
                            IsExclusiveBorrow(signature->parameters[index]),
                            call.Arguments()[index]->Range());
                    }
                }
                _temporaries = previousLoans;
                if (allocation) Merge(*_state, skipped);
                if (signature != nullptr && CanCarryBorrow(signature->result) &&
                    !signature->borrowSourceIndices.empty()) {
                    std::size_t resultIndex = 0;
                    return BorrowResult(
                        signature->result, borrowSource, expression.Range(),
                        function, resultIndex,
                        signature->result.Kind() == SemanticTypeKind::Tuple);
                }
                return {};
            }
            case Kind::CastExpression: {
                const auto& cast =
                    static_cast<const core::CastExpression&>(expression);
                if (_model.TypeOf(expression).Kind() ==
                    SemanticTypeKind::StringView) {
                    if (_model.TypeOf(cast.Value()).Kind() ==
                        SemanticTypeKind::String)
                        return Borrow(Place(cast.Value()), false,
                                      expression.Range());
                    return Evaluate(cast.Value());
                }
                (void)Evaluate(cast.Value());
                return {};
            }
            case Kind::MemberAccessExpression: {
                const auto& field =
                    static_cast<const core::MemberAccessExpression&>(
                        expression);
                if (_model.FieldFor(field)) {
                    CheckAccess(Place(expression), false, expression.Range());
                    if (CanCarryBorrow(_model.TypeOf(expression)))
                        return Evaluate(field.Receiver());
                }
                return {};
            }
            case Kind::MemberCallExpression: {
                const auto& call =
                    static_cast<const core::MemberCallExpression&>(expression);
                if (const auto* method = _model.MethodFor(call)) {
                    const std::vector<Loan> previousLoans = _temporaries;
                    Value borrowSource;
                    Value mutableReceiver;
                    bool hasMutableReceiver = false;
                    if (method->HasReceiver()) {
                        const auto receiverType = method->ParameterTypes()[0];
                        const bool owningReceiver =
                            receiverType.Kind() ==
                                SemanticTypeKind::Structure ||
                            receiverType.Kind() == SemanticTypeKind::Enum;
                        Value value =
                            owningReceiver || call.Access() == TokenType::Arrow
                                ? Evaluate(call.Receiver())
                                : Place(call.Receiver());
                        if (std::find(method->BorrowSourceIndices().begin(),
                                      method->BorrowSourceIndices().end(),
                                      0) != method->BorrowSourceIndices().end())
                            AddBorrowSource(borrowSource, value);
                        if (method->ParameterTypes()[0].Kind() ==
                                SemanticTypeKind::Reference &&
                            _model.TypeOf(call.Receiver()).Kind() !=
                                SemanticTypeKind::Pointer) {
                            const bool exclusive = !method->ParameterTypes()[0]
                                                        .Target()
                                                        ->IsConst();
                            if (exclusive) {
                                CheckAccess(value, true,
                                            call.Receiver().Range());
                                mutableReceiver = std::move(value);
                                _reservations.push_back(mutableReceiver);
                                hasMutableReceiver = true;
                            } else {
                                (void)Borrow(std::move(value), false,
                                             call.Receiver().Range());
                            }
                        }
                    }
                    const std::size_t offset = method->HasReceiver() ? 1 : 0;
                    for (std::size_t i = 0; i < call.Arguments().size(); ++i) {
                        Value value = Evaluate(*call.Arguments()[i]);
                        const std::size_t parameter = i + offset;
                        if (method->ParameterContract(parameter) ==
                            core::OwnershipContract::Own)
                            CheckAccess(Place(*call.Arguments()[i]), true,
                                        call.Arguments()[i]->Range());
                        if (std::find(method->BorrowSourceIndices().begin(),
                                      method->BorrowSourceIndices().end(),
                                      parameter) !=
                            method->BorrowSourceIndices().end())
                            AddBorrowSource(borrowSource, value);
                        const auto type = method->ParameterTypes()[parameter];
                        if (type.HasBorrowedLifetime())
                            (void)Borrow(std::move(value),
                                         IsExclusiveBorrow(type),
                                         call.Arguments()[i]->Range());
                    }
                    if (hasMutableReceiver) {
                        _reservations.pop_back();
                        (void)Borrow(std::move(mutableReceiver), true,
                                     call.Receiver().Range());
                    }
                    _temporaries = previousLoans;
                    if (CanCarryBorrow(method->ReturnType()) &&
                        !method->BorrowSourceIndices().empty()) {
                        std::size_t resultIndex = 0;
                        return BorrowResult(method->ReturnType(), borrowSource,
                                            expression.Range(), method,
                                            resultIndex,
                                            method->ReturnType().Kind() ==
                                                SemanticTypeKind::Tuple);
                    }
                    return {};
                }
                const SemanticType receiverType =
                    _model.TypeOf(call.Receiver());
                if (_model.TypeOf(expression).Kind() ==
                        SemanticTypeKind::StringView &&
                    (call.Member() == "view" || call.Member() == "substr")) {
                    Value value =
                        receiverType.Kind() == SemanticTypeKind::StringView
                            ? Evaluate(call.Receiver())
                            : Place(call.Receiver());
                    const Symbols previousPinned = _pinned;
                    if (value.handle != nullptr) _pinned.insert(value.handle);
                    for (const Origin& origin : value.origins)
                        _pinned.insert(origin.parents.begin(),
                                       origin.parents.end());
                    for (const auto& argument : call.Arguments())
                        (void)Evaluate(*argument);
                    _pinned = previousPinned;
                    return Borrow(std::move(value), false, expression.Range());
                }
                if (_model.TypeOf(call.Receiver()).IsSequence() &&
                    (call.Member() == "front" || call.Member() == "back")) {
                    CheckAccess(Place(expression), false, expression.Range());
                    return {};
                }
                if (_model.TypeOf(call.Receiver()).Kind() ==
                    SemanticTypeKind::Slice) {
                    CheckAccess(Evaluate(call.Receiver()), false,
                                call.Receiver().Range());
                } else {
                    (void)Evaluate(call.Receiver());
                }
                for (const auto& argument : call.Arguments())
                    (void)Evaluate(*argument);
                return {};
            }
            case Kind::IndexExpression: {
                const auto& index =
                    static_cast<const core::IndexExpression&>(expression);
                if (_model.TypeOf(index.Receiver()).IsSequence()) {
                    CheckAccess(Place(expression), false, expression.Range());
                } else {
                    (void)Evaluate(index.Receiver());
                    (void)Evaluate(index.Index());
                }
                return {};
            }
            default:
                return {};
        }
    }

    const SemanticModel& _model;
    diagnostics::DiagnosticBag& _diagnostics;
    const ControlFlowGraph& _graph;
    std::vector<Symbols> _live, _uses, _defs;
    State* _state = nullptr;
    Symbols _active, _pinned;
    std::vector<Loan> _temporaries;
    std::vector<Value> _reservations;
    std::size_t _nextTemporary = 1;
    const FunctionSymbol* _function = nullptr;
    Symbols _borrowSources;
    bool _report = false;
    std::map<std::pair<source::SourceRange, Code>, std::string> _reported;
};
}  // namespace

void AnalyzeBorrows(const core::FunctionDeclaration& function,
                    const SemanticModel& model,
                    diagnostics::DiagnosticBag& diagnostics,
                    const ControlFlowGraph& graph) {
    BorrowChecker(model, diagnostics, graph).Analyze(function);
}
}  // namespace flecha::semantic
