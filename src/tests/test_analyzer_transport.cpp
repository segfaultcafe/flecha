#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>
#include <string>

#include "analyzer/transport.hpp"

namespace {
using flecha::analyzer::Transport;

TEST(AnalyzerTransportTests, ReadsFramedMessage) {
    std::istringstream input("Content-Length: 4\r\n\r\ntest");
    std::ostringstream output;
    Transport transport(input, output);

    const auto message = transport.ReadMessage();

    ASSERT_TRUE(message);
    EXPECT_EQ(*message, "test");
}

TEST(AnalyzerTransportTests, ReadsConsecutiveMessages) {
    std::istringstream input("Content-Length: 3\r\n\r\none"
                             "Content-Length: 3\r\n\r\ntwo");
    std::ostringstream output;
    Transport transport(input, output);

    ASSERT_EQ(transport.ReadMessage(), "one");
    ASSERT_EQ(transport.ReadMessage(), "two");
    EXPECT_FALSE(transport.ReadMessage());
}

TEST(AnalyzerTransportTests, ReturnsEmptyOnCleanEof) {
    std::istringstream input;
    std::ostringstream output;
    Transport transport(input, output);

    EXPECT_FALSE(transport.ReadMessage());
}

TEST(AnalyzerTransportTests, RejectsMissingContentLength) {
    std::istringstream input("Content-Type: application/json\r\n\r\n{}");
    std::ostringstream output;
    Transport transport(input, output);

    EXPECT_THROW(transport.ReadMessage(), std::runtime_error);
}

TEST(AnalyzerTransportTests, RejectsInvalidContentLength) {
    std::istringstream input("Content-Length: nope\r\n\r\n");
    std::ostringstream output;
    Transport transport(input, output);

    EXPECT_THROW(transport.ReadMessage(), std::runtime_error);
}

TEST(AnalyzerTransportTests, RejectsTruncatedBody) {
    std::istringstream input("Content-Length: 10\r\n\r\nshort");
    std::ostringstream output;
    Transport transport(input, output);

    EXPECT_THROW(transport.ReadMessage(), std::runtime_error);
}

TEST(AnalyzerTransportTests, WritesFramedMessage) {
    std::istringstream input;
    std::ostringstream output;
    Transport transport(input, output);

    ASSERT_TRUE(transport.WriteMessage(R"({"ok":true})"));
    EXPECT_EQ(output.str(),
              "Content-Length: 11\r\n\r\n"
              R"({"ok":true})");
}

}  // namespace
