#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iterator>

extern "C" void* __real_flecha_runtime_v1_alloc_array(std::size_t count,
                                                      std::size_t size,
                                                      std::size_t alignment);
extern "C" void* __real_realloc(void* data, std::size_t size);

namespace {

bool failNextArrayAllocation = false;
bool failNextReallocation = false;
std::size_t clockCalls = 0;

}  // namespace

extern "C" void benchmark_fail_next_array_allocation() {
    failNextArrayAllocation = true;
}

extern "C" void benchmark_fail_next_realloc() { failNextReallocation = true; }

extern "C" void* __wrap_flecha_runtime_v1_alloc_array(std::size_t count,
                                                      std::size_t size,
                                                      std::size_t alignment) {
    if (failNextArrayAllocation) {
        failNextArrayAllocation = false;
        return nullptr;
    }
    return __real_flecha_runtime_v1_alloc_array(count, size, alignment);
}

extern "C" void* __wrap_realloc(void* data, std::size_t size) {
    if (failNextReallocation) {
        failNextReallocation = false;
        return nullptr;
    }
    return __real_realloc(data, size);
}

extern "C" std::int32_t __wrap_flecha_runtime_v1_monotonic_clock_now(
    std::uint64_t* seconds, std::uint32_t* nanoseconds) {
    constexpr std::uint32_t OBSERVATIONS[] = {100, 130, 200, 260};
    if (seconds == nullptr || nanoseconds == nullptr ||
        clockCalls >= std::size(OBSERVATIONS))
        return 1;
    *seconds = 0;
    *nanoseconds = OBSERVATIONS[clockCalls++];
    return 0;
}

extern "C" int sample_storage_allocation_failure_test() asm(
    "$m0$sample_storage_allocation_failure_test");
extern "C" int supplied_sample_allocation_failure_test() asm(
    "$m0$supplied_sample_allocation_failure_test");
extern "C" int machine_output_allocation_failure_test() asm(
    "$m0$machine_output_allocation_failure_test");
extern "C" int deterministic_clock_overhead_test() asm(
    "$m0$deterministic_clock_overhead_test");

int main() {
    int index = 0;
    for (const auto test : {
             sample_storage_allocation_failure_test,
             supplied_sample_allocation_failure_test,
             machine_output_allocation_failure_test,
             deterministic_clock_overhead_test,
         }) {
        ++index;
        if (test() != 42 || failNextArrayAllocation || failNextReallocation)
            return index;
    }
    if (clockCalls != 4) return 5;
    return 42;
}
