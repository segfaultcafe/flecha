#include <gtest/gtest.h>

#include <algorithm>
#include <memory_resource>
#include <new>
#include <optional>
#include <vector>

#include "runtime/async_executor.hpp"

using namespace flecha::runtime::async;

namespace {
struct EventFrame final {
    int initialized = 0;
    int polls = 0;
    int destroyed = 0;
    int taken = 0;
    int result = 42;
    bool ready = false;
    bool allocationFailed = false;
    bool retired = false;
    bool wakeDuringPoll = false;
    bool sawCancellation = false;
    WakeHandle callback;
    Executor* executor = nullptr;
    StepResult reentrant = StepResult::Idle;
    std::vector<int>* order = nullptr;
    int id = 0;
};
PollResult PollEvent(void* storage, const PollContext& context) noexcept {
    auto& frame = *static_cast<EventFrame*>(storage);
    ++frame.polls;
    if (frame.order) frame.order->push_back(frame.id);
    frame.callback = context.wake;
    if (frame.executor) frame.reentrant = frame.executor->Step();
    if (frame.wakeDuringPoll) {
        context.wake.Notify();
        context.wake.Notify();
        frame.wakeDuringPoll = false;
    }
    if (context.cancellationRequested) {
        frame.sawCancellation = true;
        return frame.retired ? PollResult::Cancelled : PollResult::Pending;
    }
    if (!frame.ready) return PollResult::Pending;
    return frame.allocationFailed ? PollResult::AllocationFailed
                                  : PollResult::Ready;
}
void DestroyEvent(void* storage) noexcept {
    ++static_cast<EventFrame*>(storage)->destroyed;
}
void TakeEvent(void* storage, void* destination) noexcept {
    auto& frame = *static_cast<EventFrame*>(storage);
    ++frame.taken;
    *static_cast<int*>(destination) = frame.result;
}
constexpr FrameOperations EVENT_OPERATIONS{PollEvent, DestroyEvent, TakeEvent};

struct OwnedArguments final {
    EventFrame* observation;
    std::unique_ptr<int> value;
};
struct alignas(64) OwnedFrame final {
    EventFrame* observation;
    std::unique_ptr<int> value;
    const void* address;
};
void InitializeOwned(void* storage, void* arguments) noexcept {
    auto& input = *static_cast<OwnedArguments*>(arguments);
    new (storage)
        OwnedFrame{input.observation, std::move(input.value), storage};
    ++input.observation->initialized;
}
PollResult PollOwned(void* storage, const PollContext& context) noexcept {
    auto& frame = *static_cast<OwnedFrame*>(storage);
    EXPECT_EQ(frame.address, storage);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(storage) % alignof(OwnedFrame),
              0U);
    return PollEvent(frame.observation, context);
}
void DestroyOwned(void* storage) noexcept {
    auto& frame = *static_cast<OwnedFrame*>(storage);
    ++frame.observation->destroyed;
    std::destroy_at(&frame);
}
void TakeOwned(void* storage, void* destination) noexcept {
    auto& frame = *static_cast<OwnedFrame*>(storage);
    ++frame.observation->taken;
    *static_cast<int*>(destination) = *frame.value;
}
constexpr FrameLayout OWNED_LAYOUT{sizeof(OwnedFrame),
                                   alignof(OwnedFrame),
                                   {PollOwned, DestroyOwned, TakeOwned},
                                   InitializeOwned};

class FailingResource final : public std::pmr::memory_resource {
   public:
    int failAt;
    int attempts = 0;
    int live = 0;
    explicit FailingResource(int failure) : failAt(failure) {}

