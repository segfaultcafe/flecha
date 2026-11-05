#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {

int ParseCode(const char* value) {
    char* end = nullptr;
    errno = 0;
    const long code = std::strtol(value, &end, 10);
    return errno == 0 && end != value && *end == '\0' ? static_cast<int>(code)
                                                      : 125;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 124;
    const std::string mode(argv[1]);
    if (mode == "arguments") {
        for (int index = 2; index < argc; ++index)
            std::printf("%zu:%s\n", std::strlen(argv[index]), argv[index]);
        return std::ferror(stdout) ? 123 : 0;
    }
    if (mode == "exit" && argc == 3) return ParseCode(argv[2]);
    if (mode == "large" && argc == 3) {
        const int length = ParseCode(argv[2]);
        for (int index = 0; index < length; ++index) {
            if (std::fputc('a' + index % 26, stdout) == EOF) return 122;
        }
        return 0;
    }
    if (mode == "copy") {
        char bytes[257];
        for (;;) {
            const size_t count = std::fread(bytes, 1, sizeof(bytes), stdin);
            if (count != 0 && std::fwrite(bytes, 1, count, stdout) != count)
                return 121;
            if (count != sizeof(bytes)) return std::ferror(stdin) ? 120 : 0;
        }
    }
    if (mode == "both") {
        if (std::fputs("standard output", stdout) == EOF ||
            std::fputs("standard error", stderr) == EOF)
            return 117;
        return 0;
    }
#ifndef _WIN32
    if (mode == "signal") {
        std::raise(SIGTERM);
        return 119;
    }
#else
    if (mode == "abnormal") {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        return 119;
    }
#endif
    return 118;
}
