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
        std::ofstream(_root / name, std::ios::binary) << text;
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
