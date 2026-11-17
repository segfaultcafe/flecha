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
    // Internal deterministic external-event hook used by compiler execution
    // tests. A null hook is ready. Returning false records the supplied wake
    // outside the frame and causes a genuine Pending result without executing
    // source actions. This is not a public future API.
    bool (*externalReady)(void* state, void* frame,
                          const WakeHandle& wake) noexcept = nullptr;
    void* externalState = nullptr;
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
    // Compiler-generated frames use this private form to retain their owning
    // allocation resource for recursive direct awaits. Handwritten layouts
    // may continue to provide only initialize.
    void (*initializeWithResource)(void* frame, void* arguments,
                                   std::pmr::memory_resource*) noexcept =
        nullptr;
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
    // Split creation used by compiler-generated direct awaits. Reservation
    // allocates stable storage but does not evaluate or transfer arguments.
    // Initialize is infallible and may be called exactly once after a
    // successful reservation.
    static bool TryReserve(const FrameLayout& layout,
                           std::pmr::memory_resource* resource,
                           Computation& output) noexcept;
    void Initialize(void* arguments) noexcept;
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
    bool _initialized = false;
    bool _started = false;
    bool _resultTaken = false;
};

class Executor final {
   public:
    // An injected memory resource must outlive every handle, including expired
    // weak handles. Frame allocation is separate from scheduler admission.
    explicit Executor(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource(),
        bool (*externalReady)(void*, void*,
                              const WakeHandle&) noexcept = nullptr,
        void* externalState = nullptr);
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
    bool (*_externalReady)(void*, void*, const WakeHandle&) noexcept = nullptr;
    void* _externalState = nullptr;
    friend class WakeHandle;
};

}  // namespace flecha::runtime::async

// Private compiler/runtime bridge used by generated frame bindings. These are
// C symbols only to make their ABI easy to reference from LLVM IR; they are not
// a Flecha language or library API.
extern "C" {
void flecha_async_computation_construct(void* storage) noexcept;
void flecha_async_computation_destruct(void* storage) noexcept;
bool flecha_async_computation_reserve(
    void* storage, std::size_t size, std::size_t alignment,
    flecha::runtime::async::PollResult (*poll)(
        void*, const flecha::runtime::async::PollContext&) noexcept,
    void (*destroy)(void*) noexcept, void (*take)(void*, void*) noexcept,
    void (*initialize)(void*, void*, std::pmr::memory_resource*) noexcept,
    std::pmr::memory_resource* resource) noexcept;
void flecha_async_computation_initialize(void* storage,
                                         void* arguments) noexcept;
std::uint8_t flecha_async_computation_poll(
    void* storage, const flecha::runtime::async::PollContext* context) noexcept;
std::uint8_t flecha_async_computation_cancel(
    void* storage, const flecha::runtime::async::PollContext* context) noexcept;
bool flecha_async_computation_take(void* storage, void* destination) noexcept;
bool flecha_async_computation_release(void* storage) noexcept;
bool flecha_async_context_cancelled(
    const flecha::runtime::async::PollContext* context) noexcept;
bool flecha_async_context_ready(
    const flecha::runtime::async::PollContext* context, void* frame) noexcept;
}
#endif
