#include <gtest/gtest.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory_resource>
#include <ranges>
#include <stdexcept>
#include <vector>

#include "codegen/async_value_generator.hpp"
#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "lowering/lowerer.hpp"
#include "runtime/async_executor.hpp"
#include "semantic/semantic_analyzer.hpp"

namespace {
using namespace flecha;
using namespace flecha::codegen;
using namespace flecha::runtime::async;

llvm::DataLayout HostLayout() {
    static const bool initialized = [] {
        return !llvm::InitializeNativeTarget() &&
               !llvm::InitializeNativeTargetAsmPrinter();
    }();
    if (!initialized) throw std::runtime_error("native target initialization");
    const std::string tripleName = llvm::sys::getDefaultTargetTriple();
    const llvm::Triple triple(tripleName);
    std::string error;
#if LLVM_VERSION_MAJOR >= 21
    const llvm::Target* target =
        llvm::TargetRegistry::lookupTarget(triple, error);
#else
    const llvm::Target* target =
        llvm::TargetRegistry::lookupTarget(tripleName, error);
#endif
    if (!target) throw std::runtime_error(error);
    llvm::TargetOptions options;
#if LLVM_VERSION_MAJOR >= 21
    std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
        triple, "generic", "", options, llvm::Reloc::PIC_));
#else
    std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
        tripleName, "generic", "", options, llvm::Reloc::PIC_));
#endif
    return machine->createDataLayout();
}

void PublishRuntimeSymbols() {
    static const bool published = [] {
        using llvm::sys::DynamicLibrary;
        DynamicLibrary::AddSymbol(
            "flecha_async_computation_construct",
            reinterpret_cast<void*>(flecha_async_computation_construct));
        DynamicLibrary::AddSymbol(
            "flecha_async_computation_destruct",
            reinterpret_cast<void*>(flecha_async_computation_destruct));
        DynamicLibrary::AddSymbol(
            "flecha_async_computation_reserve",
            reinterpret_cast<void*>(flecha_async_computation_reserve));
        DynamicLibrary::AddSymbol(
            "flecha_async_computation_initialize",
            reinterpret_cast<void*>(flecha_async_computation_initialize));
        DynamicLibrary::AddSymbol(
            "flecha_async_computation_poll",
            reinterpret_cast<void*>(flecha_async_computation_poll));
        DynamicLibrary::AddSymbol(
            "flecha_async_computation_cancel",
            reinterpret_cast<void*>(flecha_async_computation_cancel));
        DynamicLibrary::AddSymbol(
            "flecha_async_computation_take",
            reinterpret_cast<void*>(flecha_async_computation_take));
        DynamicLibrary::AddSymbol(
            "flecha_async_computation_release",
            reinterpret_cast<void*>(flecha_async_computation_release));
        DynamicLibrary::AddSymbol(
            "flecha_async_context_cancelled",
            reinterpret_cast<void*>(flecha_async_context_cancelled));
        DynamicLibrary::AddSymbol(
            "flecha_async_context_ready",
            reinterpret_cast<void*>(flecha_async_context_ready));
        DynamicLibrary::AddSymbol(
            "flecha_async_child_scope_construct",
            reinterpret_cast<void*>(flecha_async_child_scope_construct));
        DynamicLibrary::AddSymbol(
            "flecha_async_child_scope_destruct",
            reinterpret_cast<void*>(flecha_async_child_scope_destruct));
        DynamicLibrary::AddSymbol(
            "flecha_async_child_reserve",
            reinterpret_cast<void*>(flecha_async_child_reserve));
        DynamicLibrary::AddSymbol(
            "flecha_async_child_initialize",
            reinterpret_cast<void*>(flecha_async_child_initialize));
        DynamicLibrary::AddSymbol(
            "flecha_async_child_join_initialize",
            reinterpret_cast<void*>(flecha_async_child_join_initialize));
        DynamicLibrary::AddSymbol(
            "flecha_async_child_join_poll",
            reinterpret_cast<void*>(flecha_async_child_join_poll));
        DynamicLibrary::AddSymbol(
            "flecha_async_child_join_take",
            reinterpret_cast<void*>(flecha_async_child_join_take));
        DynamicLibrary::AddSymbol(
            "flecha_async_child_join_release",
            reinterpret_cast<void*>(flecha_async_child_join_release));
        DynamicLibrary::AddSymbol(
            "flecha_async_child_scope_cancel",
            reinterpret_cast<void*>(flecha_async_child_scope_cancel));
        DynamicLibrary::AddSymbol(
            "flecha_async_child_scope_drain",
            reinterpret_cast<void*>(flecha_async_child_scope_drain));
        DynamicLibrary::AddSymbol(
            "flecha_async_defer_scope_construct",
            reinterpret_cast<void*>(flecha_async_defer_scope_construct));
        DynamicLibrary::AddSymbol(
            "flecha_async_defer_scope_destruct",
            reinterpret_cast<void*>(flecha_async_defer_scope_destruct));
        DynamicLibrary::AddSymbol(
            "flecha_async_defer_reserve",
            reinterpret_cast<void*>(flecha_async_defer_reserve));
        DynamicLibrary::AddSymbol(
            "flecha_async_defer_initialize",
            reinterpret_cast<void*>(flecha_async_defer_initialize));
        DynamicLibrary::AddSymbol(
            "flecha_async_defer_pop",
            reinterpret_cast<void*>(flecha_async_defer_pop));
        DynamicLibrary::AddSymbol(
            "flecha_async_defer_tag",
            reinterpret_cast<void*>(flecha_async_defer_tag));
        DynamicLibrary::AddSymbol(
            "flecha_async_defer_release",
            reinterpret_cast<void*>(flecha_async_defer_release));
        return true;
    }();
    (void)published;
}

struct Compiled final {
    diagnostics::DiagnosticBag diagnostics;
    std::unique_ptr<core::CompilationUnit> unit;
    semantic::SemanticModel model;
    llvm::LLVMContext context;
    llvm::DataLayout dataLayout = HostLayout();
    GeneratedAsyncProgram program;
    GeneratedAsyncFunction main;
    std::unique_ptr<llvm::ExecutionEngine> engine;
    FrameLayout layout{};

