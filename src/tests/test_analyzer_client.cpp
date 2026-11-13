#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "lsp/analyzer_client.hpp"

namespace {

class AnalyzerClientTests : public ::testing::Test {
   protected:
    void SetUp() override {
        const auto* test =
            ::testing::UnitTest::GetInstance()->current_test_info();
        _root = std::filesystem::temp_directory_path() /
                (std::string("flecha_analyzer_client_") + test->name());
        std::filesystem::remove_all(_root);
        std::filesystem::create_directories(_root);
    }

    void TearDown() override { std::filesystem::remove_all(_root); }

    std::filesystem::path _root;
};

TEST_F(AnalyzerClientTests,
       RestartsAnalyzerAndReplaysLatestOpenDocumentsAfterCrash) {
    const std::filesystem::path source = _root / "main.fl";
    std::ofstream(source, std::ios::binary) << "module main;\nvoid main() {}\n";
    flecha::lsp::SubprocessAnalyzerClient client(FLECHA_ANALYZER_FIXTURE);
    EXPECT_EQ(client.Initialize(_root, std::nullopt), "analyzer-fixture");
    client.Open(source, "module main;\nvoid main() {}\n", 1);

    const std::string invalid =
        "module main;\n"
        "structure Holder { Missing resource; };\n"
        "&Missing borrow(&Holder holder) <-holder {\n"
        "    return &holder->resource;\n"
        "}\n";
    client.Change(source, invalid, 2);

    const flecha::lsp::AnalyzerSnapshot recovered = client.Diagnostics(source);
    ASSERT_EQ(recovered.version, 2);
    EXPECT_TRUE(
        std::ranges::any_of(recovered.diagnostics, [](const auto& diagnostic) {
            return diagnostic.message == "unknown type 'Missing'";
        }));

    const std::string valid =
        "module main;\n"
        "structure Resource { i32 value; };\n"
        "structure Holder { Resource resource; };\n"
        "&Resource borrow(&Holder holder) <-holder {\n"
        "    return &holder->resource;\n"
        "}\n";
    client.Change(source, valid, 3);

    const flecha::lsp::AnalyzerSnapshot refreshed = client.Diagnostics(source);
    ASSERT_EQ(refreshed.version, 3);
    EXPECT_TRUE(refreshed.diagnostics.empty());
    const auto tokens = client.SemanticTokens(source);
    EXPECT_FALSE(tokens.empty());
    EXPECT_TRUE(std::ranges::any_of(tokens, [](const auto& token) {
        return token.kind == "structureType";
    }));
    client.Shutdown();
}

}  // namespace
