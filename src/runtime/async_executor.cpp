#include "runtime/async_executor.hpp"

#include <exception>
#include <new>

namespace flecha::runtime::async {

namespace {
TaskStatus StatusFor(PollResult result) noexcept {
    switch (result) {
        case PollResult::Pending:
            return TaskStatus::Pending;
        case PollResult::Ready:
            return TaskStatus::Ready;
        case PollResult::Cancelled:
            return TaskStatus::Cancelled;
        case PollResult::AllocationFailed:
            return TaskStatus::AllocationFailed;
    }
    std::terminate();
}

PollResult PollComputation(void* frame, const PollContext& context) noexcept {
    return static_cast<Computation*>(frame)->Poll(context);
}
void DestroyComputation(void* frame) noexcept {
    if (!static_cast<Computation*>(frame)->Release()) std::terminate();
}
void TakeComputation(void* frame, void* destination) noexcept {
    if (!static_cast<Computation*>(frame)->TakeResult(destination))
        std::terminate();
}
}  // namespace

Computation::~Computation() {
    if (!Release()) std::terminate();
}

bool Computation::TryCreate(const FrameLayout& layout, void* arguments,
                            std::pmr::memory_resource* resource,
                            Computation& output) noexcept {
    if (!TryReserve(layout, resource, output)) return false;
    output.Initialize(arguments);
    return true;
}

bool Computation::TryReserve(const FrameLayout& layout,
                             std::pmr::memory_resource* resource,
                             Computation& output) noexcept {
    if (output._frame || !resource || !layout.size || !layout.alignment ||
        (layout.alignment & (layout.alignment - 1)) ||
        (!layout.initialize && !layout.initializeWithResource) ||
        !layout.operations.poll || !layout.operations.destroy)
        std::terminate();
    void* storage;
    try {
        storage = resource->allocate(layout.size, layout.alignment);
    } catch (const std::bad_alloc&) {
        return false;
    }
    output._frame = storage;
    output._resource = resource;
    output._layout = layout;
    output._result = PollResult::Pending;
    output._initialized = false;
    output._started = false;
    output._resultTaken = false;
    return true;
}

void Computation::Initialize(void* arguments) noexcept {
    if (!_frame || _initialized) std::terminate();
    // This callback only transfers arguments whose evaluation has completed
    // after reservation. It must not execute the async body.
    if (_layout.initializeWithResource)
        _layout.initializeWithResource(_frame, arguments, _resource);
    else
        _layout.initialize(_frame, arguments);
    _initialized = true;
}

PollResult Computation::Poll(const PollContext& context) noexcept {
    if (!_frame || !_initialized) std::terminate();
    if (_result != PollResult::Pending) return _result;
    if (!_started && context.cancellationRequested) {
        _result = PollResult::Cancelled;
        return _result;
    }
    _started = true;
    _result = _layout.operations.poll(_frame, context);
    return _result;
}

TaskStatus Computation::Status() const noexcept {
    return _frame ? StatusFor(_result) : TaskStatus::Invalid;
}

bool Computation::TakeResult(void* destination) noexcept {
    if (!_frame || !_initialized || _result != PollResult::Ready ||
        _resultTaken)
        return false;
    if (_layout.operations.takeResult && !destination) return false;
    _resultTaken = true;
    if (_layout.operations.takeResult)
        _layout.operations.takeResult(_frame, destination);
    return true;
}

bool Computation::Release() noexcept {
    if (!_frame) return true;
    if (_started && _result == PollResult::Pending) return false;
    if (_initialized) _layout.operations.destroy(_frame);
    _resource->deallocate(_frame, _layout.size, _layout.alignment);
    _frame = nullptr;
    _initialized = false;
    return true;
}

struct TaskRecord final {
    Executor* executor = nullptr;
    void* frame = nullptr;
    FrameOperations operations{};
    TaskStatus status = TaskStatus::Pending;
    TaskRecord* nextReady = nullptr;
    WakeHandle waiter;
    bool queued = false;
    bool started = false;
    bool cancellationRequested = false;
    bool resultTaken = false;
    bool admitted = false;
    Computation computation;
};

void WakeHandle::Notify() const noexcept {
    if (const std::shared_ptr<TaskRecord> task = _task.lock())
        if (task->executor) task->executor->Enqueue(*task);
}

Executor::Executor(std::pmr::memory_resource* resource,
                   bool (*externalReady)(void*, void*,
                                         const WakeHandle&) noexcept,
                   void* externalState)
    : _resource(resource),
      _tasks(resource),
      _thread(std::this_thread::get_id()),
      _externalReady(externalReady),
      _externalState(externalState) {}

Executor::~Executor() {
    CheckThread();
    // Destruction cannot synchronously force pending operations to retire.
    // The owner must drive cancellation/draining before destroying an executor.
    for (const auto& task : _tasks)
        if (task->status == TaskStatus::Pending) std::terminate();
    for (const auto& task : _tasks) task->executor = nullptr;
    for (const auto& task : _tasks) task->operations.destroy(task->frame);
}

void Executor::CheckThread() const noexcept {
    if (std::this_thread::get_id() != _thread) std::terminate();
}

std::shared_ptr<TaskRecord> Executor::Find(
    const TaskHandle& handle) const noexcept {
    CheckThread();
    std::shared_ptr<TaskRecord> task = handle._task.lock();
    return task && task->executor == this ? task : nullptr;
}

bool Executor::TrySchedule(void* frame, FrameOperations operations,
                           TaskHandle& output) noexcept {
    CheckThread();
    if (!frame || !operations.poll || !operations.destroy) return false;
    try {
        std::shared_ptr<TaskRecord> task = std::allocate_shared<TaskRecord>(
            std::pmr::polymorphic_allocator<TaskRecord>(_resource));
        _tasks.push_back(task);
        // No fallible operation follows ownership transfer/publication.
        task->frame = frame;
        task->operations = operations;
        task->executor = this;
        output._task = task;
        Enqueue(*task);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

bool Executor::TryCreateTask(const FrameLayout& layout, void* arguments,
                             TaskHandle& output) noexcept {
    if (!TryReserveTask(layout, output)) return false;
    InitializeReservedTask(output, arguments);
    return true;
}

bool Executor::TryReserveTask(const FrameLayout& layout,
                              TaskHandle& output) noexcept {
    CheckThread();
    try {
        std::shared_ptr<TaskRecord> task = std::allocate_shared<TaskRecord>(
            std::pmr::polymorphic_allocator<TaskRecord>(_resource));
        const auto reservation = _tasks.insert(_tasks.end(), task);
        if (!Computation::TryReserve(layout, _resource, task->computation)) {
            _tasks.erase(reservation);
            return false;
        }
        task->frame = &task->computation;
        task->operations = {
            PollComputation, DestroyComputation,
            layout.operations.takeResult ? TakeComputation : nullptr};
        task->executor = this;
        output._task = task;
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

void Executor::InitializeReservedTask(const TaskHandle& handle,
                                      void* arguments) noexcept {
    CheckThread();
    const std::shared_ptr<TaskRecord> task = Find(handle);
    if (!task || task->admitted) std::terminate();
    task->computation.Initialize(arguments);
    task->admitted = true;
    Enqueue(*task);
}

void Executor::Enqueue(TaskRecord& task) noexcept {
    CheckThread();
    if (task.status != TaskStatus::Pending || task.queued) return;
    task.queued = true;
    task.nextReady = nullptr;
    if (_tail)
        _tail->nextReady = &task;
    else
        _head = &task;
    _tail = &task;
}

void Executor::RemoveReady(TaskRecord& task) noexcept {
    if (!task.queued) return;
    TaskRecord* previous = nullptr;
    for (TaskRecord* current = _head; current; current = current->nextReady) {
        if (current == &task) {
            if (previous)
                previous->nextReady = current->nextReady;
            else
                _head = current->nextReady;
            if (_tail == current) _tail = previous;
            task.nextReady = nullptr;
            task.queued = false;
            return;
        }
        previous = current;
    }
    std::terminate();
}

StepResult Executor::Step() noexcept {
    CheckThread();
    if (_polling) return StepResult::Reentrant;
    if (!_head) return StepResult::Idle;
    TaskRecord& task = *_head;
    RemoveReady(task);
    _polling = true;
    PollResult result = PollResult::Cancelled;
    if (task.started || !task.cancellationRequested) {
        task.started = true;
        // Find the owning control block without allocating a fresh wake slot.
        for (const auto& owner : _tasks)
            if (owner.get() == &task) {
                const PollContext context{WakeHandle(owner),
                                          task.cancellationRequested,
                                          _externalReady, _externalState, this};
                result = task.operations.poll(task.frame, context);
                break;
            }
    }
    _polling = false;
    if (result != PollResult::Pending) {
        task.status = StatusFor(result);
        // A wake during the final poll must not leave a dangling queue entry.
        RemoveReady(task);
        task.waiter.Notify();
    }
    return StepResult::Polled;
}

TaskStatus Executor::Status(const TaskHandle& handle) const noexcept {
    const std::shared_ptr<TaskRecord> task = Find(handle);
    return task ? task->status : TaskStatus::Invalid;
}

void Executor::RequestCancellation(const TaskHandle& handle) noexcept {
    if (const std::shared_ptr<TaskRecord> task = Find(handle)) {
        if (task->status != TaskStatus::Pending || task->cancellationRequested)
            return;
        task->cancellationRequested = true;
        Enqueue(*task);
    }
}

bool Executor::Watch(const TaskHandle& handle,
                     const WakeHandle& waiter) noexcept {
    const std::shared_ptr<TaskRecord> task = Find(handle);
    if (!task || task->status != TaskStatus::Pending) return true;
    task->waiter = waiter;
    return false;
}

bool Executor::TakeResult(const TaskHandle& handle,
                          void* destination) noexcept {
    const std::shared_ptr<TaskRecord> task = Find(handle);
    if (!task || task->status != TaskStatus::Ready || task->resultTaken)
        return false;
    if (task->operations.takeResult && !destination) return false;
    task->resultTaken = true;
    if (task->operations.takeResult)
        task->operations.takeResult(task->frame, destination);
    return true;
}

bool Executor::Release(const TaskHandle& handle) noexcept {
    const std::shared_ptr<TaskRecord> task = Find(handle);
    if (!task || task->status == TaskStatus::Pending) return false;
    task->executor = nullptr;
    task->operations.destroy(task->frame);
    task->frame = nullptr;
    for (auto iterator = _tasks.begin(); iterator != _tasks.end(); ++iterator)
        if (iterator->get() == task.get()) {
            _tasks.erase(iterator);
            return true;
        }
    std::terminate();
}

WakeHandle Executor::WakeFor(const TaskHandle& handle) const noexcept {
    return WakeHandle(Find(handle));
}

struct ScopedChild final {
    ChildScope* owner = nullptr;
    ScopedChild* previous = nullptr;
    ScopedChild* next = nullptr;
    Executor* executor = nullptr;
    std::pmr::memory_resource* resource = nullptr;
    TaskHandle task;
};

namespace {
ScopedChild*& Head(ChildScope& scope) noexcept { return scope._head; }
ScopedChild*& Tail(ChildScope& scope) noexcept { return scope._tail; }
ScopedChild*& Reservation(void* storage) noexcept {
    return *static_cast<ScopedChild**>(storage);
}
ScopedChild*& Joined(void* storage) noexcept {
    return *static_cast<ScopedChild**>(storage);
}

void Link(ChildScope& scope, ScopedChild& child) noexcept {
    child.owner = &scope;
    child.previous = Tail(scope);
    if (Tail(scope))
        Tail(scope)->next = &child;
    else
        Head(scope) = &child;
    Tail(scope) = &child;
}

void Unlink(ScopedChild& child) noexcept {
    ChildScope& scope = *child.owner;
    if (child.previous)
        child.previous->next = child.next;
    else
        Head(scope) = child.next;
    if (child.next)
        child.next->previous = child.previous;
    else
        Tail(scope) = child.previous;
    child.owner = nullptr;
}

void DestroyChild(ScopedChild& child) noexcept {
    std::pmr::memory_resource* resource = child.resource;
    std::destroy_at(&child);
    resource->deallocate(&child, sizeof(ScopedChild), alignof(ScopedChild));
}

PollResult ResultFor(TaskStatus status) noexcept {
    switch (status) {
        case TaskStatus::Ready:
            return PollResult::Ready;
        case TaskStatus::Cancelled:
            return PollResult::Cancelled;
        case TaskStatus::AllocationFailed:
            return PollResult::AllocationFailed;
        case TaskStatus::Pending:
            return PollResult::Pending;
        case TaskStatus::Invalid:
            break;
    }
    std::terminate();
}
}  // namespace

ChildScope::~ChildScope() {
    if (_head || _tail) std::terminate();
}

bool ReserveScopedChild(ChildScope&, void* reservation,
                        const PollContext& context,
                        const FrameLayout& layout) noexcept {
    if (!reservation || Reservation(reservation) || !context.executor)
        return false;
    std::pmr::memory_resource* resource = context.executor->Resource();
    void* storage;
    try {
        storage = resource->allocate(sizeof(ScopedChild), alignof(ScopedChild));
    } catch (const std::bad_alloc&) {
        return false;
    }
    ScopedChild* child = new (storage) ScopedChild();
    child->executor = context.executor;
    child->resource = resource;
    if (!context.executor->TryReserveTask(layout, child->task)) {
        DestroyChild(*child);
        return false;
    }
    Reservation(reservation) = child;
    return true;
}

void InitializeScopedChild(ChildScope& scope, void* reservation,
                           void* arguments, void** output) noexcept {
    ScopedChild* child = Reservation(reservation);
    if (!child || !output) std::terminate();
    child->executor->InitializeReservedTask(child->task, arguments);
    Link(scope, *child);
    *output = child;
    Reservation(reservation) = nullptr;
}

void CancelScopedChildren(ChildScope& scope) noexcept {
    for (ScopedChild* child = Head(scope); child; child = child->next)
        child->executor->RequestCancellation(child->task);
}

bool DrainScopedChildren(ChildScope& scope,
                         const PollContext& context) noexcept {
    while (ScopedChild* child = Head(scope)) {
        if (child->executor->Status(child->task) == TaskStatus::Pending) {
            if (!child->executor->Watch(child->task, context.wake))
                return false;
            continue;
        }
        Unlink(*child);
        if (!child->executor->Release(child->task)) std::terminate();
        DestroyChild(*child);
    }
    return true;
}

void InitializeChildJoin(void* storage, void* child) noexcept {
    if (!storage || Joined(storage) || !child) std::terminate();
    Joined(storage) = static_cast<ScopedChild*>(child);
}

PollResult PollChildJoin(void* storage, const PollContext& context) noexcept {
    ScopedChild* child = Joined(storage);
    if (!child) std::terminate();
    TaskStatus status = child->executor->Status(child->task);
    if (status == TaskStatus::Pending &&
        !child->executor->Watch(child->task, context.wake))
        return PollResult::Pending;
    status = child->executor->Status(child->task);
    return ResultFor(status);
}

bool TakeChildJoin(void* storage, void* destination) noexcept {
    ScopedChild* child = Joined(storage);
    return child && child->executor->TakeResult(child->task, destination);
}

bool ReleaseChildJoin(void* storage) noexcept {
    ScopedChild* child = Joined(storage);
    if (!child || child->executor->Status(child->task) == TaskStatus::Pending)
        return false;
    Unlink(*child);
    if (!child->executor->Release(child->task)) std::terminate();
    DestroyChild(*child);
    Joined(storage) = nullptr;
    return true;
}

}  // namespace flecha::runtime::async

extern "C" {
void flecha_async_computation_construct(void* storage) noexcept {
    new (storage) flecha::runtime::async::Computation();
}

void flecha_async_computation_destruct(void* storage) noexcept {
    std::destroy_at(static_cast<flecha::runtime::async::Computation*>(storage));
}

bool flecha_async_computation_reserve(
    void* storage, std::size_t size, std::size_t alignment,
    flecha::runtime::async::PollResult (*poll)(
        void*, const flecha::runtime::async::PollContext&) noexcept,
    void (*destroy)(void*) noexcept, void (*take)(void*, void*) noexcept,
    void (*initialize)(void*, void*, std::pmr::memory_resource*) noexcept,
    std::pmr::memory_resource* resource) noexcept {
    flecha::runtime::async::FrameLayout layout{
        size, alignment, {poll, destroy, take}, nullptr, initialize};
    return flecha::runtime::async::Computation::TryReserve(
        layout, resource,
        *static_cast<flecha::runtime::async::Computation*>(storage));
}

void flecha_async_computation_initialize(void* storage,
                                         void* arguments) noexcept {
    static_cast<flecha::runtime::async::Computation*>(storage)->Initialize(
        arguments);
}

std::uint8_t flecha_async_computation_poll(
    void* storage,
    const flecha::runtime::async::PollContext* context) noexcept {
    return static_cast<std::uint8_t>(
        static_cast<flecha::runtime::async::Computation*>(storage)->Poll(
            *context));
}

std::uint8_t flecha_async_computation_cancel(
    void* storage,
    const flecha::runtime::async::PollContext* context) noexcept {
    const flecha::runtime::async::PollContext cancelled{
        context->wake, true, context->externalReady, context->externalState,
        context->executor};
    return static_cast<std::uint8_t>(
        static_cast<flecha::runtime::async::Computation*>(storage)->Poll(
            cancelled));
}

bool flecha_async_computation_take(void* storage, void* destination) noexcept {
    return static_cast<flecha::runtime::async::Computation*>(storage)
        ->TakeResult(destination);
}

bool flecha_async_computation_release(void* storage) noexcept {
    return static_cast<flecha::runtime::async::Computation*>(storage)
        ->Release();
}

bool flecha_async_context_cancelled(
    const flecha::runtime::async::PollContext* context) noexcept {
    return context->cancellationRequested;
}

bool flecha_async_context_ready(
    const flecha::runtime::async::PollContext* context, void* frame) noexcept {
    return !context->externalReady ||
           context->externalReady(context->externalState, frame, context->wake);
}

void flecha_async_child_scope_construct(void* storage) noexcept {
    new (storage) flecha::runtime::async::ChildScope();
}

void flecha_async_child_scope_destruct(void* storage) noexcept {
    std::destroy_at(static_cast<flecha::runtime::async::ChildScope*>(storage));
}

bool flecha_async_child_reserve(
    void* scope, void* reservation,
    const flecha::runtime::async::PollContext* context, std::size_t size,
    std::size_t alignment,
    flecha::runtime::async::PollResult (*poll)(
        void*, const flecha::runtime::async::PollContext&) noexcept,
    void (*destroy)(void*) noexcept, void (*take)(void*, void*) noexcept,
    void (*initialize)(void*, void*,
                       std::pmr::memory_resource*) noexcept) noexcept {
    if (!scope || !context) return false;
    const flecha::runtime::async::FrameLayout layout{
        size, alignment, {poll, destroy, take}, nullptr, initialize};
    return flecha::runtime::async::ReserveScopedChild(
        *static_cast<flecha::runtime::async::ChildScope*>(scope), reservation,
        *context, layout);
}

void flecha_async_child_initialize(void* scope, void* reservation,
                                   void* arguments, void* output) noexcept {
    flecha::runtime::async::InitializeScopedChild(
        *static_cast<flecha::runtime::async::ChildScope*>(scope), reservation,
        arguments, static_cast<void**>(output));
}

void flecha_async_child_join_initialize(void* storage, void* child) noexcept {
    flecha::runtime::async::InitializeChildJoin(storage, child);
}

std::uint8_t flecha_async_child_join_poll(
    void* storage,
    const flecha::runtime::async::PollContext* context) noexcept {
    return static_cast<std::uint8_t>(
        flecha::runtime::async::PollChildJoin(storage, *context));
}

bool flecha_async_child_join_take(void* storage, void* destination) noexcept {
    return flecha::runtime::async::TakeChildJoin(storage, destination);
}

bool flecha_async_child_join_release(void* storage) noexcept {
    return flecha::runtime::async::ReleaseChildJoin(storage);
}

void flecha_async_child_scope_cancel(void* scope) noexcept {
    flecha::runtime::async::CancelScopedChildren(
        *static_cast<flecha::runtime::async::ChildScope*>(scope));
}

bool flecha_async_child_scope_drain(
    void* scope, const flecha::runtime::async::PollContext* context) noexcept {
    return flecha::runtime::async::DrainScopedChildren(
        *static_cast<flecha::runtime::async::ChildScope*>(scope), *context);
}
}
