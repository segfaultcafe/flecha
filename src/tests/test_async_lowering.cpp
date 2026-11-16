#include <gtest/gtest.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/Support/TargetSelect.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <memory_resource>
#include <stdexcept>

#include "codegen/async_code_generator.hpp"
#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "lowering/async_lowerer.hpp"
#include "lowering/lowerer.hpp"
#include "runtime/async_executor.hpp"
#include "semantic/semantic_analyzer.hpp"

namespace {
using namespace flecha;
using namespace flecha::semantic;
using namespace flecha::codegen;
using namespace flecha::runtime::async;
using Action = AsyncAction;
using Kind = lowering::AsyncInstructionKind;

// Tests execute LLVM-generated control functions. These deliberately supplied
// value/operation bindings are not a source interpreter or a public compiler
// escape hatch. Ordinary async value code generation is a subsequent stage.
struct Compiled final {
    diagnostics::DiagnosticBag diagnostics;
    std::unique_ptr<core::CompilationUnit> unit;
    SemanticModel model;
    const AsyncFramePlan* plan = nullptr;
    std::optional<lowering::AsyncMachine> machine;
    llvm::LLVMContext context;
    std::unique_ptr<llvm::ExecutionEngine> engine;
    AsyncInitializeFunction initialize;
    AsyncPollFunction poll;

