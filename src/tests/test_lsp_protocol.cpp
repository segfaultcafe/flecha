#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/json.hpp"
#include "lsp/analyzer_client.hpp"
#include "lsp/protocol.hpp"
#include "lsp/uri.hpp"

namespace {

using flecha::lsp::AnalyzerClient;
using flecha::lsp::AnalyzerDiagnostic;
using flecha::lsp::AnalyzerSnapshot;
using flecha::lsp::ProtocolServer;
using flecha::lsp::ProtocolState;

class FakeAnalyzer final : public AnalyzerClient {
   public:
    std::string Initialize(
        const std::filesystem::path& requestedWorkspace,
        const std::optional<std::filesystem::path>& requestedGraph) override {
        workspace = requestedWorkspace;
        packageGraph = requestedGraph;
        return "0.3.1-test";
    }

    void Open(const std::filesystem::path& requestedPath, std::string text,
              std::int64_t requestedVersion) override {
        path = requestedPath;
        contents = std::move(text);
        version = requestedVersion;
        ++opens;
    }

    void Change(const std::filesystem::path& requestedPath, std::string text,
                std::int64_t requestedVersion) override {
        path = requestedPath;
        contents = std::move(text);
        version = requestedVersion;
        ++changes;
    }

    void Close(const std::filesystem::path& requestedPath) override {
        path = requestedPath;
        ++closes;
    }

    AnalyzerSnapshot Diagnostics(
        const std::filesystem::path& requestedPath) override {
        ++diagnosticRequests;
        snapshot.path = requestedPath;
        snapshot.version = diagnosticVersion.value_or(version);
        return snapshot;
    }

    std::optional<flecha::lsp::AnalyzerHover> Hover(
        const std::filesystem::path&, std::size_t requestedOffset) override {
        semanticOffset = requestedOffset;
        return hover;
    }

    std::optional<flecha::lsp::AnalyzerRange> Definition(
        const std::filesystem::path&, std::size_t requestedOffset) override {
        semanticOffset = requestedOffset;
        return definition;
    }

    std::optional<std::vector<flecha::lsp::AnalyzerRange>> References(
        const std::filesystem::path&, std::size_t requestedOffset,
        bool) override {
        semanticOffset = requestedOffset;
        return references;
    }

    std::vector<flecha::lsp::AnalyzerCompletion> Completion(
        const std::filesystem::path&,
        std::string_view requestedQuery) override {
        query = requestedQuery;
        return completions;
    }

    std::optional<flecha::lsp::AnalyzerRename> Rename(
        const std::filesystem::path&, std::size_t requestedOffset) override {
        semanticOffset = requestedOffset;
        return rename;
    }

    std::vector<flecha::lsp::AnalyzerSemanticToken> SemanticTokens(
        const std::filesystem::path&) override {
        return semanticTokens;
    }

    void Shutdown() override { ++shutdowns; }

    std::filesystem::path workspace;
    std::optional<std::filesystem::path> packageGraph;
    std::filesystem::path path;
    std::string contents;
    std::int64_t version = 0;
    std::optional<std::int64_t> diagnosticVersion;
    AnalyzerSnapshot snapshot;
    std::optional<flecha::lsp::AnalyzerHover> hover;
    std::optional<flecha::lsp::AnalyzerRange> definition;
    std::optional<std::vector<flecha::lsp::AnalyzerRange>> references;
    std::vector<flecha::lsp::AnalyzerCompletion> completions;
    std::optional<flecha::lsp::AnalyzerRename> rename;
    std::vector<flecha::lsp::AnalyzerSemanticToken> semanticTokens;
    std::size_t semanticOffset = 0;
    std::string query;
    int opens = 0;
    int changes = 0;
    int closes = 0;
    int diagnosticRequests = 0;
    int shutdowns = 0;
};

std::string Request(std::string_view id, std::string_view method,
                    std::string_view params = "{}") {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) +
           ",\"method\":" + flecha::analyzer::json::Quote(method) +
           ",\"params\":" + std::string(params) + '}';
}

std::string Notification(std::string_view method,
                         std::string_view params = "{}") {
    return "{\"jsonrpc\":\"2.0\",\"method\":" +
           flecha::analyzer::json::Quote(method) +
           ",\"params\":" + std::string(params) + '}';
}

