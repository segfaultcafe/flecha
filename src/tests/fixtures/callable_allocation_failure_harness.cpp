#include <cstddef>

extern "C" void callable_allocation_failure_test();

extern "C" void* __wrap_flecha_runtime_v1_alloc(std::size_t, std::size_t) {
    return nullptr;
}

int main() {
    callable_allocation_failure_test();
    return 1;
}