   private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (attempts++ == failAt) throw std::bad_alloc();
        void* result =
            std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++live;
        return result;
    }
    void do_deallocate(void* pointer, std::size_t bytes,
                       std::size_t alignment) override {
        --live;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct RecursiveObservation final {
    int initialized = 0;
    int destroyed = 0;
    int recovered = 0;
};
struct RecursiveArguments final {
    RecursiveObservation* observation;
    std::pmr::memory_resource* resource;
    int depth;
    int recoverAt = -1;
};
struct RecursiveFrame final {
    RecursiveArguments arguments;
    Computation child;
    int result = 0;
};
const FrameLayout& RecursiveLayout();
void InitializeRecursive(void* storage, void* arguments) noexcept {
    auto& input = *static_cast<RecursiveArguments*>(arguments);
    new (storage) RecursiveFrame{input};
    ++input.observation->initialized;
}
PollResult PollRecursive(void* storage, const PollContext& context) noexcept {
    auto& frame = *static_cast<RecursiveFrame*>(storage);
    if (frame.arguments.depth == 0) {
        frame.result = 42;
        return PollResult::Ready;
    }
    RecursiveArguments childArguments = frame.arguments;
    --childArguments.depth;
    PollResult result = PollResult::AllocationFailed;
    if (frame.child.Status() != TaskStatus::Invalid ||
        Computation::TryCreate(RecursiveLayout(), &childArguments,
                               frame.arguments.resource, frame.child)) {
        result = frame.child.Poll(context);
        if (result == PollResult::Pending) return result;
        if (result == PollResult::Ready) {
            EXPECT_TRUE(frame.child.TakeResult(&frame.result));
            ++frame.result;
        }
        EXPECT_TRUE(frame.child.Release());
    }
    // Explicit caller recovery, rather than automatic error-to-value coercion.
    if (result == PollResult::AllocationFailed &&
        frame.arguments.depth == frame.arguments.recoverAt) {
        ++frame.arguments.observation->recovered;
        frame.result = 100;
        return PollResult::Ready;
    }
    return result;
}
void DestroyRecursive(void* storage) noexcept {
    auto& frame = *static_cast<RecursiveFrame*>(storage);
    ++frame.arguments.observation->destroyed;
    std::destroy_at(&frame);
}
void TakeRecursive(void* storage, void* destination) noexcept {
    *static_cast<int*>(destination) =
        static_cast<RecursiveFrame*>(storage)->result;
}
const FrameLayout& RecursiveLayout() {
    static constexpr FrameLayout layout{
        sizeof(RecursiveFrame),
        alignof(RecursiveFrame),
        {PollRecursive, DestroyRecursive, TakeRecursive},
        InitializeRecursive};
    return layout;
}
}  // namespace

TEST(AsyncRuntimeTests, SchedulesBothChildrenBeforePollingInFifoOrder) {
    std::vector<int> order;
    EventFrame first, second;
    first.ready = second.ready = true;
    first.order = second.order = &order;
    first.id = 1;
    second.id = 2;
    Executor executor;
    TaskHandle a, b;
    ASSERT_TRUE(executor.TrySchedule(&first, EVENT_OPERATIONS, a));
    ASSERT_TRUE(executor.TrySchedule(&second, EVENT_OPERATIONS, b));
    EXPECT_TRUE(order.empty());
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
    EXPECT_EQ(executor.Step(), StepResult::Idle);
    int result = 0;
    EXPECT_TRUE(executor.TakeResult(a, &result));
    EXPECT_EQ(result, 42);
    EXPECT_FALSE(executor.TakeResult(a, &result));
    EXPECT_TRUE(executor.Release(a));
    EXPECT_TRUE(executor.Release(b));
    EXPECT_EQ(first.destroyed, 1);
    EXPECT_EQ(second.destroyed, 1);
    EXPECT_EQ(second.taken, 0);
}

TEST(AsyncRuntimeTests, PendingDoesNotBusyPollAndRepeatedWakesCoalesce) {
    EventFrame frame;
    Executor executor;
    TaskHandle task;
    ASSERT_TRUE(executor.TrySchedule(&frame, EVENT_OPERATIONS, task));
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(executor.Step(), StepResult::Idle);
    EXPECT_EQ(frame.polls, 1);
    frame.callback.Notify();
    frame.callback.Notify();
    frame.ready = true;
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(executor.Step(), StepResult::Idle);
    EXPECT_EQ(frame.polls, 2);
    EXPECT_TRUE(executor.Release(task));
}

