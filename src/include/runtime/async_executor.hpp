#ifndef FLECHA_RUNTIME_ASYNC_EXECUTOR_HPP
#define FLECHA_RUNTIME_ASYNC_EXECUTOR_HPP

#include <cstddef>
#include <list>
#include <memory>
#include <memory_resource>
#include <thread>

namespace flecha::runtime::async {

// Private compiler/runtime protocol, not a public Flecha future API. Every
// operation, including notifications, runs on the executor's owning thread.
// Foreign threads must marshal notifications before entering this protocol.
struct TaskRecord;
class Executor;

class WakeHandle final {
   public:
    WakeHandle() = default;
    // Notifications never invoke task code. Expired/retired handles are no-ops.
    void Notify() const noexcept;

   private:
    explicit WakeHandle(std::weak_ptr<TaskRecord> task)
        : _task(std::move(task)) {}
    std::weak_ptr<TaskRecord> _task;
    friend class Executor;
};

class TaskHandle final {
   public:
    TaskHandle() = default;

   private:
    std::weak_ptr<TaskRecord> _task;
    friend class Executor;
};

struct PollContext final {
    WakeHandle wake;
    bool cancellationRequested;
};

// Every terminal outcome acknowledges that callbacks/operations can no longer
// access frame storage. A cancellation request alone is not acknowledgment.
// AllocationFailed has no completed value; an ordinary error returned by user
// code is a Ready value, not an execution failure.
enum class PollResult { Pending, Ready, Cancelled, AllocationFailed };
enum class TaskStatus { Invalid, Pending, Ready, Cancelled, AllocationFailed };
enum class StepResult { Idle, Polled, Reentrant };

struct FrameOperations final {
    PollResult (*poll)(void*, const PollContext&) noexcept;
    void (*destroy)(void*) noexcept;
    // Move a completed result into caller-provided storage; nullptr for void.
    void (*takeResult)(void*, void*) noexcept = nullptr;
};

struct FrameLayout final {
    std::size_t size;
    std::size_t alignment;
    FrameOperations operations;
    // Runs only after all creation allocations succeed. It initializes frame
    // storage and transfers arguments without executing the async body.
    void (*initialize)(void* frame, void* arguments) noexcept;
};

// A stable, owned computation frame. Direct awaits poll this with their
// caller's context; creation does not independently schedule a task. Neither
// failed creation nor a terminal failure constructs a completed value.
class Computation final {
   public:
    Computation() = default;
    ~Computation();
    Computation(const Computation&) = delete;
    Computation& operator=(const Computation&) = delete;

    // Output must be empty. Failure leaves it and arguments unchanged. The
    // resource must outlive the computation. No initialization runs on failure.
    static bool TryCreate(const FrameLayout& layout, void* arguments,
                          std::pmr::memory_resource* resource,
                          Computation& output) noexcept;
    PollResult Poll(const PollContext& context) noexcept;
    TaskStatus Status() const noexcept;
    bool TakeResult(void* destination) noexcept;
    // An unstarted frame can be destroyed immediately. A started computation
    // must acknowledge retirement through a terminal poll before release.
    bool Release() noexcept;

   private:
    void* _frame = nullptr;
    std::pmr::memory_resource* _resource = nullptr;
    FrameLayout _layout{};
    PollResult _result = PollResult::Pending;
    bool _started = false;
    bool _resultTaken = false;
};

class Executor final {
   public:
    // An injected memory resource must outlive every handle, including expired
    // weak handles. Frame allocation is separate from scheduler admission.
    explicit Executor(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    ~Executor();
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    // Admission is transactional: on false, output and frame ownership remain
    // with the caller. On success the executor owns the frame until Release.
    bool TrySchedule(void* frame, FrameOperations operations,
                     TaskHandle& output) noexcept;
    // Reserves scheduler and stable frame storage before initializing arguments
    // or publishing a runnable task. On failure output/arguments are unchanged.
    bool TryCreateTask(const FrameLayout& layout, void* arguments,
                       TaskHandle& output) noexcept;
    StepResult Step() noexcept;
    TaskStatus Status(const TaskHandle& task) const noexcept;
    void RequestCancellation(const TaskHandle& task) noexcept;
    // Register a single joining task. Returns true if already terminal. A
    // completion racing with registration on this thread cannot lose a wake.
    bool Watch(const TaskHandle& task, const WakeHandle& waiter) noexcept;
    bool TakeResult(const TaskHandle& task, void* destination) noexcept;
    // Only terminal tasks can retire their storage. Taking a result does not
    // itself release the scope's ownership of the task record.
    bool Release(const TaskHandle& task) noexcept;
    WakeHandle WakeFor(const TaskHandle& task) const noexcept;

   private:
    void CheckThread() const noexcept;
    void Enqueue(TaskRecord& task) noexcept;
    void RemoveReady(TaskRecord& task) noexcept;
    std::shared_ptr<TaskRecord> Find(const TaskHandle& task) const noexcept;
    std::pmr::memory_resource* _resource;
    std::pmr::list<std::shared_ptr<TaskRecord>> _tasks;
    std::thread::id _thread;
    TaskRecord* _head = nullptr;
    TaskRecord* _tail = nullptr;
    bool _polling = false;
    friend class WakeHandle;
};

}  // namespace flecha::runtime::async
#endif
