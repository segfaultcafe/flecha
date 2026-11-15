#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "analyzer/overlay_source_provider.hpp"
#include "core/module_loader.hpp"
#include "core/module_source_index.hpp"
#include "core/source_provider.hpp"

namespace {

using flecha::analyzer::OverlaySourceProvider;
using flecha::analyzer::OverlayUpdateResult;
using flecha::analyzer::SourceOverlays;
using flecha::core::FilesystemSourceProvider;
using flecha::core::SourceProvider;

class StubSourceProvider final : public SourceProvider {
   public:
    explicit StubSourceProvider(std::optional<std::string> source)
        : _source(std::move(source)) {}

    [[nodiscard]] std::optional<std::string> Read(
        const std::filesystem::path& path) const override {
        lastPath = path;
        return _source;
    }

    mutable std::filesystem::path lastPath;

   private:
    std::optional<std::string> _source;
};

class CountingSourceProvider final : public SourceProvider {
   public:
    [[nodiscard]] std::optional<std::string> Read(
        const std::filesystem::path& path) const override {
        ++reads;
        return filesystem.Read(path);
    }

    mutable std::size_t reads = 0;

   private:
    FilesystemSourceProvider filesystem;
};

class SourceProviderFilesystemTests : public ::testing::Test {
   protected:
    void SetUp() override {
        static std::size_t sequence = 0;
        _root = std::filesystem::temp_directory_path() /
                ("flecha-source-provider-" + std::to_string(++sequence));
        std::filesystem::create_directories(_root);
    }

    void TearDown() override { std::filesystem::remove_all(_root); }

    std::filesystem::path _root;
};

TEST_F(SourceProviderFilesystemTests, ReadsSourceFromFilesystem) {
    const std::filesystem::path path = _root / "main.fl";
    std::ofstream(path, std::ios::binary) << "module main;\n";
    FilesystemSourceProvider provider;

    const std::optional<std::string> source = provider.Read(path);

    ASSERT_TRUE(source);
    EXPECT_EQ(*source, "module main;\n");
}

TEST_F(SourceProviderFilesystemTests, ReturnsEmptySourceForEmptyFile) {
    const std::filesystem::path path = _root / "empty.fl";
    std::ofstream(path, std::ios::binary);
    FilesystemSourceProvider provider;

    const std::optional<std::string> source = provider.Read(path);

    ASSERT_TRUE(source);
    EXPECT_TRUE(source->empty());
}

TEST_F(SourceProviderFilesystemTests, ReturnsNothingForMissingFile) {
    FilesystemSourceProvider provider;

    EXPECT_FALSE(provider.Read(_root / "missing.fl"));
}

TEST(SourceProviderTests, PrefersOverlayToFallback) {
    SourceOverlays overlays;
    ASSERT_EQ(overlays.Open("/workspace/main.fl", "unsaved", 1),
              OverlayUpdateResult::Applied);
    StubSourceProvider fallback("saved");
    OverlaySourceProvider provider(overlays, fallback);

    const std::optional<std::string> source =
        provider.Read("/workspace/main.fl");

    ASSERT_TRUE(source);
    EXPECT_EQ(*source, "unsaved");
    EXPECT_TRUE(fallback.lastPath.empty());
}

TEST(SourceProviderTests, UsesFallbackWhenOverlayIsMissing) {
    SourceOverlays overlays;
    StubSourceProvider fallback("saved");
    OverlaySourceProvider provider(overlays, fallback);

    const std::optional<std::string> source =
        provider.Read("/workspace/main.fl");

    ASSERT_TRUE(source);
    EXPECT_EQ(*source, "saved");
    EXPECT_EQ(fallback.lastPath, std::filesystem::path("/workspace/main.fl"));
}

TEST(SourceProviderTests, UsesFallbackAfterOverlayCloses) {
    SourceOverlays overlays;
    ASSERT_EQ(overlays.Open("/workspace/main.fl", "unsaved", 1),
              OverlayUpdateResult::Applied);
    StubSourceProvider fallback("saved");
    OverlaySourceProvider provider(overlays, fallback);
    ASSERT_TRUE(overlays.Close("/workspace/main.fl"));

    const std::optional<std::string> source =
        provider.Read("/workspace/main.fl");

    ASSERT_TRUE(source);
    EXPECT_EQ(*source, "saved");
}

TEST(SourceProviderTests, EnumeratesOnlyOverlaySourcesUnderRequestedRoot) {
    SourceOverlays overlays;
    ASSERT_EQ(overlays.Open("/workspace/src/deep/new.fl", "unsaved", 1),
              OverlayUpdateResult::Applied);
    ASSERT_EQ(overlays.Open("/other/source.fl", "other", 1),
              OverlayUpdateResult::Applied);
    StubSourceProvider fallback(std::nullopt);
    OverlaySourceProvider provider(overlays, fallback);

    EXPECT_EQ(provider.KnownSources("/workspace/src"),
              std::vector<std::filesystem::path>{"/workspace/src/deep/new.fl"});
}

TEST_F(SourceProviderFilesystemTests, ModuleLoaderReadsUnsavedOverlay) {
    const std::filesystem::path path = _root / "main.fl";
    std::ofstream(path, std::ios::binary) << "module main;\nvoid main() {}\n";
    SourceOverlays overlays;
    ASSERT_EQ(overlays.Open(path, "module main;\nvoid main() { ` }\n", 1),
              OverlayUpdateResult::Applied);
    FilesystemSourceProvider filesystem;
    OverlaySourceProvider sources(overlays, filesystem);
    flecha::diagnostics::DiagnosticBag diagnostics;
    flecha::core::ModuleLoader loader(diagnostics, sources);

    const auto unit = loader.Load(path, _root);

    ASSERT_NE(unit, nullptr);
    ASSERT_TRUE(diagnostics.HasErrors());
    bool foundUnexpectedCharacter = false;
    for (const auto& diagnostic : diagnostics.Diagnostics()) {
        if (diagnostic.code ==
            flecha::diagnostics::DiagnosticCode::UnexpectedCharacter) {
            foundUnexpectedCharacter = true;
            EXPECT_EQ(diagnostic.range.start.file, path.string());
        }
    }
    EXPECT_TRUE(foundUnexpectedCharacter);
}

TEST_F(SourceProviderFilesystemTests,
       ModuleSourceIndexReusesRootStateAndRefreshesOnePath) {
    const std::filesystem::path ordinary = _root / "foo/ordinary.fl";
    const std::filesystem::path attached = _root / "foo/deep/attached.fl";
    std::filesystem::create_directories(attached.parent_path());
    std::ofstream(ordinary) << "module ordinary;\n";
    std::ofstream(attached) << "module attached;\nunder namespace root::foo;\n";
    CountingSourceProvider sources;
    flecha::core::ModuleSourceIndex index(sources);

    const auto first = index.Discover(_root, _root / "foo");
    ASSERT_EQ(first.size(), 2);
    const std::size_t initialReads = sources.reads;

    const auto second = index.Discover(_root, _root / "foo");
    EXPECT_EQ(second.size(), 2);
    EXPECT_EQ(sources.reads, initialReads);

    std::ofstream(attached) << "module renamed;\nunder namespace root::foo;\n";
    index.Invalidate(attached);
    EXPECT_EQ(sources.reads, initialReads + 1);
    const auto refreshed = index.Discover(_root, _root / "foo");
    ASSERT_EQ(refreshed.size(), 2);
    EXPECT_TRUE(std::ranges::any_of(refreshed, [](const auto& source) {
        return source.name == "renamed";
    }));
    EXPECT_EQ(sources.reads, initialReads + 1);
}

}  // namespace
