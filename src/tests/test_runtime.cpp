#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "runtime/runtime_abi.h"

#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#endif

#ifndef FLECHA_PROCESS_FIXTURE
#error "FLECHA_PROCESS_FIXTURE must name the process test fixture"
#endif

namespace {

#ifdef __linux__
std::size_t OpenDescriptorCount() {
    std::error_code error;
    std::size_t count = 0;
    std::filesystem::directory_iterator current("/proc/self/fd", error);
    const std::filesystem::directory_iterator end;
    while (!error && current != end) {
        ++count;
        current.increment(error);
    }
    return error ? 0 : count;
}
#endif

class ScopedRuntimeEnvironment final {
   public:
    explicit ScopedRuntimeEnvironment(std::string name)
        : name_(std::move(name)) {
        FlechaRuntimeString value{};
        uint8_t present = 0;
        const int32_t status = flecha_runtime_v1_environment_get(
            &value, &present, name_.data(), name_.size());
        if (status != FLECHA_RUNTIME_ENVIRONMENT_OK) std::abort();
        if (present != 0) {
            if (value.length == 0)
                previous_.emplace();
            else
                previous_.emplace(value.data, value.length);
        }
        flecha_runtime_v1_string_destroy(value.data);
    }

    ~ScopedRuntimeEnvironment() {
        if (previous_.has_value()) {
            (void)flecha_runtime_v1_environment_set(name_.data(), name_.size(),
                                                    previous_->data(),
                                                    previous_->size());
        } else {
            (void)flecha_runtime_v1_environment_remove(name_.data(),
                                                       name_.size());
        }
    }

    ScopedRuntimeEnvironment(const ScopedRuntimeEnvironment&) = delete;
    ScopedRuntimeEnvironment& operator=(const ScopedRuntimeEnvironment&) =
        delete;

   private:
    std::string name_;
    std::optional<std::string> previous_;
};

#ifndef _WIN32
void IgnoreSignal(int) {}
#endif

struct SpawnedProcess final {
    void* process = nullptr;
    void* input = nullptr;
    void* output = nullptr;
    void* error = nullptr;
};

int32_t SpawnFixture(const std::vector<std::string>& arguments,
                     uint8_t inputMode, uint8_t outputMode, uint8_t errorMode,
                     SpawnedProcess* output) {
    std::string bytes;
    std::vector<size_t> offsets{0};
    for (const std::string& argument : arguments) {
        bytes += argument;
        offsets.push_back(bytes.size());
    }
    return flecha_runtime_v1_process_spawn(
        &output->process, &output->input, &output->output, &output->error,
        FLECHA_PROCESS_FIXTURE, std::strlen(FLECHA_PROCESS_FIXTURE),
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(),
        offsets.data(), arguments.size(), inputMode, outputMode, errorMode);
}

std::string ReadProcessOutput(void* handle) {
    std::string result;
    std::array<uint8_t, 257> bytes{};
    for (;;) {
        size_t count = 0;
        if (flecha_runtime_v1_process_pipe_read(handle, bytes.data(),
                                                bytes.size(), &count) !=
            FLECHA_RUNTIME_PROCESS_OK)
            return {};
        if (count == 0) return result;
        result.append(reinterpret_cast<const char*>(bytes.data()), count);
    }
}

}  // namespace

TEST(RuntimeStringViewTests, ReturnsCharactersWithinBounds) {
    const char text[] = "flecha";
    EXPECT_EQ(flecha_runtime_v1_string_view_at(text, 6, 0),
              static_cast<std::uint8_t>('f'));
    EXPECT_EQ(flecha_runtime_v1_string_view_at(text, 6, 5),
              static_cast<std::uint8_t>('a'));
}

TEST(RuntimeStringViewTests, AcceptsValidSubstringBounds) {
    EXPECT_NO_FATAL_FAILURE(
        flecha_runtime_v1_string_view_check_substr(6, 1, 3));
    EXPECT_NO_FATAL_FAILURE(
        flecha_runtime_v1_string_view_check_substr(6, 6, 0));
}

TEST(RuntimeStringViewTests, AcceptsNullStorageForEmptyOutput) {
    EXPECT_NO_FATAL_FAILURE(flecha_runtime_v1_stdout_string_view(nullptr, 0));
    EXPECT_NO_FATAL_FAILURE(flecha_runtime_v1_stderr_string_view(nullptr, 0));
}

TEST(RuntimeStandardStreamTests, ValidatesBuffersAndCountOutputs) {
    std::size_t count = 99;
    EXPECT_EQ(flecha_runtime_v1_standard_output_write(nullptr, 0, &count),
              FLECHA_RUNTIME_IO_OK);
    EXPECT_EQ(count, 0);

    count = 99;
    EXPECT_EQ(flecha_runtime_v1_standard_error_write(nullptr, 1, &count),
              FLECHA_RUNTIME_IO_INVALID_OPTIONS);
    EXPECT_EQ(count, 0);

    EXPECT_EQ(flecha_runtime_v1_standard_output_write(nullptr, 0, nullptr),
              FLECHA_RUNTIME_IO_INVALID_OPTIONS);
    EXPECT_EQ(flecha_runtime_v1_standard_error_write(nullptr, 0, nullptr),
              FLECHA_RUNTIME_IO_INVALID_OPTIONS);

    count = 99;
    EXPECT_EQ(flecha_runtime_v1_standard_input_read(nullptr, 0, &count),
              FLECHA_RUNTIME_IO_OK);
    EXPECT_EQ(count, 0);

    count = 99;
    EXPECT_EQ(flecha_runtime_v1_standard_input_read(nullptr, 1, &count),
              FLECHA_RUNTIME_IO_INVALID_OPTIONS);
    EXPECT_EQ(count, 0);

    EXPECT_EQ(flecha_runtime_v1_standard_input_read(nullptr, 0, nullptr),
              FLECHA_RUNTIME_IO_INVALID_OPTIONS);
}

TEST(RuntimeProcessArgumentTests, CapturesInitializedArguments) {
    const char* values[] = {"flecha-test", "", "two words"};
    flecha_runtime_v1_process_arguments_initialize(3, values);
    EXPECT_EQ(flecha_runtime_v1_process_argument_count(), 3u);

    FlechaRuntimeString output{};
    EXPECT_EQ(flecha_runtime_v1_process_argument_capture(&output, 0),
              FLECHA_RUNTIME_PROCESS_ARGUMENT_OK);
    ASSERT_NE(output.data, nullptr);
    EXPECT_EQ(std::string(output.data, output.length), "flecha-test");
    std::free(output.data);

    output = {};
    EXPECT_EQ(flecha_runtime_v1_process_argument_capture(&output, 1),
              FLECHA_RUNTIME_PROCESS_ARGUMENT_OK);
    EXPECT_EQ(output.data, nullptr);
    EXPECT_EQ(output.length, 0u);

    EXPECT_EQ(flecha_runtime_v1_process_argument_capture(&output, 3),
              FLECHA_RUNTIME_PROCESS_ARGUMENT_INVALID_INDEX);
    EXPECT_EQ(output.data, nullptr);
    EXPECT_EQ(flecha_runtime_v1_process_argument_capture(nullptr, 0),
              FLECHA_RUNTIME_PROCESS_ARGUMENT_INVALID_INDEX);

    flecha_runtime_v1_process_arguments_initialize(0, nullptr);
    EXPECT_EQ(flecha_runtime_v1_process_argument_count(), 0u);
}

TEST(RuntimeChildProcessTests, PassesArgumentsWithoutShellInterpolation) {
    SpawnedProcess child;
    ASSERT_EQ(SpawnFixture({"arguments", "two words", "; echo expanded",
                            "$(echo expanded)", "", "quote\"slash\\"},
                           FLECHA_RUNTIME_PROCESS_STDIO_NULL,
                           FLECHA_RUNTIME_PROCESS_STDIO_PIPED,
                           FLECHA_RUNTIME_PROCESS_STDIO_NULL, &child),
              FLECHA_RUNTIME_PROCESS_OK);
    ASSERT_NE(child.process, nullptr);
    ASSERT_NE(child.output, nullptr);
    EXPECT_EQ(child.input, nullptr);
    EXPECT_EQ(child.error, nullptr);

    EXPECT_EQ(ReadProcessOutput(child.output),
              "9:two words\n15:; echo expanded\n16:$(echo expanded)\n0:\n"
              "12:quote\"slash\\\n");
    EXPECT_EQ(flecha_runtime_v1_process_pipe_close(child.output),
              FLECHA_RUNTIME_PROCESS_OK);
    uint8_t kind = 99;
    uint32_t detail = 99;
    EXPECT_EQ(flecha_runtime_v1_process_wait(child.process, &kind, &detail),
              FLECHA_RUNTIME_PROCESS_OK);
    EXPECT_EQ(kind, FLECHA_RUNTIME_PROCESS_TERMINATION_EXITED);
    EXPECT_EQ(detail, 0u);
    EXPECT_EQ(flecha_runtime_v1_process_close(child.process),
              FLECHA_RUNTIME_PROCESS_OK);
}

TEST(RuntimeChildProcessTests, DistinguishesMissingAndNonzeroExit) {
    const size_t offsets[] = {0};
    SpawnedProcess missing{
        reinterpret_cast<void*>(1), reinterpret_cast<void*>(1),
        reinterpret_cast<void*>(1), reinterpret_cast<void*>(1)};
    const char executable[] = "flecha-definitely-missing-executable";
    EXPECT_EQ(flecha_runtime_v1_process_spawn(
                  &missing.process, &missing.input, &missing.output,
                  &missing.error, executable, sizeof(executable) - 1, nullptr,
                  0, offsets, 0, FLECHA_RUNTIME_PROCESS_STDIO_NULL,
                  FLECHA_RUNTIME_PROCESS_STDIO_NULL,
                  FLECHA_RUNTIME_PROCESS_STDIO_NULL),
              FLECHA_RUNTIME_PROCESS_EXECUTABLE_NOT_FOUND);
    EXPECT_EQ(missing.process, nullptr);
    EXPECT_EQ(missing.input, nullptr);
    EXPECT_EQ(missing.output, nullptr);
    EXPECT_EQ(missing.error, nullptr);

    SpawnedProcess child;
    ASSERT_EQ(SpawnFixture({"exit", "37"}, FLECHA_RUNTIME_PROCESS_STDIO_NULL,
                           FLECHA_RUNTIME_PROCESS_STDIO_NULL,
                           FLECHA_RUNTIME_PROCESS_STDIO_NULL, &child),
              FLECHA_RUNTIME_PROCESS_OK);
    uint8_t kind = 99;
    uint32_t detail = 99;
    EXPECT_EQ(flecha_runtime_v1_process_wait(child.process, &kind, &detail),
              FLECHA_RUNTIME_PROCESS_OK);
    EXPECT_EQ(kind, FLECHA_RUNTIME_PROCESS_TERMINATION_EXITED);
    EXPECT_EQ(detail, 37u);
    EXPECT_EQ(flecha_runtime_v1_process_close(child.process),
              FLECHA_RUNTIME_PROCESS_OK);
}

TEST(RuntimeChildProcessTests, TransfersLargePartialPipeTraffic) {
    constexpr size_t expectedLength = 1024 * 1024;
    SpawnedProcess child;
    ASSERT_EQ(
        SpawnFixture({"large", "1048576"}, FLECHA_RUNTIME_PROCESS_STDIO_NULL,
                     FLECHA_RUNTIME_PROCESS_STDIO_PIPED,
                     FLECHA_RUNTIME_PROCESS_STDIO_NULL, &child),
        FLECHA_RUNTIME_PROCESS_OK);
    const std::string bytes = ReadProcessOutput(child.output);
    ASSERT_EQ(bytes.size(), expectedLength);
    for (size_t index = 0; index < bytes.size(); ++index)
        ASSERT_EQ(bytes[index], 'a' + index % 26) << "at byte " << index;
    EXPECT_EQ(flecha_runtime_v1_process_pipe_close(child.output),
              FLECHA_RUNTIME_PROCESS_OK);
    uint8_t kind = 0;
    uint32_t detail = 0;
    EXPECT_EQ(flecha_runtime_v1_process_wait(child.process, &kind, &detail),
              FLECHA_RUNTIME_PROCESS_OK);
    EXPECT_EQ(detail, 0u);
    EXPECT_EQ(flecha_runtime_v1_process_close(child.process),
              FLECHA_RUNTIME_PROCESS_OK);
}

TEST(RuntimeChildProcessTests, ReportsBrokenPipeAfterChildClosesInput) {
    SpawnedProcess child;
    ASSERT_EQ(SpawnFixture({"exit", "0"}, FLECHA_RUNTIME_PROCESS_STDIO_PIPED,
                           FLECHA_RUNTIME_PROCESS_STDIO_NULL,
                           FLECHA_RUNTIME_PROCESS_STDIO_NULL, &child),
              FLECHA_RUNTIME_PROCESS_OK);
    uint8_t kind = 0;
    uint32_t detail = 0;
    ASSERT_EQ(flecha_runtime_v1_process_wait(child.process, &kind, &detail),
              FLECHA_RUNTIME_PROCESS_OK);
    const uint8_t byte = 42;
    size_t count = 99;
    EXPECT_EQ(
        flecha_runtime_v1_process_pipe_write(child.input, &byte, 1, &count),
        FLECHA_RUNTIME_PROCESS_BROKEN_PIPE);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(flecha_runtime_v1_process_pipe_close(child.input),
              FLECHA_RUNTIME_PROCESS_OK);
    EXPECT_EQ(flecha_runtime_v1_process_close(child.process),
              FLECHA_RUNTIME_PROCESS_OK);
}

TEST(RuntimeChildProcessTests, KeepsOutputAndErrorPipesDistinct) {
    SpawnedProcess child;
    ASSERT_EQ(SpawnFixture({"both"}, FLECHA_RUNTIME_PROCESS_STDIO_NULL,
                           FLECHA_RUNTIME_PROCESS_STDIO_PIPED,
                           FLECHA_RUNTIME_PROCESS_STDIO_PIPED, &child),
              FLECHA_RUNTIME_PROCESS_OK);
    EXPECT_EQ(ReadProcessOutput(child.output), "standard output");
    EXPECT_EQ(ReadProcessOutput(child.error), "standard error");
    EXPECT_EQ(flecha_runtime_v1_process_pipe_close(child.output),
              FLECHA_RUNTIME_PROCESS_OK);
    EXPECT_EQ(flecha_runtime_v1_process_pipe_close(child.error),
              FLECHA_RUNTIME_PROCESS_OK);
    uint8_t kind = 0;
    uint32_t detail = 0;
    EXPECT_EQ(flecha_runtime_v1_process_wait(child.process, &kind, &detail),
              FLECHA_RUNTIME_PROCESS_OK);
    EXPECT_EQ(detail, 0u);
    EXPECT_EQ(flecha_runtime_v1_process_close(child.process),
              FLECHA_RUNTIME_PROCESS_OK);
}

