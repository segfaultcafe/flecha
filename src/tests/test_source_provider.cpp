#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "analyzer/overlay_source_provider.hpp"
#include "core/module_loader.hpp"
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

}  // namespace