class LspProtocolTests : public ::testing::Test {
   protected:
    void SetUp() override {
        const auto* test =
            ::testing::UnitTest::GetInstance()->current_test_info();
        root = std::filesystem::temp_directory_path() /
               (std::string("flecha_lsp_") + test->name());
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
    }

    void TearDown() override { std::filesystem::remove_all(root); }

    std::string InitializeParams() const {
        return "{\"processId\":null,\"rootUri\":" +
               flecha::analyzer::json::Quote(flecha::lsp::PathToFileUri(root)) +
               "}";
    }

    void Initialize(ProtocolServer& server) const {
        const auto messages =
            server.Handle(Request("1", "initialize", InitializeParams()));
        ASSERT_EQ(messages.size(), 1u);
        ASSERT_NE(messages.front().find("\"id\":1"), std::string::npos);
    }

    std::filesystem::path root;
};

TEST(LspUriTests, DecodesFileUrisAndEscapedPaths) {
    std::string error;
    const auto path =
        flecha::lsp::FileUriToPath("file:///tmp/flecha%20source.fl", error);

    ASSERT_TRUE(path) << error;
    EXPECT_EQ(path->generic_string(), "/tmp/flecha source.fl");
    EXPECT_EQ(flecha::lsp::PathToFileUri(*path),
              "file:///tmp/flecha%20source.fl");
}

TEST(LspUriTests, ConvertsUtf8ByteOffsetsToUtf16Positions) {
    const std::string text =
        "a\xf0\x9f\x98\x80"
        "b\n\xc3\xa7";

    EXPECT_EQ(flecha::lsp::ByteOffsetToPosition(text, 0).line, 0u);
    EXPECT_EQ(flecha::lsp::ByteOffsetToPosition(text, 5).character, 3u);
    EXPECT_EQ(flecha::lsp::ByteOffsetToPosition(text, 6).character, 4u);
    const auto secondLine = flecha::lsp::ByteOffsetToPosition(text, 7);
    EXPECT_EQ(secondLine.line, 1u);
    EXPECT_EQ(secondLine.character, 0u);
    EXPECT_EQ(flecha::lsp::PositionToByteOffset(text, {0, 3}), 5u);
    EXPECT_EQ(flecha::lsp::PositionToByteOffset(text, {1, 0}), 7u);
    EXPECT_FALSE(flecha::lsp::PositionToByteOffset(text, {0, 2}));
}

TEST_F(LspProtocolTests, InitializesImplementedCapabilities) {
    FakeAnalyzer analyzer;
    ProtocolServer server(analyzer);

    const auto messages =
        server.Handle(Request("\"init\"", "initialize", InitializeParams()));

    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(server.State(), ProtocolState::Initialized);
    EXPECT_EQ(analyzer.workspace, std::filesystem::weakly_canonical(root));
    EXPECT_NE(messages.front().find("\"positionEncoding\":\"utf-16\""),
              std::string::npos);
    EXPECT_NE(messages.front().find("\"openClose\":true"), std::string::npos);
    EXPECT_NE(messages.front().find("\"change\":1"), std::string::npos);
    EXPECT_NE(messages.front().find("\"hoverProvider\":true"),
              std::string::npos);
    EXPECT_NE(messages.front().find("\"definitionProvider\":true"),
              std::string::npos);
    EXPECT_NE(messages.front().find("semanticTokensProvider"),
              std::string::npos);
}

TEST_F(LspProtocolTests, DetectsMaterializedLongbowPackageGraph) {
    std::filesystem::create_directories(root / "target");
    std::ofstream(root / "target/package-graph.json") << "{}";
    FakeAnalyzer analyzer;
    ProtocolServer server(analyzer);

    Initialize(server);

    ASSERT_TRUE(analyzer.packageGraph);
    EXPECT_EQ(*analyzer.packageGraph, root / "target/package-graph.json");
}

