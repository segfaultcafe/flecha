#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "analyzer/analysis_session.hpp"

namespace {

using flecha::analyzer::AnalysisSession;
using flecha::analyzer::AnalysisSnapshot;
using flecha::analyzer::OverlayUpdateResult;
using flecha::analyzer::SemanticItemKind;

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

bool HasModule(const AnalysisSnapshot& snapshot, const std::string& name) {
    return snapshot.unit &&
           std::ranges::any_of(snapshot.unit->modules, [&](const auto& module) {
               return module.name == name;
           });
}

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

TEST_F(AnalysisSessionTests, AnalyzesTestModuleEntryWithoutCommandDiagnostic) {
    Write("value.fl", "module value;\nexport i32 answer() { return 42; }\n");
    Write("value_test.fl",
          "module value_test;\n@imports :: root::value;\n");
    AnalysisSession session(_root);

    AnalysisSnapshot snapshot = session.Analyze("value_test.fl");

    ASSERT_TRUE(snapshot.semanticModel);
    EXPECT_FALSE(snapshot.HasErrors());
    for (const auto& diagnostic : snapshot.diagnostics.Diagnostics())
        EXPECT_NE(diagnostic.message,
                  "test modules can only be compiled as the entry of "
                  "'flecha test'");
}

TEST_F(AnalysisSessionTests, AnalyzesUnsavedTestModuleHeaderFromOverlay) {
    Write("value.fl", "module value;\nexport i32 answer() { return 42; }\n");
    Write("cases.fl", "module cases;\n");
    AnalysisSession session(_root);
    ASSERT_EQ(session.Open("cases.fl",
                           "module value_test;\n"
                           "@imports :: root::value;\n",
                           1),
              OverlayUpdateResult::Applied);

    AnalysisSnapshot snapshot = session.Analyze("cases.fl");

    ASSERT_TRUE(snapshot.semanticModel);
    EXPECT_FALSE(snapshot.HasErrors());
}

TEST_F(AnalysisSessionTests, TestModuleAnalysisStillRequiresProductionPair) {
    Write("value.fl", "module value;\nexport i32 answer() { return 42; }\n");
    Write("value_test.fl", "module value_test;\n");
    AnalysisSession session(_root);

    AnalysisSnapshot snapshot = session.Analyze("value_test.fl");

    EXPECT_TRUE(snapshot.HasErrors());
    EXPECT_TRUE(std::ranges::any_of(
        snapshot.diagnostics.Diagnostics(), [](const auto& diagnostic) {
            return diagnostic.message ==
                   "test module must explicitly import its paired production "
                   "module 'value'";
        }));
}

TEST_F(AnalysisSessionTests, StillRejectsImportedTestModules) {
    Write("value.fl", "module value;\nexport i32 answer() { return 42; }\n");
    Write("value_test.fl",
          "module value_test;\n@imports :: root::value;\n");
    Write("main.fl",
          "module main;\n@imports :: root::value_test;\nvoid main() {}\n");
    AnalysisSession session(_root);

    AnalysisSnapshot snapshot = session.Analyze("main.fl");

    EXPECT_TRUE(snapshot.HasErrors());
    EXPECT_TRUE(std::ranges::any_of(
        snapshot.diagnostics.Diagnostics(), [](const auto& diagnostic) {
            return diagnostic.message ==
                   "test modules can only be compiled as the entry of "
                   "'flecha test'";
        }));
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

TEST_F(AnalysisSessionTests,
       IncrementallyTracksOverlayOnlyAttachedWildcardModules) {
    Write("foo/ordinary.fl", "module ordinary;\n");
    Write("main.fl",
          "module main;\n@imports :: root::foo::*;\nvoid main() {}\n");
    const std::filesystem::path attached = "foo/deep/attached.fl";
    AnalysisSession session(_root);

    AnalysisSnapshot initial = session.Analyze("main.fl");
    ASSERT_FALSE(initial.HasErrors());
    EXPECT_TRUE(HasModule(initial, "ordinary"));
    EXPECT_FALSE(HasModule(initial, "attached"));

    ASSERT_EQ(session.Open(attached,
                           "module attached;\n"
                           "under namespace root::foo;\n",
                           1),
              OverlayUpdateResult::Applied);
    AnalysisSnapshot opened = session.Analyze("main.fl");
    ASSERT_FALSE(opened.HasErrors());
    EXPECT_TRUE(HasModule(opened, "attached"));

    ASSERT_EQ(session.Change(attached,
                             "module renamed;\n"
                             "under namespace root::foo;\n",
                             2),
              OverlayUpdateResult::Applied);
    AnalysisSnapshot renamed = session.Analyze("main.fl");
    ASSERT_FALSE(renamed.HasErrors());
    EXPECT_FALSE(HasModule(renamed, "attached"));
    EXPECT_TRUE(HasModule(renamed, "renamed"));

    std::filesystem::create_directories(_root / "foo/deep");
    ASSERT_EQ(session.Change(attached,
                             "module renamed;\n"
                             "under namespace root::foo::deep;\n",
                             3),
              OverlayUpdateResult::Applied);
    AnalysisSnapshot retargeted = session.Analyze("main.fl");
    ASSERT_FALSE(retargeted.HasErrors());
    EXPECT_FALSE(HasModule(retargeted, "renamed"));

    ASSERT_EQ(session.Change(attached,
                             "module renamed;\n"
                             "under namespace root::foo;\n`\n",
                             4),
              OverlayUpdateResult::Applied);
    AnalysisSnapshot invalidContent = session.Analyze("main.fl");
    EXPECT_TRUE(invalidContent.HasErrors());
    EXPECT_TRUE(std::ranges::any_of(
        invalidContent.diagnostics.Diagnostics(), [&](const auto& diagnostic) {
            return diagnostic.range.start.file == (_root / attached).string();
        }));

    ASSERT_EQ(session.Change(attached,
                             "module renamed;\n"
                             "under namespace root::foo;\n",
                             5),
              OverlayUpdateResult::Applied);
    ASSERT_FALSE(session.Analyze("main.fl").HasErrors());

    ASSERT_TRUE(session.Close(attached));
    AnalysisSnapshot closed = session.Analyze("main.fl");
    ASSERT_FALSE(closed.HasErrors());
    EXPECT_FALSE(HasModule(closed, "renamed"));
}

TEST_F(AnalysisSessionTests, RefreshesCreatedAndRemovedAttachedDiskSources) {
    Write("foo/ordinary.fl", "module ordinary;\n");
    Write("main.fl",
          "module main;\n@imports :: root::foo::*;\nvoid main() {}\n");
    AnalysisSession session(_root);
    ASSERT_FALSE(session.Analyze("main.fl").HasErrors());

    Write("foo/deep/disk.fl", "module disk;\nunder namespace root::foo;\n");
    session.Refresh("foo/deep/disk.fl");
    AnalysisSnapshot created = session.Analyze("main.fl");
    ASSERT_FALSE(created.HasErrors());
    EXPECT_TRUE(HasModule(created, "disk"));

    ASSERT_TRUE(std::filesystem::remove(_root / "foo/deep/disk.fl"));
    session.Refresh("foo/deep/disk.fl");
    AnalysisSnapshot removed = session.Analyze("main.fl");
    ASSERT_FALSE(removed.HasErrors());
    EXPECT_FALSE(HasModule(removed, "disk"));
}

TEST_F(AnalysisSessionTests, ClosingAttachedOverlayRestoresDiskIdentity) {
    Write("foo/deep/source.fl",
          "module fallback;\nunder namespace root::foo;\n");
    Write("main.fl",
          "module main;\n@imports :: root::foo::*;\nvoid main() {}\n");
    AnalysisSession session(_root);
    ASSERT_FALSE(session.Analyze("main.fl").HasErrors());

    ASSERT_EQ(session.Open("foo/deep/source.fl",
                           "module temporary;\n"
                           "under namespace root::foo;\n",
                           1),
              OverlayUpdateResult::Applied);
    AnalysisSnapshot opened = session.Analyze("main.fl");
    ASSERT_FALSE(opened.HasErrors());
    EXPECT_TRUE(HasModule(opened, "temporary"));
    EXPECT_FALSE(HasModule(opened, "fallback"));

    ASSERT_TRUE(session.Close("foo/deep/source.fl"));
    AnalysisSnapshot closed = session.Analyze("main.fl");
    ASSERT_FALSE(closed.HasErrors());
    EXPECT_FALSE(HasModule(closed, "temporary"));
    EXPECT_TRUE(HasModule(closed, "fallback"));
    EXPECT_TRUE(
        std::ranges::any_of(closed.unit->modules, [&](const auto& module) {
            return module.name == "fallback" &&
                   module.file == (_root / "foo/deep/source.fl").string();
        }));
}

TEST_F(AnalysisSessionTests, QueriesHoverDefinitionAndReferencesByByteOffset) {
    const std::string source =
        "module main;\n"
        "i32 twice(i32 value) { return value + value; }\n";
    Write("main.fl", source);
    AnalysisSession session(_root);

    AnalysisSnapshot snapshot = session.Analyze("main.fl");
    ASSERT_TRUE(snapshot.semanticModel);
    ASSERT_FALSE(snapshot.HasErrors());
    const std::size_t use = source.find("value +");
    const std::size_t declaration = source.find("value");

    const auto occurrence = snapshot.OccurrenceAt("main.fl", use + 2);
    ASSERT_TRUE(occurrence);
    EXPECT_EQ(occurrence->name, "value");
    EXPECT_EQ(occurrence->type, "i32");
    EXPECT_EQ(occurrence->kind, SemanticItemKind::Parameter);
    EXPECT_FALSE(occurrence->isDeclaration);

    const auto hover = snapshot.Hover("main.fl", use + 2);
    ASSERT_TRUE(hover);
    EXPECT_EQ(hover->range.start.offset, use);
    EXPECT_EQ(hover->type, "i32");

    const auto definition = snapshot.Definition("main.fl", use + 2);
    ASSERT_TRUE(definition);
    EXPECT_EQ(definition->start.offset, declaration);
    EXPECT_EQ(definition->end.offset, declaration + 5);

    const auto references = snapshot.References("main.fl", use + 2);
    ASSERT_TRUE(references);
    ASSERT_EQ(references->size(), 3);
    EXPECT_EQ((*references)[0].start.offset, declaration);

    const auto occurrences = snapshot.Occurrences("main.fl");
    EXPECT_GE(occurrences.size(), 4);
    EXPECT_TRUE(std::any_of(
        occurrences.begin(), occurrences.end(), [&](const auto& item) {
            return item.range.start.offset == declaration &&
                   item.isDeclaration && item.name == "value";
        }));
}

TEST_F(AnalysisSessionTests, ResolvesDefinitionsAcrossImportedFiles) {
    const std::string dependency =
        "module dependency;\nexport i32 value() { return 42; }\n";
    const std::string main =
        "module main;\n@imports :: root::dependency;\n"
        "i32 main() { return dependency::value(); }\n";
    Write("dependency.fl", dependency);
    Write("main.fl", main);
    AnalysisSession session(_root);

    AnalysisSnapshot snapshot = session.Analyze("main.fl");
    ASSERT_TRUE(snapshot.semanticModel);
    ASSERT_FALSE(snapshot.HasErrors());
    const std::size_t use = main.rfind("value");

    const auto definition = snapshot.Definition("main.fl", use + 1);
    ASSERT_TRUE(definition);
    EXPECT_EQ(definition->start.file, (_root / "dependency.fl").string());
    EXPECT_EQ(definition->start.offset, dependency.find("value"));
}

TEST_F(AnalysisSessionTests, QueriesStructureFieldUsages) {
    const std::string source =
        "module main;\n"
        "structure Point { i32 x; };\n"
        "i32 read(Point point) { return point.x; }\n";
    Write("main.fl", source);
    AnalysisSession session(_root);

    AnalysisSnapshot snapshot = session.Analyze("main.fl");
    ASSERT_TRUE(snapshot.semanticModel);
    ASSERT_FALSE(snapshot.HasErrors());
    const std::size_t use = source.rfind(".x") + 1;
    const std::size_t declaration = source.find("x;");

    const auto hover = snapshot.Hover("main.fl", use);
    ASSERT_TRUE(hover);
    EXPECT_EQ(hover->name, "x");
    EXPECT_EQ(hover->type, "i32");
    EXPECT_EQ(hover->kind, SemanticItemKind::Field);

    const auto definition = snapshot.Definition("main.fl", use);
    ASSERT_TRUE(definition);
    EXPECT_EQ(definition->start.offset, declaration);

    const auto rename = snapshot.Rename("main.fl", use);
    ASSERT_TRUE(rename);
    EXPECT_EQ(rename->name, "x");
    ASSERT_EQ(rename->ranges.size(), 2);
}

TEST_F(AnalysisSessionTests, QueriesNamedTypeUsages) {
    const std::string source =
        "module main;\n"
        "structure Point { i32 x; };\n"
        "i32 read(Point point) { return point.x; }\n";
    Write("main.fl", source);
    AnalysisSession session(_root);

    AnalysisSnapshot snapshot = session.Analyze("main.fl");
    ASSERT_TRUE(snapshot.semanticModel);
    ASSERT_FALSE(snapshot.HasErrors());
    const std::size_t declaration = source.find("Point");
    const std::size_t use = source.rfind("Point");

    const auto occurrence = snapshot.OccurrenceAt("main.fl", use + 1);
    ASSERT_TRUE(occurrence);
    EXPECT_EQ(occurrence->name, "Point");
    EXPECT_EQ(occurrence->type, "Point");
    EXPECT_EQ(occurrence->kind, SemanticItemKind::StructureType);

    const auto definition = snapshot.Definition("main.fl", use + 1);
    ASSERT_TRUE(definition);
    EXPECT_EQ(definition->start.offset, declaration);

    const auto references = snapshot.References("main.fl", use + 1);
    ASSERT_TRUE(references);
    ASSERT_EQ(references->size(), 2);
    EXPECT_EQ((*references)[0].start.offset, declaration);
    EXPECT_EQ((*references)[1].start.offset, use);
}

TEST_F(AnalysisSessionTests, ListsAnalyzedSymbolsWithFiltering) {
    Write("main.fl",
          "module main;\n"
          "i32 twice(i32 value) { return value + value; }\n");
    AnalysisSession session(_root);

    AnalysisSnapshot snapshot = session.Analyze("main.fl");
    ASSERT_TRUE(snapshot.semanticModel);
    const auto symbols = snapshot.Symbols("twice");

    ASSERT_EQ(symbols.size(), 1);
    EXPECT_EQ(symbols.front().kind, SemanticItemKind::Function);
    EXPECT_TRUE(symbols.front().name.ends_with("twice"));
}

TEST_F(AnalysisSessionTests, ResolvesNestedRelativeQueryPathsFromWorkspace) {
    const std::string source =
        "module main;\n"
        "i32 twice(i32 value) { return value + value; }\n";
    Write("src/main.fl", source);
    AnalysisSession session(_root);

    AnalysisSnapshot snapshot = session.Analyze("src/main.fl");
    ASSERT_TRUE(snapshot.semanticModel);
    ASSERT_FALSE(snapshot.HasErrors());
    const std::size_t use = source.find("value +");

    const auto hover = snapshot.Hover("src/main.fl", use + 1);
    ASSERT_TRUE(hover);
    EXPECT_EQ(hover->name, "value");
}

TEST_F(AnalysisSessionTests, UsesOnlyTerminalIdentifierRanges) {
    const std::string dependency =
        "module dependency;\n"
        "export structure Point { i32 value; };\n"
        "export i32 read(const Point point) { return point.value; }\n";
    const std::string main =
        "module main;\n"
        "@imports :: root::dependency;\n"
        "i32 main() { return dependency::read(none); }\n";
    Write("dependency.fl", dependency);
    Write("main.fl", main);
    AnalysisSession session(_root);

    AnalysisSnapshot dependencySnapshot = session.Analyze("dependency.fl");
    ASSERT_TRUE(dependencySnapshot.semanticModel);
    ASSERT_FALSE(dependencySnapshot.HasErrors());
    const std::size_t typeUse = dependency.find("Point point");
    const auto typeRename =
        dependencySnapshot.Rename("dependency.fl", typeUse + 1);
    ASSERT_TRUE(typeRename);
    EXPECT_EQ(typeRename->name, "Point");
    EXPECT_TRUE(std::all_of(typeRename->ranges.begin(),
                            typeRename->ranges.end(), [](const auto& range) {
                                return range.Length() ==
                                       std::string_view("Point").size();
                            }));

    AnalysisSnapshot mainSnapshot = session.Analyze("main.fl");
    ASSERT_TRUE(mainSnapshot.semanticModel);
    ASSERT_FALSE(mainSnapshot.HasErrors());
    const std::size_t qualified = main.find("dependency::read");
    const std::size_t use = qualified + std::string_view("dependency::").size();
    EXPECT_FALSE(mainSnapshot.OccurrenceAt("main.fl", qualified + 1));
    EXPECT_FALSE(mainSnapshot.Definition("main.fl", qualified + 1));
    const auto rename = mainSnapshot.Rename("main.fl", use + 1);
    ASSERT_TRUE(rename);
    EXPECT_EQ(rename->name, "read");
    EXPECT_TRUE(std::any_of(
        rename->ranges.begin(), rename->ranges.end(), [&](const auto& range) {
            return range.start.file == (_root / "main.fl").string() &&
                   range.start.offset == use && range.Length() == 4;
        }));
}

TEST_F(AnalysisSessionTests, GroupsGenericSpecializationsBySourceDeclaration) {
    const std::string source =
        "module main;\n"
        "T identity<T>(T value) { return value; }\n"
        "i32 main() { i32 first = identity<i32>(1); "
        "i64 second = identity<i64>(2); return first; }\n";
    Write("main.fl", source);
    AnalysisSession session(_root);

    AnalysisSnapshot snapshot = session.Analyze("main.fl");
    ASSERT_TRUE(snapshot.semanticModel);
    ASSERT_FALSE(snapshot.HasErrors());
    const std::size_t firstUse = source.find("identity<i32>");
    const std::size_t secondUse = source.find("identity<i64>");

    const auto rename = snapshot.Rename("main.fl", firstUse + 1);
    ASSERT_TRUE(rename);
    EXPECT_TRUE(std::any_of(
        rename->ranges.begin(), rename->ranges.end(),
        [&](const auto& range) { return range.start.offset == firstUse; }));
    EXPECT_TRUE(std::any_of(
        rename->ranges.begin(), rename->ranges.end(),
        [&](const auto& range) { return range.start.offset == secondUse; }));
    EXPECT_EQ(snapshot.Symbols("identity").size(), 1);
}

TEST_F(AnalysisSessionTests, ExcludesInternalAndIntrinsicSymbolsFromQueries) {
    const std::string source =
        "module main;\n"
        "unsafe void release() { free(null); }\n"
        "i32 main() { [2]i32 values = [1, 2]; "
        "for index := range values { return values[index]; } return 0; }\n";
    Write("main.fl", source);
    AnalysisSession session(_root);

    AnalysisSnapshot snapshot = session.Analyze("main.fl");
    ASSERT_TRUE(snapshot.semanticModel);
    ASSERT_FALSE(snapshot.HasErrors());
    const std::size_t freeCall = source.find("free");
    EXPECT_FALSE(snapshot.Rename("main.fl", freeCall + 1));

    const std::size_t rangeUse = source.find("range values") + 6;
    const auto occurrence = snapshot.OccurrenceAt("main.fl", rangeUse + 1);
    ASSERT_TRUE(occurrence);
    EXPECT_EQ(occurrence->name, "values");
    const auto occurrences = snapshot.Occurrences("main.fl");
    EXPECT_TRUE(std::none_of(
        occurrences.begin(), occurrences.end(),
        [](const auto& item) { return item.name.starts_with('$'); }));
}

}  // namespace