TEST(AsyncRuntimeTests, NotificationDuringRegistrationSurvivesPendingReturn) {
    EventFrame frame;
    frame.wakeDuringPoll = true;
    Executor executor;
    TaskHandle task;
    ASSERT_TRUE(executor.TrySchedule(&frame, EVENT_OPERATIONS, task));
    executor.Step();
    EXPECT_EQ(frame.polls, 1);
    frame.ready = true;
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(frame.polls, 2);
    EXPECT_EQ(executor.Step(), StepResult::Idle);
    EXPECT_TRUE(executor.Release(task));
}

TEST(AsyncRuntimeTests, FinalPollNotificationCannotQueueRetiredStorage) {
    EventFrame frame;
    frame.ready = frame.wakeDuringPoll = true;
    Executor executor;
    TaskHandle task;
    ASSERT_TRUE(executor.TrySchedule(&frame, EVENT_OPERATIONS, task));
    executor.Step();
    WakeHandle stale = frame.callback;
    EXPECT_TRUE(executor.Release(task));
    stale.Notify();
    EXPECT_EQ(executor.Step(), StepResult::Idle);
    // Reuse exactly the same frame address; old callbacks cannot wake it.
    frame.ready = false;
    ASSERT_TRUE(executor.TrySchedule(&frame, EVENT_OPERATIONS, task));
    executor.Step();
    stale.Notify();
    EXPECT_EQ(executor.Step(), StepResult::Idle);
    frame.ready = true;
    frame.callback.Notify();
    executor.Step();
    EXPECT_TRUE(executor.Release(task));
}

TEST(AsyncRuntimeTests, CancellationWaitsForOperationRetirement) {
    EventFrame frame;
    Executor executor;
    TaskHandle task;
    ASSERT_TRUE(executor.TrySchedule(&frame, EVENT_OPERATIONS, task));
    executor.Step();
    executor.RequestCancellation(task);
    executor.RequestCancellation(task);
    EXPECT_FALSE(executor.Release(task));
    executor.Step();
    EXPECT_TRUE(frame.sawCancellation);
    EXPECT_EQ(frame.destroyed, 0);
    EXPECT_EQ(executor.Status(task), TaskStatus::Pending);
    EXPECT_EQ(executor.Step(), StepResult::Idle);
    frame.retired = true;
    frame.callback.Notify();
    executor.Step();
    EXPECT_EQ(executor.Status(task), TaskStatus::Cancelled);
    int result = 0;
    EXPECT_FALSE(executor.TakeResult(task, &result));
    EXPECT_TRUE(executor.Release(task));
    EXPECT_EQ(frame.destroyed, 1);
}

TEST(AsyncRuntimeTests, CancellationBeforeStartDoesNotExecuteBody) {
    EventFrame frame;
    Executor executor;
    TaskHandle task;
    ASSERT_TRUE(executor.TrySchedule(&frame, EVENT_OPERATIONS, task));
    executor.RequestCancellation(task);
    executor.Step();
    EXPECT_EQ(frame.polls, 0);
    EXPECT_EQ(executor.Status(task), TaskStatus::Cancelled);
    EXPECT_TRUE(executor.Release(task));
    EXPECT_EQ(frame.destroyed, 1);
}

TEST(AsyncRuntimeTests, PollCannotReenterExecutor) {
    EventFrame frame;
    frame.ready = true;
    Executor executor;
    frame.executor = &executor;
    TaskHandle task;
    ASSERT_TRUE(executor.TrySchedule(&frame, EVENT_OPERATIONS, task));
    executor.Step();
    EXPECT_EQ(frame.reentrant, StepResult::Reentrant);
    EXPECT_EQ(frame.polls, 1);
    EXPECT_TRUE(executor.Release(task));
}

