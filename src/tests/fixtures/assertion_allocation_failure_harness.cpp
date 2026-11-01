#include <cstddef>
#include <cstdio>
#include <initializer_list>

extern "C" int byte_content_assertion_allocation_failure_test() asm(
    "$m0$byte_content_assertion_allocation_failure_test");
extern "C" int byte_inequality_assertion_allocation_failure_test() asm(
    "$m0$byte_inequality_assertion_allocation_failure_test");
extern "C" int byte_length_assertion_allocation_failure_test() asm(
    "$m0$byte_length_assertion_allocation_failure_test");
extern "C" int numeric_assertion_allocation_failure_test() asm(
    "$m0$numeric_assertion_allocation_failure_test");
extern "C" int string_assertion_allocation_failure_test() asm(
    "$m0$string_assertion_allocation_failure_test");
extern "C" void* __real_flecha_runtime_v1_alloc_array(std::size_t, std::size_t,
                                                      std::size_t);
extern "C" void* __real_realloc(void*, std::size_t);

namespace {
bool failNext = false;
}

extern "C" void assertion_allocation_fail_next() { failNext = true; }

extern "C" void* __wrap_flecha_runtime_v1_alloc_array(std::size_t count,
                                                      std::size_t size,
                                                      std::size_t alignment) {
    if (failNext) {
        failNext = false;
        return nullptr;
    }
    return __real_flecha_runtime_v1_alloc_array(count, size, alignment);
}

extern "C" void* __wrap_realloc(void* data, std::size_t size) {
    if (failNext) {
        failNext = false;
        return nullptr;
    }
    return __real_realloc(data, size);
}

int main() {
    int index = 0;
    for (const auto test : {
             numeric_assertion_allocation_failure_test,
             string_assertion_allocation_failure_test,
             byte_length_assertion_allocation_failure_test,
             byte_content_assertion_allocation_failure_test,
             byte_inequality_assertion_allocation_failure_test,
         }) {
        ++index;
        const int result = test();
        if (result != 42 || failNext) {
            std::fprintf(stderr,
                         "assertion allocation test %d failed at check %d\n",
                         index, result);
            return index;
        }
    }
    return 42;
}
