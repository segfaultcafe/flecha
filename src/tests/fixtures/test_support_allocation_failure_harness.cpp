#include <cstddef>
#include <cstdio>
#include <initializer_list>

extern "C" int fixture_child_path_allocation_failure_test() asm(
    "$m0$fixture_child_path_allocation_failure_test");
extern "C" int golden_byte_diagnostic_allocation_failure_test() asm(
    "$m0$golden_byte_diagnostic_allocation_failure_test");
extern "C" int golden_read_allocation_failure_test() asm(
    "$m0$golden_read_allocation_failure_test");
extern "C" int golden_text_diagnostic_allocation_failure_test() asm(
    "$m0$golden_text_diagnostic_allocation_failure_test");
extern "C" int random_seed_log_allocation_failure_test() asm(
    "$m0$random_seed_log_allocation_failure_test");

extern "C" void* __real_flecha_runtime_v1_alloc_array(std::size_t, std::size_t,
                                                      std::size_t);
extern "C" void* __real_malloc(std::size_t);
extern "C" void* __real_realloc(void*, std::size_t);

namespace {
bool failArrayAllocation = false;
bool failMalloc = false;
std::size_t reallocationsUntilFailure = 0;
}  // namespace

extern "C" void test_support_fail_next_array_allocation() {
    failArrayAllocation = true;
}

extern "C" void test_support_fail_next_malloc() { failMalloc = true; }

extern "C" void test_support_fail_next_realloc() {
    reallocationsUntilFailure = 1;
}

extern "C" void test_support_fail_second_realloc() {
    reallocationsUntilFailure = 2;
}

extern "C" void* __wrap_flecha_runtime_v1_alloc_array(std::size_t count,
                                                      std::size_t size,
                                                      std::size_t alignment) {
    if (failArrayAllocation) {
        failArrayAllocation = false;
        return nullptr;
    }
    return __real_flecha_runtime_v1_alloc_array(count, size, alignment);
}

extern "C" void* __wrap_malloc(std::size_t size) {
    if (failMalloc) {
        failMalloc = false;
        return nullptr;
    }
    return __real_malloc(size);
}

extern "C" void* __wrap_realloc(void* data, std::size_t size) {
    if (reallocationsUntilFailure != 0) {
        --reallocationsUntilFailure;
        if (reallocationsUntilFailure == 0) return nullptr;
    }
    return __real_realloc(data, size);
}

int main() {
    int index = 0;
    for (const auto test : {
             random_seed_log_allocation_failure_test,
             fixture_child_path_allocation_failure_test,
             golden_read_allocation_failure_test,
             golden_text_diagnostic_allocation_failure_test,
             golden_byte_diagnostic_allocation_failure_test,
         }) {
        ++index;
        const int result = test();
        if (result != 42 || failArrayAllocation || failMalloc ||
            reallocationsUntilFailure != 0) {
            std::fprintf(stderr,
                         "test support allocation case %d failed at check %d\n",
                         index, result);
            return index;
        }
    }
    return 42;
}