TEST_F(LspProtocolTests, OpensChangesAndClosesWithPublishedDiagnostics) {
    FakeAnalyzer analyzer;
    ProtocolServer server(analyzer);
    Initialize(server);
    const std::filesystem::path file = root / "source file.fl";
    const std::string uri = flecha::lsp::PathToFileUri(file);
    analyzer.snapshot.diagnostics = {
        AnalyzerDiagnostic{"FLECHA-S0002", file, 5, 6, "error", "unknown"}};
    const std::string text =
        "a\xf0\x9f\x98\x80"
        "b\n";

    const std::string openParams =
        "{\"textDocument\":{\"uri\":" + flecha::analyzer::json::Quote(uri) +
        ",\"languageId\":\"flecha\",\"version\":4,\"text\":" +
        flecha::analyzer::json::Quote(text) + "}}";
    const auto opened =
        server.Handle(Notification("textDocument/didOpen", openParams));

    ASSERT_EQ(opened.size(), 1u);
    EXPECT_EQ(analyzer.opens, 1);
    EXPECT_EQ(analyzer.contents, text);
    EXPECT_NE(opened.front().find("textDocument/publishDiagnostics"),
              std::string::npos);
    EXPECT_NE(opened.front().find("\"version\":4"), std::string::npos);
    EXPECT_NE(opened.front().find("\"start\":{\"line\":0,\"character\":3}"),
              std::string::npos);
    EXPECT_NE(opened.front().find("\"end\":{\"line\":0,\"character\":4}"),
              std::string::npos);

    const std::string changedText = "valid\n";
    analyzer.snapshot.diagnostics.clear();
    const std::string changeParams =
        "{\"textDocument\":{\"uri\":" + flecha::analyzer::json::Quote(uri) +
        ",\"version\":5},\"contentChanges\":[{\"text\":" +
        flecha::analyzer::json::Quote(changedText) + "}]}";
    const auto changed =
        server.Handle(Notification("textDocument/didChange", changeParams));
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(analyzer.changes, 1);
    EXPECT_EQ(analyzer.version, 5);
    EXPECT_NE(changed.front().find("\"diagnostics\":[]"), std::string::npos);

    const std::string closeParams =
        "{\"textDocument\":{\"uri\":" + flecha::analyzer::json::Quote(uri) +
        "}}";
    const auto closed =
        server.Handle(Notification("textDocument/didClose", closeParams));
    ASSERT_EQ(closed.size(), 1u);
    EXPECT_EQ(analyzer.closes, 1);
    EXPECT_NE(closed.front().find("\"diagnostics\":[]"), std::string::npos);
}

TEST_F(LspProtocolTests, DropsDiagnosticsFromSupersededDocumentVersions) {
    FakeAnalyzer analyzer;
    ProtocolServer server(analyzer);
    Initialize(server);
    const std::filesystem::path file = root / "main.fl";
    const std::string uri = flecha::lsp::PathToFileUri(file);
    analyzer.snapshot.diagnostics = {
        AnalyzerDiagnostic{"FLECHA-S0002", file, 0, 1, "error", "stale"}};
    analyzer.diagnosticVersion = 0;
    const std::string document =
        "{\"textDocument\":{\"uri\":" +
        flecha::analyzer::json::Quote(uri);

    const auto stale = server.Handle(Notification(
        "textDocument/didOpen",
        document + ",\"version\":1,\"text\":\"x\"}}"));

    EXPECT_TRUE(stale.empty());
    EXPECT_EQ(analyzer.diagnosticRequests, 1);

    analyzer.diagnosticVersion = 2;
    analyzer.snapshot.diagnostics.clear();
    const auto current = server.Handle(Notification(
        "textDocument/didChange",
        document +
            ",\"version\":2},\"contentChanges\":[{\"text\":\"ok\"}]}"));

    ASSERT_EQ(current.size(), 1u);
    EXPECT_NE(current.front().find("\"version\":2"), std::string::npos);
    EXPECT_NE(current.front().find("\"diagnostics\":[]"),
              std::string::npos);
}

TEST_F(LspProtocolTests, ConvertsHoverPositionsAndRanges) {
    FakeAnalyzer analyzer;
    ProtocolServer server(analyzer);
    Initialize(server);
    const std::filesystem::path file = root / "main.fl";
    const std::string uri = flecha::lsp::PathToFileUri(file);
    const std::string text =
        "a\xf0\x9f\x98\x80"
        "value\n";
    const std::string openParams =
        "{\"textDocument\":{\"uri\":" + flecha::analyzer::json::Quote(uri) +
        ",\"version\":1,\"text\":" + flecha::analyzer::json::Quote(text) + "}}";
    ASSERT_EQ(
        server.Handle(Notification("textDocument/didOpen", openParams)).size(),
        1u);
    analyzer.hover = flecha::lsp::AnalyzerHover{
        {file, 5, 10}, "value", "i32", "localVariable"};

    const auto messages = server.Handle(Request(
        "2", "textDocument/hover",
        "{\"textDocument\":{\"uri\":" + flecha::analyzer::json::Quote(uri) +
            "},\"position\":{\"line\":0,\"character\":3}}"));

    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(analyzer.semanticOffset, 5u);
    EXPECT_NE(messages.front().find("value: i32"), std::string::npos);
    EXPECT_NE(messages.front().find("\"start\":{\"line\":0,\"character\":3}"),
              std::string::npos);
}