    explicit Compiled(const std::string& source) {
        core::Tokenizer tokenizer(source, diagnostics);
        core::Parser parser(tokenizer.Tokenize(), diagnostics);
        unit = parser.Parse();
        semantic::SemanticAnalyzer analyzer(diagnostics);
        model = analyzer.Analyze(*unit);
        std::string error;
        auto generated = GenerateAsyncValuesForTesting(
            context, dataLayout, model, diagnostics, "source_async", error);
        if (!generated) {
            for (const auto& diagnostic : diagnostics.Diagnostics())
                error += "\n" + diagnostic.message;
            throw std::runtime_error(error);
        }
        program = std::move(*generated);
        const auto found = std::ranges::find_if(
            program.functions, [](const GeneratedAsyncFunction& function) {
                return function.symbol->Name() == "main";
            });
        if (found == program.functions.end())
            throw std::runtime_error("missing generated async main");
        main = *found;
        PublishRuntimeSymbols();
        llvm::EngineBuilder builder(std::move(program.module));
        builder.setEngineKind(llvm::EngineKind::JIT);
        builder.setErrorStr(&error);
        builder.setMCJITMemoryManager(
            std::make_unique<llvm::SectionMemoryManager>());
        engine.reset(builder.create());
        if (!engine) throw std::runtime_error(error);
        engine->finalizeObject();
        auto address = [&](const std::string& suffix) {
            return engine->getFunctionAddress(main.name + suffix);
        };
        layout = {
            main.frame.size,
            main.frame.alignment,
            {reinterpret_cast<PollResult (*)(
                 void*, const PollContext&) noexcept>(address("_frame_poll")),
             reinterpret_cast<void (*)(void*) noexcept>(address("_destroy")),
             main.symbol->ReturnType().Kind() ==
                     semantic::SemanticTypeKind::Void
                 ? nullptr
                 : reinterpret_cast<void (*)(void*, void*) noexcept>(
                       address("_take"))},
            nullptr,
            reinterpret_cast<void (*)(void*, void*,
                                      std::pmr::memory_resource*) noexcept>(
                address("_initialize"))};
    }

    template <typename T>
    std::vector<std::byte> Arguments(T value) const {
        std::vector<std::byte> result(main.argumentSize);
        EXPECT_EQ(main.argumentOffsets.size(), 1u);
        std::memcpy(result.data() + main.argumentOffsets[0], &value, sizeof(T));
        return result;
    }
};

std::string AsyncFixture(const std::string& name) {
    std::ifstream input(std::string(FLECHA_ASYNC_FIXTURE_DIR) + "/" + name);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

class FailingResource final : public std::pmr::memory_resource {
   public:
    int failAt = -1;
    int attempts = 0;
    int live = 0;

   private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (++attempts == failAt) throw std::bad_alloc();
        ++live;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes,
                       std::size_t alignment) override {
        --live;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const memory_resource& other) const noexcept override {
        return this == &other;
    }
};

int observations = 0;
extern "C" std::int32_t Observe(std::int32_t value) {
    ++observations;
    return value;
}

std::vector<std::int32_t> asyncEvents;
extern "C" std::int32_t RecordAsyncEvent(std::int32_t value) {
    asyncEvents.push_back(value);
    return value;
}

void RunUntilTerminal(Executor& executor, const TaskHandle& task,
                      unsigned limit = 100) {
    while (executor.Status(task) == TaskStatus::Pending && limit-- != 0)
        EXPECT_NE(executor.Step(), StepResult::Idle);
    EXPECT_NE(executor.Status(task), TaskStatus::Pending);
}

struct ExternalEvent final {
    void* root = nullptr;
    void* pendingFrame = nullptr;
    void* resumedFrame = nullptr;
    bool ready = false;
    unsigned waits = 0;
    WakeHandle wake;
};
bool ExternalReady(void* storage, void* frame,
                   const WakeHandle& wake) noexcept {
    auto& event = *static_cast<ExternalEvent*>(storage);
    if (!event.root) {
        event.root = frame;
        return true;
    }
    if (frame == event.root) return true;
    if (event.ready) {
        event.resumedFrame = frame;
        return true;
    }
    event.pendingFrame = frame;
    ++event.waits;
    event.wake = wake;
    return false;
}

struct SelectiveEvent final {
    void* root = nullptr;
    void* readyChild = nullptr;
    void* pendingChild = nullptr;
    WakeHandle wake;
};
bool FirstChildReady(void* storage, void* frame,
                     const WakeHandle& wake) noexcept {
    auto& event = *static_cast<SelectiveEvent*>(storage);
    if (!event.root) {
        event.root = frame;
        return true;
    }
    if (frame == event.root) return true;
    if (!event.readyChild) event.readyChild = frame;
    if (frame == event.readyChild) return true;
    event.pendingChild = frame;
    event.wake = wake;
    return false;
}

struct NestedPendingEvent final {
    void* root = nullptr;
    void* child = nullptr;
    void* pending = nullptr;
    WakeHandle wake;
};
bool WaitInsideChild(void* storage, void* frame,
                     const WakeHandle& wake) noexcept {
    auto& event = *static_cast<NestedPendingEvent*>(storage);
    if (!event.root) {
        event.root = frame;
        return true;
    }
    if (frame == event.root) return true;
    if (!event.child) {
        event.child = frame;
        return true;
    }
    if (frame == event.child) return true;
    event.pending = frame;
    event.wake = wake;
    return false;
}

TEST(AsyncValueLoweringTests,
     ExecutesSourceBranchesLoopsEarlyReturnsAndRepeatedDirectAwaits) {
    Compiled code(
        "async i32 leaf(i32 value) { return value + 1; } "
        "async i32 main(i32 value) { total := 0; while value > 0 { "
        "if value == 2 { value -= 1; proceed; } "
        "next := leaf(value).await?; total += next; value -= 1; } "
        "if total > 10 { return total; } return 0; }");
    auto arguments = code.Arguments<std::int32_t>(4);
    Computation computation;
    ASSERT_TRUE(Computation::TryCreate(code.layout, arguments.data(),
                                       std::pmr::get_default_resource(),
                                       computation));
    EXPECT_EQ(computation.Poll({{}, false}), PollResult::Ready);
    std::int32_t result = 0;
    EXPECT_TRUE(computation.TakeResult(&result));
    EXPECT_EQ(result, 11);
    EXPECT_TRUE(computation.Release());
}