    explicit Compiled(const std::string& source) {
        core::Tokenizer tokenizer(source, diagnostics);
        core::Parser parser(tokenizer.Tokenize(), diagnostics);
        unit = parser.Parse();
        semantic::SemanticAnalyzer analyzer(diagnostics);
        model = analyzer.Analyze(*unit);
        for (const auto& declaration : unit->Declarations())
            if (declaration->Kind() == core::AstNodeKind::FunctionDeclaration) {
                const auto& function =
                    static_cast<const core::FunctionDeclaration&>(*declaration);
                if (function.Name() == "main")
                    plan = model.AsyncPlanFor(function);
            }
        if (!plan) {
            std::string message = "no async plan:";
            for (const auto& diagnostic : diagnostics.Diagnostics())
                message += "\n" + diagnostic.message;
            throw std::runtime_error(message);
        }
        std::string error;
        machine = lowering::LowerAsyncMachine(*plan, model, diagnostics, error);
        if (!machine) throw std::runtime_error(error);
        static const bool initialized = [] {
            return !llvm::InitializeNativeTarget() &&
                   !llvm::InitializeNativeTargetAsmPrinter();
        }();
        if (!initialized)
            throw std::runtime_error(
                "native LLVM target initialization failed");
        auto module = GenerateAsyncMachine(context, *machine, "test_async");
        llvm::EngineBuilder builder(std::move(module));
        builder.setEngineKind(llvm::EngineKind::JIT);
        builder.setErrorStr(&error);
        builder.setMCJITMemoryManager(
            std::make_unique<llvm::SectionMemoryManager>());
        engine.reset(builder.create());
        if (!engine) throw std::runtime_error(error);
        engine->finalizeObject();
        initialize = reinterpret_cast<AsyncInitializeFunction>(
            engine->getFunctionAddress("test_async_initialize"));
        poll = reinterpret_cast<AsyncPollFunction>(
            engine->getFunctionAddress("test_async_poll"));
        if (!initialize || !poll)
            throw std::runtime_error("missing generated control functions");
    }
    std::size_t Slot(const std::string& name) const {
        for (std::size_t id = 0; id < plan->slots.size(); ++id)
            if (plan->slots[id].symbol &&
                plan->slots[id].symbol->Name() == name)
                return id;
        throw std::runtime_error("missing slot " + name);
    }
};
struct Record {
    Action action;
    std::size_t index;
    std::uint8_t outcome;
};
struct Frame final {
    Compiled& code;
    AsyncControl control{};
    std::vector<std::uint8_t> slots, operations;
    std::vector<Record> trace;
    std::function<std::uint8_t(Action, std::size_t, std::uint8_t)> binding;
    bool ready = false, retired = false, reserve = true;
    unsigned destructions = 0;
    WakeHandle wake;
    explicit Frame(Compiled& code)
        : code(code),
          slots(code.machine->SlotCount(), 91),
          operations(code.machine->OperationCount(), 91) {
        trace.reserve(4096);
        code.initialize(&control, slots.data(), operations.data());
    }
    unsigned Count(Action action) const {
        return std::ranges::count_if(
            trace, [&](const auto& record) { return record.action == action; });
    }
    static std::uint8_t Execute(void* storage, const void* context,
                                std::uint8_t action, std::uint64_t index,
                                std::uint8_t outcome) noexcept {
        auto& frame = *static_cast<Frame*>(storage);
        const auto kind = static_cast<Action>(action);
        frame.trace.push_back({kind, index, outcome});
        if (context)
            frame.wake = static_cast<const PollContext*>(context)->wake;
        if (frame.binding) return frame.binding(kind, index, outcome);
        switch (kind) {
            case Action::Reserve:
                return frame.reserve;
            case Action::Poll:
                return frame.ready ? 1 : 0;
            case Action::CancelOperation:
                return frame.retired ? 2 : 0;
            case Action::DrainChildren:
                return 1;
            default:
                return 1;
        }
    }
    PollResult Poll(bool cancellation = false) {
        const PollContext context{{}, cancellation};
        return static_cast<PollResult>(
            code.poll(&control, this, &context, cancellation, Execute));
    }
    static PollResult PollScheduled(void* storage,
                                    const PollContext& context) noexcept {
        auto& frame = *static_cast<Frame*>(storage);
        return static_cast<PollResult>(
            frame.code.poll(&frame.control, storage, &context,
                            context.cancellationRequested, Execute));
    }
    static void Destroy(void* storage) noexcept {
        ++static_cast<Frame*>(storage)->destructions;
    }
};
const std::string DIRECT =
    "async i32 work() { return 42; } async i32 main() { return work().await?; "
    "}";

TEST(AsyncLoweringTests,
     LowersOnlyPublishedValidatedPlansAndPreservesPublicGate) {
    Compiled code(DIRECT);
    EXPECT_NE(lowering::DumpAsyncMachine(*code.machine)
                  .find("reserve-before-initialize"),
              std::string::npos);
    auto copy = *code.plan;
    std::string error;
    EXPECT_FALSE(
        lowering::LowerAsyncMachine(copy, code.model, code.diagnostics, error));
    EXPECT_NE(error.find("published"), std::string::npos);
    diagnostics::DiagnosticBag invalid;
    invalid.Report(diagnostics::DiagnosticCode::InvalidAsync,
                   code.plan->function->Range(), "unvalidated");
    EXPECT_FALSE(
        lowering::LowerAsyncMachine(*code.plan, code.model, invalid, error));
    lowering::Lowerer lowerer;
    EXPECT_FALSE(lowerer.Lower(*code.unit, code.model, code.diagnostics));
    diagnostics::DiagnosticBag empty;
    EXPECT_FALSE(lowerer.Lower(*code.unit, code.model, empty));
    auto again = lowering::LowerAsyncMachine(*code.plan, code.model,
                                             code.diagnostics, error);
    ASSERT_TRUE(again);
    EXPECT_EQ(lowering::DumpAsyncMachine(*code.machine),
              lowering::DumpAsyncMachine(*again));
}

TEST(AsyncLoweringTests,
     InitializesOnlyParameterGuardsAndExecutesNativeReadyContinuation) {
    Compiled code("async i32 main(i32 value) { return value; }");
    Frame frame(code);
    EXPECT_EQ(frame.slots[code.Slot("value")], 1);
    EXPECT_EQ(std::ranges::count(frame.slots, 1), 1);
    EXPECT_EQ(frame.Poll(), PollResult::Ready);
    EXPECT_EQ(frame.slots[code.Slot("value")], 0);
    EXPECT_EQ(frame.slots[*code.plan->returnValue], 1);
    const auto calls = frame.trace.size();
    EXPECT_EQ(frame.Poll(true), PollResult::Ready);
    EXPECT_EQ(frame.trace.size(), calls);
}

TEST(AsyncLoweringTests,
     PendingResumesWithoutRepeatingReservationOrArgumentEffects) {
    Compiled code(DIRECT);
    Frame frame(code);
    EXPECT_EQ(frame.Poll(), PollResult::Pending);
    const auto continuation = frame.control.continuation;
    EXPECT_EQ(frame.Poll(), PollResult::Pending);
    EXPECT_EQ(frame.control.continuation, continuation);
    EXPECT_EQ(frame.Count(Action::Reserve), 1u);
    EXPECT_EQ(frame.Count(Action::Initialize), 1u);
    EXPECT_EQ(frame.Count(Action::Poll), 2u);
    EXPECT_EQ(frame.Count(Action::Finish), 0u);
    EXPECT_EQ(frame.slots[*code.plan->returnValue], 0);
    frame.ready = true;
    EXPECT_EQ(frame.Poll(), PollResult::Ready);
    EXPECT_EQ(frame.Count(Action::Finish), 1u);
    EXPECT_EQ(std::ranges::count(frame.operations, 1), 0);
}

TEST(AsyncLoweringTests, AllocationFailureSkipsArgumentsAndCompletedValue) {
    Compiled code(DIRECT);
    Frame frame(code);
    frame.reserve = false;
    EXPECT_EQ(frame.Poll(), PollResult::AllocationFailed);
    EXPECT_EQ(frame.Count(Action::Reserve), 1u);
    EXPECT_EQ(frame.Count(Action::Initialize), 0u);
    EXPECT_EQ(frame.Count(Action::Poll), 0u);
    EXPECT_EQ(frame.Count(Action::Finish), 1u);
    EXPECT_EQ(frame.slots[*code.plan->returnValue], 0);
    EXPECT_EQ(frame.slots[*code.plan->executionError], 1);
    const auto finish = std::ranges::find_if(
        frame.trace, [](const auto& r) { return r.action == Action::Finish; });
    EXPECT_EQ(finish->outcome, 3);
}

TEST(AsyncLoweringTests, ExecutionFailureDoesNotConstructCompletedValue) {
    Compiled code(DIRECT);
    Frame frame(code);
    frame.binding = [](Action action, std::size_t,
                       std::uint8_t) -> std::uint8_t {
        return action == Action::Poll ? 3 : 1;
    };
    EXPECT_EQ(frame.Poll(), PollResult::AllocationFailed);
    EXPECT_EQ(frame.Count(Action::Initialize), 1u);
    EXPECT_EQ(frame.Count(Action::Finish), 1u);
    EXPECT_EQ(frame.slots[*code.plan->returnValue], 0);
}

TEST(AsyncLoweringTests, VoidForwardingHasNoCompletedStorage) {
    Compiled code("async void work() {} async void main() { work().await?; }");
    EXPECT_FALSE(code.plan->returnValue);
    Frame frame(code);
    EXPECT_EQ(frame.Poll(), PollResult::Pending);
    frame.ready = true;
    EXPECT_EQ(frame.Poll(), PollResult::Ready);
    EXPECT_EQ(frame.Count(Action::Finish), 1u);
}

TEST(AsyncLoweringTests, ReadyOrdinaryErrorPayloadIsNotExecutionFailure) {
    Compiled code(
        "async error work() { return none; } async error main() { return "
        "work().await?; }");
    Frame frame(code);
    frame.ready = true;
    EXPECT_EQ(frame.Poll(), PollResult::Ready);
    EXPECT_EQ(frame.slots[*code.plan->returnValue], 1);
    EXPECT_EQ(frame.slots[*code.plan->executionError], 0);
}

TEST(AsyncLoweringTests,
     BothShootSpellingsReserveBeforePublishingAndJoinWithoutReservation) {
    for (const std::string spelling : {"shoot", ">>-"}) {
        Compiled code(
            "async i32 work() { return 42; } async i32 main() { child, e := " +
            spelling +
            " work(); if e != none { return 1; } return child.await?; }");
        Frame frame(code);
        frame.ready = true;
        EXPECT_EQ(frame.Poll(), PollResult::Ready);
        EXPECT_EQ(frame.Count(Action::Reserve), 1u);
        EXPECT_EQ(frame.Count(Action::Initialize), 2u);
        EXPECT_EQ(frame.Count(Action::Poll), 1u);
        EXPECT_EQ(frame.Count(Action::Finish), 2u);
        EXPECT_EQ(frame.slots[code.Slot("child")], 0);
        Frame failed(code);
        failed.reserve = false;
        EXPECT_EQ(failed.Poll(),
                  PollResult::Ready);  // explicit creation-error return
        EXPECT_EQ(failed.Count(Action::Initialize), 0u);
        EXPECT_EQ(failed.Count(Action::Poll), 0u);
        EXPECT_EQ(failed.slots[code.Slot("child")], 0);
    }
}

TEST(AsyncLoweringTests, CancellationWaitsForOperationRetirementBeforeCleanup) {
    Compiled code(
        "void cleanup() {} async i32 work() { return 1; } "
        "async i32 main() { defer cleanup(); return work().await?; }");
    Frame frame(code);
    EXPECT_EQ(frame.Poll(), PollResult::Pending);
    EXPECT_EQ(frame.Poll(true), PollResult::Pending);
    EXPECT_EQ(frame.Count(Action::RunDefers), 0u);
    EXPECT_EQ(frame.Count(Action::RetireOperation), 0u);
    frame.retired = true;
    // A cancellation already in flight is sticky even if a later caller
    // provides a context with cancellation=false.
    EXPECT_EQ(frame.Poll(), PollResult::Cancelled);
    EXPECT_EQ(frame.Count(Action::RunDefers), 1u);
    EXPECT_EQ(frame.Count(Action::RetireOperation), 1u);
    EXPECT_EQ(frame.Count(Action::Finish), 0u);
    EXPECT_EQ(frame.slots[*code.plan->returnValue], 0);
    const auto calls = frame.trace.size();
    EXPECT_EQ(frame.Poll(true), PollResult::Cancelled);
    EXPECT_EQ(frame.trace.size(), calls);
}

TEST(AsyncLoweringTests,
     CancellationBeforeFirstExecutionDoesNotReserveOrEvaluate) {
    Compiled code(DIRECT);
    Frame frame(code);
    EXPECT_EQ(frame.Poll(true), PollResult::Cancelled);
    EXPECT_EQ(frame.Count(Action::Flow), 0u);
    EXPECT_EQ(frame.Count(Action::Reserve), 0u);
    EXPECT_EQ(frame.Count(Action::Initialize), 0u);
    EXPECT_EQ(frame.Count(Action::Finish), 0u);
}

TEST(AsyncLoweringTests,
     ScopeDrainSuspendsWithoutRepeatingDefersOrDestruction) {
    Compiled code(
        "void cleanup() {} async i32 work() { return 1; } "
        "async i32 main() { defer cleanup(); child, e := shoot work(); "
        "if e != none { return 0; } return 7; }");
    Frame frame(code);
    bool drained = false;
    frame.binding = [&](Action action, std::size_t,
                        std::uint8_t) -> std::uint8_t {
        return action == Action::DrainChildren ? drained : 1;
    };
    EXPECT_EQ(frame.Poll(), PollResult::Pending);
    EXPECT_EQ(frame.Poll(), PollResult::Pending);
    EXPECT_EQ(frame.Count(Action::CancelChildren), 1u);
    EXPECT_EQ(frame.Count(Action::RunDefers), 0u);
    EXPECT_EQ(frame.slots[*code.plan->returnValue], 1);
    drained = true;
    EXPECT_EQ(frame.Poll(), PollResult::Ready);
    EXPECT_EQ(frame.Count(Action::RunDefers), 1u);
}

TEST(AsyncLoweringTests, GeneratedFrameRunsOnExecutorAndCoalescesWakeups) {
    Compiled code(DIRECT);
    Frame frame(code);
    Executor executor;
    TaskHandle task;
    ASSERT_TRUE(executor.TrySchedule(
        &frame, {Frame::PollScheduled, Frame::Destroy}, task));
    EXPECT_EQ(frame.Count(Action::Flow), 0u);
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(executor.Status(task), TaskStatus::Pending);
    EXPECT_EQ(executor.Step(), StepResult::Idle);
    frame.ready = true;
    const WakeHandle stale = frame.wake;
    frame.wake.Notify();
    frame.wake.Notify();
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(executor.Status(task), TaskStatus::Ready);
    EXPECT_EQ(executor.Step(), StepResult::Idle);
    EXPECT_TRUE(executor.Release(task));
    EXPECT_EQ(frame.destructions, 1u);
    stale.Notify();
    EXPECT_EQ(executor.Step(), StepResult::Idle);
}

TEST(AsyncLoweringTests,
     ExecutorCancellationDrivesGeneratedRetirementContinuation) {
    Compiled code(DIRECT);
    Frame frame(code);
    Executor executor;
    TaskHandle task;
    ASSERT_TRUE(executor.TrySchedule(
        &frame, {Frame::PollScheduled, Frame::Destroy}, task));
    ASSERT_EQ(executor.Step(), StepResult::Polled);
    executor.RequestCancellation(task);
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(executor.Status(task), TaskStatus::Pending);
    EXPECT_FALSE(executor.Release(task));
    frame.retired = true;
    frame.wake.Notify();
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(executor.Status(task), TaskStatus::Cancelled);
    EXPECT_TRUE(executor.Release(task));
    EXPECT_EQ(frame.destructions, 1u);
}

TEST(AsyncLoweringTests,
     LoopReusesOperationStorageOnlyAfterConsumingPreviousResult) {
    Compiled code(
        "async i32 work() { return 1; } async void main(bool again) { "
        "while again { v := work().await?; } }");
    Frame frame(code);
    unsigned iterations = 0;
    frame.binding = [&](Action action, std::size_t id,
                        std::uint8_t) -> std::uint8_t {
        if (action == Action::Flow && code.plan->states[id].flowNode &&
            code.plan->flow.nodes[*code.plan->states[id].flowNode].condition)
            return iterations++ < 3;
        return 1;
    };
    EXPECT_EQ(frame.Poll(), PollResult::Ready);
    EXPECT_EQ(frame.Count(Action::Reserve), 3u);
    EXPECT_EQ(frame.Count(Action::Initialize), 3u);
    EXPECT_EQ(frame.Count(Action::Poll), 3u);
    EXPECT_EQ(frame.Count(Action::Finish), 3u);
    EXPECT_EQ(std::ranges::count(frame.operations, 1), 0);
}

TEST(AsyncLoweringTests, NestedCancellationRunsDefersInnerToOuterOnce) {
    Compiled code(
        "void cleanup() {} async i32 work() { return 1; } "
        "async i32 main() { defer cleanup(); { defer cleanup(); "
        "return work().await?; } }");
    Frame frame(code);
    EXPECT_EQ(frame.Poll(), PollResult::Pending);
    const auto slotsAddress = frame.slots.data();
    frame.retired = true;
    EXPECT_EQ(frame.Poll(true), PollResult::Cancelled);
    std::vector<std::size_t> scopes;
    for (const auto& record : frame.trace)
        if (record.action == Action::RunDefers) scopes.push_back(record.index);
    EXPECT_EQ(scopes, (std::vector<std::size_t>{1, 0}));
    EXPECT_EQ(frame.slots.data(), slotsAddress);
    EXPECT_EQ(std::ranges::count(frame.slots, 1), 0);
}

TEST(AsyncLoweringTests, ScopeActionsAreSkippedBeforeActivation) {
    Compiled code(
        "void cleanup() {} async i32 work() { return 1; } "
        "async void main() { defer cleanup(); child, e := shoot work(); "
        "if e != none { return; } child.await?; }");
    Frame frame(code);
    EXPECT_EQ(frame.Poll(true), PollResult::Cancelled);
    EXPECT_EQ(frame.Count(Action::RunDefers), 0u);
    EXPECT_EQ(frame.Count(Action::CancelChildren), 0u);
    EXPECT_EQ(frame.Count(Action::DrainChildren), 0u);
}

TEST(AsyncLoweringTests,
     GuardedOwnershipTransferAndDestructionFollowCreationOutcome) {
    const std::string source =
        "structure Owner { i32 value; }; void Owner::tear(&self) override {} "
        "async Owner consume(Owner value) { return value; } async i32 main() { "
        "Owner owner = none; task, ce := shoot consume(owner); "
        "if ce != none { return owner.value; } result, ae := task.await; "
        "if ae != none { return 0; } return result.value; }";
    Compiled code(source);
    for (bool succeeds : {false, true}) {
        Frame frame(code);
        const auto owner = code.Slot("owner"), result = code.Slot("result");
        std::vector<std::unique_ptr<int>> values(code.machine->SlotCount());
        std::unique_ptr<int> operation;
        unsigned destroyed = 0, initialized = 0;
        frame.binding = [&](Action action, std::size_t id,
                            std::uint8_t outcome) -> std::uint8_t {
            if (action == Action::Flow) {
                const auto& state = code.plan->states[id];
                if (state.flowNode) {
                    const auto& node = code.plan->flow.nodes[*state.flowNode];
                    if (node.memoryDeclaration && node.write &&
                        node.write->Name() == "owner")
                        values[owner] = std::make_unique<int>(42);
                }
            } else if (action == Action::Reserve) {
                EXPECT_NE(values[owner], nullptr);
                return succeeds;
            } else if (action == Action::Initialize &&
                       code.plan->states[id].kind ==
                           AsyncStateKind::CreateChild) {
                operation = std::move(values[owner]);
                ++initialized;
            } else if (action == Action::Finish && outcome == 1 &&
                       code.plan->states[id].kind ==
                           AsyncStateKind::ChildAwait) {
                values[result] = std::move(operation);
            } else if (action == Action::Destroy) {
                EXPECT_NE(values[id], nullptr);
                EXPECT_EQ(id, succeeds ? result : owner);
                values[id].reset();
                ++destroyed;
            }
            return 1;
        };
        EXPECT_EQ(frame.Poll(), PollResult::Ready);
        EXPECT_EQ(initialized, succeeds ? 1u : 0u);
        EXPECT_EQ(destroyed, 1u);
        EXPECT_EQ(values[owner], nullptr);
        EXPECT_EQ(values[result], nullptr);
        EXPECT_EQ(operation, nullptr);
    }
}

TEST(AsyncLoweringTests,
     CancellationDuringDrainDiscardsSavedOwnedReturnExactlyOnce) {
    Compiled code(
        "structure Owner {}; void Owner::tear(&self) override {} "
        "async i32 work() { return 1; } async Owner main() { "
        "Owner owner = none; child, e := shoot work(); "
        "if e != none { return owner; } return owner; }");
    Frame frame(code);
    bool drained = false;
    frame.binding = [&](Action action, std::size_t,
                        std::uint8_t) -> std::uint8_t {
        return action == Action::DrainChildren ? drained : 1;
    };
    EXPECT_EQ(frame.Poll(), PollResult::Pending);
    ASSERT_TRUE(code.plan->returnValue);
    EXPECT_EQ(frame.slots[*code.plan->returnValue], 1);
    EXPECT_EQ(frame.Poll(true), PollResult::Pending);
    EXPECT_EQ(frame.Count(Action::Destroy), 0u);
    drained = true;
    EXPECT_EQ(frame.Poll(true), PollResult::Cancelled);
    EXPECT_EQ(frame.slots[*code.plan->returnValue], 0);
    EXPECT_EQ(std::ranges::count_if(
                  frame.trace,
                  [&](const auto& record) {
                      return record.action == Action::Destroy &&
                             record.index == *code.plan->returnValue;
                  }),
              1);
    EXPECT_EQ(
        std::ranges::count_if(frame.trace,
                              [&](const auto& record) {
                                  return record.action == Action::Destroy &&
                                         record.index == code.Slot("owner");
                              }),
        0);
}

class FailingResource final : public std::pmr::memory_resource {
   public:
    std::size_t allocations = 0, outstanding = 0, failAt = 0;

