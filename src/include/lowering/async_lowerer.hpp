#ifndef FLECHA_LOWERING_ASYNC_LOWERER_HPP
#define FLECHA_LOWERING_ASYNC_LOWERER_HPP

#include <optional>
#include <string>
#include <vector>

#include "diagnostics/diagnostic_bag.hpp"
#include "semantic/async_plan.hpp"

namespace flecha::lowering {

// Internal control lowering. Values and operation implementations are separate
// backend bindings, not an interpreter for AST expressions. In particular,
// analysis read/write events must never be used to re-evaluate an async call.
enum class AsyncInstructionKind {
    Flow,
    Branch,
    CreateChild,
    Await,
    CancelOperation,
    CancelChildren,
    DrainChildren,
    RunDefers,
    CleanupScope,
    Complete,
    Failed,
    Cancelled
};
struct AsyncInstruction final {
    AsyncInstructionKind kind = AsyncInstructionKind::Flow;
    std::optional<std::size_t> sourceState;
    std::optional<std::size_t> scope;
    std::optional<std::size_t> operation;
    std::optional<std::size_t> activation;
    bool reservation = false;
    bool flowAction = false;
    // Transfers occur once, after reservation, before the first pending poll.
    // Analysis-only postorder move events must not repeat these updates.
    std::vector<std::size_t> argumentMoves;
    std::vector<std::size_t> successors;
    std::vector<std::size_t> resets, writes, moves, destroys;
    // Cancellation is checked before first execution and at suspension/drain
    // boundaries, never between reservation and argument transfer.
    std::optional<std::size_t> cancellation;
};
class AsyncMachine final {
   public:
    const std::vector<AsyncInstruction>& Instructions() const {
        return _instructions;
    }
    const std::vector<std::size_t>& Parameters() const { return _parameters; }
    const std::vector<std::size_t>& OperationSlots() const {
        return _operationSlots;
    }
    std::size_t SlotCount() const { return _slotCount; }
    std::size_t OperationCount() const { return _operationCount; }
    std::size_t Entry() const { return _entry; }
    std::size_t InitialCancellation() const { return _initialCancellation; }

   private:
    AsyncMachine() = default;
    std::vector<AsyncInstruction> _instructions;
    std::vector<std::size_t> _parameters;
    std::vector<std::size_t> _operationSlots;
    std::size_t _slotCount = 0, _operationCount = 0, _entry = 0;
    std::size_t _initialCancellation = 0;
    friend std::optional<AsyncMachine> LowerAsyncMachine(
        const semantic::AsyncFramePlan&, const semantic::SemanticModel&,
        const diagnostics::DiagnosticBag&, std::string&);
};

// Requires the model's published plan and permits only the existing async
// execution diagnostic. Never opens a route through synchronous lowering.
std::optional<AsyncMachine> LowerAsyncMachine(
    const semantic::AsyncFramePlan& plan, const semantic::SemanticModel& model,
    const diagnostics::DiagnosticBag& diagnostics, std::string& error);
std::string DumpAsyncMachine(const AsyncMachine& machine);
}  // namespace flecha::lowering
#endif