TEST(AsyncValueLoweringTests,
     RecursionUsesIndirectFramesAndAllocationFailureSkipsArgumentEffects) {
    llvm::sys::DynamicLibrary::AddSymbol("observe",
                                         reinterpret_cast<void*>(Observe));
    Compiled code(
        "extern i32 observe(i32 value); "
        "async i32 main(i32 depth) { if depth == 0 { return 42; } "
        "next := depth - 1; value := main(observe(next)).await?; "
        "return value + 1; }");
    EXPECT_GT(code.main.frame.size, sizeof(Computation));
    FailingResource resource;
    resource.failAt = 2;
    observations = 0;
    auto arguments = code.Arguments<std::int32_t>(4);
    Computation failed;
    ASSERT_TRUE(Computation::TryCreate(code.layout, arguments.data(), &resource,
                                       failed));
    EXPECT_EQ(failed.Poll({{}, false}), PollResult::AllocationFailed);
    EXPECT_EQ(observations, 0);
    EXPECT_TRUE(failed.Release());
    EXPECT_EQ(resource.live, 0);

    resource.failAt = -1;
    resource.attempts = 0;
    Computation ready;
    ASSERT_TRUE(Computation::TryCreate(code.layout, arguments.data(), &resource,
                                       ready));
    EXPECT_EQ(ready.Poll({{}, false}), PollResult::Ready);
    std::int32_t result = 0;
    EXPECT_TRUE(ready.TakeResult(&result));
    EXPECT_EQ(result, 46);
    EXPECT_EQ(observations, 4);
    EXPECT_TRUE(ready.Release());
    EXPECT_EQ(resource.live, 0);
}

TEST(AsyncValueLoweringTests,
     CheckedReservationFailureRunsNoArgumentsAndConstructsNoSuccessValue) {
    llvm::sys::DynamicLibrary::AddSymbol("observe",
                                         reinterpret_cast<void*>(Observe));
    Compiled code(
        "extern i32 observe(i32 value); "
        "async i32 leaf(i32 value) { return value; } "
        "async i32 main() { value, failure := leaf(observe(42)).await; "
        "if failure != none { return 7; } return value; }");
    for (int failAt : {2, -1}) {
        FailingResource resource;
        resource.failAt = failAt;
        observations = 0;
        Computation computation;
        ASSERT_TRUE(Computation::TryCreate(code.layout, nullptr, &resource,
                                           computation));
        EXPECT_EQ(computation.Poll({{}, false}), PollResult::Ready);
        std::int32_t value = 0;
        EXPECT_TRUE(computation.TakeResult(&value));
        EXPECT_EQ(value, failAt == 2 ? 7 : 42);
        EXPECT_EQ(observations, failAt == 2 ? 0 : 1);
        EXPECT_TRUE(computation.Release());
        EXPECT_EQ(resource.live, 0);
    }
    EXPECT_EQ(code.main.frame.size % code.main.frame.alignment, 0u);
    for (const auto offset : code.main.frame.slotOffsets)
        if (offset) EXPECT_LT(*offset, code.main.frame.size);
    for (const std::size_t offset : code.main.frame.operationOffsets) {
        EXPECT_EQ(offset % alignof(Computation), 0u);
        EXPECT_LE(offset + sizeof(Computation), code.main.frame.size);
    }
    for (const AsyncTemporaryLayout& temporary : code.main.frame.temporaries) {
        EXPECT_EQ(temporary.offset % temporary.alignment, 0u);
        EXPECT_LE(temporary.offset + temporary.size, code.main.frame.size);
    }
}

TEST(AsyncValueLoweringTests,
     CheckedVoidAndAggregateOrdinaryErrorResultsUseGeneratedTransfers) {
    {
        Compiled code(
            "async i32 leaf() { return 42; } async i32 main() { "
            "value, failure := leaf().await; "
            "if failure != none { return -1; } return value; }");
        Computation computation;
        ASSERT_TRUE(Computation::TryCreate(code.layout, nullptr,
                                           std::pmr::get_default_resource(),
                                           computation));
        EXPECT_EQ(computation.Poll({{}, false}), PollResult::Ready);
        std::int32_t value = 0;
        EXPECT_TRUE(computation.TakeResult(&value));
        EXPECT_EQ(value, 42);
        EXPECT_TRUE(computation.Release());
    }
    {
        Compiled code(
            "async void leaf() {} async void main() { leaf().await?; }");
        Computation computation;
        ASSERT_TRUE(Computation::TryCreate(code.layout, nullptr,
                                           std::pmr::get_default_resource(),
                                           computation));
        EXPECT_EQ(computation.Poll({{}, false}), PollResult::Ready);
        EXPECT_TRUE(computation.TakeResult(nullptr));
        EXPECT_TRUE(computation.Release());
    }
    {
        struct ErrorValue {
            void* name;
            std::int32_t code;
        };
        struct Aggregate {
            std::int32_t value;
            ErrorValue ordinaryError;
        };
        Compiled code(
            "async (i32, error) leaf() { return (73, none); } "
            "async (i32, error) main() { return leaf().await?; }");
        Computation computation;
        ASSERT_TRUE(Computation::TryCreate(code.layout, nullptr,
                                           std::pmr::get_default_resource(),
                                           computation));
        EXPECT_EQ(computation.Poll({{}, false}), PollResult::Ready);
        Aggregate value{};
        EXPECT_TRUE(computation.TakeResult(&value));
        EXPECT_EQ(value.value, 73);
        EXPECT_EQ(value.ordinaryError.code, 0);
        EXPECT_TRUE(computation.Release());
    }
}