TEST(AsyncRuntimeTests, JoinRegistrationHandlesReadyAndDelayedCompletion) {
    EventFrame parent, child;
    Executor executor;
    TaskHandle a, b;
    ASSERT_TRUE(executor.TrySchedule(&parent, EVENT_OPERATIONS, a));
    ASSERT_TRUE(executor.TrySchedule(&child, EVENT_OPERATIONS, b));
    executor.Step();
    executor.Step();
    EXPECT_FALSE(executor.Watch(b, parent.callback));
    child.ready = true;
    child.callback.Notify();
    executor.Step();
    EXPECT_TRUE(executor.Watch(b, parent.callback));
    parent.ready = true;
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(parent.polls, 2);
    EXPECT_TRUE(executor.Release(b));
    EXPECT_TRUE(executor.Release(a));
}

TEST(AsyncRuntimeTests, StaleWakeOutlivesExecutorSafely) {
    EventFrame frame;
    frame.ready = true;
    {
        Executor executor;
        TaskHandle task;
        ASSERT_TRUE(executor.TrySchedule(&frame, EVENT_OPERATIONS, task));
        executor.Step();
    }
    EXPECT_EQ(frame.destroyed, 1);
    frame.callback.Notify();
}

TEST(AsyncRuntimeTests,
     EachAdmissionAllocationFailurePreservesCallerOwnership) {
    for (int failAt : {0, 1}) {
        FailingResource resource(failAt);
        EventFrame frame;
        {
            Executor executor(&resource);
            TaskHandle task;
            EXPECT_FALSE(executor.TrySchedule(&frame, EVENT_OPERATIONS, task));
            EXPECT_EQ(executor.Status(task), TaskStatus::Invalid);
            EXPECT_EQ(executor.Step(), StepResult::Idle);
            EXPECT_EQ(frame.polls, 0);
            EXPECT_EQ(frame.destroyed, 0);
        }
        EXPECT_EQ(resource.live, 0);
    }
}

TEST(AsyncRuntimeTests,
     FrameAndSchedulerFailureNeverTransferArgumentsOrPublishTasks) {
    for (int failAt : {0, 1, 2}) {
        FailingResource resource(failAt);
        EventFrame observation;
        OwnedArguments arguments{&observation, std::make_unique<int>(42)};
        {
            Executor executor(&resource);
            TaskHandle task;
            EXPECT_FALSE(
                executor.TryCreateTask(OWNED_LAYOUT, &arguments, task));
            ASSERT_NE(arguments.value, nullptr);
            EXPECT_EQ(*arguments.value, 42);
            EXPECT_EQ(observation.initialized, 0);
            EXPECT_EQ(observation.polls, 0);
            EXPECT_EQ(observation.destroyed, 0);
            EXPECT_EQ(executor.Status(task), TaskStatus::Invalid);
            EXPECT_EQ(executor.Step(), StepResult::Idle);
        }
        EXPECT_EQ(resource.live, 0);
    }
}

TEST(AsyncRuntimeTests,
     StableFrameSurvivesSuspensionAndTransfersArgumentsOnce) {
    FailingResource resource(-1);
    EventFrame observation;
    OwnedArguments arguments{&observation, std::make_unique<int>(42)};
    {
        Executor executor(&resource);
        TaskHandle task;
        ASSERT_TRUE(executor.TryCreateTask(OWNED_LAYOUT, &arguments, task));
        EXPECT_EQ(arguments.value, nullptr);
        EXPECT_EQ(observation.initialized, 1);
        EXPECT_EQ(observation.polls, 0);
        executor.Step();
        EXPECT_FALSE(executor.Release(task));
        observation.ready = true;
        observation.callback.Notify();
        executor.Step();
        int value = 0;
        EXPECT_TRUE(executor.TakeResult(task, &value));
        EXPECT_EQ(value, 42);
        EXPECT_FALSE(executor.TakeResult(task, &value));
        EXPECT_TRUE(executor.Release(task));
        EXPECT_EQ(observation.destroyed, 1);
    }
    // Weak handles retain only the scheduler control block, not the frame.
    observation.callback = {};
    EXPECT_EQ(resource.live, 0);
}

