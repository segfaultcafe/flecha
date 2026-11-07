#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "analyzer/analysis_session.hpp"

namespace {

using flecha::analyzer::AnalysisSession;
using flecha::analyzer::AnalysisSnapshot;
using flecha::analyzer::OverlayUpdateResult;

class AnalysisSessionTests : public ::testing::Test {
   protected:
    void SetUp() override {
        const auto* test =
            ::testing::UnitTest::GetInstance()->current_test_info();
        _root = std::filesystem::temp_directory_path() /
                (std::string("flecha_analysis_") + test->name());
        std::filesystem::remove_all(_root);
        std::filesystem::create_directories(_root);
    }

    void TearDown() override { std::filesystem::remove_all(_root); }

    void Write(const std::string& name, const std::string& text) {
        const std::filesystem::path path = _root / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    std::filesystem::path _root;
};

TEST_F(AnalysisSessionTests, AnalyzesSavedSource) {
    Write("main.fl", "module main;\nvoid main() {}\n");
    AnalysisSession session(_root);

    AnalysisSnapshot snapshot = session.Analyze("main.fl");

    EXPECT_EQ(snapshot.entry, _root / "main.fl");
    EXPECT_FALSE(snapshot.version);
    ASSERT_NE(snapshot.unit, nullptr);
    ASSERT_TRUE(snapshot.semanticModel);
    EXPECT_FALSE(snapshot.HasErrors());
}

TEST_F(AnalysisSessionTests, ReportsSyntaxDiagnosticsFromUnsavedSource) {
    Write("main.fl", "module main;\nvoid main() {}\n");
    AnalysisSession session(_root);
    ASSERT_EQ(session.Open("main.fl", "module main;\nvoid main() { ` }\n", 7),
              OverlayUpdateResult::Applied);

    AnalysisSnapshot snapshot = session.Analyze("main.fl");

    ASSERT_TRUE(snapshot.version);
    EXPECT_EQ(*snapshot.version, 7);
    EXPECT_TRUE(snapshot.HasErrors());
    EXPECT_FALSE(snapshot.semanticModel);
    bool foundUnexpectedCharacter = false;
    for (const auto& diagnostic : snapshot.diagnostics.Diagnostics()) {
        if (diagnostic.code ==
            flecha::diagnostics::DiagnosticCode::UnexpectedCharacter)
            foundUnexpectedCharacter = true;
    }
    EXPECT_TRUE(foundUnexpectedCharacter);
}

TEST_F(AnalysisSessionTests, ReportsSemanticDiagnosticsWithModel) {
    Write("main.fl", "module main;\nvoid main() {}\n");
    AnalysisSession session(_root);
    ASSERT_EQ(session.Open("main.fl",
                           "module main;\nvoid main() { missing(); }\n", 1),
              OverlayUpdateResult::Applied);

    AnalysisSnapshot snapshot = session.Analyze("main.fl");

    ASSERT_TRUE(snapshot.semanticModel);
    EXPECT_TRUE(snapshot.HasErrors());
    bool foundUnknownName = false;
    for (const auto& diagnostic : snapshot.diagnostics.Diagnostics()) {
        if (diagnostic.code == flecha::diagnostics::DiagnosticCode::UnknownName)
            foundUnknownName = true;
    }
    EXPECT_TRUE(foundUnknownName);
}

TEST_F(AnalysisSessionTests, TracksChangesAndReturnsToFilesystemOnClose) {
    Write("main.fl", "module main;\nvoid main() {}\n");
    AnalysisSession session(_root);
    ASSERT_EQ(session.Open("main.fl", "module main;\nvoid main() { ` }\n", 1),
              OverlayUpdateResult::Applied);
    ASSERT_EQ(session.Change("main.fl", "module main;\nvoid main() {}\n", 2),
              OverlayUpdateResult::Applied);

    AnalysisSnapshot changed = session.Analyze("main.fl");
    ASSERT_TRUE(changed.version);
    EXPECT_EQ(*changed.version, 2);
    EXPECT_FALSE(changed.HasErrors());

    ASSERT_TRUE(session.Close("main.fl"));
    AnalysisSnapshot closed = session.Analyze("main.fl");
    EXPECT_FALSE(closed.version);
    EXPECT_FALSE(closed.HasErrors());
}

TEST_F(AnalysisSessionTests, AppliesOverlaysToImportedModules) {
    Write("dependency.fl",
          "module dependency;\nexport i32 value() { return 42; }\n");
    Write("main.fl",
          "module main;\n@imports :: root::dependency;\n"
          "i32 main() { return dependency::value(); }\n");
    AnalysisSession session(_root);
    ASSERT_EQ(session.Open("dependency.fl",
                           "module dependency;\n"
                           "export i32 other() { return 0; }\n",
                           3),
              OverlayUpdateResult::Applied);

    AnalysisSnapshot snapshot = session.Analyze("main.fl");

    ASSERT_TRUE(snapshot.semanticModel);
    EXPECT_TRUE(snapshot.HasErrors());
}

}  // namespace