TEST(AsyncValueLoweringTests,
     CancellationBeforeFirstPollRunsNoSourceAndRetiresFrameOnce) {
    llvm::sys::DynamicLibrary::AddSymbol("observe",
                                         reinterpret_cast<void*>(Observe));
    Compiled code(
        "extern i32 observe(i32 value); async i32 main() { "
        "value := observe(42); return value; }");
    observations = 0;
    FailingResource resource;
    Computation computation;
    ASSERT_TRUE(
        Computation::TryCreate(code.layout, nullptr, &resource, computation));
    EXPECT_EQ(computation.Poll({{}, true}), PollResult::Cancelled);
    EXPECT_EQ(observations, 0);
    EXPECT_TRUE(computation.Release());
    EXPECT_TRUE(computation.Release());
    EXPECT_EQ(resource.live, 0);
}

TEST(AsyncValueLoweringTests,
     GeneratedFramesGenuinelySuspendCoalesceWakesAndCancelPendingChildren) {
    Compiled code(
        "async i32 leaf() { return 42; } async i32 main() { "
        "return leaf().await?; }");
    {
        ExternalEvent event;
        Executor executor(std::pmr::get_default_resource(), ExternalReady,
                          &event);
        TaskHandle task;
        ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
        EXPECT_EQ(executor.Step(), StepResult::Polled);
        EXPECT_EQ(executor.Status(task), TaskStatus::Pending);
        EXPECT_EQ(executor.Step(), StepResult::Idle);
        EXPECT_EQ(event.waits, 1u);
        ASSERT_NE(event.pendingFrame, nullptr);
        const WakeHandle stale = event.wake;
        event.wake.Notify();
        event.wake.Notify();
        event.ready = true;
        EXPECT_EQ(executor.Step(), StepResult::Polled);
        EXPECT_EQ(executor.Status(task), TaskStatus::Ready);
        EXPECT_EQ(event.resumedFrame, event.pendingFrame);
        EXPECT_EQ(executor.Step(), StepResult::Idle);
        std::int32_t result = 0;
        EXPECT_TRUE(executor.TakeResult(task, &result));
        EXPECT_EQ(result, 42);
        EXPECT_TRUE(executor.Release(task));
        stale.Notify();
        EXPECT_EQ(executor.Step(), StepResult::Idle);
    }
    {
        ExternalEvent event;
        Executor executor(std::pmr::get_default_resource(), ExternalReady,
                          &event);
        TaskHandle task;
        ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
        EXPECT_EQ(executor.Step(), StepResult::Polled);
        EXPECT_EQ(executor.Status(task), TaskStatus::Pending);
        executor.RequestCancellation(task);
        EXPECT_EQ(executor.Step(), StepResult::Polled);
        EXPECT_EQ(executor.Status(task), TaskStatus::Cancelled);
        std::int32_t untouched = 91;
        EXPECT_FALSE(executor.TakeResult(task, &untouched));
        EXPECT_EQ(untouched, 91);
        EXPECT_TRUE(executor.Release(task));
    }
}

TEST(AsyncValueLoweringTests,
     BorrowedResultsRetainOriginsAndFrameLocalAddressesStayStable) {
    Compiled code(
        "async &const i32 identity(&const i32 value) <- value { return value; "
        "} "
        "async i32 main() { local := 42; "
        "view := identity(&local).await?; return *view; }");
    ExternalEvent event;
    Executor executor(std::pmr::get_default_resource(), ExternalReady, &event);
    TaskHandle task;
    ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    ASSERT_NE(event.pendingFrame, nullptr);
    const void* stableChild = event.pendingFrame;
    event.ready = true;
    event.wake.Notify();
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(event.resumedFrame, stableChild);
    EXPECT_EQ(executor.Status(task), TaskStatus::Ready);
    std::int32_t value = 0;
    EXPECT_TRUE(executor.TakeResult(task, &value));
    EXPECT_EQ(value, 42);
    EXPECT_TRUE(executor.Release(task));
}

struct ErrorAbi final {
    void* name;
    std::int32_t code;
};
unsigned destructions = 0;
unsigned peekObservations = 0;
extern "C" void RecordDestroy(std::int32_t value) {
    EXPECT_EQ(value, 42);
    ++destructions;
}
extern "C" void RecordPeek() { ++peekObservations; }

TEST(AsyncValueLoweringTests,
     OrdinaryTryReturnsCompletedErrorInsteadOfAsyncExecutionFailure) {
    Compiled code(
        "error ordinary(error value) { return value; } "
        "async error main(error value) { try ordinary(value); return none; }");
    for (std::int32_t codeValue : {0, 73}) {
        auto arguments = code.Arguments(ErrorAbi{nullptr, codeValue});
        Computation computation;
        ASSERT_TRUE(Computation::TryCreate(code.layout, arguments.data(),
                                           std::pmr::get_default_resource(),
                                           computation));
        EXPECT_EQ(computation.Poll({{}, false}), PollResult::Ready);
        ErrorAbi result{reinterpret_cast<void*>(1), -1};
        EXPECT_TRUE(computation.TakeResult(&result));
        EXPECT_EQ(result.code, codeValue);
        EXPECT_TRUE(computation.Release());
    }
}

TEST(AsyncValueLoweringTests, TryPeekRunsOnlyOnFailureAndRewritesError) {
    llvm::sys::DynamicLibrary::AddSymbol("record_peek",
                                         reinterpret_cast<void*>(RecordPeek));
    Compiled code(
        "extern void record_peek(); "
        "error ordinary(error value) { return value; } "
        "async error main(error value) { "
        "try ordinary(value) peek err { record_peek(); err = none; }; "
        "return none; }");
    peekObservations = 0;
    for (std::int32_t codeValue : {0, 73}) {
        auto arguments = code.Arguments(ErrorAbi{nullptr, codeValue});
        Computation computation;
        ASSERT_TRUE(Computation::TryCreate(code.layout, arguments.data(),
                                           std::pmr::get_default_resource(),
                                           computation));
        EXPECT_EQ(computation.Poll({{}, false}), PollResult::Ready);
        ErrorAbi result{reinterpret_cast<void*>(1), -1};
        EXPECT_TRUE(computation.TakeResult(&result));
        EXPECT_EQ(result.code, 0);
        EXPECT_TRUE(computation.Release());
    }
    EXPECT_EQ(peekObservations, 1u);
}

