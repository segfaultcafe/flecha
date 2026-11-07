#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "analyzer/source_overlays.hpp"

namespace {

using flecha::analyzer::OverlayUpdateResult;
using flecha::analyzer::SourceOverlay;
using flecha::analyzer::SourceOverlays;

TEST(SourceOverlaysTests, OpensAndFindsSource) {
    SourceOverlays overlays;

    EXPECT_EQ(overlays.Open("/workspace/main.fl", "i32 main() {}", 1),
              OverlayUpdateResult::Applied);

    const SourceOverlay* source = overlays.Find("/workspace/main.fl");
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->path, std::filesystem::path("/workspace/main.fl"));
    EXPECT_EQ(source->version, 1);
    EXPECT_EQ(source->text, "i32 main() {}");
}

TEST(SourceOverlaysTests, ChangesOnlyOpenSource) {
    SourceOverlays overlays;

    EXPECT_EQ(overlays.Change("/workspace/main.fl", "changed", 2),
              OverlayUpdateResult::NotFound);
    EXPECT_EQ(overlays.Find("/workspace/main.fl"), nullptr);
}

TEST(SourceOverlaysTests, AppliesOnlyNewerVersions) {
    SourceOverlays overlays;
    ASSERT_EQ(overlays.Open("/workspace/main.fl", "first", 5),
              OverlayUpdateResult::Applied);

    EXPECT_EQ(overlays.Change("/workspace/main.fl", "duplicate", 5),
              OverlayUpdateResult::StaleVersion);
    EXPECT_EQ(overlays.Change("/workspace/main.fl", "stale", 4),
              OverlayUpdateResult::StaleVersion);
    EXPECT_EQ(overlays.Change("/workspace/main.fl", "new", 6),
              OverlayUpdateResult::Applied);

    const SourceOverlay* source = overlays.Find("/workspace/main.fl");
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->version, 6);
    EXPECT_EQ(source->text, "new");
}

TEST(SourceOverlaysTests, ReopensOnlyWithNewerVersion) {
    SourceOverlays overlays;
    ASSERT_EQ(overlays.Open("/workspace/main.fl", "first", 2),
              OverlayUpdateResult::Applied);

    EXPECT_EQ(overlays.Open("/workspace/main.fl", "stale", 1),
              OverlayUpdateResult::StaleVersion);
    EXPECT_EQ(overlays.Open("/workspace/main.fl", "reopened", 3),
              OverlayUpdateResult::Applied);

    const SourceOverlay* source = overlays.Find("/workspace/main.fl");
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->version, 3);
    EXPECT_EQ(source->text, "reopened");
}

TEST(SourceOverlaysTests, NormalizesPathsLexically) {
    SourceOverlays overlays;
    ASSERT_EQ(overlays.Open("/workspace/src/../main.fl", "source", 1),
              OverlayUpdateResult::Applied);

    const SourceOverlay* source = overlays.Find("/workspace/main.fl");
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->path, std::filesystem::path("/workspace/main.fl"));
}

TEST(SourceOverlaysTests, ClosesSource) {
    SourceOverlays overlays;
    ASSERT_EQ(overlays.Open("/workspace/main.fl", "source", 1),
              OverlayUpdateResult::Applied);

    EXPECT_TRUE(overlays.Close("/workspace/main.fl"));
    EXPECT_EQ(overlays.Find("/workspace/main.fl"), nullptr);
    EXPECT_FALSE(overlays.Close("/workspace/main.fl"));
}

TEST(SourceOverlaysTests, KeepsSourcesIndependent) {
    SourceOverlays overlays;
    ASSERT_EQ(overlays.Open("/workspace/first.fl", "first", 1),
              OverlayUpdateResult::Applied);
    ASSERT_EQ(overlays.Open("/workspace/second.fl", "second", 7),
              OverlayUpdateResult::Applied);

    ASSERT_NE(overlays.Find("/workspace/first.fl"), nullptr);
    ASSERT_NE(overlays.Find("/workspace/second.fl"), nullptr);
    EXPECT_EQ(overlays.Find("/workspace/first.fl")->text, "first");
    EXPECT_EQ(overlays.Find("/workspace/second.fl")->version, 7);
}

}  // namespace
