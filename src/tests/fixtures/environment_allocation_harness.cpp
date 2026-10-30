#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "runtime/runtime_abi.h"

extern "C" void* __real_malloc(std::size_t size);

namespace {

std::size_t allocationIndex = 0;
std::size_t failedAllocationIndex = 0;

constexpr char Name[] = "FLECHA_RUNTIME_ENVIRONMENT_ALLOCATION";
constexpr std::size_t NameLength = sizeof(Name) - 1;

void FailAllocation(std::size_t index) {
    allocationIndex = 0;
    failedAllocationIndex = index;
}

void AllowAllocations() { failedAllocationIndex = 0; }

bool ValueEquals(const char* expected) {
    const char* value = std::getenv(Name);
    return value != nullptr && std::strcmp(value, expected) == 0;
}

int Cleanup(int result) {
    AllowAllocations();
    (void)::unsetenv(Name);
    return result;
}

}  // namespace

extern "C" void* __wrap_malloc(std::size_t size) {
    ++allocationIndex;
    if (allocationIndex == failedAllocationIndex) return nullptr;
    return __real_malloc(size);
}

int main() {
    if (::setenv(Name, "preserved", 1) != 0) return 1;

    FlechaRuntimeString output{reinterpret_cast<char*>(1), 1, 1};
    uint8_t present = 1;
    FailAllocation(1);
    int32_t status =
        flecha_runtime_v1_environment_get(&output, &present, Name, NameLength);
    AllowAllocations();
    if (status != FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY ||
        output.data != nullptr || output.length != 0 || output.capacity != 0 ||
        present != 0 || !ValueEquals("preserved"))
        return Cleanup(2);

    output = {reinterpret_cast<char*>(1), 1, 1};
    present = 1;
    FailAllocation(2);
    status =
        flecha_runtime_v1_environment_get(&output, &present, Name, NameLength);
    AllowAllocations();
    if (status != FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY ||
        output.data != nullptr || output.length != 0 || output.capacity != 0 ||
        present != 0 || !ValueEquals("preserved"))
        return Cleanup(3);

    FailAllocation(1);
    status = flecha_runtime_v1_environment_set(Name, NameLength, "replacement",
                                               sizeof("replacement") - 1);
    AllowAllocations();
    if (status != FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY ||
        !ValueEquals("preserved"))
        return Cleanup(4);

    FailAllocation(2);
    status = flecha_runtime_v1_environment_set(Name, NameLength, "replacement",
                                               sizeof("replacement") - 1);
    AllowAllocations();
    if (status != FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY ||
        !ValueEquals("preserved"))
        return Cleanup(5);

    FailAllocation(1);
    status = flecha_runtime_v1_environment_remove(Name, NameLength);
    AllowAllocations();
    if (status != FLECHA_RUNTIME_ENVIRONMENT_OUT_OF_MEMORY ||
        !ValueEquals("preserved"))
        return Cleanup(6);

    return Cleanup(0);
}
