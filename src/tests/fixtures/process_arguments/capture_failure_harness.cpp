#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "runtime/runtime_abi.h"

extern "C" void* __real_flecha_runtime_v1_alloc_array(std::size_t, std::size_t,
                                                      std::size_t);
extern "C" void __real_flecha_runtime_v1_free(void*);
extern "C" int32_t __real_flecha_runtime_v1_process_argument_capture(
    FlechaRuntimeString*, std::size_t);
extern "C" void __real_flecha_runtime_v1_string_clone(FlechaRuntimeString*,
                                                      const char*, std::size_t);
extern "C" void __real_flecha_runtime_v1_string_destroy(char*);

namespace {

struct Audit final {
    std::set<void*> arrays;
    std::set<char*> strings;
    unsigned captureCalls = 0;
    unsigned injectedFailures = 0;

    ~Audit() {
        if (!arrays.empty() || !strings.empty() || captureCalls != 2 ||
            injectedFailures != 1) {
            std::fputs(
                "partial process arguments were not cleaned exactly once\n",
                stderr);
            std::_Exit(90);
        }
    }
} audit;

}  // namespace

extern "C" void* __wrap_flecha_runtime_v1_alloc_array(std::size_t count,
                                                      std::size_t size,
                                                      std::size_t alignment) {
    void* data = __real_flecha_runtime_v1_alloc_array(count, size, alignment);
    if (data != nullptr) audit.arrays.insert(data);
    return data;
}

extern "C" void __wrap_flecha_runtime_v1_free(void* data) {
    if (data != nullptr && audit.arrays.erase(data) != 1) {
        std::fputs("process argument array was freed more than once\n", stderr);
        std::_Exit(91);
    }
    __real_flecha_runtime_v1_free(data);
}

extern "C" int32_t __wrap_flecha_runtime_v1_process_argument_capture(
    FlechaRuntimeString* output, std::size_t index) {
    ++audit.captureCalls;
    if (audit.captureCalls == 2) {
        ++audit.injectedFailures;
        *output = {nullptr, 0, 0};
        return FLECHA_RUNTIME_PROCESS_ARGUMENT_OUT_OF_MEMORY;
    }

    const int32_t status =
        __real_flecha_runtime_v1_process_argument_capture(output, index);
    if (status == FLECHA_RUNTIME_PROCESS_ARGUMENT_OK && output->data != nullptr)
        audit.strings.insert(output->data);
    return status;
}

extern "C" void __wrap_flecha_runtime_v1_string_clone(
    FlechaRuntimeString* output, const char* data, std::size_t length) {
    __real_flecha_runtime_v1_string_clone(output, data, length);
    if (output->data != nullptr) audit.strings.insert(output->data);
}

extern "C" void __wrap_flecha_runtime_v1_string_destroy(char* data) {
    if (data != nullptr && audit.strings.erase(data) != 1) {
        std::fputs("process argument string was destroyed more than once\n",
                   stderr);
        std::_Exit(92);
    }
    __real_flecha_runtime_v1_string_destroy(data);
}
