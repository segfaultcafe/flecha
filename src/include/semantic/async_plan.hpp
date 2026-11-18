#ifndef FLECHA_SEMANTIC_ASYNC_PLAN_HPP
#define FLECHA_SEMANTIC_ASYNC_PLAN_HPP

#include <optional>
#include <string>
#include <vector>

#include "control_flow_graph.hpp"

namespace flecha::semantic {
using AsyncSlotId = std::size_t;
using AsyncStateId = std::size_t;
using AsyncScopeId = std::size_t;

enum class AsyncSlotKind {
    Parameter,
    Local,
    DeferredCall,
    DeferredStack,
    Computation,
    ChildJoin,
    TaskScope,
    ReturnValue,
    AwaitValue,
    ExecutionError
};
enum class AsyncStateKind {
    Flow,
    CreateChild,
    DirectAwait,
    ChildAwait,
    CheckedSuccess,
    CheckedFailure,
    CancelChildren,
    DrainChildren,
    RunDefers,
    CleanupScope,
    Complete,
    Failed
};
enum class AsyncEdgeKind { Next, Success, Failure, Pending, Retired };
struct AsyncEdge final {
    AsyncEdgeKind kind;
    AsyncStateId target;
    bool operator==(const AsyncEdge&) const = default;
};

// Logical storage, not a byte layout or a public runtime ABI. Every slot has
// stable storage and an activation/initialization guard. A deferred-call slot
// describes the entire saved call (including non-borrowed temporaries).
// Its scope's deferred stack holds every dynamic registration, including
// repeated visits to the same site; it is not a single overwritten record.
// Computations are indirect stable frames, never recursively embedded layouts.
struct AsyncFrameSlot final {
    AsyncSlotKind kind;
    const Symbol* symbol = nullptr;
    const core::AstNode* syntax = nullptr;
    std::optional<AsyncScopeId> scope;
    SemanticType type;
    bool stableAddress = true;
    bool guarded = true;
    bool operator==(const AsyncFrameSlot&) const = default;
};
struct AsyncScopePlan final {
    const core::BlockStatement* syntax;
    std::optional<AsyncScopeId> parent;
    std::optional<AsyncSlotId> children;
    std::optional<AsyncSlotId> deferredStack;
    bool operator==(const AsyncScopePlan&) const = default;
};

// Prepared operands retain their resolved transfer kinds and borrow contracts
// in the semantic model. The planner never flattens the completed value type.
struct AsyncOperationPlan final {
    const FunctionSymbol* callee = nullptr;  // null for a child join
    std::vector<const core::Expression*> arguments;
    std::optional<AsyncSlotId> value;
    AsyncSlotId error;
    std::optional<AsyncSlotId> consumedChild;
    // Creation reserves frame/scheduler storage before transferring arguments.
    // A direct await owns its callee frame after successful allocation, even
    // if that computation later fails. Failure never constructs a value.
    bool transactionalCreation = true;
    bool operator==(const AsyncOperationPlan&) const = default;
};

// These are analysis continuations, not executable instruction schedules.
// Flow states retain CFG events; checked operation states describe an atomic
// operation with outcome continuations. Pending resumes the same operation,
// using its guarded storage, without re-evaluating arguments or transferring
// them again. Lowering must reserve resources before evaluating arguments.
struct AsyncState final {
    AsyncStateKind kind = AsyncStateKind::Flow;
    std::optional<NodeId> flowNode;
    std::vector<AsyncEdge> edges;
    std::vector<AsyncScopeId> scopes;
    std::optional<AsyncScopeId> cleanupScope;
    const core::DestructureStatement* operation = nullptr;
    const core::AwaitExpression* awaitOperation = nullptr;
    std::optional<AsyncSlotId> storage;
    std::optional<AsyncOperationPlan> checked;
    std::optional<AsyncSlotId> read;
    std::vector<AsyncSlotId> resets, writes, moves;
    // Reverse registration order; each destruction is guarded by the slot's
    // runtime initialization state. Dynamic defers run first, in stack order.
    std::vector<AsyncSlotId> destroys;
    // Ordinary read liveness, separate from conservative lifetime retention.
    std::vector<AsyncSlotId> live;
    // Active lexical storage is retained even without a future direct read:
    // borrows, pending children, deferred calls and destruction can need it.
    std::vector<AsyncSlotId> retained;
    // Incoming alternatives: 1 = absent, 2 = initialized, 4 = moved. These are
    // static possibilities; runtime guards select the actual cleanup action.
    std::vector<unsigned char> initialization;
    bool operator==(const AsyncState&) const = default;
};
struct AsyncFramePlan final {
    const core::FunctionDeclaration* function = nullptr;
    ControlFlowGraph flow;
    std::vector<AsyncFrameSlot> slots;
    std::vector<AsyncScopePlan> scopes;
    std::vector<AsyncState> states;
    AsyncStateId entry = 0;
    std::optional<AsyncSlotId> returnValue;
    std::optional<AsyncSlotId> executionError;
};

// Called only after semantic/borrow validation succeeds.
[[nodiscard]] AsyncFramePlan BuildAsyncFramePlan(
    const core::FunctionDeclaration& function, const SemanticModel& model);
// Structural, dataflow and lifetime verification; does not replace the borrow
// checker or qualify generated code. Empty means the plan is internally valid.
[[nodiscard]] std::vector<std::string> VerifyAsyncFramePlan(
    const AsyncFramePlan& plan, const SemanticModel& model);
[[nodiscard]] std::string DumpAsyncFramePlan(const AsyncFramePlan& plan);
}  // namespace flecha::semantic
#endif