   private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        if (++allocations == failAt) throw std::bad_alloc();
        void* value =
            std::pmr::new_delete_resource()->allocate(size, alignment);
        ++outstanding;
        return value;
    }
    void do_deallocate(void* value, std::size_t size,
                       std::size_t alignment) override {
        --outstanding;
        std::pmr::new_delete_resource()->deallocate(value, size, alignment);
    }
    bool do_is_equal(const memory_resource& other) const noexcept override {
        return this == &other;
    }
};
struct RecursiveObservation final {
    Compiled& code;
    FailingResource resource;
    unsigned prepared = 0, destroyed = 0, taken = 0;
};
// Fixed storage makes frame initialization allocation-free. Real child frames
// are allocated through Computation; no recursively embedded native layout.
struct RecursiveFrame final {
    RecursiveObservation& observation;
    AsyncControl control{};
    std::array<std::uint8_t, 256> slots{};
    std::array<std::uint8_t, 16> operations{};
    Computation child;
    RecursiveFrame* pendingChild = nullptr;
    int depth = 0, result = 0;
    explicit RecursiveFrame(RecursiveObservation& observation)
        : observation(observation) {
        observation.code.initialize(&control, slots.data(), operations.data());
    }
    struct Arguments {
        RecursiveObservation* observation;
        RecursiveFrame** published;
    };
    static void Initialize(void* storage, void* arguments) noexcept {
        const auto& input = *static_cast<Arguments*>(arguments);
        *input.published = new (storage) RecursiveFrame(*input.observation);
    }
    static void Destroy(void* storage) noexcept {
        auto& frame = *static_cast<RecursiveFrame*>(storage);
        ++frame.observation.destroyed;
        std::destroy_at(&frame);
    }
    static void Take(void* storage, void* destination) noexcept {
        auto& frame = *static_cast<RecursiveFrame*>(storage);
        ++frame.observation.taken;
        *static_cast<int*>(destination) = frame.result;
    }
    static FrameLayout Layout() {
        return {sizeof(RecursiveFrame),
                alignof(RecursiveFrame),
                {Poll, Destroy, Take},
                Initialize};
    }
    static std::uint8_t Execute(void* storage, const void* context,
                                std::uint8_t rawAction, std::uint64_t id,
                                std::uint8_t outcome) noexcept {
        auto& frame = *static_cast<RecursiveFrame*>(storage);
        switch (static_cast<Action>(rawAction)) {
            case Action::Flow: {
                const auto& state = frame.observation.code.plan->states[id];
                if (state.flowNode) {
                    const auto& node = frame.observation.code.plan->flow
                                           .nodes[*state.flowNode];
                    if (node.condition) return frame.depth == 0;
                    if (node.memoryReturn && frame.depth == 0)
                        frame.result = 42;
                }
                return 1;
            }
            case Action::Reserve: {
                Arguments arguments{&frame.observation, &frame.pendingChild};
                return Computation::TryCreate(Layout(), &arguments,
                                              &frame.observation.resource,
                                              frame.child);
            }
            case Action::Initialize:
                frame.pendingChild->depth = frame.depth - 1;
                ++frame.observation.prepared;
                return 1;
            case Action::Poll:
                return static_cast<std::uint8_t>(frame.child.Poll(
                    *static_cast<const PollContext*>(context)));
            case Action::Finish:
                if (outcome == 1)
                    EXPECT_TRUE(frame.child.TakeResult(&frame.result));
                EXPECT_TRUE(frame.child.Release());
                return 1;
            default:
                return 1;
        }
    }
    static PollResult Poll(void* storage, const PollContext& context) noexcept {
        auto& frame = *static_cast<RecursiveFrame*>(storage);
        return static_cast<PollResult>(frame.observation.code.poll(
            &frame.control, storage, &context, context.cancellationRequested,
            Execute));
    }
};

