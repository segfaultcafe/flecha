#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>

#include "runtime/runtime_abi.h"

namespace {
struct AllocationHeader {
    void* base;
    size_t count;
};
}  // namespace

extern "C" void* flecha_runtime_v1_alloc(size_t size, size_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
    alignment = std::max(alignment, alignof(AllocationHeader));
    size = std::max(size, size_t{1});
    const size_t maximum = std::numeric_limits<size_t>::max();
    if (alignment - 1 > maximum - sizeof(AllocationHeader)) return nullptr;
    const size_t overhead = sizeof(AllocationHeader) + alignment - 1;
    if (size > maximum - overhead) return nullptr;
    void* base = std::malloc(size + overhead);
    if (base == nullptr) return nullptr;
    void* data = static_cast<char*>(base) + sizeof(AllocationHeader);
    size_t available = size + alignment - 1;
    if (std::align(alignment, size, data, available) == nullptr) {
        std::free(base);
        return nullptr;
    }
    static_cast<AllocationHeader*>(data)[-1] = {base, 1};
    std::memset(data, 0, size);
    return data;
}

extern "C" void flecha_runtime_v1_allocation_failure(void) {
    std::fputs("flecha runtime error: allocation failed\n", stderr);
    std::abort();
}

extern "C" void flecha_runtime_v1_free(void* data) {
    if (data != nullptr)
        std::free(static_cast<AllocationHeader*>(data)[-1].base);
}

extern "C" void* flecha_runtime_v1_alloc_array(size_t count,
                                               size_t element_size,
                                               size_t alignment) {
    if (element_size != 0 &&
        count > std::numeric_limits<size_t>::max() / element_size)
        return nullptr;
    void* data = flecha_runtime_v1_alloc(count * element_size, alignment);
    if (data) static_cast<AllocationHeader*>(data)[-1].count = count;
    return data;
}

extern "C" size_t flecha_runtime_v1_allocation_count(const void* data) {
    return data ? static_cast<const AllocationHeader*>(data)[-1].count : 0;
}