TEST(AsyncValueLoweringTests,
     MoveOnlyArgumentsResultsAndCancellationDestroyExactlyOnceAfterRetirement) {
    llvm::sys::DynamicLibrary::AddSymbol(
        "record_destroy", reinterpret_cast<void*>(RecordDestroy));
    Compiled code(
        "extern void record_destroy(i32 value); "
        "structure Owner { i32 value; }; "
        "void Owner::tear(&self) override { record_destroy(self->value); } "
        "async Owner bounce(Owner value) { return value; } "
        "async i32 main() { Owner owner = none; owner.value = 42; "
        "result := bounce(owner).await?; return result.value; }");
    destructions = 0;
    {
        Computation computation;
        ASSERT_TRUE(Computation::TryCreate(code.layout, nullptr,
                                           std::pmr::get_default_resource(),
                                           computation));
        EXPECT_EQ(computation.Poll({{}, false}), PollResult::Ready);
        std::int32_t value = 0;
        EXPECT_TRUE(computation.TakeResult(&value));
        EXPECT_EQ(value, 42);
        EXPECT_TRUE(computation.Release());
        EXPECT_EQ(destructions, 1u);
    }
    destructions = 0;
    {
        ExternalEvent event;
        Executor executor(std::pmr::get_default_resource(), ExternalReady,
                          &event);
        TaskHandle task;
        ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
        EXPECT_EQ(executor.Step(), StepResult::Polled);
        EXPECT_EQ(executor.Status(task), TaskStatus::Pending);
        EXPECT_EQ(destructions, 0u);
        executor.RequestCancellation(task);
        EXPECT_EQ(executor.Step(), StepResult::Polled);
        EXPECT_EQ(executor.Status(task), TaskStatus::Cancelled);
        EXPECT_EQ(destructions, 1u);
        EXPECT_TRUE(executor.Release(task));
        EXPECT_EQ(destructions, 1u);
    }
}

TEST(AsyncValueLoweringTests,
     SourceFixtureQueuesBothSpellingsFifoAndJoinsCheckedAndForwardedValues) {
    llvm::sys::DynamicLibrary::AddSymbol(
        "record_async_event", reinterpret_cast<void*>(RecordAsyncEvent));
    Compiled code(AsyncFixture("scoped_lowering.fl"));
    asyncEvents.clear();
    Executor executor;
    TaskHandle task;
    ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));

    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(executor.Status(task), TaskStatus::Pending);
    EXPECT_TRUE(asyncEvents.empty());
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(asyncEvents, (std::vector<std::int32_t>{4}));
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(asyncEvents, (std::vector<std::int32_t>{4, 2}));
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(executor.Status(task), TaskStatus::Ready);
    std::int32_t result = 0;
    EXPECT_TRUE(executor.TakeResult(task, &result));
    EXPECT_EQ(result, 42);
    EXPECT_TRUE(executor.Release(task));
}

TEST(AsyncValueLoweringTests,
     ChildCreationAllocationFailuresSkipArgumentsAndPreservePartialScopes) {
    llvm::sys::DynamicLibrary::AddSymbol("observe",
                                         reinterpret_cast<void*>(Observe));
    Compiled one(
        "extern i32 observe(i32 value); "
        "async i32 work(i32 value) { return value; } "
        "async i32 main() { child, e := shoot work(observe(42)); "
        "if e != none { return 7; } return child.await?; }");
    for (int boundary = 1; boundary <= 4; ++boundary) {
        FailingResource resource;
        observations = 0;
        {
            Executor executor(&resource);
            TaskHandle task;
            ASSERT_TRUE(executor.TryCreateTask(one.layout, nullptr, task));
            resource.failAt = resource.attempts + boundary;
            RunUntilTerminal(executor, task);
            ASSERT_EQ(executor.Status(task), TaskStatus::Ready);
            std::int32_t result = 0;
            EXPECT_TRUE(executor.TakeResult(task, &result));
            EXPECT_EQ(result, 7);
            EXPECT_EQ(observations, 0);
            EXPECT_TRUE(executor.Release(task));
        }
        EXPECT_EQ(resource.live, 0);
    }

    Compiled partial(
        "extern i32 observe(i32 value); "
        "async i32 work(i32 value) { return observe(value); } "
        "async i32 main() { first, e1 := shoot work(1); "
        "if e1 != none { return 8; } second, e2 := >>- work(observe(2)); "
        "if e2 != none { return 9; } a := first.await?; "
        "b := second.await?; return a + b; }");
    for (int boundary = 5; boundary <= 8; ++boundary) {
        FailingResource resource;
        observations = 0;
        {
            Executor executor(&resource);
            TaskHandle task;
            ASSERT_TRUE(executor.TryCreateTask(partial.layout, nullptr, task));
            resource.failAt = resource.attempts + boundary;
            RunUntilTerminal(executor, task);
            ASSERT_EQ(executor.Status(task), TaskStatus::Ready);
            std::int32_t result = 0;
            EXPECT_TRUE(executor.TakeResult(task, &result));
            EXPECT_EQ(result, 9);
            EXPECT_EQ(observations, 0);
            EXPECT_TRUE(executor.Release(task));
        }
        EXPECT_EQ(resource.live, 0);
    }
}

TEST(AsyncValueLoweringTests,
     RepeatedSourceSiteAndExitingLoopTransfersOwnDistinctChildren) {
    llvm::sys::DynamicLibrary::AddSymbol(
        "record_async_event", reinterpret_cast<void*>(RecordAsyncEvent));
    {
        Compiled code(
            "extern i32 record_async_event(i32 value); "
            "async i32 work(i32 value) { return record_async_event(value); } "
            "async i32 main(i32 count) { total := 0; while count > 0 { "
            "child, e := shoot work(count); if e != none { return -1; } "
            "value := child.await?; total += value; count -= 1; } return "
            "total; }");
        auto arguments = code.Arguments<std::int32_t>(3);
        asyncEvents.clear();
        Executor executor;
        TaskHandle task;
        ASSERT_TRUE(
            executor.TryCreateTask(code.layout, arguments.data(), task));
        RunUntilTerminal(executor, task);
        std::int32_t result = 0;
        EXPECT_TRUE(executor.TakeResult(task, &result));
        EXPECT_EQ(result, 6);
        EXPECT_EQ(asyncEvents, (std::vector<std::int32_t>{3, 2, 1}));
        EXPECT_TRUE(executor.Release(task));
    }
    {
        Compiled code(
            "extern i32 record_async_event(i32 value); "
            "async i32 work(i32 value) { return record_async_event(value); } "
            "async i32 main() { count := 2; while count > 0 { "
            "child, e := shoot work(count); if e != none { return -1; } "
            "if count == 2 { count -= 1; proceed; } break; } return 7; }");
        asyncEvents.clear();
        Executor executor;
        TaskHandle task;
        ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
        RunUntilTerminal(executor, task);
        std::int32_t result = 0;
        EXPECT_TRUE(executor.TakeResult(task, &result));
        EXPECT_EQ(result, 7);
        EXPECT_TRUE(asyncEvents.empty());
        EXPECT_TRUE(executor.Release(task));
    }
}

