#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>
#include <string>

#include "analyzer/protocol.hpp"
#include "analyzer/server_loop.hpp"
#include "analyzer/transport.hpp"

namespace {

using flecha::analyzer::ProtocolServer;
using flecha::analyzer::ProtocolState;
using flecha::analyzer::ServerLoop;
using flecha::analyzer::Transport;

std::string Frame(const std::string& message) {
    return "Content-Length: " + std::to_string(message.size()) + "\r\n\r\n" +
           message;
}

TEST(AnalyzerServerLoopTests, ReturnsOnCleanEndOfInput) {
    std::istringstream input;
    std::ostringstream output;
    Transport transport(input, output);
    ProtocolServer protocol("test-compiler");
    ServerLoop loop(transport, protocol);

    loop.Run();

    EXPECT_TRUE(output.str().empty());
    EXPECT_EQ(protocol.State(), ProtocolState::Starting);
}

TEST(AnalyzerServerLoopTests, ProcessesRequestsUntilShutdown) {
    const std::string initialize =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolMajor":1,"protocolMinor":0,"workspace":"/workspace"}})";
    const std::string shutdown =
        R"({"jsonrpc":"2.0","id":2,"method":"shutdown","params":{}})";
    const std::string ignored =
        R"({"jsonrpc":"2.0","id":3,"method":"unknown","params":{}})";
    std::istringstream input(Frame(initialize) + Frame(shutdown) +
                             Frame(ignored));
    std::ostringstream output;
    Transport transport(input, output);
    ProtocolServer protocol("test-compiler");
    ServerLoop loop(transport, protocol);

    loop.Run();

    EXPECT_EQ(protocol.State(), ProtocolState::Shutdown);

    std::istringstream responses(output.str());
    std::ostringstream unused;
    Transport responseTransport(responses, unused);
    const auto initialized = responseTransport.ReadMessage();
    const auto stopped = responseTransport.ReadMessage();
    ASSERT_TRUE(initialized);
    ASSERT_TRUE(stopped);
    EXPECT_NE(initialized->find("\"id\":1"), std::string::npos);
    EXPECT_NE(initialized->find("\"compilerVersion\":\"test-compiler\""),
              std::string::npos);
    EXPECT_NE(stopped->find("\"id\":2"), std::string::npos);
    EXPECT_NE(stopped->find("\"result\":null"), std::string::npos);
    EXPECT_FALSE(responseTransport.ReadMessage());
}

TEST(AnalyzerServerLoopTests, PropagatesMalformedFrames) {
    std::istringstream input("Content-Length: invalid\r\n\r\n");
    std::ostringstream output;
    Transport transport(input, output);
    ProtocolServer protocol("test-compiler");
    ServerLoop loop(transport, protocol);

    EXPECT_THROW(loop.Run(), std::runtime_error);
    EXPECT_TRUE(output.str().empty());
}

TEST(AnalyzerServerLoopTests, RejectsFailedWrites) {
    const std::string request =
        R"({"jsonrpc":"2.0","id":1,"method":"unknown","params":{}})";
    std::istringstream input(Frame(request));
    std::ostringstream output;
    output.setstate(std::ios::badbit);
    Transport transport(input, output);
    ProtocolServer protocol("test-compiler");
    ServerLoop loop(transport, protocol);

    EXPECT_THROW(loop.Run(), std::runtime_error);
}

}  // namespace
