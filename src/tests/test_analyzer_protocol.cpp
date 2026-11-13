#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "analyzer/json.hpp"
#include "analyzer/protocol.hpp"

namespace {

using flecha::analyzer::ProtocolServer;
using flecha::analyzer::ProtocolState;

std::string Request(std::string_view id, std::string_view method,
                    std::string_view params = "{}") {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) +
           ",\"method\":" + flecha::analyzer::json::Quote(method) +
           ",\"params\":" + std::string(params) + "}";
}

int ErrorCode(const std::string& response) {
    std::string parseError;
    const auto document = flecha::analyzer::json::Parse(response, parseError);
    if (!document) return 0;
    const auto* envelope = flecha::analyzer::json::AsObject(*document);
    if (!envelope) return 0;
    const auto* error = flecha::analyzer::json::Field(*envelope, "error");
    if (!error) return 0;
    const auto* object = flecha::analyzer::json::AsObject(*error);
    if (!object) return 0;
    const auto* code = flecha::analyzer::json::Field(*object, "code");
    if (!code) return 0;
    const std::string* number = flecha::analyzer::json::AsNumber(*code);
    return number ? std::stoi(*number) : 0;
}

class AnalyzerProtocolTests : public ::testing::Test {
   protected:
    void SetUp() override {
        const auto* test =
            ::testing::UnitTest::GetInstance()->current_test_info();
        _root = std::filesystem::temp_directory_path() /
                (std::string("flecha_protocol_") + test->name());
        std::filesystem::remove_all(_root);
        std::filesystem::create_directories(_root);
    }

    void TearDown() override { std::filesystem::remove_all(_root); }

    std::string Initialize(ProtocolServer& server,
                           std::string_view id = "1") const {
        const std::string params =
            "{\"protocolMajor\":1,\"protocolMinor\":0,\"workspace\":" +
            flecha::analyzer::json::Quote(_root.generic_string()) + "}";
        return server.Handle(Request(id, "initialize", params));
    }

