#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>

#include "runtime/runtime_abi.h"

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
