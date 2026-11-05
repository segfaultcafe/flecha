#include <cstddef>

extern "C" void* __real_realloc(void*, std::size_t);

#ifndef FLECHA_STRING_FAIL_CALL
#define FLECHA_STRING_FAIL_CALL 1
#endif

namespace {
unsigned allocationsUntilFailure = 0;
}

extern "C" void socket_address_begin_allocation_failure() {
    allocationsUntilFailure = FLECHA_STRING_FAIL_CALL;
}

extern "C" void* __wrap_realloc(void* allocation, std::size_t size) {
    if (allocationsUntilFailure != 0) {
        --allocationsUntilFailure;
        if (allocationsUntilFailure == 0) return nullptr;
    }
    return __real_realloc(allocation, size);
}
