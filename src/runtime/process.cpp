#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "runtime/runtime_abi.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace {

size_t argumentCount = 0;
const char* const* argumentValues = nullptr;

enum class PipeDirection : uint8_t { Read, Write };

#ifdef _WIN32
using NativeProcess = HANDLE;
using NativePipe = HANDLE;
constexpr NativeProcess InvalidProcess = nullptr;
constexpr NativePipe InvalidPipe = nullptr;
#else
using NativeProcess = pid_t;
using NativePipe = int;
constexpr NativeProcess InvalidProcess = -1;
constexpr NativePipe InvalidPipe = -1;
#endif

struct ProcessHandle final {
    static constexpr uint64_t Magic = UINT64_C(0x464c50524f434553);

    uint64_t magic = Magic;
    NativeProcess process = InvalidProcess;
    bool waited = false;
};

struct ProcessPipeHandle final {
    static constexpr uint64_t Magic = UINT64_C(0x464c505049504521);

    uint64_t magic = Magic;
    NativePipe pipe = InvalidPipe;
    PipeDirection direction = PipeDirection::Read;
};

bool ValidMode(uint8_t mode) {
    return mode <= FLECHA_RUNTIME_PROCESS_STDIO_PIPED;
}

bool ContainsNull(const char* bytes, size_t length) {
    return length != 0 && std::memchr(bytes, 0, length) != nullptr;
}

int32_t ValidateArguments(const char* executable, size_t executableLength,
                          const uint8_t* bytes, size_t bytesLength,
                          const size_t* offsets, size_t count,
                          uint8_t stdinMode, uint8_t stdoutMode,
                          uint8_t stderrMode) {
    if (count == std::numeric_limits<size_t>::max())
        return FLECHA_RUNTIME_PROCESS_OUT_OF_MEMORY;
    if (executable == nullptr || executableLength == 0 ||
        ContainsNull(executable, executableLength) ||
        (bytes == nullptr && bytesLength != 0) || offsets == nullptr ||
        !ValidMode(stdinMode) || !ValidMode(stdoutMode) ||
        !ValidMode(stderrMode))
        return FLECHA_RUNTIME_PROCESS_INVALID_ARGUMENT;

    if (offsets[0] != 0 || offsets[count] != bytesLength)
        return FLECHA_RUNTIME_PROCESS_INVALID_ARGUMENT;
    for (size_t index = 0; index < count; ++index) {
        if (offsets[index] > offsets[index + 1] ||
            offsets[index + 1] > bytesLength)
            return FLECHA_RUNTIME_PROCESS_INVALID_ARGUMENT;
        const size_t length = offsets[index + 1] - offsets[index];
        if (length != 0 && std::memchr(bytes + offsets[index], 0, length))
            return FLECHA_RUNTIME_PROCESS_INVALID_ARGUMENT;
    }
    return FLECHA_RUNTIME_PROCESS_OK;
}

ProcessHandle* GetProcessHandle(void* opaque) {
    if (opaque == nullptr) return nullptr;
    auto* handle = static_cast<ProcessHandle*>(opaque);
    if (handle->magic != ProcessHandle::Magic ||
        handle->process == InvalidProcess)
        return nullptr;
    return handle;
}

ProcessPipeHandle* GetPipeHandle(void* opaque, PipeDirection direction) {
    if (opaque == nullptr) return nullptr;
    auto* handle = static_cast<ProcessPipeHandle*>(opaque);
    if (handle->magic != ProcessPipeHandle::Magic ||
        handle->pipe == InvalidPipe || handle->direction != direction)
        return nullptr;
    return handle;
}

#ifdef _WIN32

