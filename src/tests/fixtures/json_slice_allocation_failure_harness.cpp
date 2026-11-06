#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <set>

extern "C" void* __real_flecha_runtime_v1_alloc_array(std::size_t, std::size_t,
                                                      std::size_t);
extern "C" void* __real_flecha_runtime_v1_alloc(std::size_t, std::size_t);
extern "C" void __real_flecha_runtime_v1_free(void*);

#ifndef FLECHA_ARRAY_FAIL_CALL
#define FLECHA_ARRAY_FAIL_CALL 1
#endif

namespace {
struct AllocationAudit final {
    std::set<void*> live;
    unsigned arrayCalls = 0;
    unsigned failures = 0;

    ~AllocationAudit() {
        if (!live.empty() || failures != 1) {
            std::fprintf(stderr,
                         "JSON slice allocation cleanup audit failed\n");
            std::_Exit(90);
        }
    }
} audit;
}  // namespace

extern "C" void* __wrap_flecha_runtime_v1_alloc_array(std::size_t count,
                                                      std::size_t size,
                                                      std::size_t alignment) {
    if (++audit.arrayCalls == FLECHA_ARRAY_FAIL_CALL) {
        ++audit.failures;
        return nullptr;
    }
    void* data = __real_flecha_runtime_v1_alloc_array(count, size, alignment);
    if (data) audit.live.insert(data);
    return data;
}

extern "C" void* __wrap_flecha_runtime_v1_alloc(std::size_t size,
                                                std::size_t alignment) {
    void* data = __real_flecha_runtime_v1_alloc(size, alignment);
    if (data) audit.live.insert(data);
    return data;
}

extern "C" void __wrap_flecha_runtime_v1_free(void* data) {
    if (data && audit.live.erase(data) != 1) {
        std::fprintf(stderr,
                     "JSON slice freed invalid or already freed storage\n");
        std::_Exit(91);
    }
    __real_flecha_runtime_v1_free(data);
}