TEST(AsyncRuntimeTests, DirectFrameCreationFailureCanBeHandledAndRetried) {
    FailingResource resource(0);
    EventFrame observation;
    OwnedArguments arguments{&observation, std::make_unique<int>(42)};
    Computation computation;
    EXPECT_FALSE(Computation::TryCreate(OWNED_LAYOUT, &arguments, &resource,
                                        computation));
    EXPECT_EQ(computation.Status(), TaskStatus::Invalid);
    EXPECT_EQ(observation.initialized, 0);
    ASSERT_NE(arguments.value, nullptr);
    resource.failAt = -1;
    ASSERT_TRUE(Computation::TryCreate(OWNED_LAYOUT, &arguments, &resource,
                                       computation));
    EXPECT_EQ(arguments.value, nullptr);
    EXPECT_EQ(computation.Poll({{}, false}), PollResult::Pending);
    EXPECT_FALSE(computation.Release());
    observation.ready = true;
    EXPECT_EQ(computation.Poll({{}, false}), PollResult::Ready);
    int result = 0;
    EXPECT_TRUE(computation.TakeResult(&result));
    EXPECT_EQ(result, 42);
    EXPECT_TRUE(computation.Release());
    EXPECT_EQ(resource.live, 0);
}

TEST(AsyncRuntimeTests, ExecutionFailureHasNoCompletedValueAndWakesJoiner) {
    EventFrame parent, child, sibling;
    Executor executor;
    TaskHandle a, b, c;
    ASSERT_TRUE(executor.TrySchedule(&parent, EVENT_OPERATIONS, a));
    ASSERT_TRUE(executor.TrySchedule(&child, EVENT_OPERATIONS, b));
    ASSERT_TRUE(executor.TrySchedule(&sibling, EVENT_OPERATIONS, c));
    executor.Step();
    executor.Step();
    executor.Step();
    EXPECT_FALSE(executor.Watch(b, parent.callback));
    child.ready = child.allocationFailed = child.wakeDuringPoll = true;
    child.callback.Notify();
    executor.Step();
    EXPECT_EQ(executor.Status(b), TaskStatus::AllocationFailed);
    EXPECT_TRUE(executor.Watch(b, parent.callback));
    int result = 123;
    EXPECT_FALSE(executor.TakeResult(b, &result));
    EXPECT_EQ(result, 123);
    EXPECT_EQ(child.taken, 0);
    parent.ready = true;
    EXPECT_EQ(executor.Step(), StepResult::Polled);
    EXPECT_EQ(parent.polls, 2);
    EXPECT_EQ(executor.Status(c), TaskStatus::Pending);
    EXPECT_FALSE(sibling.sawCancellation);
    sibling.ready = true;
    sibling.callback.Notify();
    executor.Step();
    EXPECT_TRUE(executor.Release(a));
    EXPECT_TRUE(executor.Release(b));
    EXPECT_TRUE(executor.Release(c));
    child.callback.Notify();
    EXPECT_EQ(executor.Step(), StepResult::Idle);
}

TEST(AsyncRuntimeTests, UnstartedAllocatedFrameCanRetireWithoutExecutingBody) {
    FailingResource resource(-1);
    EventFrame observation;
    OwnedArguments arguments{&observation, std::make_unique<int>(42)};
    {
        Computation computation;
        ASSERT_TRUE(Computation::TryCreate(OWNED_LAYOUT, &arguments, &resource,
                                           computation));
        EXPECT_EQ(computation.Poll({{}, true}), PollResult::Cancelled);
        EXPECT_EQ(observation.polls, 0);
    }
    EXPECT_EQ(observation.destroyed, 1);
    EXPECT_EQ(resource.live, 0);
}

