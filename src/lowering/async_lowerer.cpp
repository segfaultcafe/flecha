#include "lowering/async_lowerer.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

#include "core/ast_walk.hpp"

namespace flecha::lowering {
using namespace semantic;
using Kind = AsyncInstructionKind;

std::optional<AsyncMachine> LowerAsyncMachine(
    const AsyncFramePlan& plan, const SemanticModel& model,
    const diagnostics::DiagnosticBag& diagnostics, std::string& error) {
    error.clear();
    for (const auto& diagnostic : diagnostics.Diagnostics())
        if (diagnostic.severity == diagnostics::DiagnosticSeverity::Error &&
            diagnostic.code !=
                diagnostics::DiagnosticCode::AsyncNotImplemented) {
            error =
                "async control lowering requires successful semantic analysis";
            return std::nullopt;
        }
    if (!plan.function || model.AsyncPlanFor(*plan.function) != &plan) {
        error = "async control lowering requires the model's published plan";
        return std::nullopt;
    }
    const auto errors = VerifyAsyncFramePlan(plan, model);
    if (!errors.empty()) {
        error = errors.front();
        return std::nullopt;
    }
    AsyncMachine machine;
    machine._entry = plan.entry;
    machine._slotCount = plan.slots.size();
    for (std::size_t slot = 0; slot < plan.slots.size(); ++slot)
        if (plan.slots[slot].kind == AsyncSlotKind::Parameter)
            machine._parameters.push_back(slot);
    machine._instructions.resize(plan.states.size());
    std::set<const core::AstNode*> prepared;
    std::set<const core::NameExpression*> argumentMoves;
    for (const auto& state : plan.states) {
        if (!state.checked) continue;
        // Async operand evaluation belongs to Initialize, not to postorder
        // analysis events on an outcome path (including allocation failure).
        const core::Expression* operand = nullptr;
        if (state.operation) {
            operand = &state.operation->Value();
            while (operand->Kind() ==
                   core::AstNodeKind::ParenthesizedExpression)
                operand =
                    &static_cast<const core::ParenthesizedExpression*>(operand)
                         ->Inner();
            if (operand->Kind() == core::AstNodeKind::SpawnExpression)
                operand = &static_cast<const core::SpawnExpression*>(operand)
                               ->Operand();
            else if (operand->Kind() == core::AstNodeKind::AwaitExpression)
                operand = &static_cast<const core::AwaitExpression*>(operand)
                               ->Operand();
        } else if (state.awaitOperation)
            operand = &state.awaitOperation->Operand();
        if (operand)
            core::WalkAst(*operand,
                          [&](const core::AstNode& node, bool entering) {
                              if (entering) prepared.insert(&node);
                          });
        for (const auto* argument : state.checked->arguments)
            core::WalkAst(
                *argument, [&](const core::AstNode& node, bool entering) {
                    if (!entering) return;
                    prepared.insert(&node);
                    if (node.Kind() == core::AstNodeKind::NameExpression &&
                        model.TransferOf(static_cast<const core::Expression&>(
                            node)) == TransferKind::Move)
                        argumentMoves.insert(
                            static_cast<const core::NameExpression*>(&node));
                });
    }
    std::map<AsyncSlotId, std::size_t> operations;
    for (std::size_t id = 0; id < plan.states.size(); ++id) {
        const auto& source = plan.states[id];
        auto& instruction = machine._instructions[id];
        instruction.sourceState = id;
        instruction.scope = source.cleanupScope;
        instruction.resets = source.resets;
        instruction.writes = source.writes;
        instruction.moves = source.moves;
        instruction.destroys = source.destroys;
        if (source.flowNode) {
            const auto& node = plan.flow.nodes[*source.flowNode];
            instruction.flowAction =
                (node.memoryExpression &&
                 !prepared.contains(node.memoryExpression)) ||
                node.memoryDeclaration || node.memoryDestructure ||
                node.memoryDefer || node.memoryReturn || node.memoryBoolean ||
                node.memoryUntrackedStore || node.memoryDiscard ||
                node.condition || node.propagationDispatch ||
                node.propagationReturn;
            if (node.move)
                for (auto predecessor : node.predecessors)
                    if (argumentMoves.contains(
                            plan.flow.nodes[predecessor].read)) {
                        instruction.moves.clear();
                        break;
                    }
        }
        if (source.checked)
            for (const auto* argument : source.checked->arguments)
                core::WalkAst(*argument, [&](const core::AstNode& node,
                                             bool entering) {
                    if (!entering ||
                        node.Kind() != core::AstNodeKind::NameExpression ||
                        !argumentMoves.contains(
                            static_cast<const core::NameExpression*>(&node)))
                        return;
                    const auto* symbol = model.SymbolFor(
                        static_cast<const core::NameExpression&>(node));
                    for (std::size_t slot = 0; slot < plan.slots.size(); ++slot)
                        if (plan.slots[slot].symbol == symbol)
                            instruction.argumentMoves.push_back(slot);
                });
        for (const auto& edge : source.edges)
            if (edge.kind != AsyncEdgeKind::Pending)
                instruction.successors.push_back(edge.target);
        switch (source.kind) {
            case AsyncStateKind::CreateChild:
                instruction.kind = Kind::CreateChild;
                instruction.reservation = true;
                break;
            case AsyncStateKind::DirectAwait:
            case AsyncStateKind::ChildAwait: {
                instruction.kind = Kind::Await;
                instruction.reservation =
                    source.kind == AsyncStateKind::DirectAwait;
                const auto [position, inserted] =
                    operations.emplace(*source.storage, operations.size());
                instruction.operation = position->second;
                break;
            }
            case AsyncStateKind::CancelChildren:
                instruction.kind = Kind::CancelChildren;
                instruction.activation = source.storage;
                break;
            case AsyncStateKind::DrainChildren:
                instruction.kind = Kind::DrainChildren;
                instruction.activation = source.storage;
                break;
            case AsyncStateKind::RunDefers:
                instruction.kind = Kind::RunDefers;
                instruction.activation = source.storage;
                break;
            case AsyncStateKind::CleanupScope:
                instruction.kind = Kind::CleanupScope;
                break;
            case AsyncStateKind::Complete:
                instruction.kind = Kind::Complete;
                break;
            case AsyncStateKind::Failed:
                instruction.kind = Kind::Failed;
                break;
            default:
                instruction.kind = instruction.successors.size() == 2
                                       ? Kind::Branch
                                       : Kind::Flow;
                break;
        }
        const bool checked = instruction.kind == Kind::CreateChild ||
                             instruction.kind == Kind::Await ||
                             instruction.kind == Kind::Branch;
        const bool terminal = instruction.kind == Kind::Complete ||
                              instruction.kind == Kind::Failed;
        if (instruction.kind == Kind::Branch && !instruction.flowAction) {
            error = "unsupported async branch without a bound condition";
            return std::nullopt;
        }
        if (source.flowNode && plan.flow.nodes[*source.flowNode].condition &&
            prepared.contains(plan.flow.nodes[*source.flowNode].condition)) {
            error = "async argument control flow requires value lowering";
            return std::nullopt;
        }
        if (instruction.successors.size() != (terminal  ? 0u
                                              : checked ? 2u
                                                        : 1u)) {
            // Unreachable CFG nodes can have no successors. They never enter
            // emitted execution and are represented by an unreachable terminal.
            if (source.flowNode &&
                !plan.flow.nodes[*source.flowNode].reachable &&
                instruction.successors.empty())
                instruction.kind = Kind::Cancelled;
            else {
                error = "unsupported async continuation arity";
                return std::nullopt;
            }
        }
    }
    machine._operationCount = operations.size();
    auto append = [&](AsyncInstruction instruction) {
        const auto id = machine._instructions.size();
        machine._instructions.push_back(std::move(instruction));
        return id;
    };
    AsyncInstruction cancelled;
    cancelled.kind = Kind::Cancelled;
    if (plan.returnValue) cancelled.resets.push_back(*plan.returnValue);
    if (plan.returnValue && plan.slots[*plan.returnValue].type.OwnsStorage())
        cancelled.destroys.push_back(*plan.returnValue);
    const auto cancelledId = append(std::move(cancelled));
    // Intern cleanup suffixes by active lexical scope. Cancellation never
    // constructs a result and never treats requesting cancellation as
    // retirement.
    std::map<std::vector<AsyncScopeId>, std::size_t> cleanupChains;
    auto cancellationFor = [&](const std::vector<AsyncScopeId>& scopes) {
        if (const auto found = cleanupChains.find(scopes);
            found != cleanupChains.end())
            return found->second;
        auto next = cancelledId;
        for (auto scope : scopes) {
            AsyncInstruction cleanup;
            cleanup.kind = Kind::CleanupScope;
            cleanup.scope = scope;
            cleanup.successors = {next};
            for (std::size_t slot = 0; slot < plan.slots.size(); ++slot)
                if (plan.slots[slot].scope == scope)
                    cleanup.resets.push_back(slot);
            for (std::size_t slot = plan.slots.size(); slot-- > 0;) {
                const auto& value = plan.slots[slot];
                if (value.scope == scope && value.type.OwnsStorage() &&
                    (value.symbol || value.kind == AsyncSlotKind::AwaitValue) &&
                    !plan.flow.deferredCaptures.contains(value.symbol))
                    cleanup.destroys.push_back(slot);
            }
            next = append(std::move(cleanup));
            if (plan.scopes[scope].deferredStack) {
                AsyncInstruction deferred;
                deferred.kind = Kind::RunDefers;
                deferred.activation = plan.scopes[scope].deferredStack;
                deferred.scope = scope;
                deferred.successors = {next};
                next = append(std::move(deferred));
            }
            if (plan.scopes[scope].children) {
                AsyncInstruction drain;
                drain.kind = Kind::DrainChildren;
                drain.activation = plan.scopes[scope].children;
                drain.scope = scope;
                drain.successors = {next};
                next = append(std::move(drain));
                AsyncInstruction cancel;
                cancel.kind = Kind::CancelChildren;
                cancel.activation = plan.scopes[scope].children;
                cancel.scope = scope;
                cancel.successors = {next};
                next = append(std::move(cancel));
            }
        }
        cleanupChains.emplace(scopes, next);
        return next;
    };
    machine._initialCancellation =
        cancellationFor(plan.states[plan.entry].scopes);
    for (std::size_t id = 0; id < plan.states.size(); ++id) {
        const auto kind = machine._instructions[id].kind;
        if (kind != Kind::Await && kind != Kind::DrainChildren) continue;
        auto next = cancellationFor(plan.states[id].scopes);
        if (kind == Kind::Await) {
            AsyncInstruction retire;
            retire.kind = Kind::CancelOperation;
            retire.sourceState = id;
            retire.operation = machine._instructions[id].operation;
            retire.successors = {next};
            next = append(std::move(retire));
        }
        machine._instructions[id].cancellation = next;
    }
    return machine;
}

std::string DumpAsyncMachine(const AsyncMachine& machine) {
    static constexpr const char* NAMES[] = {
        "flow",     "branch",           "create",
        "await",    "cancel-operation", "cancel-children",
        "drain",    "defers",           "cleanup",
        "complete", "failed",           "cancelled"};
    std::ostringstream output;
    output << "async-machine entry=" << machine.Entry()
           << " slots=" << machine.SlotCount()
           << " operations=" << machine.OperationCount() << '\n';
    for (std::size_t id = 0; id < machine.Instructions().size(); ++id) {
        const auto& instruction = machine.Instructions()[id];
        output << id << ' ' << NAMES[static_cast<unsigned>(instruction.kind)];
        if (instruction.sourceState)
            output << " source=" << *instruction.sourceState;
        if (instruction.scope) output << " scope=" << *instruction.scope;
        if (instruction.operation)
            output << " operation=" << *instruction.operation;
        if (instruction.reservation) output << " reserve-before-initialize";
        for (auto next : instruction.successors) output << " ->" << next;
        if (instruction.cancellation)
            output << " cancel->" << *instruction.cancellation;
        output << '\n';
    }
    return output.str();
}
}  // namespace flecha::lowering
