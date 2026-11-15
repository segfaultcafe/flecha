#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int test_output_should_fail() {
    return std::getenv("FLECHA_OUTPUT_FAIL") != nullptr;
}

extern "C" int test_output_is_visible() {
    const char* path = std::getenv("FLECHA_OUTPUT_LOG");
    if (!path) return 0;
    auto* file = std::fopen(path, "rb");
    if (!file) return 0;
    char buffer[8192]{};
    const auto size = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    buffer[size] = '\0';
    // The second test observes redirected stdout while the harness is still
    // running. Deferred reporting or missing fflush both fail this check.
    return std::strstr(buffer, "first_result") &&
           std::strstr(buffer, "log: completed first test") &&
           !std::strstr(buffer, "tests passed");
}