int32_t MapProcessError(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_MOD_NOT_FOUND:
            return FLECHA_RUNTIME_PROCESS_EXECUTABLE_NOT_FOUND;
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
        case ERROR_ELEVATION_REQUIRED:
            return FLECHA_RUNTIME_PROCESS_PERMISSION_DENIED;
        case ERROR_BAD_EXE_FORMAT:
        case ERROR_EXE_MACHINE_TYPE_MISMATCH:
        case ERROR_INVALID_NAME:
        case ERROR_INVALID_PARAMETER:
        case ERROR_FILENAME_EXCED_RANGE:
            return FLECHA_RUNTIME_PROCESS_INVALID_ARGUMENT;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
        case ERROR_NO_SYSTEM_RESOURCES:
            return FLECHA_RUNTIME_PROCESS_OUT_OF_MEMORY;
        case ERROR_BROKEN_PIPE:
        case ERROR_NO_DATA:
        case ERROR_PIPE_NOT_CONNECTED:
            return FLECHA_RUNTIME_PROCESS_BROKEN_PIPE;
        case ERROR_INVALID_HANDLE:
            return FLECHA_RUNTIME_PROCESS_CLOSED;
        default:
            return FLECHA_RUNTIME_PROCESS_FAILED;
    }
}

class ScopedHandle final {
   public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE value) : value_(value) {}
    ~ScopedHandle() { reset(); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const { return value_; }
    HANDLE release() {
        const HANDLE value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(HANDLE value = nullptr) {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
        value_ = value;
    }

   private:
    HANDLE value_ = nullptr;
};

int32_t Utf8ToWide(const char* bytes, size_t length, std::wstring* output) {
    if (length == 0) {
        output->clear();
        return FLECHA_RUNTIME_PROCESS_OK;
    }
    if (length > static_cast<size_t>(INT_MAX))
        return FLECHA_RUNTIME_PROCESS_INVALID_ARGUMENT;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes,
                                          static_cast<int>(length), nullptr, 0);
    if (count <= 0) return MapProcessError(GetLastError());
    output->resize(static_cast<size_t>(count));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes,
                            static_cast<int>(length), output->data(),
                            count) == 0)
        return MapProcessError(GetLastError());
    return FLECHA_RUNTIME_PROCESS_OK;
}

void AppendQuotedWindowsArgument(const std::wstring& argument,
                                 std::wstring* commandLine) {
    if (!commandLine->empty()) commandLine->push_back(L' ');
    const bool quote =
        argument.empty() ||
        argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!quote) {
        commandLine->append(argument);
        return;
    }

    commandLine->push_back(L'"');
    size_t slashes = 0;
    for (wchar_t value : argument) {
        if (value == L'\\') {
            ++slashes;
            continue;
        }
        if (value == L'"') {
            commandLine->append(slashes * 2 + 1, L'\\');
            commandLine->push_back(L'"');
        } else {
            commandLine->append(slashes, L'\\');
            commandLine->push_back(value);
        }
        slashes = 0;
    }
    commandLine->append(slashes * 2, L'\\');
    commandLine->push_back(L'"');
}

int32_t ResolveExecutable(const std::wstring& executable,
                          std::wstring* resolved) {
    DWORD capacity = MAX_PATH;
    for (;;) {
        resolved->resize(capacity);
        const DWORD length = SearchPathW(nullptr, executable.c_str(), L".exe",
                                         capacity, resolved->data(), nullptr);
        if (length == 0) return MapProcessError(GetLastError());
        if (length < capacity) {
            resolved->resize(length);
            return FLECHA_RUNTIME_PROCESS_OK;
        }
        if (length == std::numeric_limits<DWORD>::max())
            return FLECHA_RUNTIME_PROCESS_INVALID_ARGUMENT;
        capacity = length + 1;
    }
}

int32_t OpenNull(bool input, ScopedHandle* output) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE handle = CreateFileW(L"NUL", input ? GENERIC_READ : GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return MapProcessError(GetLastError());
    output->reset(handle);
    return FLECHA_RUNTIME_PROCESS_OK;
}