TEST_F(LspProtocolTests, ServesSemanticFeatureRequests) {
    FakeAnalyzer analyzer;
    ProtocolServer server(analyzer);
    Initialize(server);
    const std::filesystem::path file = root / "main.fl";
    const std::string uri = flecha::lsp::PathToFileUri(file);
    const std::string text = "value value\n";
    const std::string document =
        "{\"textDocument\":{\"uri\":" + flecha::analyzer::json::Quote(uri);
    ASSERT_EQ(server
                  .Handle(Notification("textDocument/didOpen",
                                       document + ",\"version\":1,\"text\":" +
                                           flecha::analyzer::json::Quote(text) +
                                           "}}"))
                  .size(),
              1u);

    analyzer.definition = flecha::lsp::AnalyzerRange{file, 0, 5};
    analyzer.references =
        std::vector<flecha::lsp::AnalyzerRange>{{file, 0, 5}, {file, 6, 11}};
    analyzer.completions = {{"value", "i32", "localVariable", {file, 0, 5}}};
    analyzer.rename =
        flecha::lsp::AnalyzerRename{"value", {{file, 0, 5}, {file, 6, 11}}};
    analyzer.semanticTokens = {{{file, 0, 5}, "localVariable", true},
                               {{file, 6, 11}, "localVariable", false}};
    const std::string atReference =
        document + "},\"position\":{\"line\":0,\"character\":6}";

    const auto definition = server.Handle(
        Request("2", "textDocument/definition", atReference + "}"));
    ASSERT_EQ(definition.size(), 1u);
    EXPECT_NE(definition.front().find("\"uri\":" +
                                      flecha::analyzer::json::Quote(uri)),
              std::string::npos);
    EXPECT_NE(definition.front().find("\"end\":{\"line\":0,\"character\":5}"),
              std::string::npos);

    const auto references = server.Handle(
        Request("3", "textDocument/references",
                atReference + ",\"context\":{\"includeDeclaration\":true}}"));
    ASSERT_EQ(references.size(), 1u);
    EXPECT_NE(references.front().find("\"start\":{\"line\":0,\"character\":6}"),
              std::string::npos);

    const auto completion = server.Handle(
        Request("4", "textDocument/completion",
                document + "},\"position\":{\"line\":0,\"character\":11}}"));
    ASSERT_EQ(completion.size(), 1u);
    EXPECT_EQ(analyzer.query, "value");
    EXPECT_NE(completion.front().find("\"label\":\"value\""),
              std::string::npos);

    const auto rename = server.Handle(Request(
        "5", "textDocument/rename", atReference + ",\"newName\":\"renamed\"}"));
    ASSERT_EQ(rename.size(), 1u);
    EXPECT_NE(rename.front().find("\"newText\":\"renamed\""),
              std::string::npos);

    const auto tokens = server.Handle(
        Request("6", "textDocument/semanticTokens/full", document + "}}"));
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_NE(tokens.front().find("\"data\":[0,0,5,2,1,0,6,5,2,0]"),
              std::string::npos);
}

TEST_F(LspProtocolTests, UsesShutdownThenExitLifecycle) {
    FakeAnalyzer analyzer;
    ProtocolServer server(analyzer);
    Initialize(server);

    const auto shutdown = server.Handle(Request("2", "shutdown"));
    ASSERT_EQ(shutdown.size(), 1u);
    EXPECT_EQ(server.State(), ProtocolState::Shutdown);
    EXPECT_EQ(analyzer.shutdowns, 1);

    EXPECT_TRUE(server.Handle(Notification("exit")).empty());
    EXPECT_EQ(server.State(), ProtocolState::Exited);
    EXPECT_EQ(server.ExitCode(), 0);
}

}  // namespace