TEST(RuntimeChildProcessTests, ResetsOutputsWhenArgumentsAreInvalid) {
    SpawnedProcess child{reinterpret_cast<void*>(1), reinterpret_cast<void*>(1),
                         reinterpret_cast<void*>(1),
                         reinterpret_cast<void*>(1)};
    const size_t invalidOffsets[] = {1};
    EXPECT_EQ(
        flecha_runtime_v1_process_spawn(
            &child.process, &child.input, &child.output, &child.error,
            FLECHA_PROCESS_FIXTURE, std::strlen(FLECHA_PROCESS_FIXTURE),
            nullptr, 0, invalidOffsets, 0, FLECHA_RUNTIME_PROCESS_STDIO_NULL,
            FLECHA_RUNTIME_PROCESS_STDIO_NULL,
            FLECHA_RUNTIME_PROCESS_STDIO_NULL),
        FLECHA_RUNTIME_PROCESS_INVALID_ARGUMENT);
    EXPECT_EQ(child.process, nullptr);
    EXPECT_EQ(child.input, nullptr);
    EXPECT_EQ(child.output, nullptr);
    EXPECT_EQ(child.error, nullptr);

    child = {reinterpret_cast<void*>(1), reinterpret_cast<void*>(1),
             reinterpret_cast<void*>(1), reinterpret_cast<void*>(1)};
    EXPECT_EQ(
        flecha_runtime_v1_process_spawn(
            &child.process, &child.input, &child.output, &child.error,
            FLECHA_PROCESS_FIXTURE, std::strlen(FLECHA_PROCESS_FIXTURE),
            nullptr, 0, invalidOffsets, std::numeric_limits<size_t>::max(),
            FLECHA_RUNTIME_PROCESS_STDIO_NULL,
            FLECHA_RUNTIME_PROCESS_STDIO_NULL,
            FLECHA_RUNTIME_PROCESS_STDIO_NULL),
        FLECHA_RUNTIME_PROCESS_OUT_OF_MEMORY);
    EXPECT_EQ(child.process, nullptr);
    EXPECT_EQ(child.input, nullptr);
    EXPECT_EQ(child.output, nullptr);
    EXPECT_EQ(child.error, nullptr);

    size_t count = 99;
    EXPECT_EQ(flecha_runtime_v1_process_pipe_read(nullptr, nullptr, 0, &count),
              FLECHA_RUNTIME_PROCESS_CLOSED);
    EXPECT_EQ(count, 0u);
    count = 99;
    EXPECT_EQ(flecha_runtime_v1_process_pipe_write(nullptr, nullptr, 0, &count),
              FLECHA_RUNTIME_PROCESS_CLOSED);
    EXPECT_EQ(count, 0u);
}

TEST(RuntimeChildProcessTests, ReleasesEveryOwnedHandle) {
#ifdef __linux__
    const size_t descriptorsBefore = OpenDescriptorCount();
#endif
    for (int iteration = 0; iteration < 32; ++iteration) {
        SpawnedProcess child;
        ASSERT_EQ(SpawnFixture({"both"}, FLECHA_RUNTIME_PROCESS_STDIO_PIPED,
                               FLECHA_RUNTIME_PROCESS_STDIO_PIPED,
                               FLECHA_RUNTIME_PROCESS_STDIO_PIPED, &child),
                  FLECHA_RUNTIME_PROCESS_OK);
        ASSERT_EQ(flecha_runtime_v1_process_pipe_close(child.input),
                  FLECHA_RUNTIME_PROCESS_OK);
        EXPECT_EQ(ReadProcessOutput(child.output), "standard output");
        EXPECT_EQ(ReadProcessOutput(child.error), "standard error");
        ASSERT_EQ(flecha_runtime_v1_process_pipe_close(child.output),
                  FLECHA_RUNTIME_PROCESS_OK);
        ASSERT_EQ(flecha_runtime_v1_process_pipe_close(child.error),
                  FLECHA_RUNTIME_PROCESS_OK);
        ASSERT_EQ(flecha_runtime_v1_process_close(child.process),
                  FLECHA_RUNTIME_PROCESS_OK);
    }
#ifdef __linux__
    EXPECT_EQ(OpenDescriptorCount(), descriptorsBefore);
#endif
}

#ifndef _WIN32
TEST(RuntimeChildProcessTests, ReportsSignalTermination) {
    SpawnedProcess child;
    ASSERT_EQ(SpawnFixture({"signal"}, FLECHA_RUNTIME_PROCESS_STDIO_NULL,
                           FLECHA_RUNTIME_PROCESS_STDIO_NULL,
                           FLECHA_RUNTIME_PROCESS_STDIO_NULL, &child),
              FLECHA_RUNTIME_PROCESS_OK);
    uint8_t kind = 0;
    uint32_t detail = 0;
    EXPECT_EQ(flecha_runtime_v1_process_wait(child.process, &kind, &detail),
              FLECHA_RUNTIME_PROCESS_OK);
    EXPECT_EQ(kind, FLECHA_RUNTIME_PROCESS_TERMINATION_SIGNALED);
    EXPECT_EQ(detail, static_cast<uint32_t>(SIGTERM));
    EXPECT_EQ(flecha_runtime_v1_process_close(child.process),
              FLECHA_RUNTIME_PROCESS_OK);
}
#else
TEST(RuntimeChildProcessTests, ReportsAbnormalWindowsTermination) {
    SpawnedProcess child;
    ASSERT_EQ(SpawnFixture({"abnormal"}, FLECHA_RUNTIME_PROCESS_STDIO_NULL,
                           FLECHA_RUNTIME_PROCESS_STDIO_NULL,
                           FLECHA_RUNTIME_PROCESS_STDIO_NULL, &child),
              FLECHA_RUNTIME_PROCESS_OK);
    uint8_t kind = 0;
    uint32_t detail = 0;
    EXPECT_EQ(flecha_runtime_v1_process_wait(child.process, &kind, &detail),
              FLECHA_RUNTIME_PROCESS_OK);
    EXPECT_EQ(kind, FLECHA_RUNTIME_PROCESS_TERMINATION_ABNORMAL);
    EXPECT_NE(detail, 0u);
    EXPECT_EQ(flecha_runtime_v1_process_close(child.process),
              FLECHA_RUNTIME_PROCESS_OK);
}
#endif

TEST(RuntimeEnvironmentTests, DistinguishesMissingEmptyAndPresentValues) {
    const std::string name = "FLECHA_RUNTIME_ENVIRONMENT_ROUND_TRIP";
    ScopedRuntimeEnvironment restore(name);

    ASSERT_EQ(flecha_runtime_v1_environment_remove(name.data(), name.size()),
              FLECHA_RUNTIME_ENVIRONMENT_OK);

    FlechaRuntimeString output{reinterpret_cast<char*>(1), 1, 1};
    uint8_t present = 9;
    EXPECT_EQ(flecha_runtime_v1_environment_get(&output, &present, name.data(),
                                                name.size()),
              FLECHA_RUNTIME_ENVIRONMENT_OK);
    EXPECT_EQ(present, 0);
    EXPECT_EQ(output.data, nullptr);
    EXPECT_EQ(output.length, 0u);
    EXPECT_EQ(output.capacity, 0u);

    const std::string value = "ol\xC3\xA1=flecha";
    ASSERT_EQ(flecha_runtime_v1_environment_set(name.data(), name.size(),
                                                value.data(), value.size()),
              FLECHA_RUNTIME_ENVIRONMENT_OK);
    EXPECT_EQ(flecha_runtime_v1_environment_get(&output, &present, name.data(),
                                                name.size()),
              FLECHA_RUNTIME_ENVIRONMENT_OK);
    EXPECT_EQ(present, 1);
    EXPECT_EQ(std::string(output.data, output.length), value);
    flecha_runtime_v1_string_destroy(output.data);

    ASSERT_EQ(
        flecha_runtime_v1_environment_set(name.data(), name.size(), nullptr, 0),
        FLECHA_RUNTIME_ENVIRONMENT_OK);
    output = {reinterpret_cast<char*>(1), 1, 1};
    present = 9;
    EXPECT_EQ(flecha_runtime_v1_environment_get(&output, &present, name.data(),
                                                name.size()),
              FLECHA_RUNTIME_ENVIRONMENT_OK);
    EXPECT_EQ(present, 1);
    EXPECT_EQ(output.data, nullptr);
    EXPECT_EQ(output.length, 0u);

    EXPECT_EQ(flecha_runtime_v1_environment_remove(name.data(), name.size()),
              FLECHA_RUNTIME_ENVIRONMENT_OK);
    EXPECT_EQ(flecha_runtime_v1_environment_remove(name.data(), name.size()),
              FLECHA_RUNTIME_ENVIRONMENT_OK);
}