int32_t DuplicateInheritedHandle(HANDLE source, bool input,
                                 ScopedHandle* output) {
    if (source == nullptr || source == INVALID_HANDLE_VALUE)
        return OpenNull(input, output);
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
                         &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return MapProcessError(GetLastError());
    output->reset(duplicate);
    return FLECHA_RUNTIME_PROCESS_OK;
}

int32_t CreateWindowsPipe(bool childReads, ScopedHandle* child,
                          ScopedHandle* parent) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    if (!CreatePipe(&readHandle, &writeHandle, &security, 0))
        return MapProcessError(GetLastError());

    ScopedHandle read(readHandle);
    ScopedHandle write(writeHandle);
    HANDLE parentHandle = childReads ? write.get() : read.get();
    if (!SetHandleInformation(parentHandle, HANDLE_FLAG_INHERIT, 0))
        return MapProcessError(GetLastError());

    if (childReads) {
        child->reset(read.release());
        parent->reset(write.release());
    } else {
        child->reset(write.release());
        parent->reset(read.release());
    }
    return FLECHA_RUNTIME_PROCESS_OK;
}

int32_t PrepareWindowsStream(uint8_t mode, bool input, DWORD standardId,
                             ScopedHandle* child, ScopedHandle* parent) {
    if (mode == FLECHA_RUNTIME_PROCESS_STDIO_PIPED)
        return CreateWindowsPipe(input, child, parent);
    if (mode == FLECHA_RUNTIME_PROCESS_STDIO_NULL)
        return OpenNull(input, child);
    return DuplicateInheritedHandle(GetStdHandle(standardId), input, child);
}

#else

int32_t MapProcessError(int error) {
    switch (error) {
        case ENOENT:
        case ENOTDIR:
            return FLECHA_RUNTIME_PROCESS_EXECUTABLE_NOT_FOUND;
        case EACCES:
        case EPERM:
            return FLECHA_RUNTIME_PROCESS_PERMISSION_DENIED;
        case EINVAL:
        case ENOEXEC:
        case ENAMETOOLONG:
            return FLECHA_RUNTIME_PROCESS_INVALID_ARGUMENT;
        case ENOMEM:
        case ENFILE:
        case EMFILE:
            return FLECHA_RUNTIME_PROCESS_OUT_OF_MEMORY;
        case EPIPE:
            return FLECHA_RUNTIME_PROCESS_BROKEN_PIPE;
        case EBADF:
            return FLECHA_RUNTIME_PROCESS_CLOSED;
        default:
            return FLECHA_RUNTIME_PROCESS_FAILED;
    }
}

class ScopedDescriptor final {
   public:
    ScopedDescriptor() = default;
    explicit ScopedDescriptor(int value) : value_(value) {}
    ~ScopedDescriptor() { reset(); }
    ScopedDescriptor(const ScopedDescriptor&) = delete;
    ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;

    int get() const { return value_; }
    int release() {
        const int value = value_;
        value_ = -1;
        return value;
    }
    void reset(int value = -1) {
        if (value_ >= 0) close(value_);
        value_ = value;
    }

   private:
    int value_ = -1;
};

int NormalizeDescriptor(int descriptor) {
    if (descriptor > STDERR_FILENO) return descriptor;
    const int duplicate = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
    const int error = errno;
    close(descriptor);
    errno = error;
    return duplicate;
}

int32_t CreatePosixPipe(bool childReads, ScopedDescriptor* child,
                        ScopedDescriptor* parent) {
    int descriptors[2] = {-1, -1};
    if (pipe(descriptors) != 0) return MapProcessError(errno);
    ScopedDescriptor read(NormalizeDescriptor(descriptors[0]));
    if (read.get() < 0) {
        const int error = errno;
        close(descriptors[1]);
        return MapProcessError(error);
    }
    ScopedDescriptor write(NormalizeDescriptor(descriptors[1]));
    if (write.get() < 0) return MapProcessError(errno);

    for (int descriptor : {read.get(), write.get()}) {
        const int flags = fcntl(descriptor, F_GETFD);
        if (flags < 0 || fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0)
            return MapProcessError(errno);
    }
    if (childReads) {
        child->reset(read.release());
        parent->reset(write.release());
    } else {
        child->reset(write.release());
        parent->reset(read.release());
    }
    return FLECHA_RUNTIME_PROCESS_OK;
}

