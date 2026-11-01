#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "runtime/runtime_abi.h"

#include <cstdio>
#include <cstdlib>

#if defined(__linux__) || defined(__APPLE__)
#include <execinfo.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <ucontext.h>
#endif

namespace {

void WriteText(const char* text, std::size_t length) {
    if (text != nullptr && length != 0)
        (void)std::fwrite(text, 1, length, stderr);
}

void PrintRegisters() {
    std::fputs("registers:\n", stderr);
#if defined(__linux__) && defined(__x86_64__)
    ucontext_t context{};
    if (getcontext(&context) != 0) {
        std::fputs("  unavailable\n", stderr);
        return;
    }
    const auto& registers = context.uc_mcontext.gregs;
    std::fprintf(stderr,
                 "  RIP=%#llx RSP=%#llx RBP=%#llx\n"
                 "  RAX=%#llx RBX=%#llx RCX=%#llx RDX=%#llx\n"
                 "  RSI=%#llx RDI=%#llx R8=%#llx R9=%#llx\n"
                 "  R10=%#llx R11=%#llx R12=%#llx R13=%#llx\n"
                 "  R14=%#llx R15=%#llx EFL=%#llx\n",
                 static_cast<unsigned long long>(registers[REG_RIP]),
                 static_cast<unsigned long long>(registers[REG_RSP]),
                 static_cast<unsigned long long>(registers[REG_RBP]),
                 static_cast<unsigned long long>(registers[REG_RAX]),
                 static_cast<unsigned long long>(registers[REG_RBX]),
                 static_cast<unsigned long long>(registers[REG_RCX]),
                 static_cast<unsigned long long>(registers[REG_RDX]),
                 static_cast<unsigned long long>(registers[REG_RSI]),
                 static_cast<unsigned long long>(registers[REG_RDI]),
                 static_cast<unsigned long long>(registers[REG_R8]),
                 static_cast<unsigned long long>(registers[REG_R9]),
                 static_cast<unsigned long long>(registers[REG_R10]),
                 static_cast<unsigned long long>(registers[REG_R11]),
                 static_cast<unsigned long long>(registers[REG_R12]),
                 static_cast<unsigned long long>(registers[REG_R13]),
                 static_cast<unsigned long long>(registers[REG_R14]),
                 static_cast<unsigned long long>(registers[REG_R15]),
                 static_cast<unsigned long long>(registers[REG_EFL]));
#elif defined(__linux__) && defined(__aarch64__)
    ucontext_t context{};
    if (getcontext(&context) != 0) {
        std::fputs("  unavailable\n", stderr);
        return;
    }
    std::fprintf(stderr, "  PC=%#llx SP=%#llx PSTATE=%#llx\n",
                 static_cast<unsigned long long>(context.uc_mcontext.pc),
                 static_cast<unsigned long long>(context.uc_mcontext.sp),
                 static_cast<unsigned long long>(context.uc_mcontext.pstate));
    for (unsigned index = 0; index < 31; index += 2) {
        std::fprintf(
            stderr, "  X%-2u=%#llx", index,
            static_cast<unsigned long long>(context.uc_mcontext.regs[index]));
        if (index + 1 < 31)
            std::fprintf(stderr, " X%-2u=%#llx", index + 1,
                         static_cast<unsigned long long>(
                             context.uc_mcontext.regs[index + 1]));
        std::fputc('\n', stderr);
    }
#else
    std::fputs("  unavailable on this platform\n", stderr);
#endif
}

void PrintStackTrace() {
    std::fputs("stack trace:\n", stderr);
#if defined(__linux__) || defined(__APPLE__)
    void* frames[64]{};
    const int count = backtrace(frames, 64);
    if (count <= 0) {
        std::fputs("  unavailable\n", stderr);
        return;
    }
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
#else
    std::fputs("  unavailable on this platform\n", stderr);
#endif
}

}  // namespace

extern "C" void flecha_runtime_v1_crash(
    const char* message, size_t message_length, const char* file,
    size_t file_length, const char* function, size_t function_length,
    uint32_t line, uint32_t column) {
    std::fputs("flecha: unrecoverable crash\nmessage: ", stderr);
    WriteText(message, message_length);
    std::fputs("\nlocation: ", stderr);
    WriteText(file, file_length);
    std::fprintf(stderr, ":%u:%u in ", line, column);
    WriteText(function, function_length);
    std::fputc('\n', stderr);
    PrintRegisters();
    PrintStackTrace();
    std::fflush(stderr);
    std::abort();
}
