#pragma once

#include <gtest/gtest.h>
#include <llvm/Support/Process.h>

#include <filesystem>
#include <string>

namespace flecha::testing {

// CTest runs each discovered GoogleTest case in a separate process. Include
// both identities so cases and simultaneous test runs cannot remove each
// other's temporary files. Return the same path throughout one test case.
inline std::filesystem::path TemporaryPath(const std::string& purpose) {
    const ::testing::TestInfo* test =
        ::testing::UnitTest::GetInstance()->current_test_info();
    std::string identity = std::string(test->test_suite_name()) + "-" +
                           test->name();
    for (char& character : identity) {
        if (character == '/' || character == '\\' || character == ':')
            character = '_';
    }
    return std::filesystem::temp_directory_path() /
           ("flecha-" + purpose + "-" + identity + "-" +
            std::to_string(llvm::sys::Process::getProcessId()));
}

}  // namespace flecha::testing
