#include <cstddef>
#include <cstdint>
#include <limits>

extern "C" {
std::int32_t exercise_arrays();
std::int32_t exercise_array_evaluation();
std::uint8_t array_read(std::size_t index);
std::uint8_t array_write(std::size_t index);
std::uint8_t array_borrow(std::size_t index);
std::uint8_t array_unsafe_read(std::size_t index);
std::uint8_t array_nested_read(std::size_t row, std::size_t column);
std::uint8_t array_store_before_rhs(std::size_t index);
std::uint8_t owned_array_read(std::size_t length, std::size_t index);
std::uint8_t string_view_read(std::int32_t index);
}

int main(int argc, char** argv) {
    if (argc > 1) {
        switch (argv[1][0]) {
            case 'r':
                (void)array_read(2);
                break;
            case 'w':
                (void)array_write(2);
                break;
            case 'b':
                (void)array_borrow(2);
                break;
            case 'u':
                (void)array_unsafe_read(2);
                break;
            case 'm':
                (void)array_read(std::numeric_limits<std::size_t>::max());
                break;
            case 'n':
                (void)array_nested_read(1, 2);
                break;
            case 's':
                (void)array_store_before_rhs(2);
                break;
            case 'z':
                (void)owned_array_read(0, 0);
                break;
            case 'g':
                (void)owned_array_read(2, 3);
                break;
            case 'q':
                (void)string_view_read(-1);
                break;
        }
        return 99;  // Reaching here means the required bounds failure did not
                    // run.
    }
    if (exercise_arrays() != 42) return 1;
    if (exercise_array_evaluation() != 42) return 2;
    if (array_read(0) != 7 || array_read(1) != 8) return 3;
    if (array_write(1) != 42 || array_borrow(1) != 8) return 4;
    if (array_unsafe_read(0) != 7 || array_nested_read(1, 1) != 4) return 5;
    return 42;
}