int32_t OpenPosixNull(bool input, ScopedDescriptor* output) {
    int descriptor;
    do {
        descriptor = open("/dev/null", input ? O_RDONLY : O_WRONLY);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) return MapProcessError(errno);
    descriptor = NormalizeDescriptor(descriptor);
    if (descriptor < 0) return MapProcessError(errno);
    output->reset(descriptor);
    return FLECHA_RUNTIME_PROCESS_OK;
}

int32_t PreparePosixStream(uint8_t mode, bool input, ScopedDescriptor* child,
                           ScopedDescriptor* parent) {
    if (mode == FLECHA_RUNTIME_PROCESS_STDIO_PIPED)
        return CreatePosixPipe(input, child, parent);
    if (mode == FLECHA_RUNTIME_PROCESS_STDIO_NULL)
        return OpenPosixNull(input, child);
    return FLECHA_RUNTIME_PROCESS_OK;
}

int32_t AddPosixStreamActions(posix_spawn_file_actions_t* actions, uint8_t mode,
                              int target, const ScopedDescriptor& child,
                              const ScopedDescriptor& parent) {
    if (mode == FLECHA_RUNTIME_PROCESS_STDIO_INHERIT)
        return FLECHA_RUNTIME_PROCESS_OK;
    int error = posix_spawn_file_actions_adddup2(actions, child.get(), target);
    if (error == 0)
        error = posix_spawn_file_actions_addclose(actions, child.get());
    if (error == 0 && parent.get() >= 0)
        error = posix_spawn_file_actions_addclose(actions, parent.get());
    return error == 0 ? FLECHA_RUNTIME_PROCESS_OK : MapProcessError(error);
}

ssize_t WriteWithoutSigpipe(int descriptor, const uint8_t* source,
                            size_t length, int* outputError) {
    sigset_t blocked;
    sigset_t previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    const int maskStatus = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (maskStatus != 0) {
        *outputError = maskStatus;
        return -1;
    }

    sigset_t pending;
    sigpending(&pending);
    const bool alreadyPending = sigismember(&pending, SIGPIPE) == 1;
    ssize_t result;
    do {
        result = write(descriptor, source, length);
    } while (result < 0 && errno == EINTR);
    *outputError = result < 0 ? errno : 0;

    if (result < 0 && *outputError == EPIPE && !alreadyPending) {
        int signalNumber = 0;
        (void)sigwait(&blocked, &signalNumber);
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    return result;
}

#endif

}  // namespace