TEST(AsyncValueLoweringTests,
     PendingChildCancellationDrainsBeforeFrameDestructionAndStaleWakeIsSafe) {
    Compiled code(
        "async i32 work() { return 42; } async i32 main() { "
        "child, e := shoot work(); if e != none { return 0; } "
        "return child.await?; }");
    ExternalEvent event;
    Executor executor(std::pmr::get_default_resource(), ExternalReady, &event);
    TaskHandle task;
    ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    ASSERT_NE(event.pendingFrame, nullptr);
    const WakeHandle stale = event.wake;
    executor.RequestCancellation(task);
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(executor.Status(task), TaskStatus::Pending);
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(executor.Status(task), TaskStatus::Cancelled);
    EXPECT_TRUE(executor.Release(task));
    stale.Notify();
    EXPECT_EQ(executor.Step(), StepResult::Idle);
}

TEST(AsyncValueLoweringTests,
     ChildVoidAndOrdinaryErrorCompletionsRemainDistinctFromExecutionFailure) {
    {
        Compiled code(
            "async void work() {} async i32 main() { child, e := shoot work(); "
            "if e != none { return 0; } child.await?; return 42; }");
        Executor executor;
        TaskHandle task;
        ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
        RunUntilTerminal(executor, task);
        std::int32_t result = 0;
        EXPECT_TRUE(executor.TakeResult(task, &result));
        EXPECT_EQ(result, 42);
        EXPECT_TRUE(executor.Release(task));
    }
    {
        struct ErrorValue {
            void* name;
            std::int32_t code;
        };
        struct Aggregate {
            std::int32_t value;
            ErrorValue ordinaryError;
        };
        Compiled code(
            "async (i32, error) work() { return (73, none); } "
            "async (i32, error) main() { child, e := shoot work(); "
            "if e != none { return (0, none); } return child.await?; }");
        Executor executor;
        TaskHandle task;
        ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
        RunUntilTerminal(executor, task);
        Aggregate result{};
        EXPECT_TRUE(executor.TakeResult(task, &result));
        EXPECT_EQ(result.value, 73);
        EXPECT_EQ(result.ordinaryError.code, 0);
        EXPECT_TRUE(executor.Release(task));
    }
}

TEST(AsyncValueLoweringTests,
     ChildExecutionFailureIsCheckedOrForwardedWithoutConstructingAValue) {
    for (bool forward : {false, true}) {
        Compiled code(
            "async i32 leaf() { return 42; } async i32 work() { "
            "return leaf().await?; } async i32 main() { child, e := shoot "
            "work(); if e != none { return 1; } " +
            std::string(forward ? "return child.await?; }"
                                : "value, ae := child.await; if ae != none { "
                                  "return 7; } return value; }"));
        FailingResource resource;
        Executor executor(&resource);
        TaskHandle task;
        ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
        resource.failAt = resource.attempts + 5;
        RunUntilTerminal(executor, task);
        if (forward) {
            EXPECT_EQ(executor.Status(task), TaskStatus::AllocationFailed);
            std::int32_t untouched = 91;
            EXPECT_FALSE(executor.TakeResult(task, &untouched));
            EXPECT_EQ(untouched, 91);
        } else {
            EXPECT_EQ(executor.Status(task), TaskStatus::Ready);
            std::int32_t result = 0;
            EXPECT_TRUE(executor.TakeResult(task, &result));
            EXPECT_EQ(result, 7);
        }
        EXPECT_TRUE(executor.Release(task));
        task = TaskHandle();
        EXPECT_EQ(resource.live, 0);
    }
}

TEST(AsyncValueLoweringTests,
     ChildMethodsBorrowedResultsAndMoveOnlyValuesPreserveOwnership) {
    llvm::sys::DynamicLibrary::AddSymbol("observe",
                                         reinterpret_cast<void*>(Observe));
    {
        Compiled code(
            "extern i32 observe(i32 value); structure Box { i32 value; }; "
            "async i32 Box::read(&const self, i32 add) { "
            "return self->value + add; } async i32 main() { Box box = none; "
            "box.value = 40; child, e := shoot box.read(observe(2)); "
            "if e != none { return 0; } return child.await?; }");
        observations = 0;
        Executor executor;
        TaskHandle task;
        ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
        RunUntilTerminal(executor, task);
        std::int32_t result = 0;
        EXPECT_TRUE(executor.TakeResult(task, &result));
        EXPECT_EQ(result, 42);
        EXPECT_EQ(observations, 1);
        EXPECT_TRUE(executor.Release(task));
    }
    {
        Compiled code(
            "async &const i32 identity(&const i32 value) <- value { "
            "return value; } async i32 main() { local := 42; child, e := "
            "shoot identity(&local); if e != none { return 0; } "
            "view := child.await?; return *view; }");
        Executor executor;
        TaskHandle task;
        ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
        RunUntilTerminal(executor, task);
        std::int32_t result = 0;
        EXPECT_TRUE(executor.TakeResult(task, &result));
        EXPECT_EQ(result, 42);
        EXPECT_TRUE(executor.Release(task));
    }
    {
        Compiled code(
            "async i32 read(&const i32 value) { return *value; } "
            "async void write(&i32 value) { *value = 42; } "
            "async i32 main() { local := 21; first, e1 := shoot read(&local); "
            "if e1 != none { return 0; } second, e2 := >>- read(&local); "
            "if e2 != none { return 0; } a := first.await?; "
            "b := second.await?; writer, ew := shoot write(&local); "
            "if ew != none { return 0; } writer.await?; "
            "return a + b + local; }");
        Executor executor;
        TaskHandle task;
        ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
        RunUntilTerminal(executor, task);
        std::int32_t result = 0;
        EXPECT_TRUE(executor.TakeResult(task, &result));
        EXPECT_EQ(result, 84);
        EXPECT_TRUE(executor.Release(task));
    }
    llvm::sys::DynamicLibrary::AddSymbol(
        "record_destroy", reinterpret_cast<void*>(RecordDestroy));
    {
        Compiled code(
            "extern void record_destroy(i32 value); structure Owner { i32 "
            "value; }; void Owner::tear(&self) override { "
            "record_destroy(self->value); } async Owner bounce(Owner value) { "
            "return value; } async i32 main() { Owner owner = none; "
            "owner.value = 42; child, e := shoot bounce(owner); "
            "if e != none { return 0; } result := child.await?; "
            "return result.value; }");
        destructions = 0;
        Executor executor;
        TaskHandle task;
        ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
        RunUntilTerminal(executor, task);
        std::int32_t result = 0;
        EXPECT_TRUE(executor.TakeResult(task, &result));
        EXPECT_EQ(result, 42);
        EXPECT_TRUE(executor.Release(task));
        EXPECT_EQ(destructions, 1u);
    }
}