TEST(RuntimeNetworkTests, ResolvesPortableNumericAddresses) {
    void* ipv4Results = reinterpret_cast<void*>(1);
    ASSERT_EQ(flecha_runtime_v1_dns_resolve(&ipv4Results, "127.0.0.1", 9),
              FLECHA_RUNTIME_NETWORK_OK);
    ASSERT_NE(ipv4Results, nullptr);
    ASSERT_GE(flecha_runtime_v1_dns_result_count(ipv4Results), 1u);

    uint8_t family = 0;
    uint64_t high = 1;
    uint64_t low = 1;
    EXPECT_EQ(
        flecha_runtime_v1_dns_result_get(ipv4Results, 0, &family, &high, &low),
        FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(family, 4);
    EXPECT_EQ(high, 0u);
    EXPECT_EQ(low, 0x7f000001u);
    flecha_runtime_v1_dns_result_close(ipv4Results);

    void* ipv6Results = nullptr;
    ASSERT_EQ(flecha_runtime_v1_dns_resolve(&ipv6Results, "localhost", 9),
              FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_GE(flecha_runtime_v1_dns_result_count(ipv6Results), 1u);
    flecha_runtime_v1_dns_result_close(ipv6Results);
}

TEST(RuntimeNetworkTests, ValidatesNamesHandlesAndResultIndexes) {
    void* output = reinterpret_cast<void*>(1);
    EXPECT_EQ(flecha_runtime_v1_dns_resolve(&output, nullptr, 0),
              FLECHA_RUNTIME_NETWORK_INVALID_NAME);
    EXPECT_EQ(output, nullptr);

    EXPECT_EQ(flecha_runtime_v1_dns_resolve(&output, "bad..name", 9),
              FLECHA_RUNTIME_NETWORK_INVALID_NAME);
    EXPECT_EQ(output, nullptr);
    EXPECT_EQ(flecha_runtime_v1_dns_resolve(&output, "-bad.example", 12),
              FLECHA_RUNTIME_NETWORK_INVALID_NAME);
    EXPECT_EQ(output, nullptr);
    const std::string oversizedLabel(64, 'a');
    EXPECT_EQ(flecha_runtime_v1_dns_resolve(&output, oversizedLabel.data(),
                                            oversizedLabel.size()),
              FLECHA_RUNTIME_NETWORK_INVALID_NAME);
    EXPECT_EQ(output, nullptr);
    const std::array<char, 8> embeddedNull{'f',  'l', 'e', 'c',
                                           '\0', 'h', 'a', 'x'};
    EXPECT_EQ(flecha_runtime_v1_dns_resolve(&output, embeddedNull.data(),
                                            embeddedNull.size()),
              FLECHA_RUNTIME_NETWORK_INVALID_NAME);
    EXPECT_EQ(output, nullptr);
    EXPECT_EQ(flecha_runtime_v1_dns_resolve(nullptr, "localhost", 9),
              FLECHA_RUNTIME_NETWORK_RESOLUTION_FAILED);

    uint8_t family = 9;
    uint64_t high = 9;
    uint64_t low = 9;
    EXPECT_EQ(
        flecha_runtime_v1_dns_result_get(nullptr, 0, &family, &high, &low),
        FLECHA_RUNTIME_NETWORK_RESOLUTION_FAILED);
    EXPECT_EQ(family, 0);
    EXPECT_EQ(high, 0u);
    EXPECT_EQ(low, 0u);

    ASSERT_EQ(flecha_runtime_v1_dns_resolve(&output, "localhost", 9),
              FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(flecha_runtime_v1_dns_result_get(
                  output, flecha_runtime_v1_dns_result_count(output), &family,
                  &high, &low),
              FLECHA_RUNTIME_NETWORK_RESOLUTION_FAILED);
    EXPECT_EQ(family, 0);
    EXPECT_EQ(high, 0u);
    EXPECT_EQ(low, 0u);
    flecha_runtime_v1_dns_result_close(output);
    flecha_runtime_v1_dns_result_close(nullptr);
}

TEST(RuntimeTcpTests, ExchangesDataOverAnIpv4LoopbackConnection) {
    constexpr uint64_t loopback = UINT64_C(0x7f000001);
    void* listener = nullptr;
    ASSERT_EQ(
        flecha_runtime_v1_tcp_listener_bind(&listener, 4, 0, loopback, 0, 8),
        FLECHA_RUNTIME_NETWORK_OK);
    ASSERT_NE(listener, nullptr);

    uint8_t listenerFamily = 0;
    uint64_t listenerHigh = 1;
    uint64_t listenerLow = 0;
    uint16_t listenerPort = 0;
    ASSERT_EQ(flecha_runtime_v1_tcp_listener_local_address(
                  listener, &listenerFamily, &listenerHigh, &listenerLow,
                  &listenerPort),
              FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(listenerFamily, 4);
    EXPECT_EQ(listenerHigh, 0u);
    EXPECT_EQ(listenerLow, loopback);
    ASSERT_NE(listenerPort, 0);

    std::atomic<int32_t> clientConnectStatus{FLECHA_RUNTIME_NETWORK_FAILED};
    std::atomic<int32_t> clientWriteStatus{FLECHA_RUNTIME_NETWORK_FAILED};
    std::atomic<int32_t> clientReadStatus{FLECHA_RUNTIME_NETWORK_FAILED};
    std::atomic<int32_t> clientCloseStatus{FLECHA_RUNTIME_NETWORK_FAILED};
    std::atomic<size_t> clientWriteCount{0};
    std::atomic<size_t> clientReadCount{0};
    std::atomic<bool> clientMayWrite{false};
    std::array<uint8_t, 4> clientReply{};

    std::thread client([&] {
        void* stream = nullptr;
        clientConnectStatus = flecha_runtime_v1_tcp_stream_connect(
            &stream, 4, 0, loopback, listenerPort, 1, 2, 0);
        if (clientConnectStatus != FLECHA_RUNTIME_NETWORK_OK) return;
        while (!clientMayWrite.load()) std::this_thread::yield();

        const std::array<uint8_t, 4> request{'p', 'i', 'n', 'g'};
        size_t count = 0;
        clientWriteStatus = flecha_runtime_v1_tcp_stream_write(
            stream, request.data(), request.size(), &count);
        clientWriteCount = count;
        if (clientWriteStatus == FLECHA_RUNTIME_NETWORK_OK) {
            (void)flecha_runtime_v1_tcp_stream_set_timeout(stream, 0, 1, 2, 0);
            count = 0;
            clientReadStatus = flecha_runtime_v1_tcp_stream_read(
                stream, clientReply.data(), clientReply.size(), &count);
            clientReadCount = count;
        }
        clientCloseStatus = flecha_runtime_v1_tcp_stream_close(stream);
    });

    void* accepted = nullptr;
    uint8_t peerFamily = 0;
    uint64_t peerHigh = 1;
    uint64_t peerLow = 0;
    uint16_t peerPort = 0;
    const int32_t acceptStatus = flecha_runtime_v1_tcp_listener_accept(
        listener, &accepted, &peerFamily, &peerHigh, &peerLow, &peerPort);

    int32_t serverReadStatus = FLECHA_RUNTIME_NETWORK_FAILED;
    int32_t serverWriteStatus = FLECHA_RUNTIME_NETWORK_FAILED;
    int32_t acceptedCloseStatus = FLECHA_RUNTIME_NETWORK_FAILED;
    int32_t immediateReadStatus = FLECHA_RUNTIME_NETWORK_FAILED;
    size_t immediateReadCount = 99;
    size_t serverReadCount = 0;
    size_t serverWriteCount = 0;
    std::array<uint8_t, 4> request{};
    if (acceptStatus == FLECHA_RUNTIME_NETWORK_OK) {
        (void)flecha_runtime_v1_tcp_stream_set_timeout(accepted, 0, 1, 0, 0);
        uint8_t byte = 0;
        immediateReadStatus = flecha_runtime_v1_tcp_stream_read(
            accepted, &byte, 1, &immediateReadCount);
        (void)flecha_runtime_v1_tcp_stream_set_timeout(accepted, 0, 1, 2, 0);
        clientMayWrite = true;
        serverReadStatus = flecha_runtime_v1_tcp_stream_read(
            accepted, request.data(), request.size(), &serverReadCount);
        const std::array<uint8_t, 4> reply{'p', 'o', 'n', 'g'};
        serverWriteStatus = flecha_runtime_v1_tcp_stream_write(
            accepted, reply.data(), reply.size(), &serverWriteCount);
        acceptedCloseStatus = flecha_runtime_v1_tcp_stream_close(accepted);
    } else {
        clientMayWrite = true;
    }
    const int32_t listenerCloseStatus =
        flecha_runtime_v1_tcp_listener_close(listener);
    client.join();

    ASSERT_EQ(acceptStatus, FLECHA_RUNTIME_NETWORK_OK);
    ASSERT_NE(accepted, nullptr);
    EXPECT_EQ(peerFamily, 4);
    EXPECT_EQ(peerHigh, 0u);
    EXPECT_EQ(peerLow, loopback);
    EXPECT_NE(peerPort, 0);
    EXPECT_EQ(immediateReadStatus, FLECHA_RUNTIME_NETWORK_TIMED_OUT);
    EXPECT_EQ(immediateReadCount, 0u);
    EXPECT_EQ(serverReadStatus, FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(serverReadCount, 4u);
    EXPECT_EQ(request, (std::array<uint8_t, 4>{'p', 'i', 'n', 'g'}));
    EXPECT_EQ(serverWriteStatus, FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(serverWriteCount, 4u);
    EXPECT_EQ(acceptedCloseStatus, FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(listenerCloseStatus, FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(clientConnectStatus, FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(clientWriteStatus, FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(clientWriteCount, 4u);
    EXPECT_EQ(clientReadStatus, FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(clientReadCount, 4u);
    EXPECT_EQ(clientReply, (std::array<uint8_t, 4>{'p', 'o', 'n', 'g'}));
    EXPECT_EQ(clientCloseStatus, FLECHA_RUNTIME_NETWORK_OK);
}

TEST(RuntimeTcpTests, ExchangesDataAndObservesEofOverIpv6Loopback) {
    void* listener = nullptr;
    const int32_t bindStatus =
        flecha_runtime_v1_tcp_listener_bind(&listener, 6, 0, 1, 0, 8);
    if (bindStatus == FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY ||
        bindStatus == FLECHA_RUNTIME_NETWORK_ADDRESS_NOT_AVAILABLE) {
        GTEST_SKIP() << "IPv6 loopback is unavailable on this host";
    }
    ASSERT_EQ(bindStatus, FLECHA_RUNTIME_NETWORK_OK);
    ASSERT_NE(listener, nullptr);

    uint8_t listenerFamily = 0;
    uint64_t listenerHigh = 1;
    uint64_t listenerLow = 0;
    uint16_t listenerPort = 0;
    ASSERT_EQ(flecha_runtime_v1_tcp_listener_local_address(
                  listener, &listenerFamily, &listenerHigh, &listenerLow,
                  &listenerPort),
              FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(listenerFamily, 6);
    EXPECT_EQ(listenerHigh, 0u);
    EXPECT_EQ(listenerLow, 1u);
    ASSERT_NE(listenerPort, 0);

    std::atomic<int32_t> connectStatus{FLECHA_RUNTIME_NETWORK_FAILED};
    std::atomic<int32_t> writeStatus{FLECHA_RUNTIME_NETWORK_FAILED};
    std::atomic<int32_t> shutdownStatus{FLECHA_RUNTIME_NETWORK_FAILED};
    std::atomic<int32_t> closeStatus{FLECHA_RUNTIME_NETWORK_FAILED};
    std::thread client([&] {
        void* stream = nullptr;
        connectStatus = flecha_runtime_v1_tcp_stream_connect(
            &stream, 6, 0, 1, listenerPort, 1, 2, 0);
        if (connectStatus != FLECHA_RUNTIME_NETWORK_OK) return;
        const uint8_t byte = 42;
        size_t count = 0;
        writeStatus =
            flecha_runtime_v1_tcp_stream_write(stream, &byte, 1, &count);
        if (writeStatus != FLECHA_RUNTIME_NETWORK_OK || count != 1) {
            (void)flecha_runtime_v1_tcp_stream_close(stream);
            return;
        }
        shutdownStatus = flecha_runtime_v1_tcp_stream_shutdown(stream, 1);
        closeStatus = flecha_runtime_v1_tcp_stream_close(stream);
    });

    void* accepted = nullptr;
    uint8_t peerFamily = 0;
    uint64_t peerHigh = 1;
    uint64_t peerLow = 0;
    uint16_t peerPort = 0;
    const int32_t acceptStatus = flecha_runtime_v1_tcp_listener_accept(
        listener, &accepted, &peerFamily, &peerHigh, &peerLow, &peerPort);

    int32_t readStatus = FLECHA_RUNTIME_NETWORK_FAILED;
    int32_t eofStatus = FLECHA_RUNTIME_NETWORK_FAILED;
    size_t readCount = 0;
    size_t eofCount = 1;
    uint8_t byte = 0;
    if (acceptStatus == FLECHA_RUNTIME_NETWORK_OK) {
        (void)flecha_runtime_v1_tcp_stream_set_timeout(accepted, 0, 1, 2, 0);
        readStatus =
            flecha_runtime_v1_tcp_stream_read(accepted, &byte, 1, &readCount);
        uint8_t extra = 0;
        eofStatus =
            flecha_runtime_v1_tcp_stream_read(accepted, &extra, 1, &eofCount);
        (void)flecha_runtime_v1_tcp_stream_close(accepted);
    }
    (void)flecha_runtime_v1_tcp_listener_close(listener);
    client.join();

    ASSERT_EQ(acceptStatus, FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(peerFamily, 6);
    EXPECT_EQ(peerHigh, 0u);
    EXPECT_EQ(peerLow, 1u);
    EXPECT_NE(peerPort, 0);
    EXPECT_EQ(readStatus, FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(readCount, 1u);
    EXPECT_EQ(byte, 42);
    EXPECT_EQ(eofStatus, FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(eofCount, 0u);
    EXPECT_EQ(connectStatus, FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(writeStatus, FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(shutdownStatus, FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(closeStatus, FLECHA_RUNTIME_NETWORK_OK);
}

TEST(RuntimeTcpTests, RepeatedConnectionsReleaseEveryNativeDescriptor) {
#ifdef __linux__
    const std::size_t descriptorsBefore = OpenDescriptorCount();
#endif
    constexpr uint64_t loopback = UINT64_C(0x7f000001);
    constexpr int iterations = 64;
    void* listener = nullptr;
    ASSERT_EQ(
        flecha_runtime_v1_tcp_listener_bind(&listener, 4, 0, loopback, 0, 8),
        FLECHA_RUNTIME_NETWORK_OK);

    uint8_t family = 0;
    uint64_t high = 0;
    uint64_t low = 0;
    uint16_t port = 0;
    ASSERT_EQ(flecha_runtime_v1_tcp_listener_local_address(listener, &family,
                                                           &high, &low, &port),
              FLECHA_RUNTIME_NETWORK_OK);

    std::atomic<int32_t> clientStatus{FLECHA_RUNTIME_NETWORK_OK};
    std::thread client([&] {
        for (int iteration = 0; iteration < iterations; ++iteration) {
            void* stream = nullptr;
            int32_t status = flecha_runtime_v1_tcp_stream_connect(
                &stream, 4, 0, loopback, port, 1, 2, 0);
            if (status == FLECHA_RUNTIME_NETWORK_OK) {
                const uint8_t byte = static_cast<uint8_t>(iteration);
                size_t count = 0;
                status = flecha_runtime_v1_tcp_stream_write(stream, &byte, 1,
                                                            &count);
                if (status == FLECHA_RUNTIME_NETWORK_OK && count != 1)
                    status = FLECHA_RUNTIME_NETWORK_FAILED;
                const int32_t closeStatus =
                    flecha_runtime_v1_tcp_stream_close(stream);
                if (status == FLECHA_RUNTIME_NETWORK_OK) status = closeStatus;
            }
            if (status != FLECHA_RUNTIME_NETWORK_OK) {
                clientStatus = status;
                return;
            }
        }
    });

    int32_t serverStatus = FLECHA_RUNTIME_NETWORK_OK;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        void* stream = nullptr;
        uint8_t peerFamily = 0;
        uint64_t peerHigh = 0;
        uint64_t peerLow = 0;
        uint16_t peerPort = 0;
        serverStatus = flecha_runtime_v1_tcp_listener_accept(
            listener, &stream, &peerFamily, &peerHigh, &peerLow, &peerPort);
        if (serverStatus != FLECHA_RUNTIME_NETWORK_OK) break;
        uint8_t byte = 0;
        size_t count = 0;
        serverStatus =
            flecha_runtime_v1_tcp_stream_read(stream, &byte, 1, &count);
        if (serverStatus == FLECHA_RUNTIME_NETWORK_OK &&
            (count != 1 || byte != static_cast<uint8_t>(iteration)))
            serverStatus = FLECHA_RUNTIME_NETWORK_FAILED;
        const int32_t closeStatus = flecha_runtime_v1_tcp_stream_close(stream);
        if (serverStatus == FLECHA_RUNTIME_NETWORK_OK)
            serverStatus = closeStatus;
        if (serverStatus != FLECHA_RUNTIME_NETWORK_OK) break;
    }
    const int32_t listenerCloseStatus =
        flecha_runtime_v1_tcp_listener_close(listener);
    client.join();

    EXPECT_EQ(serverStatus, FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(clientStatus, FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(listenerCloseStatus, FLECHA_RUNTIME_NETWORK_OK);
#ifdef __linux__
    const std::size_t descriptorsAfter = OpenDescriptorCount();
    ASSERT_NE(descriptorsBefore, 0u);
    ASSERT_NE(descriptorsAfter, 0u);
    EXPECT_LE(descriptorsAfter, descriptorsBefore + 1);
#endif
}

TEST(RuntimeTcpTests, ResetsOutputsAndValidatesPortableEndpoints) {
    void* output = reinterpret_cast<void*>(1);
    EXPECT_EQ(flecha_runtime_v1_tcp_listener_bind(&output, 3, 0, 0, 0, 1),
              FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY);
    EXPECT_EQ(output, nullptr);
    output = reinterpret_cast<void*>(1);
    EXPECT_EQ(flecha_runtime_v1_tcp_listener_bind(&output, 4, 1,
                                                  UINT64_C(0x7f000001), 0, 1),
              FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY);
    EXPECT_EQ(output, nullptr);
    EXPECT_EQ(flecha_runtime_v1_tcp_stream_close(nullptr),
              FLECHA_RUNTIME_NETWORK_CLOSED);
    EXPECT_EQ(flecha_runtime_v1_tcp_listener_close(nullptr),
              FLECHA_RUNTIME_NETWORK_CLOSED);

    uint8_t family = 9;
    uint64_t high = 9;
    uint64_t low = 9;
    uint16_t port = 9;
    EXPECT_EQ(flecha_runtime_v1_tcp_stream_local_address(nullptr, &family,
                                                         &high, &low, &port),
              FLECHA_RUNTIME_NETWORK_CLOSED);
    EXPECT_EQ(family, 0);
    EXPECT_EQ(high, 0u);
    EXPECT_EQ(low, 0u);
    EXPECT_EQ(port, 0);
}

TEST(RuntimeUdpTests, ExchangesDatagramsAndReportsTruncation) {
    constexpr uint64_t loopback = UINT64_C(0x7f000001);
    void* receiver = nullptr;
    void* sender = nullptr;
    ASSERT_EQ(flecha_runtime_v1_udp_socket_bind(&receiver, 4, 0, loopback, 0),
              FLECHA_RUNTIME_NETWORK_OK);
    ASSERT_EQ(flecha_runtime_v1_udp_socket_bind(&sender, 4, 0, loopback, 0),
              FLECHA_RUNTIME_NETWORK_OK);

    uint8_t receiverFamily = 0;
    uint64_t receiverHigh = 1;
    uint64_t receiverLow = 0;
    uint16_t receiverPort = 0;
    ASSERT_EQ(flecha_runtime_v1_udp_socket_local_address(
                  receiver, &receiverFamily, &receiverHigh, &receiverLow,
                  &receiverPort),
              FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(receiverFamily, 4);
    EXPECT_EQ(receiverHigh, 0u);
    EXPECT_EQ(receiverLow, loopback);
    ASSERT_NE(receiverPort, 0);

    uint8_t senderFamily = 0;
    uint64_t senderHigh = 1;
    uint64_t senderLow = 0;
    uint16_t senderPort = 0;
    ASSERT_EQ(flecha_runtime_v1_udp_socket_local_address(
                  sender, &senderFamily, &senderHigh, &senderLow, &senderPort),
              FLECHA_RUNTIME_NETWORK_OK);
    ASSERT_NE(senderPort, 0);

    ASSERT_EQ(flecha_runtime_v1_udp_socket_set_timeout(receiver, 0, 1, 0, 0),
              FLECHA_RUNTIME_NETWORK_OK);
    std::array<uint8_t, 2> immediate{};
    size_t immediateCount = 99;
    uint8_t immediateFamily = 9;
    uint64_t immediateHigh = 9;
    uint64_t immediateLow = 9;
    uint16_t immediatePort = 9;
    EXPECT_EQ(
        flecha_runtime_v1_udp_socket_receive_from(
            receiver, immediate.data(), immediate.size(), &immediateCount,
            &immediateFamily, &immediateHigh, &immediateLow, &immediatePort),
        FLECHA_RUNTIME_NETWORK_TIMED_OUT);
    EXPECT_EQ(immediateCount, 0u);
    EXPECT_EQ(immediateFamily, 0);
    EXPECT_EQ(immediateHigh, 0u);
    EXPECT_EQ(immediateLow, 0u);
    EXPECT_EQ(immediatePort, 0);

    ASSERT_EQ(flecha_runtime_v1_udp_socket_set_timeout(receiver, 0, 1, 2, 0),
              FLECHA_RUNTIME_NETWORK_OK);
    const std::array<uint8_t, 4> request{'p', 'i', 'n', 'g'};
    size_t sent = 0;
    ASSERT_EQ(flecha_runtime_v1_udp_socket_send_to(
                  sender, request.data(), request.size(), 4, 0, loopback,
                  receiverPort, &sent),
              FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(sent, request.size());

    std::array<uint8_t, 2> truncated{};
    size_t received = 0;
    uint8_t peerFamily = 0;
    uint64_t peerHigh = 1;
    uint64_t peerLow = 0;
    uint16_t peerPort = 0;
    EXPECT_EQ(flecha_runtime_v1_udp_socket_receive_from(
                  receiver, truncated.data(), truncated.size(), &received,
                  &peerFamily, &peerHigh, &peerLow, &peerPort),
              FLECHA_RUNTIME_NETWORK_MESSAGE_TOO_LARGE);
    EXPECT_EQ(received, truncated.size());
    EXPECT_EQ(truncated, (std::array<uint8_t, 2>{'p', 'i'}));
    EXPECT_EQ(peerFamily, 4);
    EXPECT_EQ(peerHigh, 0u);
    EXPECT_EQ(peerLow, loopback);
    EXPECT_EQ(peerPort, senderPort);

    const std::array<uint8_t, 4> reply{'p', 'o', 'n', 'g'};
    ASSERT_EQ(flecha_runtime_v1_udp_socket_send_to(
                  receiver, reply.data(), reply.size(), peerFamily, peerHigh,
                  peerLow, peerPort, &sent),
              FLECHA_RUNTIME_NETWORK_OK);
    ASSERT_EQ(flecha_runtime_v1_udp_socket_set_timeout(sender, 0, 1, 2, 0),
              FLECHA_RUNTIME_NETWORK_OK);
    std::array<uint8_t, 4> response{};
    received = 0;
    EXPECT_EQ(flecha_runtime_v1_udp_socket_receive_from(
                  sender, response.data(), response.size(), &received,
                  &peerFamily, &peerHigh, &peerLow, &peerPort),
              FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(received, response.size());
    EXPECT_EQ(response, reply);
    EXPECT_EQ(peerPort, receiverPort);

    EXPECT_EQ(flecha_runtime_v1_udp_socket_close(receiver),
              FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(flecha_runtime_v1_udp_socket_close(sender),
              FLECHA_RUNTIME_NETWORK_OK);
}

TEST(RuntimeUdpTests, SupportsIpv6LoopbackWhenAvailable) {
    void* receiver = nullptr;
    const int32_t bindStatus =
        flecha_runtime_v1_udp_socket_bind(&receiver, 6, 0, 1, 0);
    if (bindStatus == FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY ||
        bindStatus == FLECHA_RUNTIME_NETWORK_ADDRESS_NOT_AVAILABLE) {
        GTEST_SKIP() << "IPv6 loopback is unavailable on this host";
    }
    ASSERT_EQ(bindStatus, FLECHA_RUNTIME_NETWORK_OK);

    uint8_t family = 0;
    uint64_t high = 1;
    uint64_t low = 0;
    uint16_t port = 0;
    ASSERT_EQ(flecha_runtime_v1_udp_socket_local_address(receiver, &family,
                                                         &high, &low, &port),
              FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(family, 6);
    EXPECT_EQ(high, 0u);
    EXPECT_EQ(low, 1u);
    ASSERT_NE(port, 0);

    void* sender = nullptr;
    ASSERT_EQ(flecha_runtime_v1_udp_socket_bind(&sender, 6, 0, 1, 0),
              FLECHA_RUNTIME_NETWORK_OK);
    const uint8_t payload = 42;
    size_t sent = 0;
    ASSERT_EQ(flecha_runtime_v1_udp_socket_send_to(sender, &payload, 1, 6, 0, 1,
                                                   port, &sent),
              FLECHA_RUNTIME_NETWORK_OK);
    ASSERT_EQ(flecha_runtime_v1_udp_socket_set_timeout(receiver, 0, 1, 2, 0),
              FLECHA_RUNTIME_NETWORK_OK);
    uint8_t receivedByte = 0;
    size_t received = 0;
    uint16_t peerPort = 0;
    ASSERT_EQ(flecha_runtime_v1_udp_socket_receive_from(receiver, &receivedByte,
                                                        1, &received, &family,
                                                        &high, &low, &peerPort),
              FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(received, 1u);
    EXPECT_EQ(receivedByte, payload);
    EXPECT_EQ(family, 6);
    EXPECT_EQ(high, 0u);
    EXPECT_EQ(low, 1u);
    EXPECT_NE(peerPort, 0);
    EXPECT_EQ(flecha_runtime_v1_udp_socket_close(sender),
              FLECHA_RUNTIME_NETWORK_OK);
    EXPECT_EQ(flecha_runtime_v1_udp_socket_close(receiver),
              FLECHA_RUNTIME_NETWORK_OK);
}

TEST(RuntimeUdpTests, ResetsOutputsAndRejectsInvalidCalls) {
    EXPECT_EQ(flecha_runtime_v1_udp_socket_bind(nullptr, 4, 0,
                                                UINT64_C(0x7f000001), 0),
              FLECHA_RUNTIME_NETWORK_FAILED);
    void* output = reinterpret_cast<void*>(1);
    EXPECT_EQ(flecha_runtime_v1_udp_socket_bind(&output, 3, 0, 0, 0),
              FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY);
    EXPECT_EQ(output, nullptr);
    output = reinterpret_cast<void*>(1);
    EXPECT_EQ(flecha_runtime_v1_udp_socket_bind(&output, 4, 1,
                                                UINT64_C(0x7f000001), 0),
              FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY);
    EXPECT_EQ(output, nullptr);
    EXPECT_EQ(flecha_runtime_v1_udp_socket_close(nullptr),
              FLECHA_RUNTIME_NETWORK_CLOSED);

    size_t count = 9;
    uint8_t family = 9;
    uint64_t high = 9;
    uint64_t low = 9;
    uint16_t port = 9;
    EXPECT_EQ(flecha_runtime_v1_udp_socket_receive_from(
                  nullptr, nullptr, 0, &count, &family, &high, &low, &port),
              FLECHA_RUNTIME_NETWORK_CLOSED);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(family, 0);
    EXPECT_EQ(high, 0u);
    EXPECT_EQ(low, 0u);
    EXPECT_EQ(port, 0);

    ASSERT_EQ(flecha_runtime_v1_udp_socket_bind(&output, 4, 0,
                                                UINT64_C(0x7f000001), 0),
              FLECHA_RUNTIME_NETWORK_OK);
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(flecha_runtime_v1_udp_socket_set_timeout(output, 2, 1, 0, 0),
              FLECHA_RUNTIME_NETWORK_FAILED);
    EXPECT_EQ(flecha_runtime_v1_udp_socket_set_timeout(output, 0, 2, 0, 0),
              FLECHA_RUNTIME_NETWORK_FAILED);
    EXPECT_EQ(
        flecha_runtime_v1_udp_socket_set_timeout(output, 0, 1, 0, 1000000000),
        FLECHA_RUNTIME_NETWORK_FAILED);

    count = 9;
    EXPECT_EQ(flecha_runtime_v1_udp_socket_send_to(output, nullptr, 0, 3, 0, 0,
                                                   0, &count),
              FLECHA_RUNTIME_NETWORK_UNSUPPORTED_FAMILY);
    EXPECT_EQ(count, 0u);

    high = 9;
    low = 9;
    port = 9;
    EXPECT_EQ(flecha_runtime_v1_udp_socket_local_address(output, nullptr, &high,
                                                         &low, &port),
              FLECHA_RUNTIME_NETWORK_FAILED);
    EXPECT_EQ(high, 0u);
    EXPECT_EQ(low, 0u);
    EXPECT_EQ(port, 0);
    EXPECT_EQ(flecha_runtime_v1_udp_socket_close(output),
              FLECHA_RUNTIME_NETWORK_OK);
}

TEST(RuntimeEnvironmentTests, RejectsInvalidNamesAndValuesTransactionally) {
    const std::string name = "FLECHA_RUNTIME_ENVIRONMENT_VALIDATION";
    ScopedRuntimeEnvironment restore(name);
    const std::string original = "preserved";
    ASSERT_EQ(flecha_runtime_v1_environment_set(
                  name.data(), name.size(), original.data(), original.size()),
              FLECHA_RUNTIME_ENVIRONMENT_OK);

    FlechaRuntimeString output{reinterpret_cast<char*>(1), 1, 1};
    uint8_t present = 9;
    EXPECT_EQ(flecha_runtime_v1_environment_get(&output, &present, nullptr, 0),
              FLECHA_RUNTIME_ENVIRONMENT_INVALID_NAME);
    EXPECT_EQ(output.data, nullptr);
    EXPECT_EQ(output.length, 0u);
    EXPECT_EQ(present, 0);

    const char equalsName[] = "INVALID=NAME";
    EXPECT_EQ(flecha_runtime_v1_environment_set(
                  equalsName, sizeof(equalsName) - 1, "value", 5),
              FLECHA_RUNTIME_ENVIRONMENT_INVALID_NAME);
    const std::array<char, 3> nullName{'A', '\0', 'B'};
    EXPECT_EQ(
        flecha_runtime_v1_environment_remove(nullName.data(), nullName.size()),
        FLECHA_RUNTIME_ENVIRONMENT_INVALID_NAME);

    const std::array<char, 3> nullValue{'A', '\0', 'B'};
    EXPECT_EQ(flecha_runtime_v1_environment_set(
                  name.data(), name.size(), nullValue.data(), nullValue.size()),
              FLECHA_RUNTIME_ENVIRONMENT_INVALID_VALUE);

    output = {};
    EXPECT_EQ(flecha_runtime_v1_environment_get(&output, &present, name.data(),
                                                name.size()),
              FLECHA_RUNTIME_ENVIRONMENT_OK);
    EXPECT_EQ(present, 1);
    EXPECT_EQ(std::string(output.data, output.length), original);
    flecha_runtime_v1_string_destroy(output.data);
}

TEST(RuntimeEnvironmentTests, RejectsInvalidOutputPointers) {
    const std::string name = "FLECHA_RUNTIME_ENVIRONMENT_POINTERS";
    FlechaRuntimeString output{};
    uint8_t present = 0;
    EXPECT_EQ(flecha_runtime_v1_environment_get(nullptr, &present, name.data(),
                                                name.size()),
              FLECHA_RUNTIME_ENVIRONMENT_FAILED);
    EXPECT_EQ(flecha_runtime_v1_environment_get(&output, nullptr, name.data(),
                                                name.size()),
              FLECHA_RUNTIME_ENVIRONMENT_FAILED);
}

TEST(RuntimeRandomTests, AcceptsAnEmptyDestination) {
    EXPECT_EQ(flecha_runtime_v1_secure_random_fill(nullptr, 0),
              FLECHA_RUNTIME_RANDOM_OK);
}

TEST(RuntimeRandomTests, RejectsNullStorageForANonemptyDestination) {
    EXPECT_EQ(flecha_runtime_v1_secure_random_fill(nullptr, 1),
              FLECHA_RUNTIME_RANDOM_FAILED);
}

TEST(RuntimeRandomTests, FillsOnlyTheRequestedRegion) {
    std::array<uint8_t, 515> storage{};
    storage.fill(165);

    ASSERT_EQ(flecha_runtime_v1_secure_random_fill(storage.data() + 1, 513),
              FLECHA_RUNTIME_RANDOM_OK);
    EXPECT_EQ(storage.front(), 165);
    EXPECT_EQ(storage.back(), 165);
}

TEST(RuntimeSecurityTests, EmptyRegionsAreEqual) {
    EXPECT_EQ(flecha_runtime_v1_constant_time_difference(nullptr, nullptr, 0),
              0);
}

TEST(RuntimeSecurityTests, RejectsNullNonemptyRegions) {
    const std::array<uint8_t, 1> value{42};

    EXPECT_NE(flecha_runtime_v1_constant_time_difference(nullptr, value.data(),
                                                         value.size()),
              0);
    EXPECT_NE(flecha_runtime_v1_constant_time_difference(value.data(), nullptr,
                                                         value.size()),
              0);
}

TEST(RuntimeSecurityTests, EqualRegionsHaveNoDifference) {
    const std::array<uint8_t, 5> left{1, 2, 3, 4, 5};
    const std::array<uint8_t, 5> right{1, 2, 3, 4, 5};

    EXPECT_EQ(flecha_runtime_v1_constant_time_difference(
                  left.data(), right.data(), left.size()),
              0);
}

TEST(RuntimeSecurityTests, DetectsDifferencesThroughoutTheRegion) {
    const std::array<uint8_t, 5> expected{1, 2, 3, 4, 5};
    const std::array<uint8_t, 5> first{0, 2, 3, 4, 5};
    const std::array<uint8_t, 5> middle{1, 2, 0, 4, 5};
    const std::array<uint8_t, 5> last{1, 2, 3, 4, 0};

    EXPECT_NE(flecha_runtime_v1_constant_time_difference(
                  expected.data(), first.data(), expected.size()),
              0);
    EXPECT_NE(flecha_runtime_v1_constant_time_difference(
                  expected.data(), middle.data(), expected.size()),
              0);
    EXPECT_NE(flecha_runtime_v1_constant_time_difference(
                  expected.data(), last.data(), expected.size()),
              0);
}

TEST(RuntimeSecurityTests, SecureZeroAcceptsAnEmptyDestination) {
    flecha_runtime_v1_secure_zero(nullptr, 0);
}

TEST(RuntimeSecurityTests, SecureZeroOverwritesOnlyTheRequestedRegion) {
    std::array<uint8_t, 7> storage{11, 22, 33, 44, 55, 66, 77};

    flecha_runtime_v1_secure_zero(storage.data() + 1, 5);

    EXPECT_EQ(storage.front(), 11);
    EXPECT_EQ(storage.back(), 77);
    for (size_t index = 1; index < storage.size() - 1; ++index) {
        EXPECT_EQ(storage[index], 0);
    }
}

TEST(RuntimeTimeTests, ReturnsNormalizedClockObservations) {
    uint64_t firstSeconds = 0;
    uint32_t firstNanoseconds = 0;
    ASSERT_EQ(
        flecha_runtime_v1_monotonic_clock_now(&firstSeconds, &firstNanoseconds),
        FLECHA_RUNTIME_TIME_OK);
    EXPECT_LT(firstNanoseconds, 1000000000u);

    uint64_t secondSeconds = 0;
    uint32_t secondNanoseconds = 0;
    ASSERT_EQ(flecha_runtime_v1_monotonic_clock_now(&secondSeconds,
                                                    &secondNanoseconds),
              FLECHA_RUNTIME_TIME_OK);
    EXPECT_TRUE(secondSeconds > firstSeconds ||
                (secondSeconds == firstSeconds &&
                 secondNanoseconds >= firstNanoseconds));

    int64_t unixSeconds = 0;
    uint32_t wallNanoseconds = 0;
    ASSERT_EQ(flecha_runtime_v1_wall_clock_now(&unixSeconds, &wallNanoseconds),
              FLECHA_RUNTIME_TIME_OK);
    EXPECT_LT(wallNanoseconds, 1000000000u);
}

TEST(RuntimeTimeTests, ClearsOutputsWhenPointersAreInvalid) {
    uint64_t monotonicSeconds = 99;
    uint32_t monotonicNanoseconds = 99;
    EXPECT_EQ(
        flecha_runtime_v1_monotonic_clock_now(nullptr, &monotonicNanoseconds),
        FLECHA_RUNTIME_TIME_FAILED);
    EXPECT_EQ(monotonicNanoseconds, 0u);
    EXPECT_EQ(flecha_runtime_v1_monotonic_clock_now(&monotonicSeconds, nullptr),
              FLECHA_RUNTIME_TIME_FAILED);
    EXPECT_EQ(monotonicSeconds, 0u);

    int64_t unixSeconds = 99;
    uint32_t wallNanoseconds = 99;
    EXPECT_EQ(flecha_runtime_v1_wall_clock_now(nullptr, &wallNanoseconds),
              FLECHA_RUNTIME_TIME_FAILED);
    EXPECT_EQ(wallNanoseconds, 0u);
    EXPECT_EQ(flecha_runtime_v1_wall_clock_now(&unixSeconds, nullptr),
              FLECHA_RUNTIME_TIME_FAILED);
    EXPECT_EQ(unixSeconds, 0);
}

TEST(RuntimeTimeTests, ValidatesAndWaitsForRelativeDurations) {
    EXPECT_EQ(flecha_runtime_v1_sleep(0, 1000000000u),
              FLECHA_RUNTIME_TIME_INVALID_DURATION);
    EXPECT_EQ(flecha_runtime_v1_sleep(0, 0), FLECHA_RUNTIME_TIME_OK);
    EXPECT_EQ(flecha_runtime_v1_sleep(std::numeric_limits<uint64_t>::max(), 0),
              FLECHA_RUNTIME_TIME_OUT_OF_RANGE);

    uint64_t startSeconds = 0;
    uint32_t startNanoseconds = 0;
    ASSERT_EQ(
        flecha_runtime_v1_monotonic_clock_now(&startSeconds, &startNanoseconds),
        FLECHA_RUNTIME_TIME_OK);
    ASSERT_EQ(flecha_runtime_v1_sleep(0, 5000000), FLECHA_RUNTIME_TIME_OK);
    uint64_t endSeconds = 0;
    uint32_t endNanoseconds = 0;
    ASSERT_EQ(
        flecha_runtime_v1_monotonic_clock_now(&endSeconds, &endNanoseconds),
        FLECHA_RUNTIME_TIME_OK);

    const uint64_t elapsedSeconds = endSeconds - startSeconds;
    const int64_t elapsedNanoseconds =
        static_cast<int64_t>(elapsedSeconds * 1000000000) +
        static_cast<int64_t>(endNanoseconds) -
        static_cast<int64_t>(startNanoseconds);
    EXPECT_GE(elapsedNanoseconds, 5000000);
}

#ifndef _WIN32
TEST(RuntimeTimeTests, ResumesTheRemainingDurationAfterInterruption) {
    struct sigaction action{};
    action.sa_handler = IgnoreSignal;
    ASSERT_EQ(sigemptyset(&action.sa_mask), 0);
    struct sigaction previous{};
    ASSERT_EQ(sigaction(SIGUSR1, &action, &previous), 0);

    std::atomic<bool> started = false;
    std::atomic<int32_t> status = FLECHA_RUNTIME_TIME_FAILED;
    std::chrono::steady_clock::duration elapsed{};
    std::thread sleeper([&] {
        started.store(true, std::memory_order_release);
        const std::chrono::steady_clock::time_point start =
            std::chrono::steady_clock::now();
        status.store(flecha_runtime_v1_sleep(0, 30000000),
                     std::memory_order_release);
        elapsed = std::chrono::steady_clock::now() - start;
    });

    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const int signalResult = pthread_kill(sleeper.native_handle(), SIGUSR1);
    sleeper.join();
    const int restoreResult = sigaction(SIGUSR1, &previous, nullptr);

    EXPECT_EQ(signalResult, 0);
    EXPECT_EQ(restoreResult, 0);
    EXPECT_EQ(status.load(std::memory_order_acquire), FLECHA_RUNTIME_TIME_OK);
    EXPECT_GE(elapsed, std::chrono::milliseconds(30));
}
#endif

TEST(RuntimePathTests, ReportsHostLexicalStyle) {
#ifdef _WIN32
    EXPECT_EQ(flecha_runtime_v1_path_style(),
              FLECHA_RUNTIME_PATH_STYLE_WINDOWS);
#else
    EXPECT_EQ(flecha_runtime_v1_path_style(), FLECHA_RUNTIME_PATH_STYLE_POSIX);
#endif
}

TEST(RuntimeStandardInputTests, ParsesCompleteTypedValues) {
    std::int32_t signedValue = 0;
    EXPECT_EQ(flecha_runtime_v1_input_parse_i32(&signedValue, "+42", 3),
              FLECHA_RUNTIME_INPUT_OK);
    EXPECT_EQ(signedValue, 42);
    EXPECT_EQ(flecha_runtime_v1_input_parse_i32(&signedValue, "-7", 2),
              FLECHA_RUNTIME_INPUT_OK);
    EXPECT_EQ(signedValue, -7);

    std::uint8_t byte = 0;
    EXPECT_EQ(flecha_runtime_v1_input_parse_u8(&byte, "255", 3),
              FLECHA_RUNTIME_INPUT_OK);
    EXPECT_EQ(byte, 255);
    EXPECT_EQ(flecha_runtime_v1_input_parse_u8(&byte, "256", 3),
              FLECHA_RUNTIME_INPUT_OUT_OF_RANGE);
    EXPECT_EQ(byte, 0);

    double floating = 0;
    EXPECT_EQ(flecha_runtime_v1_input_parse_f64(&floating, "1.25", 4),
              FLECHA_RUNTIME_INPUT_OK);
    EXPECT_DOUBLE_EQ(floating, 1.25);

    std::uint8_t boolean = 0;
    EXPECT_EQ(flecha_runtime_v1_input_parse_bool(&boolean, "true", 4),
              FLECHA_RUNTIME_INPUT_OK);
    EXPECT_EQ(boolean, 1);
    EXPECT_EQ(flecha_runtime_v1_input_parse_bool(&boolean, "false", 5),
              FLECHA_RUNTIME_INPUT_OK);
    EXPECT_EQ(boolean, 0);

    std::uint8_t character = 0;
    EXPECT_EQ(flecha_runtime_v1_input_parse_char(&character, "x", 1),
              FLECHA_RUNTIME_INPUT_OK);
    EXPECT_EQ(character, static_cast<std::uint8_t>('x'));
}

TEST(RuntimeStandardInputTests, RejectsPartialAndInvalidConversions) {
    std::int32_t integer = 99;
    EXPECT_EQ(flecha_runtime_v1_input_parse_i32(&integer, "42x", 3),
              FLECHA_RUNTIME_INPUT_INVALID_SYNTAX);
    EXPECT_EQ(integer, 0);
    EXPECT_EQ(flecha_runtime_v1_input_parse_i32(&integer, " 42", 3),
              FLECHA_RUNTIME_INPUT_INVALID_SYNTAX);
    EXPECT_EQ(flecha_runtime_v1_input_parse_i32(&integer, nullptr, 0),
              FLECHA_RUNTIME_INPUT_INVALID_SYNTAX);

    std::uint8_t boolean = 1;
    EXPECT_EQ(flecha_runtime_v1_input_parse_bool(&boolean, "TRUE", 4),
              FLECHA_RUNTIME_INPUT_INVALID_SYNTAX);
    EXPECT_EQ(boolean, 0);

    std::uint8_t character = 1;
    EXPECT_EQ(flecha_runtime_v1_input_parse_char(&character, "xy", 2),
              FLECHA_RUNTIME_INPUT_INVALID_SYNTAX);
    EXPECT_EQ(character, 0);
}

TEST(RuntimeStringViewTests, RejectsInvalidBounds) {
    const char text[] = "flecha";
    EXPECT_DEATH((void)flecha_runtime_v1_string_view_at(text, 6, 6),
                 "out of bounds");
    EXPECT_DEATH(flecha_runtime_v1_string_view_check_substr(6, 5, 2),
                 "out of bounds");
}

TEST(RuntimeStringTests, OwnsClonesAndDestroysIndependentStorage) {
    FlechaRuntimeString first{};
    flecha_runtime_v1_string_from_view(&first, "hi mom", 6);
    ASSERT_NE(first.data, nullptr);
    EXPECT_EQ(first.length, 6);
    EXPECT_EQ(first.capacity, 6);
    EXPECT_STREQ(first.data, "hi mom");

    FlechaRuntimeString copy{};
    flecha_runtime_v1_string_clone(&copy, first.data, first.length);
    ASSERT_NE(copy.data, nullptr);
    EXPECT_NE(copy.data, first.data);
    EXPECT_EQ(copy.length, first.length);
    EXPECT_STREQ(copy.data, first.data);

    flecha_runtime_v1_string_destroy(copy.data);
    flecha_runtime_v1_string_destroy(first.data);
}

TEST(RuntimeStringTests, EmptyStringNeedsNoAllocation) {
    FlechaRuntimeString empty{};
    flecha_runtime_v1_string_from_view(&empty, "", 0);
    EXPECT_EQ(empty.data, nullptr);
    EXPECT_EQ(empty.length, 0);
    EXPECT_EQ(empty.capacity, 0);
    flecha_runtime_v1_string_destroy(empty.data);
}

TEST(RuntimeStringTests, TryFromBytesCopiesEmbeddedNulls) {
    const std::array<std::uint8_t, 4> bytes{'A', 0, 'B', '\n'};
    FlechaRuntimeString output{};

    EXPECT_EQ(flecha_runtime_v1_string_try_from_bytes(&output, bytes.data(),
                                                      bytes.size()),
              FLECHA_RUNTIME_STRING_OK);
    ASSERT_NE(output.data, nullptr);
    EXPECT_EQ(output.length, bytes.size());
    EXPECT_EQ(output.capacity, bytes.size());
    EXPECT_EQ(std::memcmp(output.data, bytes.data(), bytes.size()), 0);
    EXPECT_EQ(output.data[output.length], '\0');

    flecha_runtime_v1_string_destroy(output.data);
}

TEST(RuntimeStringTests, TryFromBytesAcceptsEmptyNullStorage) {
    FlechaRuntimeString output{reinterpret_cast<char*>(1), 1, 1};

    EXPECT_EQ(flecha_runtime_v1_string_try_from_bytes(&output, nullptr, 0),
              FLECHA_RUNTIME_STRING_OK);
    EXPECT_EQ(output.data, nullptr);
    EXPECT_EQ(output.length, 0);
    EXPECT_EQ(output.capacity, 0);
}

TEST(RuntimeStringTests, TryFromBytesRejectsSizeOverflowTransactionally) {
    const std::uint8_t byte = 'x';
    FlechaRuntimeString output{reinterpret_cast<char*>(1), 1, 1};

    EXPECT_EQ(flecha_runtime_v1_string_try_from_bytes(
                  &output, &byte, std::numeric_limits<std::size_t>::max()),
              FLECHA_RUNTIME_STRING_OUT_OF_MEMORY);
    EXPECT_EQ(output.data, nullptr);
    EXPECT_EQ(output.length, 0);
    EXPECT_EQ(output.capacity, 0);
}

TEST(RuntimeStringTests, ConcatenatesIndependentOwnedStorage) {
    FlechaRuntimeString joined{};
    flecha_runtime_v1_string_concat(&joined, "fle", 3, "cha", 3);
    ASSERT_NE(joined.data, nullptr);
    EXPECT_EQ(joined.length, 6);
    EXPECT_EQ(joined.capacity, 6);
    EXPECT_STREQ(joined.data, "flecha");
    flecha_runtime_v1_string_destroy(joined.data);
}

TEST(RuntimeFormatBufferTests, AppendsTypedValuesAndTransfersOwnership) {
    FlechaRuntimeFormatBuffer buffer{};
    flecha_runtime_v1_format_buffer_init(&buffer);
    flecha_runtime_v1_format_buffer_append_string_view(&buffer, "value=", 6);
    flecha_runtime_v1_format_buffer_append_i32(&buffer, -42);
    flecha_runtime_v1_format_buffer_append_char(&buffer, '|');
    flecha_runtime_v1_format_buffer_append_bool(&buffer, 1);
    flecha_runtime_v1_format_buffer_append_char(&buffer, '|');
    flecha_runtime_v1_format_buffer_append_f64(&buffer, 1.5);

    FlechaRuntimeString output{};
    EXPECT_EQ(flecha_runtime_v1_format_buffer_finish(&buffer, &output),
              FLECHA_RUNTIME_STRING_OK);
    ASSERT_NE(output.data, nullptr);
    EXPECT_EQ(std::string(output.data, output.length), "value=-42|true|1.5");
    EXPECT_EQ(output.data[output.length], '\0');
    EXPECT_EQ(buffer.data, nullptr);
    EXPECT_EQ(buffer.length, 0u);
    EXPECT_EQ(buffer.capacity, 0u);
    EXPECT_EQ(buffer.status, FLECHA_RUNTIME_STRING_OK);
    flecha_runtime_v1_string_destroy(output.data);
}

TEST(RuntimeFormatBufferTests, FinishesEmptyOutputWithoutAllocation) {
    FlechaRuntimeFormatBuffer buffer{};
    flecha_runtime_v1_format_buffer_init(&buffer);
    flecha_runtime_v1_format_buffer_append_string_view(&buffer, nullptr, 0);

    FlechaRuntimeString output{reinterpret_cast<char*>(1), 1, 1};
    EXPECT_EQ(flecha_runtime_v1_format_buffer_finish(&buffer, &output),
              FLECHA_RUNTIME_STRING_OK);
    EXPECT_EQ(output.data, nullptr);
    EXPECT_EQ(output.length, 0u);
    EXPECT_EQ(output.capacity, 0u);
}

TEST(RuntimeFormatBufferTests, PreservesFailureAndSupportsAFreshRetry) {
    FlechaRuntimeFormatBuffer buffer{};
    flecha_runtime_v1_format_buffer_init(&buffer);
    flecha_runtime_v1_format_buffer_append_string_view(&buffer, "partial", 7);
    ASSERT_NE(buffer.data, nullptr);
    buffer.status = FLECHA_RUNTIME_STRING_OUT_OF_MEMORY;
    flecha_runtime_v1_format_buffer_append_string_view(&buffer, "ignored", 7);

    FlechaRuntimeString output{reinterpret_cast<char*>(1), 1, 1};
    EXPECT_EQ(flecha_runtime_v1_format_buffer_finish(&buffer, &output),
              FLECHA_RUNTIME_STRING_OUT_OF_MEMORY);
    EXPECT_EQ(output.data, nullptr);
    EXPECT_EQ(output.length, 0u);
    EXPECT_EQ(output.capacity, 0u);
    EXPECT_EQ(buffer.data, nullptr);
    EXPECT_EQ(buffer.status, FLECHA_RUNTIME_STRING_OK);

    flecha_runtime_v1_format_buffer_append_string_view(&buffer, "retry", 5);
    EXPECT_EQ(flecha_runtime_v1_format_buffer_finish(&buffer, &output),
              FLECHA_RUNTIME_STRING_OK);
    ASSERT_NE(output.data, nullptr);
    EXPECT_EQ(std::string(output.data, output.length), "retry");
    flecha_runtime_v1_string_destroy(output.data);
}

TEST(RuntimeFloatTextTests, ParsesCompleteLocaleIndependentValues) {
    float narrow = 0;
    double wide = 0;

    EXPECT_EQ(flecha_runtime_v1_parse_f32(&narrow, "-12.5e2", 7),
              FLECHA_RUNTIME_FLOAT_TEXT_OK);
    EXPECT_EQ(narrow, -1250.0F);
    EXPECT_EQ(flecha_runtime_v1_parse_f64(&wide, "+0.125", 6),
              FLECHA_RUNTIME_FLOAT_TEXT_OK);
    EXPECT_EQ(wide, 0.125);

    EXPECT_EQ(flecha_runtime_v1_parse_f64(&wide, "1.5x", 4),
              FLECHA_RUNTIME_FLOAT_TEXT_INVALID_SYNTAX);
    EXPECT_EQ(wide, 0.0);
    EXPECT_EQ(flecha_runtime_v1_parse_f64(&wide, " 1.5", 4),
              FLECHA_RUNTIME_FLOAT_TEXT_INVALID_SYNTAX);
    EXPECT_EQ(flecha_runtime_v1_parse_f64(&wide, "1e9999", 6),
              FLECHA_RUNTIME_FLOAT_TEXT_OUT_OF_RANGE);
    EXPECT_EQ(wide, 0.0);
    EXPECT_EQ(flecha_runtime_v1_parse_f64(&wide, "NAN", 3),
              FLECHA_RUNTIME_FLOAT_TEXT_INVALID_SYNTAX);
    EXPECT_EQ(flecha_runtime_v1_parse_f64(&wide, "nan(payload)", 12),
              FLECHA_RUNTIME_FLOAT_TEXT_INVALID_SYNTAX);
}

TEST(RuntimeFloatTextTests, FormatsShortestRoundTrippableValues) {
    std::array<std::uint8_t, 64> text{};
    std::size_t written = 99;

    EXPECT_EQ(
        flecha_runtime_v1_format_f64(text.data(), text.size(), 1.25, &written),
        FLECHA_RUNTIME_FLOAT_TEXT_OK);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(text.data()), written),
              "1.25");

    double parsed = 0;
    EXPECT_EQ(flecha_runtime_v1_parse_f64(
                  &parsed, reinterpret_cast<char*>(text.data()), written),
              FLECHA_RUNTIME_FLOAT_TEXT_OK);
    EXPECT_EQ(parsed, 1.25);

    EXPECT_EQ(
        flecha_runtime_v1_format_f64(text.data(), text.size(), -0.0, &written),
        FLECHA_RUNTIME_FLOAT_TEXT_OK);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(text.data()), written), "-0");
    EXPECT_EQ(flecha_runtime_v1_parse_f64(
                  &parsed, reinterpret_cast<char*>(text.data()), written),
              FLECHA_RUNTIME_FLOAT_TEXT_OK);
    EXPECT_TRUE(std::signbit(parsed));

    EXPECT_EQ(flecha_runtime_v1_format_f64(
                  text.data(), text.size(),
                  std::numeric_limits<double>::infinity(), &written),
              FLECHA_RUNTIME_FLOAT_TEXT_OK);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(text.data()), written),
              "inf");
    EXPECT_EQ(flecha_runtime_v1_format_f64(
                  text.data(), text.size(),
                  std::numeric_limits<double>::quiet_NaN(), &written),
              FLECHA_RUNTIME_FLOAT_TEXT_OK);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(text.data()), written),
              "nan");
}

TEST(RuntimeFloatTextTests, PreservesDestinationWhenSpaceIsInsufficient) {
    std::array<std::uint8_t, 3> text{'k', 'e', 'p'};
    std::size_t written = 99;

    EXPECT_EQ(
        flecha_runtime_v1_format_f64(text.data(), text.size(), 12.5, &written),
        FLECHA_RUNTIME_FLOAT_TEXT_INSUFFICIENT_SPACE);
    EXPECT_EQ(written, 0);
    EXPECT_EQ(text, (std::array<std::uint8_t, 3>{'k', 'e', 'p'}));
}

TEST(RuntimeFloatTextTests, RoundTripsFiniteWidthBoundaries) {
    const auto roundTrip32 = [](float value) {
        std::array<std::uint8_t, 64> text{};
        std::size_t written = 0;
        EXPECT_EQ(flecha_runtime_v1_format_f32(text.data(), text.size(), value,
                                               &written),
                  FLECHA_RUNTIME_FLOAT_TEXT_OK);

        float parsed = 0;
        EXPECT_EQ(flecha_runtime_v1_parse_f32(
                      &parsed, reinterpret_cast<char*>(text.data()), written),
                  FLECHA_RUNTIME_FLOAT_TEXT_OK);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(parsed),
                  std::bit_cast<std::uint32_t>(value));
    };
    const auto roundTrip64 = [](double value) {
        std::array<std::uint8_t, 64> text{};
        std::size_t written = 0;
        EXPECT_EQ(flecha_runtime_v1_format_f64(text.data(), text.size(), value,
                                               &written),
                  FLECHA_RUNTIME_FLOAT_TEXT_OK);

        double parsed = 0;
        EXPECT_EQ(flecha_runtime_v1_parse_f64(
                      &parsed, reinterpret_cast<char*>(text.data()), written),
                  FLECHA_RUNTIME_FLOAT_TEXT_OK);
        EXPECT_EQ(std::bit_cast<std::uint64_t>(parsed),
                  std::bit_cast<std::uint64_t>(value));
    };

    roundTrip32(std::numeric_limits<float>::denorm_min());
    roundTrip32(std::numeric_limits<float>::min());
    roundTrip32(std::numeric_limits<float>::max());
    roundTrip32(1.0F / 3.0F);
    roundTrip64(std::numeric_limits<double>::denorm_min());
    roundTrip64(std::numeric_limits<double>::min());
    roundTrip64(std::numeric_limits<double>::max());
    roundTrip64(1.0 / 3.0);
}

TEST(RuntimeFilesystemTests, ReadsEmptyAndBinaryFiles) {
    const auto directory = std::filesystem::temp_directory_path();
    const auto path = directory / "flecha_runtime_read_file_test.bin";
    const std::string bytes{"A\0B\n", 4};
    {
        std::ofstream stream(path, std::ios::binary);
        ASSERT_TRUE(stream.good());
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    FlechaRuntimeString output{};
    const std::string nativePath = path.string();
    EXPECT_EQ(flecha_runtime_v1_read_file(&output, nativePath.data(),
                                          nativePath.size()),
              FLECHA_RUNTIME_READ_FILE_OK);
    ASSERT_NE(output.data, nullptr);
    EXPECT_EQ(output.length, bytes.size());
    EXPECT_EQ(std::memcmp(output.data, bytes.data(), bytes.size()), 0);
    EXPECT_EQ(output.data[output.length], '\0');
    flecha_runtime_v1_string_destroy(output.data);

    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stream.good());
    }
    EXPECT_EQ(flecha_runtime_v1_read_file(&output, nativePath.data(),
                                          nativePath.size()),
              FLECHA_RUNTIME_READ_FILE_OK);
    ASSERT_NE(output.data, nullptr);
    EXPECT_EQ(output.length, 0);
    EXPECT_EQ(output.data[0], '\0');
    flecha_runtime_v1_string_destroy(output.data);
    EXPECT_TRUE(std::filesystem::remove(path));
}

TEST(RuntimeFilesystemTests, RejectsInvalidAndMissingPaths) {
    FlechaRuntimeString output{reinterpret_cast<char*>(1), 1, 1};
    const char invalid[] = {'a', '\0', 'b'};
    EXPECT_EQ(flecha_runtime_v1_read_file(&output, invalid, sizeof(invalid)),
              FLECHA_RUNTIME_READ_FILE_INVALID_PATH);
    EXPECT_EQ(output.data, nullptr);
    EXPECT_EQ(output.length, 0);

    const std::string missing = (std::filesystem::temp_directory_path() /
                                 "flecha_file_that_does_not_exist_7d12")
                                    .string();
    EXPECT_EQ(
        flecha_runtime_v1_read_file(&output, missing.data(), missing.size()),
        FLECHA_RUNTIME_READ_FILE_NOT_FOUND);
    EXPECT_EQ(output.data, nullptr);
}

class RuntimeFilesystemMutationTests : public ::testing::Test {
   protected:
    void SetUp() override { std::filesystem::remove_all(Root()); }
    void TearDown() override { std::filesystem::remove_all(Root()); }

    static std::filesystem::path Root() {
        return std::filesystem::temp_directory_path() /
               "flecha_runtime_filesystem_mutation_test";
    }
};

TEST_F(RuntimeFilesystemMutationTests, CreatesRenamesAndRemovesPaths) {
    const std::string directory = Root().string();
    uint8_t present = 99;
    EXPECT_EQ(flecha_runtime_v1_path_exists(&present, directory.data(),
                                            directory.size()),
              FLECHA_RUNTIME_FILESYSTEM_OK);
    EXPECT_EQ(present, 0);

    ASSERT_EQ(
        flecha_runtime_v1_create_directory(directory.data(), directory.size()),
        FLECHA_RUNTIME_FILESYSTEM_OK);
    EXPECT_EQ(
        flecha_runtime_v1_create_directory(directory.data(), directory.size()),
        FLECHA_RUNTIME_FILESYSTEM_ALREADY_EXISTS);
    EXPECT_EQ(flecha_runtime_v1_path_exists(&present, directory.data(),
                                            directory.size()),
              FLECHA_RUNTIME_FILESYSTEM_OK);
    EXPECT_EQ(present, 1);

    const auto sourcePath = Root() / "source.bin";
    const auto destinationPath = Root() / "destination.bin";
    {
        std::ofstream source(sourcePath, std::ios::binary);
        ASSERT_TRUE(source.good());
        source << "flecha";
    }
    const std::string source = sourcePath.string();
    const std::string destination = destinationPath.string();
    EXPECT_EQ(
        flecha_runtime_v1_path_exists(&present, source.data(), source.size()),
        FLECHA_RUNTIME_FILESYSTEM_OK);
    EXPECT_EQ(present, 1);
    EXPECT_EQ(flecha_runtime_v1_create_directory(source.data(), source.size()),
              FLECHA_RUNTIME_FILESYSTEM_ALREADY_EXISTS);

    ASSERT_EQ(flecha_runtime_v1_rename(source.data(), source.size(),
                                       destination.data(), destination.size()),
              FLECHA_RUNTIME_FILESYSTEM_OK);
    EXPECT_FALSE(std::filesystem::exists(sourcePath));
    EXPECT_TRUE(std::filesystem::exists(destinationPath));
    {
        std::ifstream destinationFile(destinationPath, std::ios::binary);
        ASSERT_TRUE(destinationFile.good());
        std::string contents;
        destinationFile >> contents;
        EXPECT_EQ(contents, "flecha");
    }

    const std::string missingParent = (Root() / "missing" / "child").string();
    EXPECT_EQ(flecha_runtime_v1_create_directory(missingParent.data(),
                                                 missingParent.size()),
              FLECHA_RUNTIME_FILESYSTEM_NOT_FOUND);
    const std::string missingSource = (Root() / "missing.bin").string();
    EXPECT_EQ(
        flecha_runtime_v1_rename(missingSource.data(), missingSource.size(),
                                 source.data(), source.size()),
        FLECHA_RUNTIME_FILESYSTEM_NOT_FOUND);

    ASSERT_EQ(
        flecha_runtime_v1_remove_file(destination.data(), destination.size()),
        FLECHA_RUNTIME_FILESYSTEM_OK);
    EXPECT_EQ(flecha_runtime_v1_path_exists(&present, destination.data(),
                                            destination.size()),
              FLECHA_RUNTIME_FILESYSTEM_OK);
    EXPECT_EQ(present, 0);
    EXPECT_EQ(
        flecha_runtime_v1_remove_file(destination.data(), destination.size()),
        FLECHA_RUNTIME_FILESYSTEM_NOT_FOUND);
}

TEST_F(RuntimeFilesystemMutationTests, RejectsInvalidPathsAndOutputs) {
    const char invalid[] = {'a', '\0', 'b'};
    uint8_t present = 99;
    EXPECT_EQ(flecha_runtime_v1_path_exists(&present, invalid, sizeof(invalid)),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH);
    EXPECT_EQ(present, 0);
    EXPECT_EQ(flecha_runtime_v1_path_exists(nullptr, "path", 4),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS);
    EXPECT_EQ(flecha_runtime_v1_path_contains(nullptr, "parent", 6, "child", 5),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS);
    present = 99;
    EXPECT_EQ(flecha_runtime_v1_path_contains(&present, invalid,
                                              sizeof(invalid), "child", 5),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH);
    EXPECT_EQ(present, 0);
    uint8_t type = 99;
    uint64_t size = 99;
    EXPECT_EQ(
        flecha_runtime_v1_metadata(&type, &size, invalid, sizeof(invalid)),
        FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH);
    EXPECT_EQ(type, 0);
    EXPECT_EQ(size, 0u);
    EXPECT_EQ(flecha_runtime_v1_metadata(nullptr, &size, "path", 4),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS);
    EXPECT_EQ(flecha_runtime_v1_metadata(&type, nullptr, "path", 4),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS);
    present = 99;
    EXPECT_EQ(flecha_runtime_v1_path_exists(&present, nullptr, 0),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH);
    EXPECT_EQ(present, 0);
    EXPECT_EQ(flecha_runtime_v1_path_exists(&present, "", 0),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH);
    EXPECT_EQ(flecha_runtime_v1_remove_file(invalid, sizeof(invalid)),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH);
    EXPECT_EQ(flecha_runtime_v1_remove_file(nullptr, 0),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH);
    EXPECT_EQ(flecha_runtime_v1_create_directory(invalid, sizeof(invalid)),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH);
    EXPECT_EQ(flecha_runtime_v1_create_directory(nullptr, 0),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH);
    EXPECT_EQ(flecha_runtime_v1_rename("source", 6, invalid, sizeof(invalid)),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH);
    EXPECT_EQ(flecha_runtime_v1_rename(nullptr, 0, "destination", 11),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH);
    EXPECT_EQ(
        flecha_runtime_v1_remove_directory(invalid, sizeof(invalid), 0, 0),
        FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH);
    EXPECT_EQ(flecha_runtime_v1_remove_directory("path", 4, 2, 0),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS);
    EXPECT_EQ(flecha_runtime_v1_remove_directory("path", 4, 0, 2),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS);
}

TEST_F(RuntimeFilesystemMutationTests,
       ResolvesContainmentIncludingSymbolicLinkAliases) {
    ASSERT_TRUE(std::filesystem::create_directory(Root()));
    const auto sourcePath = Root() / "source";
    const auto siblingPath = Root() / "sibling";
    ASSERT_TRUE(std::filesystem::create_directory(sourcePath));
    ASSERT_TRUE(std::filesystem::create_directory(siblingPath));

    const std::string source = sourcePath.string();
    const std::string child = (sourcePath / "missing-child").string();
    const std::string sibling = (siblingPath / "missing-child").string();
    uint8_t contained = 99;
    EXPECT_EQ(flecha_runtime_v1_path_contains(&contained, source.data(),
                                              source.size(), child.data(),
                                              child.size()),
              FLECHA_RUNTIME_FILESYSTEM_OK);
    EXPECT_EQ(contained, 1);
    EXPECT_EQ(flecha_runtime_v1_path_contains(&contained, source.data(),
                                              source.size(), sibling.data(),
                                              sibling.size()),
              FLECHA_RUNTIME_FILESYSTEM_OK);
    EXPECT_EQ(contained, 0);

    std::error_code linkError;
    const auto aliasPath = Root() / "source-alias";
    std::filesystem::create_directory_symlink(sourcePath, aliasPath, linkError);
    if (!linkError) {
        const std::string aliasedChild = (aliasPath / "nested").string();
        EXPECT_EQ(flecha_runtime_v1_path_contains(
                      &contained, source.data(), source.size(),
                      aliasedChild.data(), aliasedChild.size()),
                  FLECHA_RUNTIME_FILESYSTEM_OK);
        EXPECT_EQ(contained, 1);
    }

    const std::string missing = (Root() / "missing-parent").string();
    contained = 99;
    EXPECT_EQ(flecha_runtime_v1_path_contains(&contained, missing.data(),
                                              missing.size(), child.data(),
                                              child.size()),
              FLECHA_RUNTIME_FILESYSTEM_NOT_FOUND);
    EXPECT_EQ(contained, 0);
}

TEST_F(RuntimeFilesystemMutationTests, ReportsFileAndDirectoryMetadata) {
    ASSERT_TRUE(std::filesystem::create_directory(Root()));
    const auto filePath = Root() / "data.bin";
    const std::array<char, 4> contents{'A', '\0', 'B', '\n'};
    {
        std::ofstream file(filePath, std::ios::binary);
        ASSERT_TRUE(file.good());
        file.write(contents.data(),
                   static_cast<std::streamsize>(contents.size()));
    }

    uint8_t type = 99;
    uint64_t size = 99;
    const std::string file = filePath.string();
    EXPECT_EQ(
        flecha_runtime_v1_metadata(&type, &size, file.data(), file.size()),
        FLECHA_RUNTIME_FILESYSTEM_OK);
    EXPECT_EQ(type, 1);
    EXPECT_EQ(size, contents.size());

    const std::string directory = Root().string();
    EXPECT_EQ(flecha_runtime_v1_metadata(&type, &size, directory.data(),
                                         directory.size()),
              FLECHA_RUNTIME_FILESYSTEM_OK);
    EXPECT_EQ(type, 2);
    EXPECT_EQ(size, 0u);

    const std::string missing = (Root() / "missing").string();
    EXPECT_EQ(flecha_runtime_v1_metadata(&type, &size, missing.data(),
                                         missing.size()),
              FLECHA_RUNTIME_FILESYSTEM_NOT_FOUND);
    EXPECT_EQ(type, 0);
    EXPECT_EQ(size, 0u);
}

TEST_F(RuntimeFilesystemMutationTests, RemovesEmptyAndRecursiveDirectories) {
    const std::string directory = Root().string();
    ASSERT_EQ(
        flecha_runtime_v1_create_directory(directory.data(), directory.size()),
        FLECHA_RUNTIME_FILESYSTEM_OK);

    const auto childPath = Root() / "child.bin";
    {
        std::ofstream child(childPath, std::ios::binary);
        ASSERT_TRUE(child.good());
        child << "data";
    }
    EXPECT_EQ(flecha_runtime_v1_remove_directory(directory.data(),
                                                 directory.size(), 0, 0),
              FLECHA_RUNTIME_FILESYSTEM_DIRECTORY_NOT_EMPTY);
    EXPECT_EQ(flecha_runtime_v1_remove_directory(directory.data(),
                                                 directory.size(), 0, 1),
              FLECHA_RUNTIME_FILESYSTEM_OK);
    EXPECT_FALSE(std::filesystem::exists(Root()));

    EXPECT_EQ(flecha_runtime_v1_remove_directory(directory.data(),
                                                 directory.size(), 0, 0),
              FLECHA_RUNTIME_FILESYSTEM_NOT_FOUND);
    EXPECT_EQ(flecha_runtime_v1_remove_directory(directory.data(),
                                                 directory.size(), 1, 1),
              FLECHA_RUNTIME_FILESYSTEM_OK);

    ASSERT_EQ(
        flecha_runtime_v1_create_directory(directory.data(), directory.size()),
        FLECHA_RUNTIME_FILESYSTEM_OK);
    EXPECT_EQ(flecha_runtime_v1_remove_directory(directory.data(),
                                                 directory.size(), 0, 0),
              FLECHA_RUNTIME_FILESYSTEM_OK);
    EXPECT_FALSE(std::filesystem::exists(Root()));
}

TEST_F(RuntimeFilesystemMutationTests,
       CreatesPrivateTemporaryDirectoriesAtomically) {
    FlechaRuntimeString first{reinterpret_cast<char*>(1), 1, 1};
    ASSERT_EQ(flecha_runtime_v1_temporary_directory_create(&first),
              FLECHA_RUNTIME_FILESYSTEM_OK);
    ASSERT_NE(first.data, nullptr);
    ASSERT_GT(first.length, 0u);

    const auto* firstBegin = reinterpret_cast<const char8_t*>(first.data);
    const std::u8string firstEncoded(firstBegin, firstBegin + first.length);
    const std::filesystem::path firstPath(firstEncoded);
    EXPECT_TRUE(firstPath.is_absolute());
    EXPECT_TRUE(std::filesystem::is_directory(firstPath));
    EXPECT_TRUE(firstPath.filename().string().starts_with("flecha-"));
#ifndef _WIN32
    const auto permissions = std::filesystem::status(firstPath).permissions();
    EXPECT_EQ(permissions & std::filesystem::perms::group_all,
              std::filesystem::perms::none);
    EXPECT_EQ(permissions & std::filesystem::perms::others_all,
              std::filesystem::perms::none);
#endif

    FlechaRuntimeString second{};
    ASSERT_EQ(flecha_runtime_v1_temporary_directory_create(&second),
              FLECHA_RUNTIME_FILESYSTEM_OK);
    const auto* secondBegin = reinterpret_cast<const char8_t*>(second.data);
    const std::u8string secondEncoded(secondBegin, secondBegin + second.length);
    const std::filesystem::path secondPath(secondEncoded);
    EXPECT_NE(firstPath, secondPath);
    EXPECT_TRUE(std::filesystem::is_directory(secondPath));

    EXPECT_TRUE(std::filesystem::remove(firstPath));
    EXPECT_TRUE(std::filesystem::remove(secondPath));
    flecha_runtime_v1_string_destroy(first.data);
    flecha_runtime_v1_string_destroy(second.data);
}

TEST_F(RuntimeFilesystemMutationTests, ValidatesTemporaryDirectoryOutput) {
    EXPECT_EQ(flecha_runtime_v1_temporary_directory_create(nullptr),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS);
}

TEST_F(RuntimeFilesystemMutationTests,
       IteratesEntryNamesAndTypesWithoutFollowingLinks) {
    ASSERT_TRUE(std::filesystem::create_directory(Root()));
    const auto filePath = Root() / "data.bin";
    const auto directoryPath = Root() / "nested";
    {
        std::ofstream file(filePath, std::ios::binary);
        ASSERT_TRUE(file.good());
        file << "data";
    }
    ASSERT_TRUE(std::filesystem::create_directory(directoryPath));

#ifndef _WIN32
    const auto linkPath = Root() / "data.link";
    std::error_code linkError;
    std::filesystem::create_symlink(filePath, linkPath, linkError);
    ASSERT_FALSE(linkError);
#endif

    const std::string root = Root().string();
    void* handle = nullptr;
    ASSERT_EQ(
        flecha_runtime_v1_directory_open(&handle, root.data(), root.size()),
        FLECHA_RUNTIME_FILESYSTEM_OK);
    ASSERT_NE(handle, nullptr);

    std::map<std::string, uint8_t> entries;
    while (true) {
        FlechaRuntimeString name{};
        uint8_t type = 99;
        uint8_t hasEntry = 99;
        ASSERT_EQ(
            flecha_runtime_v1_directory_next(handle, &name, &type, &hasEntry),
            FLECHA_RUNTIME_FILESYSTEM_OK);
        if (hasEntry == 0) {
            EXPECT_EQ(name.data, nullptr);
            EXPECT_EQ(type, 0);
            break;
        }

        ASSERT_NE(name.data, nullptr);
        const auto [position, inserted] =
            entries.emplace(std::string(name.data, name.length), type);
        ASSERT_TRUE(inserted) << position->first;
        flecha_runtime_v1_string_destroy(name.data);
    }

    entries.erase(".");
    entries.erase("..");
#ifdef _WIN32
    EXPECT_EQ(entries.size(), 2U);
#else
    EXPECT_EQ(entries.size(), 3U);
#endif
    EXPECT_EQ(entries["data.bin"], 1);
    EXPECT_EQ(entries["nested"], 2);
#ifndef _WIN32
    EXPECT_EQ(entries["data.link"], 3);
#endif

    FlechaRuntimeString name{reinterpret_cast<char*>(1), 1, 1};
    uint8_t type = 99;
    uint8_t hasEntry = 99;
    EXPECT_EQ(flecha_runtime_v1_directory_next(handle, &name, &type, &hasEntry),
              FLECHA_RUNTIME_FILESYSTEM_OK);
    EXPECT_EQ(name.data, nullptr);
    EXPECT_EQ(type, 0);
    EXPECT_EQ(hasEntry, 0);
    EXPECT_EQ(flecha_runtime_v1_directory_close(handle),
              FLECHA_RUNTIME_FILESYSTEM_OK);
}

TEST_F(RuntimeFilesystemMutationTests, ValidatesDirectoryIteratorArguments) {
    void* handle = reinterpret_cast<void*>(1);
    EXPECT_EQ(flecha_runtime_v1_directory_open(nullptr, "path", 4),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS);
    EXPECT_EQ(flecha_runtime_v1_directory_open(&handle, nullptr, 0),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH);
    EXPECT_EQ(handle, nullptr);

    const char invalid[] = {'a', '\0', 'b'};
    handle = reinterpret_cast<void*>(1);
    EXPECT_EQ(
        flecha_runtime_v1_directory_open(&handle, invalid, sizeof(invalid)),
        FLECHA_RUNTIME_FILESYSTEM_INVALID_PATH);
    EXPECT_EQ(handle, nullptr);

    const std::string missing = (Root() / "missing").string();
    EXPECT_EQ(flecha_runtime_v1_directory_open(&handle, missing.data(),
                                               missing.size()),
              FLECHA_RUNTIME_FILESYSTEM_NOT_FOUND);
    EXPECT_EQ(handle, nullptr);

    ASSERT_TRUE(std::filesystem::create_directory(Root()));
    const auto filePath = Root() / "not-a-directory";
    {
        std::ofstream file(filePath, std::ios::binary);
        ASSERT_TRUE(file.good());
    }
    const std::string file = filePath.string();
    EXPECT_EQ(
        flecha_runtime_v1_directory_open(&handle, file.data(), file.size()),
        FLECHA_RUNTIME_FILESYSTEM_NOT_FOUND);
    EXPECT_EQ(handle, nullptr);

    FlechaRuntimeString name{reinterpret_cast<char*>(1), 1, 1};
    uint8_t type = 99;
    uint8_t hasEntry = 99;
    EXPECT_EQ(
        flecha_runtime_v1_directory_next(nullptr, &name, &type, &hasEntry),
        FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS);
    EXPECT_EQ(name.data, nullptr);
    EXPECT_EQ(type, 0);
    EXPECT_EQ(hasEntry, 0);
    EXPECT_EQ(flecha_runtime_v1_directory_next(reinterpret_cast<void*>(1),
                                               nullptr, &type, &hasEntry),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS);
    EXPECT_EQ(type, 0);
    EXPECT_EQ(hasEntry, 0);
    EXPECT_EQ(flecha_runtime_v1_directory_close(nullptr),
              FLECHA_RUNTIME_FILESYSTEM_INVALID_OPTIONS);
}

class RuntimeFileStreamTests : public ::testing::Test {
   protected:
    void SetUp() override { std::filesystem::remove(Path()); }
    void TearDown() override { std::filesystem::remove(Path()); }

    static std::filesystem::path Path() {
        return std::filesystem::temp_directory_path() /
               "flecha_runtime_file_stream_test.bin";
    }

    static std::string NativePath() { return Path().string(); }
};

TEST_F(RuntimeFileStreamTests, WritesReadsSeeksAndFlushesBinaryData) {
    const std::string path = NativePath();
    void* handle = nullptr;
    ASSERT_EQ(flecha_runtime_v1_file_stream_open(&handle, path.data(),
                                                 path.size(), 0, 1, 0, 1, 1, 0),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    ASSERT_NE(handle, nullptr);

    const std::array<uint8_t, 4> source{'A', 0, 'B', '\n'};
    size_t written = 99;
    EXPECT_EQ(flecha_runtime_v1_file_stream_write(handle, source.data(),
                                                  source.size(), &written),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    EXPECT_EQ(written, source.size());
    EXPECT_EQ(flecha_runtime_v1_file_stream_flush(handle),
              FLECHA_RUNTIME_FILE_STREAM_OK);

    uint64_t position = 99;
    EXPECT_EQ(flecha_runtime_v1_file_stream_seek(handle, 0, 0, &position),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    EXPECT_EQ(position, 0u);
    EXPECT_EQ(flecha_runtime_v1_file_stream_close(handle),
              FLECHA_RUNTIME_FILE_STREAM_OK);

    ASSERT_EQ(flecha_runtime_v1_file_stream_open(&handle, path.data(),
                                                 path.size(), 1, 1, 0, 0, 0, 0),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    std::array<uint8_t, 8> destination{};
    size_t read = 99;
    EXPECT_EQ(flecha_runtime_v1_file_stream_read(handle, destination.data(),
                                                 destination.size(), &read),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    EXPECT_EQ(read, source.size());
    EXPECT_EQ(std::memcmp(destination.data(), source.data(), source.size()), 0);

    EXPECT_EQ(flecha_runtime_v1_file_stream_seek(handle, -2, 2, &position),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    EXPECT_EQ(position, 2u);
    destination.fill(0);
    EXPECT_EQ(flecha_runtime_v1_file_stream_read(handle, destination.data(), 2,
                                                 &read),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    EXPECT_EQ(read, 2u);
    EXPECT_EQ(destination[0], static_cast<uint8_t>('B'));
    EXPECT_EQ(destination[1], static_cast<uint8_t>('\n'));
    EXPECT_EQ(flecha_runtime_v1_file_stream_close(handle),
              FLECHA_RUNTIME_FILE_STREAM_OK);
}

TEST_F(RuntimeFileStreamTests, CreatesPrivateTemporaryFilesAtomically) {
    void* handle = reinterpret_cast<void*>(1);
    FlechaRuntimeString path{reinterpret_cast<char*>(1), 1, 1};
    ASSERT_EQ(flecha_runtime_v1_temporary_file_open(&handle, &path),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    ASSERT_NE(handle, nullptr);
    ASSERT_NE(path.data, nullptr);
    ASSERT_GT(path.length, 0u);

    const auto* pathBegin = reinterpret_cast<const char8_t*>(path.data);
    const std::u8string encodedPath(pathBegin, pathBegin + path.length);
    const std::filesystem::path nativePath(encodedPath);
    EXPECT_TRUE(nativePath.is_absolute());
    EXPECT_TRUE(std::filesystem::is_regular_file(nativePath));
    EXPECT_EQ(nativePath.filename().string().starts_with("flecha-"), true);
#ifndef _WIN32
    const auto permissions = std::filesystem::status(nativePath).permissions();
    EXPECT_EQ(permissions & std::filesystem::perms::group_all,
              std::filesystem::perms::none);
    EXPECT_EQ(permissions & std::filesystem::perms::others_all,
              std::filesystem::perms::none);
#endif

    const std::array<uint8_t, 4> source{'t', 'e', 'm', 'p'};
    size_t written = 0;
    ASSERT_EQ(flecha_runtime_v1_file_stream_write(handle, source.data(),
                                                  source.size(), &written),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    ASSERT_EQ(written, source.size());

    uint64_t position = 99;
    ASSERT_EQ(flecha_runtime_v1_file_stream_seek(handle, 0, 0, &position),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    std::array<uint8_t, 4> destination{};
    size_t read = 0;
    ASSERT_EQ(flecha_runtime_v1_file_stream_read(handle, destination.data(),
                                                 destination.size(), &read),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    EXPECT_EQ(read, destination.size());
    EXPECT_EQ(destination, source);

    EXPECT_EQ(flecha_runtime_v1_file_stream_close(handle),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    EXPECT_TRUE(std::filesystem::remove(nativePath));
    flecha_runtime_v1_string_destroy(path.data);
}

TEST_F(RuntimeFileStreamTests, ValidatesTemporaryFileOutputs) {
    FlechaRuntimeString path{reinterpret_cast<char*>(1), 1, 1};
    EXPECT_EQ(flecha_runtime_v1_temporary_file_open(nullptr, &path),
              FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS);
    EXPECT_EQ(path.data, nullptr);
    EXPECT_EQ(path.length, 0u);

    void* handle = reinterpret_cast<void*>(1);
    EXPECT_EQ(flecha_runtime_v1_temporary_file_open(&handle, nullptr),
              FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS);
    EXPECT_EQ(handle, nullptr);
}

TEST_F(RuntimeFileStreamTests, CreatesExclusivelyAppendsAndTruncates) {
    const std::string path = NativePath();
    void* handle = nullptr;
    ASSERT_EQ(flecha_runtime_v1_file_stream_open(&handle, path.data(),
                                                 path.size(), 0, 1, 0, 0, 0, 1),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    const std::array<uint8_t, 3> first{'o', 'n', 'e'};
    size_t written = 0;
    ASSERT_EQ(flecha_runtime_v1_file_stream_write(handle, first.data(),
                                                  first.size(), &written),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    ASSERT_EQ(flecha_runtime_v1_file_stream_close(handle),
              FLECHA_RUNTIME_FILE_STREAM_OK);

    handle = reinterpret_cast<void*>(1);
    EXPECT_EQ(flecha_runtime_v1_file_stream_open(&handle, path.data(),
                                                 path.size(), 0, 1, 0, 0, 0, 1),
              FLECHA_RUNTIME_FILE_STREAM_ALREADY_EXISTS);
    EXPECT_EQ(handle, nullptr);

    ASSERT_EQ(flecha_runtime_v1_file_stream_open(&handle, path.data(),
                                                 path.size(), 0, 1, 1, 0, 0, 0),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    uint64_t position = 0;
    ASSERT_EQ(flecha_runtime_v1_file_stream_seek(handle, 0, 0, &position),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    const std::array<uint8_t, 3> second{'t', 'w', 'o'};
    ASSERT_EQ(flecha_runtime_v1_file_stream_write(handle, second.data(),
                                                  second.size(), &written),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    ASSERT_EQ(flecha_runtime_v1_file_stream_close(handle),
              FLECHA_RUNTIME_FILE_STREAM_OK);

    FlechaRuntimeString contents{};
    ASSERT_EQ(flecha_runtime_v1_read_file(&contents, path.data(), path.size()),
              FLECHA_RUNTIME_READ_FILE_OK);
    ASSERT_EQ(contents.length, 6u);
    EXPECT_EQ(std::memcmp(contents.data, "onetwo", 6), 0);
    flecha_runtime_v1_string_destroy(contents.data);

    ASSERT_EQ(flecha_runtime_v1_file_stream_open(&handle, path.data(),
                                                 path.size(), 0, 1, 0, 1, 0, 0),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    ASSERT_EQ(flecha_runtime_v1_file_stream_close(handle),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    ASSERT_EQ(flecha_runtime_v1_read_file(&contents, path.data(), path.size()),
              FLECHA_RUNTIME_READ_FILE_OK);
    EXPECT_EQ(contents.length, 0u);
    flecha_runtime_v1_string_destroy(contents.data);
}

TEST_F(RuntimeFileStreamTests, ValidatesPathsOptionsHandlesAndBuffers) {
    const std::string path = NativePath();
    void* handle = reinterpret_cast<void*>(1);
    const char invalidPath[] = {'a', '\0', 'b'};
    EXPECT_EQ(flecha_runtime_v1_file_stream_open(
                  &handle, invalidPath, sizeof(invalidPath), 1, 0, 0, 0, 0, 0),
              FLECHA_RUNTIME_FILE_STREAM_INVALID_PATH);
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(flecha_runtime_v1_file_stream_open(&handle, path.data(),
                                                 path.size(), 0, 0, 0, 0, 0, 0),
              FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS);
    EXPECT_EQ(flecha_runtime_v1_file_stream_open(&handle, path.data(),
                                                 path.size(), 2, 0, 0, 0, 0, 0),
              FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS);
    EXPECT_EQ(flecha_runtime_v1_file_stream_open(&handle, path.data(),
                                                 path.size(), 1, 0, 1, 0, 0, 0),
              FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS);
    EXPECT_EQ(flecha_runtime_v1_file_stream_open(&handle, path.data(),
                                                 path.size(), 1, 0, 0, 0, 0, 0),
              FLECHA_RUNTIME_FILE_STREAM_NOT_FOUND);
    EXPECT_EQ(handle, nullptr);

    ASSERT_EQ(flecha_runtime_v1_file_stream_open(&handle, path.data(),
                                                 path.size(), 1, 1, 0, 1, 1, 0),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    size_t count = 99;
    EXPECT_EQ(flecha_runtime_v1_file_stream_read(handle, nullptr, 0, &count),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    EXPECT_EQ(count, 0u);
    count = 99;
    EXPECT_EQ(flecha_runtime_v1_file_stream_write(handle, nullptr, 0, &count),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(flecha_runtime_v1_file_stream_read(handle, nullptr, 1, &count),
              FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS);
    EXPECT_EQ(count, 0u);
    uint64_t position = 99;
    EXPECT_EQ(flecha_runtime_v1_file_stream_seek(handle, 0, 3, &position),
              FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS);
    EXPECT_EQ(position, 0u);
    EXPECT_EQ(flecha_runtime_v1_file_stream_close(handle),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    EXPECT_EQ(flecha_runtime_v1_file_stream_close(nullptr),
              FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS);
}

TEST_F(RuntimeFileStreamTests, EnforcesReadAndWriteCapabilities) {
    const std::string path = NativePath();
    {
        std::ofstream file(Path(), std::ios::binary);
        ASSERT_TRUE(file.good());
        file << "data";
    }

    void* handle = nullptr;
    ASSERT_EQ(flecha_runtime_v1_file_stream_open(&handle, path.data(),
                                                 path.size(), 1, 0, 0, 0, 0, 0),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    const uint8_t byte = 1;
    size_t count = 99;
    EXPECT_EQ(flecha_runtime_v1_file_stream_write(handle, &byte, 1, &count),
              FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(flecha_runtime_v1_file_stream_flush(handle),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    EXPECT_EQ(flecha_runtime_v1_file_stream_close(handle),
              FLECHA_RUNTIME_FILE_STREAM_OK);

    ASSERT_EQ(flecha_runtime_v1_file_stream_open(&handle, path.data(),
                                                 path.size(), 0, 1, 0, 0, 0, 0),
              FLECHA_RUNTIME_FILE_STREAM_OK);
    uint8_t destination = 0;
    count = 99;
    EXPECT_EQ(
        flecha_runtime_v1_file_stream_read(handle, &destination, 1, &count),
        FLECHA_RUNTIME_FILE_STREAM_INVALID_OPTIONS);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(flecha_runtime_v1_file_stream_close(handle),
              FLECHA_RUNTIME_FILE_STREAM_OK);
}

TEST(RuntimeAllocationTests, ZeroesStorageAndHonorsAlignment) {
    for (const size_t alignment :
         {size_t{1}, size_t{8}, size_t{64}, size_t{4096}}) {
        void* data = flecha_runtime_v1_alloc(137, alignment);
        ASSERT_NE(data, nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(data) % alignment, 0);
        for (size_t i = 0; i < 137; ++i)
            EXPECT_EQ(static_cast<unsigned char*>(data)[i], 0);
        static_cast<unsigned char*>(data)[136] = 42;
        flecha_runtime_v1_free(data);
    }
    void* empty = flecha_runtime_v1_alloc(0, 1);
    ASSERT_NE(empty, nullptr);
    flecha_runtime_v1_free(empty);
    flecha_runtime_v1_free(nullptr);
}

TEST(RuntimeAllocationTests, RejectsInvalidAlignmentAndSizeOverflow) {
    EXPECT_EQ(flecha_runtime_v1_alloc(1, 0), nullptr);
    EXPECT_EQ(flecha_runtime_v1_alloc(1, 3), nullptr);
    EXPECT_EQ(flecha_runtime_v1_alloc(SIZE_MAX, 8), nullptr);
    EXPECT_EQ(flecha_runtime_v1_alloc(SIZE_MAX - sizeof(void*), 64), nullptr);
}

TEST(RuntimeAllocationTests, AllocatesZeroedRuntimeElementCounts) {
    auto* elements = static_cast<int32_t*>(
        flecha_runtime_v1_alloc_array(37, sizeof(int32_t), alignof(int32_t)));
    ASSERT_NE(elements, nullptr);
    for (size_t i = 0; i < 37; ++i) {
        EXPECT_EQ(elements[i], 0);
        elements[i] = static_cast<int32_t>(i);
    }
    EXPECT_EQ(elements[36], 36);
    EXPECT_EQ(flecha_runtime_v1_allocation_count(elements), 37u);
    flecha_runtime_v1_free(elements);
    void* empty = flecha_runtime_v1_alloc_array(0, sizeof(int32_t), 64);
    ASSERT_NE(empty, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(empty) % 64, 0u);
    EXPECT_EQ(flecha_runtime_v1_allocation_count(empty), 0u);
    EXPECT_EQ(flecha_runtime_v1_allocation_count(nullptr), 0u);
    flecha_runtime_v1_free(empty);
}

TEST(RuntimeAllocationTests, AllocatesZeroSizedRuntimeElements) {
    void* elements = flecha_runtime_v1_alloc_array(37, 0, 64);
    ASSERT_NE(elements, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(elements) % 64, 0u);
    EXPECT_EQ(flecha_runtime_v1_allocation_count(elements), 37u);
    flecha_runtime_v1_free(elements);
}

TEST(RuntimeAllocationTests, RejectsArrayOverflowBeforeAllocating) {
    EXPECT_EQ(flecha_runtime_v1_alloc_array(SIZE_MAX / 4 + 1, 4, 4), nullptr);
    EXPECT_EQ(flecha_runtime_v1_alloc_array(SIZE_MAX, 2, 8), nullptr);
    EXPECT_EQ(flecha_runtime_v1_alloc_array(0, 4, 3), nullptr);
}
