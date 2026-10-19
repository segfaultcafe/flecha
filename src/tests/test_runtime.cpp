#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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

TEST(RuntimeStringTests, ConcatenatesIndependentOwnedStorage) {
    FlechaRuntimeString joined{};
    flecha_runtime_v1_string_concat(&joined, "fle", 3, "cha", 3);
    ASSERT_NE(joined.data, nullptr);
    EXPECT_EQ(joined.length, 6);
    EXPECT_EQ(joined.capacity, 6);
    EXPECT_STREQ(joined.data, "flecha");
    flecha_runtime_v1_string_destroy(joined.data);
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

TEST(RuntimeAllocationTests, RejectsArrayOverflowBeforeAllocating) {
    EXPECT_EQ(flecha_runtime_v1_alloc_array(SIZE_MAX / 4 + 1, 4, 4), nullptr);
    EXPECT_EQ(flecha_runtime_v1_alloc_array(SIZE_MAX, 2, 8), nullptr);
    EXPECT_EQ(flecha_runtime_v1_alloc_array(1, 0, 8), nullptr);
    EXPECT_EQ(flecha_runtime_v1_alloc_array(0, 4, 3), nullptr);
}
