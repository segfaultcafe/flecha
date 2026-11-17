#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
extern "C" int array_values_test();
extern "C" int array_zero_test();
extern "C" int array_overflow_test();
extern "C" int array_failure_test();
extern "C" int array_bool_test();
extern "C" int array_bounds_test(std::size_t);
extern "C" void* __real_flecha_runtime_v1_alloc_array(std::size_t, std::size_t,
                                                      std::size_t);
static bool failNext = false;
extern "C" void* __wrap_flecha_runtime_v1_alloc_array(std::size_t count,
                                                      std::size_t size,
                                                      std::size_t alignment) {
    if (failNext) {
        failNext = false;
        return nullptr;
    }
    return __real_flecha_runtime_v1_alloc_array(count, size, alignment);
}
int main() {
    for (const auto test : {array_values_test, array_zero_test,
                            array_overflow_test, array_bool_test})
        if (test() != 42) return 1;
    failNext = true;
    if (array_failure_test() != 42 || failNext) return 2;
    for (std::size_t count : {0, 5}) {
        const pid_t child = fork();
        if (child < 0) return 3;
        if (child == 0) {
            array_bounds_test(count);
            _exit(0);
        }
        int status = 0;
        if (waitpid(child, &status, 0) != child ||
            (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT))
            return 4;
    }
    return 42;
}