TEST(AsyncRuntimeTests,
     RecursiveDirectAwaitFailurePropagatesOrRecoversWithoutAnotherAllocation) {
    for (int recoverAt : {-1, 3}) {
        FailingResource resource(3);
        RecursiveObservation observation;
        RecursiveArguments arguments{&observation, &resource, 4, recoverAt};
        Computation computation;
        ASSERT_TRUE(Computation::TryCreate(RecursiveLayout(), &arguments,
                                           &resource, computation));
        EXPECT_EQ(
            computation.Poll({{}, false}),
            recoverAt == -1 ? PollResult::AllocationFailed : PollResult::Ready);
        EXPECT_EQ(resource.attempts, 4);
        EXPECT_EQ(observation.initialized, 3);
        EXPECT_EQ(observation.destroyed, 2);
        int result = 123;
        EXPECT_EQ(computation.TakeResult(&result), recoverAt != -1);
        EXPECT_EQ(result, recoverAt == -1 ? 123 : 101);
        EXPECT_EQ(observation.recovered, recoverAt == -1 ? 0 : 1);
        EXPECT_TRUE(computation.Release());
        EXPECT_EQ(observation.destroyed, 3);
        EXPECT_EQ(resource.live, 0);
    }
}

TEST(AsyncRuntimeTests, RecursiveDirectAwaitsCompleteWithoutIndependentTasks) {
    FailingResource resource(-1);
    RecursiveObservation observation;
    RecursiveArguments arguments{&observation, &resource, 8};
    Computation computation;
    ASSERT_TRUE(Computation::TryCreate(RecursiveLayout(), &arguments, &resource,
                                       computation));
    EXPECT_EQ(computation.Poll({{}, false}), PollResult::Ready);
    int result = 0;
    EXPECT_TRUE(computation.TakeResult(&result));
    EXPECT_EQ(result, 50);
    EXPECT_EQ(observation.initialized, 9);
    EXPECT_TRUE(computation.Release());
    EXPECT_EQ(observation.destroyed, 9);
    EXPECT_EQ(resource.live, 0);
}

namespace {
struct CompletionCounts {
    int constructed = 0;
    int destroyed = 0;
    int taken = 0;
    int argumentsDestroyed = 0;
};
struct CountedDelete {
    CompletionCounts* counts;
    void operator()(int* value) const noexcept {
        ++counts->argumentsDestroyed;
        delete value;
    }
};
using CountedOwner = std::unique_ptr<int, CountedDelete>;
struct ErrorCompletion {
    // An ordinary error inside T, distinct from PollResult::AllocationFailed.
    int error;
    CountedOwner owner;
    CompletionCounts* counts;
    ErrorCompletion(int e, CountedOwner value, CompletionCounts* c)
        : error(e), owner(std::move(value)), counts(c) {
        ++counts->constructed;
    }
    ErrorCompletion(ErrorCompletion&& other) noexcept
        : error(other.error),
          owner(std::move(other.owner)),
          counts(other.counts) {
        ++counts->constructed;
    }
    ErrorCompletion() = delete;
    ErrorCompletion(const ErrorCompletion&) = delete;
    ~ErrorCompletion() { ++counts->destroyed; }
};
struct CheckedArguments {
    CountedOwner owner;
    CompletionCounts* counts;
    bool failExecution;
};
struct CheckedFrame {
    CheckedArguments arguments;
    std::optional<ErrorCompletion> completion;
};
void InitializeChecked(void* storage, void* arguments) noexcept {
    new (storage)
        CheckedFrame{std::move(*static_cast<CheckedArguments*>(arguments)), {}};
}
PollResult PollChecked(void* storage, const PollContext&) noexcept {
    auto& frame = *static_cast<CheckedFrame*>(storage);
    if (frame.arguments.failExecution) return PollResult::AllocationFailed;
    frame.completion.emplace(73, std::move(frame.arguments.owner),
                             frame.arguments.counts);
    return PollResult::Ready;
}
void DestroyChecked(void* storage) noexcept {
    std::destroy_at(static_cast<CheckedFrame*>(storage));
}
void TakeChecked(void* storage, void* destination) noexcept {
    auto& frame = *static_cast<CheckedFrame*>(storage);
    ++frame.arguments.counts->taken;
    new (destination) ErrorCompletion(std::move(*frame.completion));
}
constexpr FrameLayout CHECKED_LAYOUT{sizeof(CheckedFrame),
                                     alignof(CheckedFrame),
                                     {PollChecked, DestroyChecked, TakeChecked},
                                     InitializeChecked};
}  // namespace