TEST(AsyncValueLoweringTests,
     CancellationDestroysUnconsumedCompletedChildResultExactlyOnce) {
    llvm::sys::DynamicLibrary::AddSymbol(
        "record_destroy", reinterpret_cast<void*>(RecordDestroy));
    Compiled code(
        "extern void record_destroy(i32 value); structure Owner { i32 value; "
        "}; void Owner::tear(&self) override { record_destroy(self->value); } "
        "async Owner make() { Owner owner = none; owner.value = 42; return "
        "owner; } async i32 wait() { return 1; } async i32 main() { "
        "first, e1 := shoot make(); if e1 != none { return 0; } "
        "second, e2 := shoot wait(); if e2 != none { return 0; } "
        "value := second.await?; owner := first.await?; "
        "return owner.value + value; }");
    destructions = 0;
    SelectiveEvent event;
    Executor executor(std::pmr::get_default_resource(), FirstChildReady,
                      &event);
    TaskHandle task;
    ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
    EXPECT_EQ(executor.Step(), StepResult::Polled);  // parent queues both
    EXPECT_EQ(executor.Step(), StepResult::Polled);  // first completes
    EXPECT_EQ(executor.Step(), StepResult::Polled);  // second waits
    ASSERT_NE(event.pendingChild, nullptr);
    EXPECT_EQ(destructions, 0u);
    executor.RequestCancellation(task);
    EXPECT_EQ(executor.Step(), StepResult::Polled);  // parent cancels/drains
    EXPECT_EQ(destructions, 1u);
    EXPECT_EQ(executor.Step(), StepResult::Polled);  // second retires
    EXPECT_EQ(executor.Step(), StepResult::Polled);  // parent finishes drain
    EXPECT_EQ(executor.Status(task), TaskStatus::Cancelled);
    EXPECT_TRUE(executor.Release(task));
    EXPECT_EQ(destructions, 1u);
}

TEST(AsyncValueLoweringTests,
     DynamicDefersCaptureAtRegistrationAndRunLifoAcrossRepeatedSiteVisits) {
    llvm::sys::DynamicLibrary::AddSymbol(
        "record_async_event", reinterpret_cast<void*>(RecordAsyncEvent));
    Compiled code(AsyncFixture("defer_lowering.fl"));
    asyncEvents.clear();
    Executor executor;
    TaskHandle task;
    ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
    RunUntilTerminal(executor, task);
    ASSERT_EQ(executor.Status(task), TaskStatus::Ready);
    std::int32_t result = 0;
    ASSERT_TRUE(executor.TakeResult(task, &result));
    EXPECT_EQ(result, 42);
    EXPECT_EQ(asyncEvents, (std::vector<std::int32_t>{2, 1, 0, 10, 20}));
    EXPECT_TRUE(executor.Release(task));
}

TEST(AsyncValueLoweringTests,
     DeferAllocationFailureSkipsCaptureEffectsAndRunsEarlierCleanup) {
    llvm::sys::DynamicLibrary::AddSymbol("observe",
                                         reinterpret_cast<void*>(Observe));
    llvm::sys::DynamicLibrary::AddSymbol(
        "record_async_event", reinterpret_cast<void*>(RecordAsyncEvent));
    Compiled code(
        "extern i32 observe(i32 value); extern i32 record_async_event(i32 "
        "value); async i32 main() { defer record_async_event(1); defer "
        "record_async_event(observe(2)); return 42; }");
    FailingResource resource;
    observations = 0;
    asyncEvents.clear();
    {
        Executor executor(&resource);
        TaskHandle task;
        ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
        resource.failAt = resource.attempts + 2;
        RunUntilTerminal(executor, task);
        EXPECT_EQ(executor.Status(task), TaskStatus::AllocationFailed);
        EXPECT_EQ(observations, 0);
        EXPECT_EQ(asyncEvents, (std::vector<std::int32_t>{1}));
        EXPECT_TRUE(executor.Release(task));
    }
    EXPECT_EQ(resource.live, 0);
}

TEST(AsyncValueLoweringTests,
     LoopProceedAndFallthroughRunEachActiveDeferExactlyOnce) {
    llvm::sys::DynamicLibrary::AddSymbol(
        "record_async_event", reinterpret_cast<void*>(RecordAsyncEvent));
    Compiled code(
        "extern i32 record_async_event(i32 value); async i32 main() { "
        "value := 0; while value < 3 { defer record_async_event(value); "
        "value++; if value < 3 { proceed; } } return value; }");
    asyncEvents.clear();
    Executor executor;
    TaskHandle task;
    ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
    RunUntilTerminal(executor, task);
    ASSERT_EQ(executor.Status(task), TaskStatus::Ready);
    std::int32_t result = 0;
    ASSERT_TRUE(executor.TakeResult(task, &result));
    EXPECT_EQ(result, 3);
    EXPECT_EQ(asyncEvents, (std::vector<std::int32_t>{0, 1, 2}));
    EXPECT_TRUE(executor.Release(task));
}

