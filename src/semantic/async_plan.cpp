#include "semantic/async_plan.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

#include "core/ast_walk.hpp"

namespace flecha::semantic {
namespace {
const core::Expression& Unwrap(const core::Expression& value) {
    if (value.Kind() == core::AstNodeKind::ParenthesizedExpression)
        return Unwrap(
            static_cast<const core::ParenthesizedExpression&>(value).Inner());
    return value;
}
bool Suspends(const AsyncState& state) {
    return state.kind == AsyncStateKind::DirectAwait ||
           state.kind == AsyncStateKind::ChildAwait ||
           state.kind == AsyncStateKind::DrainChildren;
}
bool Active(const AsyncFrameSlot& slot, const AsyncState& state) {
    return !slot.scope ||
           std::ranges::find(state.scopes, *slot.scope) != state.scopes.end();
}
std::vector<AsyncSlotId> Retained(const AsyncFramePlan& plan,
                                  const AsyncState& state) {
    std::vector<AsyncSlotId> slots;
    if (!Suspends(state)) return slots;
    for (AsyncSlotId id = 0; id < plan.slots.size(); ++id) {
        const auto& slot = plan.slots[id];
        // Pending operation records belong to their own suspension, not to
        // every later await in the same block. Everything else is conservative.
        if (slot.kind == AsyncSlotKind::Computation ||
            slot.kind == AsyncSlotKind::ChildJoin) {
            if (state.storage == id) slots.push_back(id);
        } else if (Active(slot, state))
            slots.push_back(id);
    }
    return slots;
}

// Compute a forward may analysis over initialization alternatives and backward
// read liveness. Starting at bottom avoids treating an unvisited loop backedge
// as evidence of initialization. Lexical scope exit clears activation state.
void AnalyzeStates(AsyncFramePlan& plan) {
    const auto count = plan.slots.size();
    std::vector<std::vector<unsigned char>> incoming(
        plan.states.size(), std::vector<unsigned char>(count));
    std::vector<bool> reached(plan.states.size());
    reached[plan.entry] = true;
    for (AsyncSlotId slot = 0; slot < count; ++slot)
        incoming[plan.entry][slot] =
            plan.slots[slot].kind == AsyncSlotKind::Parameter ? 2 : 1;
    bool changed = true;
    while (changed) {
        changed = false;
        for (AsyncStateId id = 0; id < plan.states.size(); ++id) {
            if (!reached[id]) continue;
            const auto& state = plan.states[id];
            auto output = incoming[id];
            for (auto slot : state.resets) output[slot] = 1;
            for (auto slot : state.moves) output[slot] = 4;
            for (auto slot : state.writes) output[slot] = 2;
            for (const auto& edge : state.edges) {
                if (!reached[edge.target]) {
                    reached[edge.target] = true;
                    changed = true;
                }
                for (AsyncSlotId slot = 0; slot < count; ++slot) {
                    const auto value =
                        Active(plan.slots[slot], plan.states[edge.target])
                            ? output[slot]
                            : 1;
                    const auto merged = incoming[edge.target][slot] | value;
                    if (merged != incoming[edge.target][slot]) {
                        incoming[edge.target][slot] =
                            static_cast<unsigned char>(merged);
                        changed = true;
                    }
                }
            }
        }
    }
    std::vector<std::set<AsyncSlotId>> live(plan.states.size());
    changed = true;
    while (changed) {
        changed = false;
        for (AsyncStateId id = plan.states.size(); id-- > 0;) {
            if (!reached[id]) continue;
            const auto& state = plan.states[id];
            std::set<AsyncSlotId> next;
            for (const auto& edge : state.edges)
                next.insert(live[edge.target].begin(), live[edge.target].end());
            for (auto slot : state.writes) next.erase(slot);
            for (auto slot : state.resets) next.erase(slot);
            for (auto slot : state.moves) next.erase(slot);
            if (state.read) next.insert(*state.read);
            std::erase_if(next, [&](auto slot) {
                return !Active(plan.slots[slot], state);
            });
            if (next != live[id]) {
                live[id] = std::move(next);
                changed = true;
            }
        }
    }
    for (AsyncStateId id = 0; id < plan.states.size(); ++id) {
        auto& state = plan.states[id];
        state.initialization = std::move(incoming[id]);
        state.live.assign(live[id].begin(), live[id].end());
        state.retained = Retained(plan, state);
    }
}

const char* Name(AsyncSlotKind kind) {
    switch (kind) {
        case AsyncSlotKind::Parameter:
            return "parameter";
        case AsyncSlotKind::Local:
            return "local";
        case AsyncSlotKind::DeferredCall:
            return "saved-defer";
        case AsyncSlotKind::DeferredStack:
            return "defer-stack";
        case AsyncSlotKind::Computation:
            return "indirect-computation";
        case AsyncSlotKind::ChildJoin:
            return "child-join";
        case AsyncSlotKind::TaskScope:
            return "task-scope";
        case AsyncSlotKind::ReturnValue:
            return "return-value";
        case AsyncSlotKind::AwaitValue:
            return "await-value";
        case AsyncSlotKind::ExecutionError:
            return "execution-error";
    }
    return "invalid";
}
const char* Name(AsyncStateKind kind) {
    switch (kind) {
        case AsyncStateKind::Flow:
            return "flow";
        case AsyncStateKind::CreateChild:
            return "create-child";
        case AsyncStateKind::DirectAwait:
            return "direct-await";
        case AsyncStateKind::ChildAwait:
            return "child-await";
        case AsyncStateKind::CheckedSuccess:
            return "checked-success";
        case AsyncStateKind::CheckedFailure:
            return "checked-failure";
        case AsyncStateKind::CancelChildren:
            return "cancel-children";
        case AsyncStateKind::DrainChildren:
            return "drain-children";
        case AsyncStateKind::RunDefers:
            return "run-defers";
        case AsyncStateKind::CleanupScope:
            return "cleanup-scope";
        case AsyncStateKind::Complete:
            return "complete";
        case AsyncStateKind::Failed:
            return "failed";
    }
    return "invalid";
}
const char* Name(AsyncEdgeKind kind) {
    switch (kind) {
        case AsyncEdgeKind::Next:
            return "next";
        case AsyncEdgeKind::Success:
            return "success";
        case AsyncEdgeKind::Failure:
            return "failure";
        case AsyncEdgeKind::Pending:
            return "pending";
        case AsyncEdgeKind::Retired:
            return "retired";
    }
    return "invalid";
}
}  // namespace

AsyncFramePlan BuildAsyncFramePlan(const core::FunctionDeclaration& function,
                                   const SemanticModel& model) {
    AsyncFramePlan plan;
    plan.function = &function;
    plan.flow = BuildControlFlowGraph(function, model);
    plan.entry = plan.flow.entry;
    std::map<const core::BlockStatement*, AsyncScopeId> scopes;
    // AST order, never pointer/map iteration order, defines public dump IDs.
    core::WalkAst(function.Body(), [&](const core::AstNode& syntax,
                                       bool entering) {
        if (!entering || syntax.Kind() != core::AstNodeKind::BlockStatement)
            return;
        const auto* block = static_cast<const core::BlockStatement*>(&syntax);
        if (!plan.flow.scopeParents.contains(block)) return;
        const auto* parent = plan.flow.scopeParents.at(block);
        const auto id = plan.scopes.size();
        scopes.emplace(block, id);
        plan.scopes.push_back(
            {block, parent ? std::optional(scopes.at(parent)) : std::nullopt,
             std::nullopt, std::nullopt});
    });
    std::map<const Symbol*, AsyncSlotId> symbols;
    auto addSlot = [&](AsyncSlotKind kind, const Symbol* symbol,
                       const core::AstNode* syntax,
                       std::optional<AsyncScopeId> scope,
                       SemanticType type = SemanticType::Error()) {
        const auto id = plan.slots.size();
        plan.slots.push_back({kind, symbol, syntax, scope, type});
        if (symbol) symbols.emplace(symbol, id);
        return id;
    };
    for (const auto& parameter : function.Parameters()) {
        const auto* symbol = model.DeclaredSymbol(*parameter);
        if (symbol)
            addSlot(AsyncSlotKind::Parameter, symbol, parameter.get(),
                    scopes.at(&function.Body()), symbol->Type());
    }
    for (const auto& owned : model.Symbols()) {
        const auto* symbol = owned.get();
        if (symbols.contains(symbol)) continue;
        const auto found = plan.flow.symbolScopes.find(symbol);
        if (found != plan.flow.symbolScopes.end())
            addSlot(AsyncSlotKind::Local, symbol, nullptr,
                    scopes.at(found->second), symbol->Type());
    }
    const auto* functionSymbol =
        static_cast<const FunctionSymbol*>(model.DeclaredSymbol(function));
    if (functionSymbol &&
        functionSymbol->ReturnType().Kind() != SemanticTypeKind::Void)
        plan.returnValue = addSlot(AsyncSlotKind::ReturnValue, nullptr,
                                   &function, {}, functionSymbol->ReturnType());
    std::map<const core::DeferStatement*, AsyncSlotId> defers;
    std::map<const core::DestructureStatement*, AsyncSlotId> operations;
    // First assign scope records so every specialized instance shares storage.
    for (const auto& node : plan.flow.nodes) {
        if (!node.reachable || !node.checkedDispatch) continue;
        if (!model.CheckedResultFor(*node.checkedDispatch)->creation) continue;
        const auto scope = scopes.at(node.scopes.back());
        if (!plan.scopes[scope].children)
            plan.scopes[scope].children =
                addSlot(AsyncSlotKind::TaskScope, nullptr,
                        plan.scopes[scope].syntax, scope);
    }
    std::map<const core::AwaitExpression*, AsyncOperationPlan> awaits;
    std::map<const core::AwaitExpression*, AsyncSlotId> awaitStorage;
    for (const auto& node : plan.flow.nodes) {
        const auto* syntax = node.awaitDispatch;
        if (!syntax || awaits.contains(syntax)) continue;
        const auto& result = *model.AwaitResultFor(*syntax);
        const auto scope = scopes.at(node.scopes.back());
        std::optional<AsyncSlotId> value;
        if (result.completed.Kind() != SemanticTypeKind::Void)
            value = addSlot(AsyncSlotKind::AwaitValue, nullptr, syntax, scope,
                            result.completed);
        if (result.forwardsFailure && !plan.executionError)
            plan.executionError =
                addSlot(AsyncSlotKind::ExecutionError, nullptr, &function, {},
                        SemanticType(SemanticTypeKind::ErrorValue));
        const auto error = result.forwardsFailure
                               ? *plan.executionError
                               : symbols.at(result.destination);
        awaits.emplace(
            syntax, AsyncOperationPlan{nullptr, {}, value, error, {}, false});
        const bool child = model.TypeOf(Unwrap(syntax->Operand())).Kind() ==
                           SemanticTypeKind::Task;
        awaitStorage.emplace(syntax, addSlot(child ? AsyncSlotKind::ChildJoin
                                                   : AsyncSlotKind::Computation,
                                             nullptr, syntax, scope));
    }
    plan.states.resize(plan.flow.nodes.size());
    for (NodeId id = 0; id < plan.flow.nodes.size(); ++id) {
        const auto& node = plan.flow.nodes[id];
        auto& state = plan.states[id];
        state.flowNode = id;
        for (const auto* scope : node.scopes)
            state.scopes.push_back(scopes.at(scope));
        for (auto successor : node.successors)
            state.edges.push_back({AsyncEdgeKind::Next, successor});
        auto slotFor = [&](const Symbol* symbol) -> std::optional<AsyncSlotId> {
            const auto found = symbols.find(symbol);
            return found == symbols.end() ? std::nullopt
                                          : std::optional(found->second);
        };
        if (node.read) state.read = slotFor(model.SymbolFor(*node.read));
        if (node.captureRead)
            state.read = slotFor(model.CaptureSource(*node.captureRead));
        if (auto slot = slotFor(node.reset)) state.resets.push_back(*slot);
        if (auto slot = slotFor(node.move)) state.moves.push_back(*slot);
        if (auto slot = slotFor(node.write)) state.writes.push_back(*slot);
        // Joining consumes the handle on either terminal outcome. The borrow
        // checker tracks this separately from ordinary expression transfers;
        // record it explicitly for frame initialization/cleanup planning.
        if (node.memoryExpression && node.memoryExpression->Kind() ==
                                         core::AstNodeKind::AwaitExpression) {
            const auto& operand =
                Unwrap(static_cast<const core::AwaitExpression&>(
                           *node.memoryExpression)
                           .Operand());
            if (model.TypeOf(operand).Kind() == SemanticTypeKind::Task &&
                operand.Kind() == core::AstNodeKind::NameExpression)
                if (auto slot = slotFor(model.SymbolFor(
                        static_cast<const core::NameExpression&>(operand))))
                    state.moves.push_back(*slot);
        }
        if (node.memoryExpression && node.memoryExpression->Kind() ==
                                         core::AstNodeKind::AwaitExpression) {
            const auto* syntax = static_cast<const core::AwaitExpression*>(
                node.memoryExpression);
            if (const auto found = awaits.find(syntax); found != awaits.end() &&
                                                        found->second.value &&
                                                        node.asyncSuccess)
                state.moves.push_back(*found->second.value);
        }
        if (node.awaitOutcome) {
            state.awaitOperation = node.awaitOutcome;
            state.kind = node.asyncSuccess ? AsyncStateKind::CheckedSuccess
                                           : AsyncStateKind::CheckedFailure;
            const auto& contract = awaits.at(node.awaitOutcome);
            if (contract.value) {
                state.resets.push_back(*contract.value);
                if (node.asyncSuccess) state.writes.push_back(*contract.value);
            }
            // Keep the forwarded error outside lexical cleanup storage.
            if (!node.awaitOutcome->ForwardsFailure() || !node.asyncSuccess)
                state.writes.push_back(contract.error);
        }
        if (node.memoryReturn && node.memoryReturn->Value() && plan.returnValue)
            state.writes.push_back(*plan.returnValue);
        if (node.memoryDefer) {
            const auto* syntax = node.memoryDefer;
            if (!defers.contains(syntax))
                defers.emplace(syntax,
                               addSlot(AsyncSlotKind::DeferredCall, nullptr,
                                       syntax, scopes.at(node.scopes.back())));
            const auto scope = scopes.at(node.scopes.back());
            if (!plan.scopes[scope].deferredStack)
                plan.scopes[scope].deferredStack =
                    addSlot(AsyncSlotKind::DeferredStack, nullptr,
                            plan.scopes[scope].syntax, scope);
            state.writes.push_back(*plan.scopes[scope].deferredStack);
            state.writes.push_back(defers.at(syntax));
            for (const auto* capture : model.DeferredCaptures(*syntax))
                if (auto slot = slotFor(capture)) state.writes.push_back(*slot);
        }
        if (node.destructure && model.CheckedResultFor(*node.destructure)) {
            state.operation = node.destructure;
            state.kind = node.asyncSuccess ? AsyncStateKind::CheckedSuccess
                                           : AsyncStateKind::CheckedFailure;
            if (node.asyncSuccess &&
                model.CheckedResultFor(*node.destructure)->creation)
                state.writes.push_back(
                    *plan.scopes[scopes.at(node.scopes.back())].children);
        }
        if (!node.checkedDispatch && !node.awaitDispatch) continue;
        state.operation = node.checkedDispatch;
        state.awaitOperation = node.awaitDispatch;
        const auto* checked = state.operation
                                  ? model.CheckedResultFor(*state.operation)
                                  : nullptr;
        const bool creation = checked && checked->creation;
        const auto& expression =
            state.operation
                ? Unwrap(state.operation->Value())
                : static_cast<const core::Expression&>(*state.awaitOperation);
        if (creation) {
            state.kind = AsyncStateKind::CreateChild;
            state.storage = plan.scopes[scopes.at(node.scopes.back())].children;
        } else {
            const auto& await =
                static_cast<const core::AwaitExpression&>(expression);
            const bool child = model.TypeOf(Unwrap(await.Operand())).Kind() ==
                               SemanticTypeKind::Task;
            state.kind = child ? AsyncStateKind::ChildAwait
                               : AsyncStateKind::DirectAwait;
            if (state.awaitOperation) {
                state.storage = awaitStorage.at(state.awaitOperation);
            } else {
                if (!operations.contains(state.operation))
                    operations.emplace(
                        state.operation,
                        addSlot(child ? AsyncSlotKind::ChildJoin
                                      : AsyncSlotKind::Computation,
                                nullptr, &await,
                                scopes.at(node.scopes.back())));
                state.storage = operations.at(state.operation);
            }
        }
        const auto& operand = Unwrap(
            creation ? static_cast<const core::SpawnExpression&>(expression)
                           .Operand()
                     : static_cast<const core::AwaitExpression&>(expression)
                           .Operand());
        AsyncOperationPlan contract =
            checked ? AsyncOperationPlan{nullptr,
                                         {},
                                         symbols.at(checked->value),
                                         symbols.at(checked->error),
                                         {},
                                         creation}
                    : awaits.at(state.awaitOperation);
        if (operand.Kind() == core::AstNodeKind::CallExpression) {
            const auto& call =
                static_cast<const core::CallExpression&>(operand);
            const auto& callee = Unwrap(call.Callee());
            if (callee.Kind() == core::AstNodeKind::NameExpression) {
                const auto* symbol = model.SymbolFor(
                    static_cast<const core::NameExpression&>(callee));
                if (symbol && symbol->Kind() == SymbolKind::Function)
                    contract.callee =
                        static_cast<const FunctionSymbol*>(symbol);
            }
            contract.arguments = model.ArgumentsFor(call);
        } else if (operand.Kind() == core::AstNodeKind::MemberCallExpression) {
            const auto& call =
                static_cast<const core::MemberCallExpression&>(operand);
            contract.callee = model.MethodFor(call);
            if (contract.callee && contract.callee->HasReceiver())
                contract.arguments.push_back(&call.Receiver());
            const auto arguments = model.ArgumentsFor(call);
            contract.arguments.insert(contract.arguments.end(),
                                      arguments.begin(), arguments.end());
        } else if (operand.Kind() == core::AstNodeKind::NameExpression) {
            contract.consumedChild = slotFor(model.SymbolFor(
                static_cast<const core::NameExpression&>(operand)));
        }
        state.checked = std::move(contract);
        state.edges[0].kind = AsyncEdgeKind::Success;
        state.edges[1].kind = AsyncEdgeKind::Failure;
        if (Suspends(state))
            state.edges.push_back({AsyncEdgeKind::Pending, id});
    }
    plan.states[plan.flow.exit].kind = AsyncStateKind::Complete;
    auto addState = [&](AsyncState state) {
        const auto id = plan.states.size();
        plan.states.push_back(std::move(state));
        return id;
    };
    // Replace each exit edge with inner-to-outer drain and synchronous cleanup.
    // Cancellation is requested before polling; Pending loops only on the
    // drain. Retired is the sole edge that may reach destruction.
    for (const auto& exit : plan.flow.taskScopeExits) {
        AsyncStateId next;
        if (exit.successor)
            next = *exit.successor;
        else {
            AsyncState complete;
            const bool failed =
                plan.flow.nodes[exit.source].failureExit != nullptr;
            complete.kind =
                failed ? AsyncStateKind::Failed : AsyncStateKind::Complete;
            complete.read = failed ? plan.executionError : plan.returnValue;
            next = addState(std::move(complete));
        }
        for (auto scope = exit.scopes.rbegin(); scope != exit.scopes.rend();
             ++scope) {
            const auto sid = scopes.at(*scope);
            std::vector<AsyncScopeId> active;
            for (const auto* block : plan.flow.nodes[exit.source].scopes) {
                active.push_back(scopes.at(block));
                if (block == *scope) break;
            }
            AsyncState cleanup;
            cleanup.kind = AsyncStateKind::CleanupScope;
            cleanup.scopes = active;
            cleanup.cleanupScope = sid;
            cleanup.edges.push_back({AsyncEdgeKind::Next, next});
            for (AsyncSlotId slot = 0; slot < plan.slots.size(); ++slot)
                if (plan.slots[slot].scope == sid)
                    cleanup.resets.push_back(slot);
            for (AsyncSlotId slot = plan.slots.size(); slot-- > 0;) {
                const auto& value = plan.slots[slot];
                if (value.scope == sid &&
                    (value.symbol || value.kind == AsyncSlotKind::AwaitValue) &&
                    value.type.OwnsStorage() &&
                    !plan.flow.deferredCaptures.contains(value.symbol))
                    cleanup.destroys.push_back(slot);
            }
            next = addState(std::move(cleanup));
            if (const auto stack = plan.scopes[sid].deferredStack) {
                AsyncState deferred;
                deferred.kind = AsyncStateKind::RunDefers;
                deferred.scopes = active;
                deferred.cleanupScope = sid;
                deferred.storage = stack;
                deferred.edges.push_back({AsyncEdgeKind::Next, next});
                deferred.resets.push_back(*stack);
                for (const auto& [syntax, slot] : defers)
                    if (plan.slots[slot].scope == sid)
                        deferred.resets.push_back(slot);
                std::ranges::sort(deferred.resets);
                next = addState(std::move(deferred));
            }
            if (const auto storage = plan.scopes[sid].children) {
                AsyncState drain;
                drain.kind = AsyncStateKind::DrainChildren;
                drain.scopes = active;
                drain.cleanupScope = sid;
                drain.storage = storage;
                drain.edges = {{AsyncEdgeKind::Retired, next},
                               {AsyncEdgeKind::Pending, plan.states.size()}};
                next = addState(std::move(drain));
                if (exit.kind == TaskScopeExitKind::CancelAndDrain) {
                    AsyncState cancel;
                    cancel.kind = AsyncStateKind::CancelChildren;
                    cancel.scopes = active;
                    cancel.cleanupScope = sid;
                    cancel.storage = storage;
                    cancel.edges.push_back({AsyncEdgeKind::Next, next});
                    next = addState(std::move(cancel));
                }
            }
        }
        if (exit.successor) {
            for (auto& edge : plan.states[exit.source].edges)
                if (edge.target == *exit.successor) edge.target = next;
        } else
            plan.states[exit.source].edges.push_back(
                {AsyncEdgeKind::Next, next});
    }
    AnalyzeStates(plan);
    return plan;
}

std::vector<std::string> VerifyAsyncFramePlan(const AsyncFramePlan& plan,
                                              const SemanticModel& model) {
    std::vector<std::string> errors;
    auto fail = [&](std::string message) {
        errors.push_back(std::move(message));
    };
    if (!plan.function || !plan.function->IsAsync())
        fail("plan requires an async function");
    if (plan.entry >= plan.states.size()) fail("invalid entry state");
    for (const auto& slot : plan.slots) {
        if (!slot.stableAddress || !slot.guarded)
            fail("frame slot requires stable guarded storage");
        if (slot.scope && *slot.scope >= plan.scopes.size())
            fail("invalid slot scope");
    }
    for (AsyncStateId id = 0; id < plan.states.size(); ++id) {
        const auto& state = plan.states[id];
        for (const auto& edge : state.edges)
            if (edge.target >= plan.states.size())
                fail("invalid continuation target");
        for (auto scope : state.scopes)
            if (scope >= plan.scopes.size()) fail("invalid active scope");
        for (const auto* slots :
             {&state.resets, &state.writes, &state.moves, &state.destroys,
              &state.live, &state.retained})
            for (auto slot : *slots)
                if (slot >= plan.slots.size())
                    fail("invalid frame slot reference");
        for (auto slot : {state.read, state.storage})
            if (slot && *slot >= plan.slots.size())
                fail("invalid frame slot reference");
        if (state.checked) {
            if ((state.checked->value &&
                 *state.checked->value >= plan.slots.size()) ||
                state.checked->error >= plan.slots.size())
                fail("invalid checked-result slot");
            if (state.checked->consumedChild &&
                *state.checked->consumedChild >= plan.slots.size())
                fail("invalid consumed child slot");
        }
        if (state.initialization.size() != plan.slots.size())
            fail("missing initialization state");
    }
    if (!errors.empty()) return errors;
    auto analyzed = plan;
    AnalyzeStates(analyzed);
    for (AsyncStateId id = 0; id < plan.states.size(); ++id) {
        const auto& state = plan.states[id];
        if (state.initialization != analyzed.states[id].initialization)
            fail("inconsistent initialization state");
        if (state.live != analyzed.states[id].live)
            fail("inconsistent read liveness");
        if (state.retained != Retained(plan, state))
            fail("suspension loses required lexical storage");
        if (state.read) {
            const auto mask = analyzed.states[id].initialization[*state.read];
            if (mask && mask != 2) fail("read of absent or moved frame value");
        }
        if (Suspends(state)) {
            if (!state.storage)
                fail("suspension lacks guarded operation storage");
            if (std::ranges::count(state.edges,
                                   AsyncEdge{AsyncEdgeKind::Pending, id}) != 1)
                fail("pending must resume the same operation");
        }
        if (state.kind == AsyncStateKind::CreateChild ||
            state.kind == AsyncStateKind::DirectAwait ||
            state.kind == AsyncStateKind::ChildAwait) {
            if ((state.operation && state.awaitOperation) ||
                (state.operation
                     ? !model.CheckedResultFor(*state.operation)
                     : !state.awaitOperation ||
                           !model.AwaitResultFor(*state.awaitOperation))) {
                fail("operation lacks checked-result contract");
                continue;
            }
            if (state.operation) {
                const auto* contract = model.CheckedResultFor(*state.operation);
                if (!state.checked || !state.checked->value ||
                    plan.slots[*state.checked->value].symbol !=
                        contract->value ||
                    plan.slots[state.checked->error].symbol != contract->error)
                    fail(
                        "checked bindings lost their conditional "
                        "initialization contract");
            } else {
                const auto& contract =
                    *model.AwaitResultFor(*state.awaitOperation);
                if (!state.checked ||
                    state.checked->value.has_value() !=
                        (contract.completed.Kind() != SemanticTypeKind::Void) ||
                    (contract.forwardsFailure
                         ? state.checked->error != plan.executionError
                         : plan.slots[state.checked->error].symbol !=
                               contract.destination))
                    fail("await lost its void or failure-forwarding contract");
            }
            if (state.checked && state.kind == AsyncStateKind::ChildAwait &&
                (!state.checked->consumedChild || state.checked->callee))
                fail(
                    "child await must consume a handle on either terminal "
                    "outcome");
            if (state.checked && state.kind != AsyncStateKind::ChildAwait &&
                (!state.checked->callee || !state.checked->callee->IsAsync()))
                fail("async call lacks resolved callee");
            for (const auto [edgeKind, targetKind] :
                 {std::pair{AsyncEdgeKind::Success,
                            AsyncStateKind::CheckedSuccess},
                  std::pair{AsyncEdgeKind::Failure,
                            AsyncStateKind::CheckedFailure}}) {
                const auto edge = std::ranges::find_if(
                    state.edges, [&](auto e) { return e.kind == edgeKind; });
                if (edge == state.edges.end() ||
                    plan.states[edge->target].kind != targetKind ||
                    plan.states[edge->target].operation != state.operation ||
                    plan.states[edge->target].awaitOperation !=
                        state.awaitOperation)
                    fail("operation has invalid checked continuation");
            }
        }
        if (state.kind == AsyncStateKind::DrainChildren) {
            const auto edge = std::ranges::find_if(state.edges, [](auto e) {
                return e.kind == AsyncEdgeKind::Retired;
            });
            if (edge == state.edges.end() ||
                (plan.states[edge->target].kind !=
                     AsyncStateKind::CleanupScope &&
                 plan.states[edge->target].kind != AsyncStateKind::RunDefers) ||
                plan.states[edge->target].cleanupScope != state.cleanupScope)
                fail("drain must retire before scope cleanup");
        }
    }
    // Independently trace each lexical exit. Every scope with children must
    // retire them before deferred calls or owner destruction; a saved return
    // cannot skip this chain. Pending edges are deliberately not followed.
    for (const auto& exit : plan.flow.taskScopeExits) {
        if (exit.source >= plan.states.size() ||
            exit.source >= plan.flow.nodes.size()) {
            fail("invalid cleanup source");
            continue;
        }
        std::vector<std::pair<AsyncStateKind, AsyncScopeId>> sequence;
        for (const auto* syntax : exit.scopes) {
            const auto scope =
                std::ranges::find_if(plan.scopes, [&](const auto& candidate) {
                    return candidate.syntax == syntax;
                });
            if (scope == plan.scopes.end()) {
                fail("cleanup lost a lexical scope");
                continue;
            }
            const auto sid =
                static_cast<AsyncScopeId>(scope - plan.scopes.begin());
            if (scope->children) {
                if (exit.kind == TaskScopeExitKind::CancelAndDrain)
                    sequence.emplace_back(AsyncStateKind::CancelChildren, sid);
                sequence.emplace_back(AsyncStateKind::DrainChildren, sid);
            }
            if (scope->deferredStack)
                sequence.emplace_back(AsyncStateKind::RunDefers, sid);
            sequence.emplace_back(AsyncStateKind::CleanupScope, sid);
        }
        if (sequence.empty()) continue;
        const auto& source = plan.states[exit.source];
        auto start = std::ranges::find_if(source.edges, [&](auto edge) {
            const auto& state = plan.states[edge.target];
            return state.kind == sequence.front().first &&
                   state.cleanupScope == sequence.front().second;
        });
        if (start == source.edges.end()) {
            fail("scope exit bypasses cleanup");
            continue;
        }
        auto target = start->target;
        bool valid = true;
        for (const auto [kind, scope] : sequence) {
            const auto& state = plan.states[target];
            if (state.kind != kind || state.cleanupScope != scope) {
                valid = false;
                break;
            }
            std::vector<AsyncEdge> terminal;
            for (const auto& edge : state.edges)
                if (edge.kind != AsyncEdgeKind::Pending)
                    terminal.push_back(edge);
            if (terminal.size() != 1) {
                valid = false;
                break;
            }
            target = terminal.front().target;
        }
        if (!valid ||
            (exit.successor ? target != *exit.successor
                            : plan.states[target].kind !=
                                  (plan.flow.nodes[exit.source].failureExit
                                       ? AsyncStateKind::Failed
                                       : AsyncStateKind::Complete)))
            fail("scope exit has incorrect cleanup order or destination");
    }
    // Reconstructing the structural contract catches omitted exits, forged
    // outcome writes, early cleanup, lost return saves and changed scope order.
    // Dataflow above independently checks safety of the supplied state graph.
    if (plan.function) {
        const auto expected = BuildAsyncFramePlan(*plan.function, model);
        if (plan.entry != expected.entry ||
            plan.returnValue != expected.returnValue ||
            plan.executionError != expected.executionError ||
            plan.flow != expected.flow || plan.slots != expected.slots ||
            plan.scopes != expected.scopes || plan.states != expected.states)
            fail(
                "plan differs from validated control-flow and storage "
                "contract");
    }
    return errors;
}

std::string DumpAsyncFramePlan(const AsyncFramePlan& plan) {
    std::ostringstream out;
    out << "async-plan "
        << (plan.function ? plan.function->Name() : "<invalid>") << " entry=s"
        << plan.entry << " conservative-stable-storage\n";
    for (AsyncScopeId id = 0; id < plan.scopes.size(); ++id) {
        const auto& scope = plan.scopes[id];
        out << "scope b" << id;
        if (scope.parent) out << " parent=b" << *scope.parent;
        if (scope.children) out << " children=f" << *scope.children;
        if (scope.deferredStack) out << " defers=f" << *scope.deferredStack;
        out << '\n';
    }
    for (AsyncSlotId id = 0; id < plan.slots.size(); ++id) {
        const auto& slot = plan.slots[id];
        out << "slot f" << id << ' ' << Name(slot.kind);
        if (slot.symbol) out << ' ' << slot.symbol->Name();
        if (!slot.type.IsError()) out << ':' << slot.type.Name();
        if (slot.scope) out << " scope=b" << *slot.scope;
        out << " stable=" << slot.stableAddress << " guarded=" << slot.guarded
            << '\n';
    }
    for (AsyncStateId id = 0; id < plan.states.size(); ++id) {
        const auto& state = plan.states[id];
        out << "state s" << id << ' ' << Name(state.kind);
        if (state.flowNode) out << " flow=" << *state.flowNode;
        if (state.operation)
            out << " operation@" << state.operation->Range().start.offset;
        if (state.awaitOperation)
            out << " await@" << state.awaitOperation->Range().start.offset
                << " forward=" << state.awaitOperation->ForwardsFailure();
        if (state.cleanupScope) out << " cleanup=b" << *state.cleanupScope;
        if (state.storage) out << " storage=f" << *state.storage;
        if (state.read) out << " read=f" << *state.read;
        if (state.checked) {
            if (state.checked->value)
                out << " value=f" << *state.checked->value;
            else
                out << " value=void";
            out << " error=f" << state.checked->error
                << " transactional-create="
                << state.checked->transactionalCreation;
            if (state.checked->callee)
                out << " callee=" << state.checked->callee->Name();
            if (state.checked->consumedChild)
                out << " consume=f" << *state.checked->consumedChild;
            for (const auto* argument : state.checked->arguments)
                out << " argument@" << argument->Range().start.offset;
        }

        auto list = [&](const char* label, const auto& values) {
            out << ' ' << label << "=[";
            bool first = true;
            for (auto value : values) {
                if (!first) out << ',';
                out << static_cast<std::size_t>(value);
                first = false;
            }
            out << ']';
        };
        list("scopes", state.scopes);
        list("reset", state.resets);
        list("write", state.writes);
        list("move", state.moves);
        list("destroy-if-initialized", state.destroys);
        list("live", state.live);
        list("retain", state.retained);
        list("init", state.initialization);
        for (auto edge : state.edges)
            out << ' ' << Name(edge.kind) << "=s" << edge.target;
        out << '\n';
    }
    return out.str();
}
}  // namespace flecha::semantic