TEST(AsyncLoweringTests,
     NativeContinuationsComposeWithRecursiveComputationFrames) {
    Compiled code(
        "async i32 main(i32 depth) { if depth == 0 { return 42; } "
        "next := depth - 1; return main(next).await?; }");
    ASSERT_LE(code.machine->SlotCount(), 256u);
    ASSERT_LE(code.machine->OperationCount(), 16u);
    for (std::size_t failure : {0u, 1u, 2u, 4u, 7u}) {
        RecursiveObservation observation{code};
        observation.resource.failAt = failure;
        RecursiveFrame* root = nullptr;
        RecursiveFrame::Arguments arguments{&observation, &root};
        Computation computation;
        const bool created =
            Computation::TryCreate(RecursiveFrame::Layout(), &arguments,
                                   &observation.resource, computation);
        if (failure == 1) {
            EXPECT_FALSE(created);
            EXPECT_EQ(root, nullptr);
            EXPECT_EQ(observation.prepared, 0u);
            continue;
        }
        ASSERT_TRUE(created);
        root->depth = 5;
        const auto result = computation.Poll({{}, false});
        EXPECT_EQ(result, failure == 0 || failure > 6
                              ? PollResult::Ready
                              : PollResult::AllocationFailed);
        int value = -1;
        if (result == PollResult::Ready) {
            EXPECT_TRUE(computation.TakeResult(&value));
            EXPECT_EQ(value, 42);
            EXPECT_EQ(observation.prepared, 5u);
            EXPECT_EQ(observation.taken, 6u);
        } else {
            EXPECT_FALSE(computation.TakeResult(&value));
            EXPECT_EQ(value, -1);
            EXPECT_EQ(observation.prepared, failure - 2);
            EXPECT_EQ(observation.taken, 0u);
        }
        EXPECT_TRUE(computation.Release());
        EXPECT_EQ(observation.resource.outstanding, 0u);
        EXPECT_EQ(observation.destroyed,
                  failure == 0 || failure > 6 ? 6u : failure - 1);
    }
}