TEST(AsyncValueLoweringTests,
     DeferredStructureMethodsRetainReceiversAndSnapshotArguments) {
    llvm::sys::DynamicLibrary::AddSymbol(
        "record_async_event", reinterpret_cast<void*>(RecordAsyncEvent));
    Compiled code(
        "extern i32 record_async_event(i32 value); structure Recorder { i32 "
        "base; }; i32 Recorder::save(&const self, i32 value) { return "
        "record_async_event(self->base + value); } async i32 main() { "
        "Recorder recorder = none; recorder.base = 40; value := 2; defer "
        "recorder.save(value); value = 9; return value; }");
    asyncEvents.clear();
    Executor executor;
    TaskHandle task;
    ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
    RunUntilTerminal(executor, task);
    ASSERT_EQ(executor.Status(task), TaskStatus::Ready);
    std::int32_t result = 0;
    ASSERT_TRUE(executor.TakeResult(task, &result));
    EXPECT_EQ(result, 9);
    EXPECT_EQ(asyncEvents, (std::vector<std::int32_t>{42}));
    EXPECT_TRUE(executor.Release(task));
}

TEST(AsyncValueLoweringTests,
     CancellationRetiresChildrenBeforeRunningNestedAndParentDefers) {
    llvm::sys::DynamicLibrary::AddSymbol(
        "record_async_event", reinterpret_cast<void*>(RecordAsyncEvent));
    Compiled code(
        "extern i32 record_async_event(i32 value); async i32 leaf() { return "
        "1; } async i32 child() { defer record_async_event(2); return "
        "leaf().await?; } async i32 main() { defer record_async_event(1); "
        "task, e := shoot child(); if e != none { return 0; } return "
        "task.await?; }");
    asyncEvents.clear();
    NestedPendingEvent event;
    Executor executor(std::pmr::get_default_resource(), WaitInsideChild,
                      &event);
    TaskHandle task;
    ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
    EXPECT_EQ(executor.Step(), StepResult::Polled);  // parent joins child
    EXPECT_EQ(executor.Step(), StepResult::Polled);  // child waits in leaf
    ASSERT_NE(event.pending, nullptr);
    executor.RequestCancellation(task);
    EXPECT_EQ(executor.Step(), StepResult::Polled);  // parent cancels/drains
    EXPECT_TRUE(asyncEvents.empty());
    EXPECT_EQ(executor.Step(), StepResult::Polled);  // child retires + defer
    EXPECT_EQ(asyncEvents, (std::vector<std::int32_t>{2}));
    EXPECT_EQ(executor.Step(), StepResult::Polled);  // parent drain + defer
    EXPECT_EQ(executor.Status(task), TaskStatus::Cancelled);
    EXPECT_EQ(asyncEvents, (std::vector<std::int32_t>{2, 1}));
    EXPECT_TRUE(executor.Release(task));
}

TEST(AsyncValueLoweringTests,
     MoveOnlyDeferredCapturesTransferOrRemainOwnedOnReservationFailure) {
    llvm::sys::DynamicLibrary::AddSymbol(
        "record_destroy", reinterpret_cast<void*>(RecordDestroy));
    Compiled code(
        "extern void record_destroy(i32 value); structure Owner { i32 value; "
        "}; void Owner::tear(&self) override { record_destroy(self->value); } "
        "void consume_deferred_owner(Owner value) {} async i32 main() { Owner "
        "owner = none; owner.value = 42; defer "
        "consume_deferred_owner(owner); return 7; }");
    {
        destructions = 0;
        Executor executor;
        TaskHandle task;
        ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
        RunUntilTerminal(executor, task);
        ASSERT_EQ(executor.Status(task), TaskStatus::Ready);
        std::int32_t result = 0;
        ASSERT_TRUE(executor.TakeResult(task, &result));
        EXPECT_EQ(result, 7);
        EXPECT_TRUE(executor.Release(task));
        EXPECT_EQ(destructions, 1u);
    }
    {
        FailingResource resource;
        destructions = 0;
        Executor executor(&resource);
        TaskHandle task;
        ASSERT_TRUE(executor.TryCreateTask(code.layout, nullptr, task));
        resource.failAt = resource.attempts + 1;
        RunUntilTerminal(executor, task);
        EXPECT_EQ(executor.Status(task), TaskStatus::AllocationFailed);
        EXPECT_TRUE(executor.Release(task));
        EXPECT_EQ(destructions, 1u);
    }
}

TEST(AsyncValueLoweringTests,
     RejectsUnsupportedIndirectDeferredDispatchWithoutDroppingTheCall) {
    diagnostics::DiagnosticBag diagnostics;
    core::Tokenizer tokenizer(
        "void cleanup() {} async void main() { action := cleanup; defer "
        "action(); }",
        diagnostics);
    core::Parser parser(tokenizer.Tokenize(), diagnostics);
    auto unit = parser.Parse();
    semantic::SemanticAnalyzer analyzer(diagnostics);
    auto model = analyzer.Analyze(*unit);
    llvm::LLVMContext context;
    std::string error;
    EXPECT_FALSE(GenerateAsyncValuesForTesting(
        context, HostLayout(), model, diagnostics, "unsupported_async", error));
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("unsupported"), std::string::npos);
}

TEST(AsyncValueLoweringTests, KeepsPublicAndSynchronousExecutionGatesClosed) {
    diagnostics::DiagnosticBag diagnostics;
    core::Tokenizer tokenizer("async i32 main() { return 1; }", diagnostics);
    core::Parser parser(tokenizer.Tokenize(), diagnostics);
    auto unit = parser.Parse();
    semantic::SemanticAnalyzer analyzer(diagnostics);
    auto model = analyzer.Analyze(*unit);
    EXPECT_TRUE(std::ranges::any_of(
        diagnostics.Diagnostics(), [](const diagnostics::Diagnostic& value) {
            return value.code ==
                   diagnostics::DiagnosticCode::AsyncNotImplemented;
        }));
    lowering::Lowerer lowerer;
    EXPECT_FALSE(lowerer.Lower(*unit, model, diagnostics));
}

}  // namespace
