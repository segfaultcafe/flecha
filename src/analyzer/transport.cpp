#include "analyzer/transport.hpp"

#include <charconv>
#include <cstddef>
#include <ios>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace flecha::analyzer {

namespace {

constexpr std::size_t MAX_HEADER_BYTES = 8 * 1024;
constexpr std::size_t MAX_MESSAGES_BYTES = 64 * 1024 * 1024;

char AsciiLower(char character) {
    if (character >= 'A' && character <= 'Z')
        return static_cast<char>(character + ('a' - 'A'));
    return character;
}

bool EqualsIgnoreAsciiCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;

    for (std::size_t index = 0; index < left.size(); index++) {
        if (AsciiLower(left[index]) != AsciiLower(right[index])) return false;
    }

    return true;
}

std::string_view TrimWhitespace(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);

    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.remove_suffix(1);

    return value;
}

}  // namespace

std::optional<std::string> Transport::ReadMessage() {
    std::optional<std::size_t> contentLength;
    std::size_t headerBytes = 0;
    bool readAnyHeaderData = false;

    while (true) {
        std::string line;

        if (!std::getline(_input, line)) {
            if (!readAnyHeaderData && _input.eof()) return std::nullopt;

            throw std::runtime_error("truncated LSP message header");
        }

        readAnyHeaderData = true;
        headerBytes += line.size() + 1;

        if (headerBytes > MAX_HEADER_BYTES)
            throw std::runtime_error("LSP message header is too large");

        if (line.empty() || line.back() != '\r')
            throw std::runtime_error(
                "LSP message headers must use CRLF line endings");

        line.pop_back();
        if (line.empty()) break;

        const std::size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0)
            throw std::runtime_error("invalid LSP message header");

        const std::string_view name(line.data(), colon);
        const std::string_view value =
            TrimWhitespace(std::string_view(line).substr(colon + 1));

        if (!EqualsIgnoreAsciiCase(name, "Content-Length")) continue;

        if (contentLength)
            throw std::runtime_error("duplicate Content-Length header");

        std::size_t parsedLength = 0;
        const auto [end, error] = std::from_chars(
            value.data(), value.data() + value.size(), parsedLength);

        if (value.empty() || error != std::errc{} ||
            end != value.data() + value.size())
            throw std::runtime_error("invalid Content-Length header");

        if (parsedLength > MAX_MESSAGES_BYTES)
            throw std::runtime_error("LSP message body is too large");

        contentLength = parsedLength;
    }

    if (!contentLength)
        throw std::runtime_error("missing Content-Length header");

    std::string msg(*contentLength, '\0');
    if (!msg.empty()) {
        _input.read(msg.data(), static_cast<std::streamsize>(msg.size()));

        if (static_cast<std::size_t>(_input.gcount()) != msg.size())
            throw std::runtime_error("truncated LSP message body");
    }

    return msg;
}

bool Transport::WriteMessage(std::string_view message) {
    if (message.size() > MAX_MESSAGES_BYTES) return false;

    _output << "Content-Length: " << message.size() << "\r\n\r\n";
    _output.write(message.data(), static_cast<std::streamsize>(message.size()));
    _output.flush();
    return static_cast<bool>(_output);
}

}  // namespace flecha::analyzer
