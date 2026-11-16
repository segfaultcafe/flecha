#include <cstdio>
#include <initializer_list>

extern "C" int error_comparison_test();
extern "C" int error_tuple_test();
extern "C" int error_storage_test();
extern "C" int error_defer_test();
extern "C" int error_try_test();

int main() {
    for (const auto test :
         {error_comparison_test, error_tuple_test, error_storage_test,
          error_defer_test, error_try_test}) {
        const int result = test();
        if (result != 42) {
            std::fprintf(stderr, "error value test failed at check %d\n",
                         result);
            return result;
        }
    }
    return 42;
}