    void Write(const std::string& name, const std::string& text) const {
        const std::filesystem::path path = _root / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    std::filesystem::path _root;
};

TEST_F(AnalyzerProtocolTests, ReportsJsonParseErrors) {
    ProtocolServer server("test-compiler");

    const std::string response = server.Handle("{");

    EXPECT_EQ(ErrorCode(response), -32700);
    EXPECT_NE(response.find("\"id\":null"), std::string::npos);
}

TEST_F(AnalyzerProtocolTests, InitializesAndNegotiatesCapabilities) {
    ProtocolServer server("0.3.1-test");

    const std::string response = Initialize(server, "\"request-1\"");

    EXPECT_EQ(server.State(), ProtocolState::Initialized);
    EXPECT_NE(response.find("\"id\":\"request-1\""), std::string::npos);
    EXPECT_NE(response.find("\"protocolMajor\":1"), std::string::npos);
    EXPECT_NE(response.find("\"protocolMinor\":0"), std::string::npos);
    EXPECT_NE(response.find("\"compilerVersion\":\"0.3.1-test\""),
              std::string::npos);
    EXPECT_NE(response.find("\"diagnostics\":true"), std::string::npos);
}

TEST_F(AnalyzerProtocolTests, RejectsIncompatibleProtocolMajor) {
    ProtocolServer server("test-compiler");
    const std::string params =
        "{\"protocolMajor\":2,\"protocolMinor\":0,\"workspace\":" +
        flecha::analyzer::json::Quote(_root.generic_string()) + "}";

    const std::string response =
        server.Handle(Request("1", "initialize", params));

    EXPECT_EQ(ErrorCode(response), -32004);
    EXPECT_EQ(server.State(), ProtocolState::Starting);
}

TEST_F(AnalyzerProtocolTests, LoadsPackageGraphForSemanticDiagnostics) {
    Write("src/main.fl",
          "module main;\n"
          "@imports :: lib::dependency::value;\n"
          "i32 main() { return missing + value::answer(); }\n");
    Write("dependency/value.fl",
          "module value;\nexport i32 answer() { return 42; }\n");
    Write("target/package-graph.json",
          "{\"schema\":1,\"root\":\"root\",\"packages\":["
          "{\"id\":\"root\",\"identity\":\"local/root\","
          "\"source\":\"../src\",\"dependencies\":{"
          "\"dependency\":\"dependency\"}},"
          "{\"id\":\"dependency\",\"identity\":\"local/dependency\","
          "\"source\":\"../dependency\",\"dependencies\":{}}]}");
    const std::filesystem::path graph = _root / "target/package-graph.json";
    const std::string params =
        "{\"protocolMajor\":1,\"protocolMinor\":0,\"workspace\":" +
        flecha::analyzer::json::Quote(_root.generic_string()) +
        ",\"packageGraph\":" +
        flecha::analyzer::json::Quote(graph.generic_string()) + "}";
    ProtocolServer server("test-compiler");
    ASSERT_EQ(ErrorCode(server.Handle(Request("1", "initialize", params))), 0);

    const std::string path = (_root / "src/main.fl").generic_string();
    const std::string diagnostics = server.Handle(
        Request("2", "document/diagnostics",
                "{\"path\":" + flecha::analyzer::json::Quote(path) + "}"));

    EXPECT_NE(diagnostics.find("FLECHA-S0002"), std::string::npos);
    EXPECT_NE(diagnostics.find("unknown name 'missing'"), std::string::npos);
    EXPECT_EQ(diagnostics.find("library 'dependency' is not configured"),
              std::string::npos);
}

TEST_F(AnalyzerProtocolTests, RejectsInvalidPackageGraph) {
    const std::filesystem::path graph = _root / "missing-package-graph.json";
    const std::string params =
        "{\"protocolMajor\":1,\"protocolMinor\":0,\"workspace\":" +
        flecha::analyzer::json::Quote(_root.generic_string()) +
        ",\"packageGraph\":" +
        flecha::analyzer::json::Quote(graph.generic_string()) + "}";
    ProtocolServer server("test-compiler");

    const std::string response =
        server.Handle(Request("1", "initialize", params));

    EXPECT_EQ(ErrorCode(response), -32602);
    EXPECT_NE(response.find("cannot read package graph"), std::string::npos);
    EXPECT_EQ(server.State(), ProtocolState::Starting);
}

TEST_F(AnalyzerProtocolTests, RequiresInitializationBeforeDocumentMethods) {
    ProtocolServer server("test-compiler");

    const std::string response = server.Handle(
        Request("1", "document/diagnostics", "{\"path\":\"main.fl\"}"));

    EXPECT_EQ(ErrorCode(response), -32001);
}

TEST_F(AnalyzerProtocolTests, OpensChangesClosesAndDiagnosesDocuments) {
    Write("main.fl", "module main;\nvoid main() {}\n");
    ProtocolServer server("test-compiler");
    ASSERT_EQ(ErrorCode(Initialize(server)), 0);
    const std::string invalidText = "module main;\nvoid main() { ` }\n";
    const std::string path = (_root / "main.fl").generic_string();
    const std::string openParams =
        "{\"path\":" + flecha::analyzer::json::Quote(path) +
        ",\"version\":4,\"text\":" +
        flecha::analyzer::json::Quote(invalidText) + "}";

    const std::string opened =
        server.Handle(Request("2", "document/open", openParams));
    EXPECT_NE(opened.find("\"status\":\"applied\""), std::string::npos);

    const std::string documentParams =
        "{\"path\":" + flecha::analyzer::json::Quote(path) + "}";
    const std::string diagnostics =
        server.Handle(Request("3", "document/diagnostics", documentParams));
    EXPECT_NE(diagnostics.find("\"version\":4"), std::string::npos);
    EXPECT_NE(diagnostics.find("FLECHA-L0001"), std::string::npos);
    EXPECT_NE(diagnostics.find("\"severity\":\"error\""), std::string::npos);

    const std::string stale =
        server.Handle(Request("4", "document/change", openParams));
    EXPECT_EQ(ErrorCode(stale), -32002);

    const std::string validText = "module main;\nvoid main() {}\n";
    const std::string changeParams =
        "{\"path\":" + flecha::analyzer::json::Quote(path) +
        ",\"version\":5,\"text\":" + flecha::analyzer::json::Quote(validText) +
        "}";
    ASSERT_EQ(
        ErrorCode(server.Handle(Request("5", "document/change", changeParams))),
        0);
    const std::string validDiagnostics =
        server.Handle(Request("6", "document/diagnostics", documentParams));
    EXPECT_NE(validDiagnostics.find("\"version\":5"), std::string::npos);
    EXPECT_NE(validDiagnostics.find("\"diagnostics\":[]"), std::string::npos);

    const std::string closed =
        server.Handle(Request("7", "document/close", documentParams));
    EXPECT_NE(closed.find("\"closed\":true"), std::string::npos);
    const std::string savedDiagnostics =
        server.Handle(Request("8", "document/diagnostics", documentParams));
    EXPECT_NE(savedDiagnostics.find("\"version\":null"), std::string::npos);
    EXPECT_NE(savedDiagnostics.find("\"diagnostics\":[]"), std::string::npos);
}

TEST_F(AnalyzerProtocolTests,
       RecoversFromErrorTypedBorrowAcrossFullDocumentChanges) {
    Write("main.fl", "module main;\nvoid main() {}\n");
    ProtocolServer server("test-compiler");
    ASSERT_EQ(ErrorCode(Initialize(server)), 0);
    const std::string path = (_root / "main.fl").generic_string();
    const std::string invalidText =
        "module main;\n"
        "structure Holder { Missing resource; };\n"
        "&Missing borrow(&Holder holder) <-holder {\n"
        "    return &holder->resource;\n"
        "}\n";
    const std::string openParams =
        "{\"path\":" + flecha::analyzer::json::Quote(path) +
        ",\"version\":1,\"text\":" +
        flecha::analyzer::json::Quote(invalidText) + "}";
    ASSERT_EQ(
        ErrorCode(server.Handle(Request("2", "document/open", openParams))), 0);
    const std::string documentParams =
        "{\"path\":" + flecha::analyzer::json::Quote(path) + "}";

    const std::string invalidDiagnostics =
        server.Handle(Request("3", "document/diagnostics", documentParams));
    EXPECT_NE(invalidDiagnostics.find("\"version\":1"), std::string::npos);
    EXPECT_NE(invalidDiagnostics.find("unknown type 'Missing'"),
              std::string::npos);
    EXPECT_EQ(invalidDiagnostics.find("borrowed result does not originate"),
              std::string::npos)
        << invalidDiagnostics;

    const std::string validText =
        "module main;\n"
        "structure Resource { i32 value; };\n"
        "structure Holder { Resource resource; };\n"
        "&Resource borrow(&Holder holder) <-holder {\n"
        "    return &holder->resource;\n"
        "}\n";
    const std::string changeParams =
        "{\"path\":" + flecha::analyzer::json::Quote(path) +
        ",\"version\":2,\"text\":" + flecha::analyzer::json::Quote(validText) +
        "}";
    ASSERT_EQ(
        ErrorCode(server.Handle(Request("4", "document/change", changeParams))),
        0);

    const std::string validDiagnostics =
        server.Handle(Request("5", "document/diagnostics", documentParams));
    EXPECT_NE(validDiagnostics.find("\"version\":2"), std::string::npos);
    EXPECT_NE(validDiagnostics.find("\"diagnostics\":[]"), std::string::npos)
        << validDiagnostics;
    const std::string tokens =
        server.Handle(Request("6", "document/semanticTokens", documentParams));
    EXPECT_NE(tokens.find("\"tokens\":[{"), std::string::npos) << tokens;
    EXPECT_NE(tokens.find("\"name\":\"Resource\""), std::string::npos)
        << tokens;
}

TEST_F(AnalyzerProtocolTests, DoesNotReportCommandErrorForTestModuleEntry) {
    Write("value.fl", "module value;\nexport i32 answer() { return 42; }\n");
    Write("value_test.fl",
          "module value_test;\n@imports :: root::value;\n");
    ProtocolServer server("test-compiler");
    ASSERT_EQ(ErrorCode(Initialize(server)), 0);
    const std::string path = (_root / "value_test.fl").generic_string();

    const std::string diagnostics = server.Handle(
        Request("2", "document/diagnostics",
                "{\"path\":" + flecha::analyzer::json::Quote(path) + "}"));

    EXPECT_NE(diagnostics.find("\"diagnostics\":[]"), std::string::npos)
        << diagnostics;
    EXPECT_EQ(diagnostics.find("test modules can only be compiled"),
              std::string::npos);
}

TEST_F(AnalyzerProtocolTests,
       DoesNotReportCommandErrorForPackageGraphTestEntry) {
    Write("src/value.fl",
          "module value;\nexport i32 answer() { return 42; }\n");
    Write("src/value_test.fl",
          "module value_test;\n@imports :: root::value;\n");
    Write("target/package-graph.json",
          "{\"schema\":1,\"root\":\"root\",\"packages\":["
          "{\"id\":\"root\",\"identity\":\"local/root\","
          "\"source\":\"../src\",\"dependencies\":{}}]}");
    const std::filesystem::path graph = _root / "target/package-graph.json";
    const std::string initializeParams =
        "{\"protocolMajor\":1,\"protocolMinor\":0,\"workspace\":" +
        flecha::analyzer::json::Quote(_root.generic_string()) +
        ",\"packageGraph\":" +
        flecha::analyzer::json::Quote(graph.generic_string()) + "}";
    ProtocolServer server("test-compiler");
    ASSERT_EQ(ErrorCode(server.Handle(
                  Request("1", "initialize", initializeParams))),
              0);
    const std::string path = (_root / "src/value_test.fl").generic_string();

    const std::string diagnostics = server.Handle(
        Request("2", "document/diagnostics",
                "{\"path\":" + flecha::analyzer::json::Quote(path) + "}"));

    EXPECT_NE(diagnostics.find("\"diagnostics\":[]"), std::string::npos)
        << diagnostics;
    EXPECT_EQ(diagnostics.find("test modules can only be compiled"),
              std::string::npos);
}

TEST_F(AnalyzerProtocolTests, ServesSemanticToolingQueries) {
    const std::string source =
        "module main;\n"
        "i32 double(i32 value) { return value + value; }\n"
        "i32 main() { return double(21); }\n";
    Write("main.fl", source);
    ProtocolServer server("test-compiler");
    ASSERT_EQ(ErrorCode(Initialize(server)), 0);
    const std::string path = (_root / "main.fl").generic_string();
    const std::string base = "{\"path\":" + flecha::analyzer::json::Quote(path);
    const std::size_t call = source.rfind("double");
    ASSERT_NE(call, std::string::npos);

    const std::string hover = server.Handle(
        Request("2", "document/hover",
                base + ",\"offset\":" + std::to_string(call) + "}"));
    EXPECT_NE(hover.find("\"name\":\"double\""), std::string::npos);
    EXPECT_NE(hover.find("\"kind\":\"function\""), std::string::npos);

    const std::string definition = server.Handle(
        Request("3", "document/definition",
                base + ",\"offset\":" + std::to_string(call) + "}"));
    EXPECT_NE(definition.find("\"definition\":{"), std::string::npos);

    const std::string references =
        server.Handle(Request("4", "document/references",
                              base + ",\"offset\":" + std::to_string(call) +
                                  ",\"includeDeclaration\":true}"));
    EXPECT_NE(references.find("\"references\":[{"), std::string::npos);

    const std::string rename = server.Handle(
        Request("5", "document/rename",
                base + ",\"offset\":" + std::to_string(call) + "}"));
    EXPECT_NE(rename.find("\"name\":\"double\""), std::string::npos);
    EXPECT_NE(rename.find("\"ranges\":[{"), std::string::npos);

    const std::string completion = server.Handle(
        Request("6", "document/completion", base + ",\"query\":\"dou\"}"));
    EXPECT_NE(completion.find("\"name\":\"double\""), std::string::npos);

    const std::string tokens =
        server.Handle(Request("7", "document/semanticTokens", base + "}"));
    EXPECT_NE(tokens.find("\"tokens\":[{"), std::string::npos);
    EXPECT_NE(tokens.find("\"isDeclaration\":true"), std::string::npos);
}

TEST_F(AnalyzerProtocolTests, RejectsChangeForClosedDocument) {
    ProtocolServer server("test-compiler");
    ASSERT_EQ(ErrorCode(Initialize(server)), 0);
    const std::string params =
        "{\"path\":\"main.fl\",\"version\":1,\"text\":\"\"}";

    const std::string response =
        server.Handle(Request("2", "document/change", params));

    EXPECT_EQ(ErrorCode(response), -32003);
}

TEST_F(AnalyzerProtocolTests, ReportsUnknownMethods) {
    ProtocolServer server("test-compiler");
    ASSERT_EQ(ErrorCode(Initialize(server)), 0);

    EXPECT_EQ(ErrorCode(server.Handle(Request("2", "unknown/method"))), -32601);
}

TEST_F(AnalyzerProtocolTests, ShutsDownAndRejectsFurtherRequests) {
    ProtocolServer server("test-compiler");
    ASSERT_EQ(ErrorCode(Initialize(server)), 0);

    const std::string shutdown = server.Handle(Request("2", "shutdown", "{}"));

    EXPECT_EQ(ErrorCode(shutdown), 0);
    EXPECT_NE(shutdown.find("\"result\":null"), std::string::npos);
    EXPECT_EQ(server.State(), ProtocolState::Shutdown);
    EXPECT_EQ(ErrorCode(server.Handle(Request("3", "unknown/method"))), -32001);
}

}  // namespace
