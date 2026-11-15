#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <set>
extern "C" void* __real_flecha_runtime_v1_alloc_array(std::size_t, std::size_t,
                                                      std::size_t);
extern "C" void* __real_flecha_runtime_v1_alloc(std::size_t, std::size_t);
extern "C" void __real_flecha_runtime_v1_free(void*);
struct Audit {
    std::set<void*> live;
    unsigned calls = 0, failures = 0;
    unsigned failAt =
        std::getenv("FAIL_AT") ? std::atoi(std::getenv("FAIL_AT")) : 0;
    bool Fail() {
        if (++calls == failAt) {
            ++failures;
            return true;
        }
        return false;
    }
    ~Audit() {
        if (!live.empty() || failures != (failAt != 0)) {
            std::fprintf(stderr,
                         "audit failed: %zu live, %u failures at %u of %u\n",
                         live.size(), failures, failAt, calls);
            std::_Exit(90);
        }
        if (!failAt) std::fprintf(stderr, "allocation_calls=%u\n", calls);
    }
} audit;
extern "C" void* __wrap_flecha_runtime_v1_alloc_array(std::size_t count,
                                                      std::size_t size,
                                                      std::size_t alignment) {
    if (audit.Fail()) return nullptr;
    void* ptr = __real_flecha_runtime_v1_alloc_array(count, size, alignment);
    if (ptr) audit.live.insert(ptr);
    return ptr;
}
extern "C" void* __wrap_flecha_runtime_v1_alloc(std::size_t size,
                                                std::size_t alignment) {
    if (audit.Fail()) return nullptr;
    void* ptr = __real_flecha_runtime_v1_alloc(size, alignment);
    if (ptr) audit.live.insert(ptr);
    return ptr;
}
extern "C" void __wrap_flecha_runtime_v1_free(void* ptr) {
    if (ptr && audit.live.erase(ptr) != 1) {
        std::fprintf(stderr, "invalid free\n");
        std::_Exit(91);
    }
    __real_flecha_runtime_v1_free(ptr);
}