TEST(AsyncRuntimeTests,
     CreationFailuresCanRetryWithoutTransferringOrDestroyingOwner) {
    for (int failAt : {0, 1, 2}) {
        CompletionCounts counts;
        FailingResource resource(failAt);
        CheckedArguments arguments{
            CountedOwner(new int(42), CountedDelete{&counts}), &counts, false};
        {
            Executor executor(&resource);
            TaskHandle task;
            EXPECT_FALSE(
                executor.TryCreateTask(CHECKED_LAYOUT, &arguments, task));
            ASSERT_NE(arguments.owner, nullptr);
            EXPECT_EQ(counts.argumentsDestroyed, 0);
            EXPECT_EQ(counts.constructed, 0);
            EXPECT_EQ(executor.Status(task), TaskStatus::Invalid);
            resource.failAt = -1;
            ASSERT_TRUE(
                executor.TryCreateTask(CHECKED_LAYOUT, &arguments, task));
            EXPECT_EQ(arguments.owner, nullptr);
            executor.RequestCancellation(task);
            executor.Step();
            EXPECT_TRUE(executor.Release(task));
            EXPECT_FALSE(executor.Release(task));
        }
        EXPECT_EQ(counts.constructed, 0);
        EXPECT_EQ(counts.argumentsDestroyed, 1);
        EXPECT_EQ(resource.live, 0);
    }
}

TEST(AsyncRuntimeTests,
     ExecutionFailureNeverConstructsMoveOnlyErrorCompletion) {
    for (bool fail : {false, true}) {
        CompletionCounts counts;
        FailingResource resource(-1);
        CheckedArguments arguments{
            CountedOwner(new int(42), CountedDelete{&counts}), &counts, fail};
        Computation computation;
        ASSERT_TRUE(Computation::TryCreate(CHECKED_LAYOUT, &arguments,
                                           &resource, computation));
        EXPECT_EQ(counts.constructed, 0);
        EXPECT_EQ(computation.Poll({{}, false}),
                  fail ? PollResult::AllocationFailed : PollResult::Ready);
        alignas(ErrorCompletion) unsigned char storage[sizeof(ErrorCompletion)];
        std::fill(std::begin(storage), std::end(storage), 0xA5);
        EXPECT_EQ(computation.TakeResult(storage), !fail);
        if (fail) {
            EXPECT_TRUE(std::all_of(std::begin(storage), std::end(storage),
                                    [](auto b) { return b == 0xA5; }));
            EXPECT_EQ(counts.constructed, 0);
            EXPECT_EQ(counts.taken, 0);
        } else {
            auto* result =
                std::launder(reinterpret_cast<ErrorCompletion*>(storage));
            EXPECT_EQ(result->error, 73);
            ASSERT_NE(result->owner, nullptr);
            EXPECT_EQ(*result->owner, 42);
            EXPECT_FALSE(computation.TakeResult(storage));
            std::destroy_at(result);
        }
        EXPECT_TRUE(computation.Release());
        EXPECT_TRUE(computation.Release());
        EXPECT_EQ(counts.argumentsDestroyed, 1);
        EXPECT_EQ(counts.constructed, counts.destroyed);
        EXPECT_EQ(resource.live, 0);
    }
}