extern "C" {

void flecha_runtime_v1_process_arguments_initialize(int32_t count,
                                                    const char* const* values) {
    if (count <= 0 || values == nullptr) {
        argumentCount = 0;
        argumentValues = nullptr;
        return;
    }

    argumentCount = static_cast<size_t>(count);
    argumentValues = values;
}

size_t flecha_runtime_v1_process_argument_count(void) { return argumentCount; }

int32_t flecha_runtime_v1_process_argument_capture(FlechaRuntimeString* output,
                                                   size_t index) {
    if (output == nullptr) return FLECHA_RUNTIME_PROCESS_ARGUMENT_INVALID_INDEX;
    *output = {nullptr, 0, 0};

    if (index >= argumentCount || argumentValues == nullptr ||
        argumentValues[index] == nullptr)
        return FLECHA_RUNTIME_PROCESS_ARGUMENT_INVALID_INDEX;

    const char* value = argumentValues[index];
    const size_t length = std::strlen(value);
    const int32_t status = flecha_runtime_v1_string_try_from_bytes(
        output, reinterpret_cast<const uint8_t*>(value), length);
    return status == FLECHA_RUNTIME_STRING_OK
               ? FLECHA_RUNTIME_PROCESS_ARGUMENT_OK
               : FLECHA_RUNTIME_PROCESS_ARGUMENT_OUT_OF_MEMORY;
}

int32_t flecha_runtime_v1_process_spawn(
    void** processOutput, void** stdinOutput, void** stdoutOutput,
    void** stderrOutput, const char* executable, size_t executableLength,
    const uint8_t* argumentBytes, size_t argumentBytesLength,
    const size_t* argumentOffsets, size_t argumentCount, uint8_t stdinMode,
    uint8_t stdoutMode, uint8_t stderrMode) {
    if (processOutput == nullptr || stdinOutput == nullptr ||
        stdoutOutput == nullptr || stderrOutput == nullptr)
        return FLECHA_RUNTIME_PROCESS_INVALID_ARGUMENT;
    *processOutput = nullptr;
    *stdinOutput = nullptr;
    *stdoutOutput = nullptr;
    *stderrOutput = nullptr;

    const int32_t validation = ValidateArguments(
        executable, executableLength, argumentBytes, argumentBytesLength,
        argumentOffsets, argumentCount, stdinMode, stdoutMode, stderrMode);
    if (validation != FLECHA_RUNTIME_PROCESS_OK) return validation;

    try {
        auto process =
            std::unique_ptr<ProcessHandle>(new (std::nothrow) ProcessHandle{});
        std::unique_ptr<ProcessPipeHandle> input;
        std::unique_ptr<ProcessPipeHandle> output;
        std::unique_ptr<ProcessPipeHandle> error;
        if (!process) return FLECHA_RUNTIME_PROCESS_OUT_OF_MEMORY;
        if (stdinMode == FLECHA_RUNTIME_PROCESS_STDIO_PIPED)
            input.reset(new (std::nothrow) ProcessPipeHandle{
                ProcessPipeHandle::Magic, InvalidPipe, PipeDirection::Write});
        if (stdoutMode == FLECHA_RUNTIME_PROCESS_STDIO_PIPED)
            output.reset(new (std::nothrow) ProcessPipeHandle{
                ProcessPipeHandle::Magic, InvalidPipe, PipeDirection::Read});
        if (stderrMode == FLECHA_RUNTIME_PROCESS_STDIO_PIPED)
            error.reset(new (std::nothrow) ProcessPipeHandle{
                ProcessPipeHandle::Magic, InvalidPipe, PipeDirection::Read});
        if ((stdinMode == FLECHA_RUNTIME_PROCESS_STDIO_PIPED && !input) ||
            (stdoutMode == FLECHA_RUNTIME_PROCESS_STDIO_PIPED && !output) ||
            (stderrMode == FLECHA_RUNTIME_PROCESS_STDIO_PIPED && !error))
            return FLECHA_RUNTIME_PROCESS_OUT_OF_MEMORY;

#ifdef _WIN32
        std::wstring executableWide;
        int32_t status =
            Utf8ToWide(executable, executableLength, &executableWide);
        if (status != FLECHA_RUNTIME_PROCESS_OK) return status;
        std::wstring resolved;
        status = ResolveExecutable(executableWide, &resolved);
        if (status != FLECHA_RUNTIME_PROCESS_OK) return status;

        std::wstring commandLine;
        AppendQuotedWindowsArgument(executableWide, &commandLine);
        const char* argumentData =
            argumentBytes == nullptr
                ? ""
                : reinterpret_cast<const char*>(argumentBytes);
        for (size_t index = 0; index < argumentCount; ++index) {
            std::wstring argument;
            const size_t begin = argumentOffsets[index];
            status = Utf8ToWide(argumentData + begin,
                                argumentOffsets[index + 1] - begin, &argument);
            if (status != FLECHA_RUNTIME_PROCESS_OK) return status;
            AppendQuotedWindowsArgument(argument, &commandLine);
        }
        std::vector<wchar_t> mutableCommand(commandLine.begin(),
                                            commandLine.end());
        mutableCommand.push_back(L'\0');

        ScopedHandle childInput;
        ScopedHandle parentInput;
        ScopedHandle childOutput;
        ScopedHandle parentOutput;
        ScopedHandle childError;
        ScopedHandle parentError;
        status = PrepareWindowsStream(stdinMode, true, STD_INPUT_HANDLE,
                                      &childInput, &parentInput);
        if (status == FLECHA_RUNTIME_PROCESS_OK)
            status = PrepareWindowsStream(stdoutMode, false, STD_OUTPUT_HANDLE,
                                          &childOutput, &parentOutput);
        if (status == FLECHA_RUNTIME_PROCESS_OK)
            status = PrepareWindowsStream(stderrMode, false, STD_ERROR_HANDLE,
                                          &childError, &parentError);
        if (status != FLECHA_RUNTIME_PROCESS_OK) return status;

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = childInput.get();
        startup.hStdOutput = childOutput.get();
        startup.hStdError = childError.get();
        PROCESS_INFORMATION information{};
        if (!CreateProcessW(resolved.c_str(), mutableCommand.data(), nullptr,
                            nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT, nullptr,
                            nullptr, &startup, &information))
            return MapProcessError(GetLastError());
        CloseHandle(information.hThread);
        process->process = information.hProcess;
        if (input) input->pipe = parentInput.release();
        if (output) output->pipe = parentOutput.release();
        if (error) error->pipe = parentError.release();
#else
        std::vector<std::string> arguments;
        arguments.reserve(argumentCount + 1);
        arguments.emplace_back(executable, executableLength);
        const char* argumentData =
            argumentBytes == nullptr
                ? ""
                : reinterpret_cast<const char*>(argumentBytes);
        for (size_t index = 0; index < argumentCount; ++index) {
            const size_t begin = argumentOffsets[index];
            arguments.emplace_back(argumentData + begin,
                                   argumentOffsets[index + 1] - begin);
        }
        std::vector<char*> nativeArguments;
        nativeArguments.reserve(arguments.size() + 1);
        for (std::string& argument : arguments)
            nativeArguments.push_back(argument.data());
        nativeArguments.push_back(nullptr);

        ScopedDescriptor childInput;
        ScopedDescriptor parentInput;
        ScopedDescriptor childOutput;
        ScopedDescriptor parentOutput;
        ScopedDescriptor childError;
        ScopedDescriptor parentError;
        int32_t status =
            PreparePosixStream(stdinMode, true, &childInput, &parentInput);
        if (status == FLECHA_RUNTIME_PROCESS_OK)
            status = PreparePosixStream(stdoutMode, false, &childOutput,
                                        &parentOutput);
        if (status == FLECHA_RUNTIME_PROCESS_OK)
            status = PreparePosixStream(stderrMode, false, &childError,
                                        &parentError);
        if (status != FLECHA_RUNTIME_PROCESS_OK) return status;

        posix_spawn_file_actions_t actions;
        int nativeStatus = posix_spawn_file_actions_init(&actions);
        if (nativeStatus != 0) return MapProcessError(nativeStatus);
        status = AddPosixStreamActions(&actions, stdinMode, STDIN_FILENO,
                                       childInput, parentInput);
        if (status == FLECHA_RUNTIME_PROCESS_OK)
            status = AddPosixStreamActions(&actions, stdoutMode, STDOUT_FILENO,
                                           childOutput, parentOutput);
        if (status == FLECHA_RUNTIME_PROCESS_OK)
            status = AddPosixStreamActions(&actions, stderrMode, STDERR_FILENO,
                                           childError, parentError);
        if (status == FLECHA_RUNTIME_PROCESS_OK) {
            pid_t child = -1;
            nativeStatus =
                posix_spawnp(&child, arguments[0].c_str(), &actions, nullptr,
                             nativeArguments.data(), environ);
            if (nativeStatus == 0)
                process->process = child;
            else
                status = MapProcessError(nativeStatus);
        }
        posix_spawn_file_actions_destroy(&actions);
        if (status != FLECHA_RUNTIME_PROCESS_OK) return status;

        if (input) input->pipe = parentInput.release();
        if (output) output->pipe = parentOutput.release();
        if (error) error->pipe = parentError.release();
#endif

        *processOutput = process.release();
        *stdinOutput = input.release();
        *stdoutOutput = output.release();
        *stderrOutput = error.release();
        return FLECHA_RUNTIME_PROCESS_OK;
    } catch (const std::bad_alloc&) {
        return FLECHA_RUNTIME_PROCESS_OUT_OF_MEMORY;
    } catch (...) {
        return FLECHA_RUNTIME_PROCESS_FAILED;
    }
}

int32_t flecha_runtime_v1_process_wait(void* opaque, uint8_t* terminationKind,
                                       uint32_t* terminationDetail) {
    if (terminationKind == nullptr || terminationDetail == nullptr)
        return FLECHA_RUNTIME_PROCESS_INVALID_ARGUMENT;
    *terminationKind = FLECHA_RUNTIME_PROCESS_TERMINATION_ABNORMAL;
    *terminationDetail = 0;
    ProcessHandle* handle = GetProcessHandle(opaque);
    if (handle == nullptr || handle->waited)
        return FLECHA_RUNTIME_PROCESS_CLOSED;

#ifdef _WIN32
    const DWORD waitResult = WaitForSingleObject(handle->process, INFINITE);
    if (waitResult != WAIT_OBJECT_0) return MapProcessError(GetLastError());
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(handle->process, &exitCode))
        return MapProcessError(GetLastError());
    handle->waited = true;
    *terminationDetail = exitCode;
    *terminationKind = (exitCode & UINT32_C(0xc0000000)) == UINT32_C(0xc0000000)
                           ? FLECHA_RUNTIME_PROCESS_TERMINATION_ABNORMAL
                           : FLECHA_RUNTIME_PROCESS_TERMINATION_EXITED;
#else
    int nativeStatus = 0;
    pid_t result;
    do {
        result = waitpid(handle->process, &nativeStatus, 0);
    } while (result < 0 && errno == EINTR);
    if (result < 0) return MapProcessError(errno);
    handle->waited = true;
    if (WIFEXITED(nativeStatus)) {
        *terminationKind = FLECHA_RUNTIME_PROCESS_TERMINATION_EXITED;
        *terminationDetail = static_cast<uint32_t>(WEXITSTATUS(nativeStatus));
    } else if (WIFSIGNALED(nativeStatus)) {
        *terminationKind = FLECHA_RUNTIME_PROCESS_TERMINATION_SIGNALED;
        *terminationDetail = static_cast<uint32_t>(WTERMSIG(nativeStatus));
    } else {
        *terminationKind = FLECHA_RUNTIME_PROCESS_TERMINATION_ABNORMAL;
        *terminationDetail = static_cast<uint32_t>(nativeStatus);
    }
#endif
    return FLECHA_RUNTIME_PROCESS_OK;
}

