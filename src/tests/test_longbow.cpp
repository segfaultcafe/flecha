#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/package_graph.hpp"
#include "longbow/archive.hpp"
#include "longbow/cache.hpp"
#include "longbow/manifest.hpp"
#include "longbow/resolver.hpp"
#include "longbow/sha256.hpp"

namespace {
namespace fs = std::filesystem;
using flecha::longbow::DependencyKind;
using flecha::longbow::PackageKind;
using flecha::longbow::SemanticVersion;
using flecha::longbow::VersionRequirement;

SemanticVersion Version(const std::string& text) {
    std::string error;
    const auto version = SemanticVersion::Parse(text, error);
    EXPECT_TRUE(version) << error;
    return version.value_or(SemanticVersion{});
}

VersionRequirement Requirement(const std::string& text) {
    std::string error;
    const auto requirement = VersionRequirement::Parse(text, error);
    EXPECT_TRUE(requirement) << error;
    return requirement.value_or(VersionRequirement{});
}

TEST(LongbowVersionTests, ParsesAndOrdersSemanticVersions) {
    EXPECT_EQ(Version("1.2.3-alpha.1+linux.x86").String(),
              "1.2.3-alpha.1+linux.x86");
    const std::vector<std::string> ordered = {
        "1.0.0-alpha",  "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
        "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",       "1.0.0",
    };
    for (std::size_t index = 1; index < ordered.size(); ++index)
        EXPECT_LT(Version(ordered[index - 1]), Version(ordered[index]));
    EXPECT_EQ(Version("1.2.3+first"), Version("1.2.3+second"));

    for (const std::string invalid :
         {"1", "1.2", "01.2.3", "1.02.3", "1.2.03", "1.2.3-01", "1.2.3-"}) {
        std::string error;
        EXPECT_FALSE(SemanticVersion::Parse(invalid, error)) << invalid;
        EXPECT_FALSE(error.empty()) << invalid;
    }
}

TEST(LongbowHashTests, MatchesPublishedSha256Vectors) {
    EXPECT_EQ(
        flecha::longbow::Sha256(""),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(
        flecha::longbow::Sha256("abc"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(LongbowCacheTests, StoresVerifiesAndReusesContentAddressedSources) {
    const fs::path root =
        fs::temp_directory_path() / "flecha_content_addressed_cache_test";
    fs::remove_all(root);
    fs::create_directories(root / "input/nested");
    std::ofstream(root / "input/main.fl") << "module main;\n";
    std::ofstream(root / "input/nested/data.txt") << "verified bytes\n";
    const std::string checksum =
        "sha256:" + flecha::longbow::Sha256Tree(root / "input");
    const flecha::longbow::PackageCache cache(root / "cache");
    const fs::path stored = cache.Store(root / "input", checksum);
    EXPECT_TRUE(fs::is_regular_file(stored / "nested/data.txt"));
    fs::remove_all(root / "input");
    const auto found = cache.Find(checksum);
    ASSERT_TRUE(found);
    EXPECT_EQ(*found, stored);

    std::ofstream(stored / "nested/data.txt") << "corrupt\n";
    EXPECT_THROW(
        {
            [[maybe_unused]] const auto corrupt = cache.Find(checksum);
        },
        std::runtime_error);

    fs::create_directories(root / "input/nested");
    std::ofstream(root / "input/main.fl") << "module main;\n";
    std::ofstream(root / "input/nested/data.txt") << "verified bytes\n";
    EXPECT_EQ(cache.Store(root / "input", checksum), stored);
    EXPECT_EQ(cache.Find(checksum), stored);
    fs::remove_all(root);
}

TEST(LongbowArchiveTests, NormalizesHashesAndExtractsPublishedSources) {
    const fs::path root =
        fs::temp_directory_path() / "flecha_package_archive_test";
    fs::remove_all(root);
    fs::create_directories(root / "package/src");
    std::ofstream(root / "package/longbow.toml") << R"([package]
name = "sample"
registry = "owner/sample"
version = "1.2.3"
kind = "library"
flecha = "^0.3"

[dependencies]
json = { package = "flecha/json", version = "^1", path = "../json" }
)";
    std::ofstream(root / "package/src/sample.fl") << "module sample;\n";
    std::ofstream(root / "package/README.md") << "# Sample\n";
    std::ofstream(root / "package/private.txt") << "not published\n";

    const auto first =
        flecha::longbow::BuildPackageArchive(root / "package");
    const auto second =
        flecha::longbow::BuildPackageArchive(root / "package");
    EXPECT_EQ(first.bytes, second.bytes);
    EXPECT_EQ(first.checksum, second.checksum);
    EXPECT_EQ(first.files,
              (std::vector<std::string>{"README.md", "longbow.toml",
                                        "src/sample.fl"}));

    flecha::longbow::ExtractPackageArchive(first.bytes, root / "extracted");
    const auto manifest =
        flecha::longbow::ReadManifest(root / "extracted");
    EXPECT_EQ(manifest.registry, "owner/sample");
    EXPECT_FALSE(manifest.dependencies.at("json").path);
    EXPECT_EQ(flecha::longbow::Sha256(first.bytes),
              first.checksum.substr(std::string("sha256:").size()));
    EXPECT_THROW(flecha::longbow::ExtractPackageArchive(
                     first.bytes + "trailing", root / "invalid"),
                 std::runtime_error);
    fs::remove_all(root);
}

TEST(LongbowVersionTests, MatchesCaretTildePartialAndComparatorRequirements) {
    const auto major = Requirement("^1.2.3");
    EXPECT_TRUE(major.Matches(Version("1.9.0")));
    EXPECT_FALSE(major.Matches(Version("2.0.0")));

    const auto zeroMinor = Requirement("^0.2.3");
    EXPECT_TRUE(zeroMinor.Matches(Version("0.2.99")));
    EXPECT_FALSE(zeroMinor.Matches(Version("0.3.0")));

    const auto patch = Requirement("^0.0.3");
    EXPECT_TRUE(patch.Matches(Version("0.0.3")));
    EXPECT_FALSE(patch.Matches(Version("0.0.4")));

    const auto tilde = Requirement("~1.2.3");
    EXPECT_TRUE(tilde.Matches(Version("1.2.9")));
    EXPECT_FALSE(tilde.Matches(Version("1.3.0")));

    const auto partial = Requirement("1.2");
    EXPECT_TRUE(partial.Matches(Version("1.2.99")));
    EXPECT_FALSE(partial.Matches(Version("1.3.0")));

    const auto range = Requirement(">=0.3.0, <0.4.0");
    EXPECT_TRUE(range.Matches(Version("0.3.9")));
    EXPECT_FALSE(range.Matches(Version("0.4.0")));
    EXPECT_FALSE(range.Matches(Version("0.3.1-alpha")));

    const auto prerelease = Requirement(">=0.3.1-alpha, <0.3.1");
    EXPECT_TRUE(prerelease.Matches(Version("0.3.1-beta")));
}

class LongbowManifestTests : public ::testing::Test {
   protected:
    fs::path root;
    void SetUp() override {
        const auto* test =
            ::testing::UnitTest::GetInstance()->current_test_info();
        root = fs::temp_directory_path() /
               (std::string("flecha_manifest_") + test->name());
        fs::remove_all(root);
        fs::create_directories(root);
    }
    void TearDown() override { fs::remove_all(root); }
    void Write(const std::string& text) {
        std::ofstream(root / "longbow.toml") << text;
    }
    std::string Error(const std::string& text) {
        Write(text);
        try {
            (void)flecha::longbow::ReadManifest(root);
            return {};
        } catch (const std::exception& error) {
            return error.what();
        }
    }
};

TEST_F(LongbowManifestTests, ParsesEveryDependencySourceAndMetadata) {
    Write(R"([package]
name = "example-app"
version = "1.2.3-alpha.1+dev"
kind = "program"
flecha = ">=0.3.0, <0.4.0"

[dependencies]
local = { path = "../local" }
source = { git = "https://example.com/source.git", rev = "0123456789abcdef0123456789abcdef01234567" }
json = { package = "flecha/json", version = "^1.2.0" }
chain = { package = "alexbsec/blockchain", version = "~0.2", path = "../chain" }

[native]
bridge = { path = "native/bridge.o" }
)");
    const auto manifest = flecha::longbow::ReadManifest(root);
    EXPECT_EQ(manifest.name, "example-app");
    EXPECT_EQ(manifest.version.String(), "1.2.3-alpha.1+dev");
    EXPECT_EQ(manifest.kind, PackageKind::Program);
    ASSERT_TRUE(manifest.flecha);
    EXPECT_TRUE(manifest.flecha->Matches(Version("0.3.5")));

    EXPECT_EQ(manifest.dependencies.at("local").kind, DependencyKind::Path);
    EXPECT_EQ(manifest.dependencies.at("local").path, root / "../local");
    EXPECT_EQ(manifest.dependencies.at("source").kind, DependencyKind::Git);
    EXPECT_EQ(manifest.dependencies.at("source").revision,
              "0123456789abcdef0123456789abcdef01234567");
    EXPECT_EQ(manifest.dependencies.at("json").kind, DependencyKind::Registry);
    EXPECT_EQ(manifest.dependencies.at("json").package, "flecha/json");
    EXPECT_FALSE(manifest.dependencies.at("json").path);
    EXPECT_EQ(manifest.dependencies.at("chain").kind, DependencyKind::Registry);
    EXPECT_TRUE(manifest.dependencies.at("chain").path);
    EXPECT_EQ(manifest.native.at("bridge"), root / "native/bridge.o");
}

TEST_F(LongbowManifestTests, RejectsInvalidVersionsSourcesAndStdReplacement) {
    const std::string package = R"([package]
name = "sample"
version = "0.1.0"
kind = "library"

[dependencies]
)";
    EXPECT_NE(Error(R"([package]
name = "sample"
version = "01.0.0"
kind = "library"
[dependencies]
)")
                  .find("invalid package.version"),
              std::string::npos);

    EXPECT_NE(Error(package + "broken = { package = \"flecha/json\" }\n")
                  .find("requires a valid owner/package and version"),
              std::string::npos);
    EXPECT_NE(
        Error(package + "broken = { git = \"https://example.com/repo.git\" }\n")
            .find("requires only nonempty git and rev"),
        std::string::npos);
    EXPECT_NE(Error(package +
                    "floating = { git = \"https://example.com/repo.git\", "
                    "rev = \"main\" }\n")
                  .find("must be a full 40- or 64-digit commit hash"),
              std::string::npos);
    EXPECT_NE(Error(package +
                    "std = { package = \"flecha/json\", version = \"^1\" }\n")
                  .find("std may only be"),
              std::string::npos);
    EXPECT_NE(
        Error(package +
              "replacement = { package = \"flecha/std\", version = \"^1\" }\n")
            .find("cannot be a registry dependency"),
        std::string::npos);
}

class LongbowResolverTests : public ::testing::Test {
   protected:
    fs::path root;
    void SetUp() override {
        const auto* test =
            ::testing::UnitTest::GetInstance()->current_test_info();
        root = fs::temp_directory_path() /
               (std::string("flecha_resolver_") + test->name());
        fs::remove_all(root);
        fs::create_directories(root);
    }
    void TearDown() override { fs::remove_all(root); }
    void Project(const std::string& directory, const std::string& name,
                 const std::string& dependencies = {}) {
        const fs::path project = root / directory;
        fs::create_directories(project / "src");
        std::ofstream(project / "longbow.toml")
            << "[package]\nname = \"" << name
            << "\"\nversion = \"1.2.3\"\nkind = \"library\"\n\n"
               "[dependencies]\n"
            << dependencies;
        std::ofstream(project / "src" /
                      (flecha::longbow::ModuleName(name) + ".fl"))
            << "module " << flecha::longbow::ModuleName(name) << ";\n";
    }
};

TEST_F(LongbowResolverTests, ResolvesRecursivePathsAndPackageLocalAliases) {
    Project("app", "app",
            "first = { path = \"../first\" }\n"
            "second = { path = \"../second\" }\n");
    Project("first", "first", "common = { path = \"../left\" }\n");
    Project("second", "second", "common = { path = \"../right\" }\n");
    Project("left", "left");
    Project("right", "right");
    fs::create_directories(root / "std");

    const auto manifest = flecha::longbow::ReadManifest(root / "app");
    const auto graph =
        flecha::longbow::ResolveLocalPackages(manifest, root / "std");
    EXPECT_EQ(graph.Packages().size(), 6u);
    const auto& rootNode = graph.Packages().at("root");
    ASSERT_TRUE(rootNode.dependencies.contains("first"));
    ASSERT_TRUE(rootNode.dependencies.contains("second"));
    EXPECT_NE(rootNode.dependencies.at("first"),
              rootNode.dependencies.at("second"));
    const auto& first = graph.Packages().at(rootNode.dependencies.at("first"));
    const auto& second =
        graph.Packages().at(rootNode.dependencies.at("second"));
    ASSERT_TRUE(first.dependencies.contains("common"));
    ASSERT_TRUE(second.dependencies.contains("common"));
    EXPECT_NE(first.dependencies.at("common"),
              second.dependencies.at("common"));
    for (const auto& [id, package] : graph.Packages())
        if (id != "toolchain-std")
            EXPECT_EQ(package.dependencies.at("std"), "toolchain-std");

    const fs::path graphFile = root / "app/target/package-graph.json";
    graph.WriteCompilerGraph(graphFile);
    std::string error;
    const auto compilerGraph =
        flecha::core::PackageGraph::Load(graphFile, error);
    ASSERT_TRUE(compilerGraph) << error;
    EXPECT_EQ(compilerGraph->Nodes().size(), graph.Packages().size());

    const fs::path lock = root / "app/longbow.lock";
    graph.WriteLock(lock, root / "app");
    const std::string firstLockHash = flecha::longbow::Sha256File(lock);
    graph.WriteLock(lock, root / "app");
    EXPECT_EQ(flecha::longbow::Sha256File(lock), firstLockHash);
    std::ifstream lockStream(lock);
    const std::string contents((std::istreambuf_iterator<char>(lockStream)),
                               std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find("\"checksum\": \"sha256:"), std::string::npos);
    EXPECT_NE(contents.find("\"kind\": \"toolchain\""), std::string::npos);
}

TEST_F(LongbowResolverTests, DiagnosesPathCyclesAndOverrideVersionMismatch) {
    Project("one", "one", "two = { path = \"../two\" }\n");
    Project("two", "two", "one = { path = \"../one\" }\n");
    EXPECT_THROW(flecha::longbow::ResolveLocalPackages(
                     flecha::longbow::ReadManifest(root / "one"), std::nullopt),
                 std::runtime_error);

    Project("app", "app",
            "one = { package = \"owner/one\", version = \"^2\", "
            "path = \"../one\" }\n");
    try {
        (void)flecha::longbow::ResolveLocalPackages(
            flecha::longbow::ReadManifest(root / "app"), std::nullopt);
        FAIL() << "expected version mismatch";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("does not match ^2"),
                  std::string::npos);
    }
}

}  // namespace
