#ifndef FLECHA_CODEGEN_ASYNC_CODE_GENERATOR_HPP
#define FLECHA_CODEGEN_ASYNC_CODE_GENERATOR_HPP

#include <llvm/IR/Module.h>

#include <cstdint>
#include <memory>
#include <string>

#include "lowering/async_lowerer.hpp"

namespace flecha::codegen {
// Private backend ABI. All callbacks are synchronous and nonthrowing; only
// Poll, CancelOperation and DrainChildren may report Pending. They must not
// allocate to report failure or to retire storage. The context is borrowed
// for one invocation; callbacks must copy wake handles, never retain it.
enum class AsyncAction : std::uint8_t {
    Flow,
    Reserve,
    Initialize,
    Poll,
    Finish,
    CancelOperation,
    RetireOperation,
    CancelChildren,
    DrainChildren,
    RunDefers,
    Destroy
};
// Flow returns 0/1 for a branch; Reserve returns 0/1. Poll and CancelOperation
// return the private runtime's PollResult encoding. Drain returns 0 (pending)
// or 1 (retired). All other return values are ignored.
// Index is the original plan state, except scope actions (scope ID), Destroy
// (slot ID), and cancellation of an operation (its original dispatch state).
// Finish receives the terminal PollResult. It consumes/releases the operation
// and constructs a value ONLY for Ready. RetireOperation never publishes one.
// Reserve must reserve ALL resources before Initialize evaluates arguments;
// failed reservation leaves caller storage untouched. Initialize cannot fail.
// Flow handles only the indicated postorder value/branch event. Its inputs
// have already been evaluated; it must not recursively execute the source AST.
// Reserve must not publish a child handle. Initialize transfers arguments and
// may publish/queue a child only after those transfers have completed.
// CancelOperation requests cancellation and polls for retirement even if a
// later supplied context no longer carries cancellationRequested. A direct
// computation binding must supply a cancellation context to its nested poll.
using AsyncActionFunction = std::uint8_t (*)(void* storage, const void* context,
                                             std::uint8_t action,
                                             std::uint64_t index,
                                             std::uint8_t outcome) noexcept;

// Arrays belong to the containing stable frame, with the sizes specified by
// AsyncMachine. They are allocated before Initialize, never while polling.
struct AsyncControl final {
    std::uint64_t continuation;
    std::uint8_t terminal;
    std::uint8_t started;
    std::uint8_t* initialized;
    std::uint8_t* activeOperations;
};
using AsyncInitializeFunction = void (*)(AsyncControl*, std::uint8_t*,
                                         std::uint8_t*) noexcept;
using AsyncPollFunction = std::uint8_t (*)(AsyncControl*, void*, const void*,
                                           std::uint8_t,
                                           AsyncActionFunction) noexcept;

// Emits <name>_initialize and <name>_poll. No CLI entry point, public ABI,
// user-value layout or ordinary expression code generation is enabled here.
std::unique_ptr<llvm::Module> GenerateAsyncMachine(
    llvm::LLVMContext& context, const lowering::AsyncMachine& machine,
    const std::string& name);
}  // namespace flecha::codegen
#endif