TEST(AsyncLoweringTests,
     ArgumentOwnershipChangesBeforeSuspensionButNotOnReservationFailure) {
    Compiled code(
        "structure Owner {}; void Owner::tear(&self) override {} "
        "async i32 consume(Owner value) { return 1; } async i32 main() { "
        "Owner owner = none; return consume(owner).await?; }");
    for (bool allocated : {false, true}) {
        Frame frame(code);
        frame.reserve = allocated;
        const auto owner = code.Slot("owner");
        if (allocated) {
            EXPECT_EQ(frame.Poll(), PollResult::Pending);
            EXPECT_EQ(frame.slots[owner], 0);
            frame.retired = true;
            EXPECT_EQ(frame.Poll(true), PollResult::Cancelled);
            EXPECT_EQ(frame.Count(Action::Destroy),
                      0u);  // retired operation owns its argument
        } else {
            EXPECT_EQ(frame.Poll(), PollResult::AllocationFailed);
            EXPECT_EQ(frame.Count(Action::Initialize), 0u);
            EXPECT_EQ(frame.Count(Action::Destroy),
                      1u);  // caller still owns it
        }
    }
}

TEST(AsyncLoweringTests, OrdinaryTryFailureTakesCompletedReturnPath) {
    Compiled code(
        "error work() { return none; } async error main() { try work(); return "
        "none; }");
    Frame frame(code);
    frame.binding = [&](Action action, std::size_t id,
                        std::uint8_t) -> std::uint8_t {
        if (action == Action::Flow && code.plan->states[id].flowNode &&
            code.plan->flow.nodes[*code.plan->states[id].flowNode]
                .propagationDispatch)
            return 0;
        return 1;
    };
    EXPECT_EQ(frame.Poll(), PollResult::Ready);
    EXPECT_TRUE(frame.slots[*code.plan->returnValue]);
    EXPECT_FALSE(code.plan->executionError);
}

TEST(AsyncLoweringTests,
     UnsupportedMatchDispatchIsRejectedRatherThanChoosingAnArm) {
    try {
        Compiled code(
            "enum Pick { A, B } async i32 main(Pick pick) { "
            "return match pick { Pick.A => 1, Pick.B => 2, }; }");
        FAIL() << "match dispatch must have explicit executable selection";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("unsupported async branch"),
                  std::string::npos)
            << error.what();
    }
}

TEST(AsyncLoweringTests, AsyncOperandsAreNotEvaluatedAgainOnOutcomePaths) {
    Compiled code(DIRECT);
    Frame frame(code);
    frame.binding = [&](Action action, std::size_t id,
                        std::uint8_t) -> std::uint8_t {
        if (action == Action::Flow && code.plan->states[id].flowNode) {
            const auto* expression =
                code.plan->flow.nodes[*code.plan->states[id].flowNode]
                    .memoryExpression;
            if (expression)
                EXPECT_NE(expression->Kind(),
                          core::AstNodeKind::CallExpression);
        }
        return 1;
    };
    EXPECT_EQ(frame.Poll(), PollResult::Ready);
    EXPECT_EQ(frame.Count(Action::Initialize), 1u);
}
}  // namespace