int32_t flecha_runtime_v1_process_close(void* opaque) {
    ProcessHandle* handle = GetProcessHandle(opaque);
    if (handle == nullptr) return FLECHA_RUNTIME_PROCESS_CLOSED;
    int32_t status = FLECHA_RUNTIME_PROCESS_OK;
#ifdef _WIN32
    if (!CloseHandle(handle->process)) status = MapProcessError(GetLastError());
#else
    if (!handle->waited) {
        int nativeStatus = 0;
        pid_t result;
        do {
            result = waitpid(handle->process, &nativeStatus, 0);
        } while (result < 0 && errno == EINTR);
        if (result < 0) status = MapProcessError(errno);
    }
#endif
    handle->process = InvalidProcess;
    handle->magic = 0;
    delete handle;
    return status;
}

int32_t flecha_runtime_v1_process_pipe_read(void* opaque, uint8_t* destination,
                                            size_t capacity,
                                            size_t* bytesRead) {
    if (bytesRead == nullptr) return FLECHA_RUNTIME_PROCESS_INVALID_ARGUMENT;
    *bytesRead = 0;
    ProcessPipeHandle* handle = GetPipeHandle(opaque, PipeDirection::Read);
    if (handle == nullptr) return FLECHA_RUNTIME_PROCESS_CLOSED;
    if (destination == nullptr && capacity != 0)
        return FLECHA_RUNTIME_PROCESS_INVALID_ARGUMENT;
    if (capacity == 0) return FLECHA_RUNTIME_PROCESS_OK;

#ifdef _WIN32
    DWORD count = 0;
    const DWORD requested = static_cast<DWORD>(
        std::min<size_t>(capacity, std::numeric_limits<DWORD>::max()));
    if (!ReadFile(handle->pipe, destination, requested, &count, nullptr)) {
        const DWORD error = GetLastError();
        *bytesRead = count;
        if (error == ERROR_BROKEN_PIPE) return FLECHA_RUNTIME_PROCESS_OK;
        return MapProcessError(error);
    }
    *bytesRead = count;
#else
    ssize_t result;
    do {
        result = read(handle->pipe, destination,
                      std::min<size_t>(capacity, SSIZE_MAX));
    } while (result < 0 && errno == EINTR);
    if (result < 0) return MapProcessError(errno);
    *bytesRead = static_cast<size_t>(result);
#endif
    return FLECHA_RUNTIME_PROCESS_OK;
}

int32_t flecha_runtime_v1_process_pipe_write(void* opaque,
                                             const uint8_t* source,
                                             size_t length,
                                             size_t* bytesWritten) {
    if (bytesWritten == nullptr) return FLECHA_RUNTIME_PROCESS_INVALID_ARGUMENT;
    *bytesWritten = 0;
    ProcessPipeHandle* handle = GetPipeHandle(opaque, PipeDirection::Write);
    if (handle == nullptr) return FLECHA_RUNTIME_PROCESS_CLOSED;
    if (source == nullptr && length != 0)
        return FLECHA_RUNTIME_PROCESS_INVALID_ARGUMENT;
    if (length == 0) return FLECHA_RUNTIME_PROCESS_OK;

#ifdef _WIN32
    DWORD count = 0;
    const DWORD requested = static_cast<DWORD>(
        std::min<size_t>(length, std::numeric_limits<DWORD>::max()));
    if (!WriteFile(handle->pipe, source, requested, &count, nullptr)) {
        *bytesWritten = count;
        return MapProcessError(GetLastError());
    }
    *bytesWritten = count;
#else
    int error = 0;
    const ssize_t result = WriteWithoutSigpipe(
        handle->pipe, source, std::min<size_t>(length, SSIZE_MAX), &error);
    if (result < 0) return MapProcessError(error);
    *bytesWritten = static_cast<size_t>(result);
#endif
    return FLECHA_RUNTIME_PROCESS_OK;
}

int32_t flecha_runtime_v1_process_pipe_close(void* opaque) {
    if (opaque == nullptr) return FLECHA_RUNTIME_PROCESS_CLOSED;
    auto* handle = static_cast<ProcessPipeHandle*>(opaque);
    if (handle->magic != ProcessPipeHandle::Magic ||
        handle->pipe == InvalidPipe)
        return FLECHA_RUNTIME_PROCESS_CLOSED;

    int32_t status = FLECHA_RUNTIME_PROCESS_OK;
#ifdef _WIN32
    if (!CloseHandle(handle->pipe)) status = MapProcessError(GetLastError());
#else
    const int result = close(handle->pipe);
    if (result < 0) status = MapProcessError(errno);
#endif
    handle->pipe = InvalidPipe;
    handle->magic = 0;
    delete handle;
    return status;
}

}  // extern "C"
