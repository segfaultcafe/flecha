#include "lsp/uri.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace flecha::lsp {
namespace {

int HexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::optional<std::string> PercentDecode(std::string_view value,
                                         std::string& error) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            result += value[index];
            continue;
        }
        if (index + 2 >= value.size()) {
            error = "incomplete percent escape in file URI";
            return std::nullopt;
        }
        const int high = HexValue(value[index + 1]);
        const int low = HexValue(value[index + 2]);
        if (high < 0 || low < 0) {
            error = "invalid percent escape in file URI";
            return std::nullopt;
        }
        const char decoded = static_cast<char>((high << 4) | low);
        if (decoded == '\0') {
            error = "file URI contains a null byte";
            return std::nullopt;
        }
        result += decoded;
        index += 2;
    }
    return result;
}

bool Unreserved(unsigned char value) {
    return std::isalnum(value) != 0 || value == '-' || value == '.' ||
           value == '_' || value == '~';
}

std::string PercentEncodePath(std::string_view path) {
    constexpr char HEX[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char value : path) {
        if (Unreserved(value) || value == '/' || value == ':') {
            result += static_cast<char>(value);
            continue;
        }
        result += '%';
        result += HEX[value >> 4];
        result += HEX[value & 0x0f];
    }
    return result;
}

std::uint32_t DecodeUtf8(std::string_view text, std::size_t& offset) {
    const unsigned char first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) {
        ++offset;
        return first;
    }

    std::size_t length = 0;
    std::uint32_t codepoint = 0;
    if ((first & 0xe0) == 0xc0) {
        length = 2;
        codepoint = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
        length = 3;
        codepoint = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
        length = 4;
        codepoint = first & 0x07;
    } else {
        ++offset;
        return 0xfffd;
    }

    if (offset + length > text.size()) {
        ++offset;
        return 0xfffd;
    }
    for (std::size_t index = 1; index < length; ++index) {
        const unsigned char continuation =
            static_cast<unsigned char>(text[offset + index]);
        if ((continuation & 0xc0) != 0x80) {
            ++offset;
            return 0xfffd;
        }
        codepoint = (codepoint << 6) | (continuation & 0x3f);
    }
    offset += length;
    return codepoint;
}

}  // namespace

std::optional<std::filesystem::path> FileUriToPath(std::string_view uri,
                                                   std::string& error) {
    constexpr std::string_view SCHEME = "file://";
    if (!uri.starts_with(SCHEME)) {
        error = "only file URIs are supported";
        return std::nullopt;
    }

    std::string_view remainder = uri.substr(SCHEME.size());
    const std::size_t slash = remainder.find('/');
    const std::string_view authority = remainder.substr(0, slash);
    if (slash == std::string_view::npos) {
        error = "file URI has no absolute path";
        return std::nullopt;
    }
    std::string_view encodedPath = remainder.substr(slash);
    std::optional<std::string> decoded = PercentDecode(encodedPath, error);
    if (!decoded) return std::nullopt;

#ifdef _WIN32
    if (!authority.empty() && authority != "localhost")
        *decoded = "//" + std::string(authority) + *decoded;
    else if (decoded->size() >= 3 && (*decoded)[0] == '/' &&
             std::isalpha(static_cast<unsigned char>((*decoded)[1])) != 0 &&
             (*decoded)[2] == ':')
        decoded->erase(decoded->begin());
#else
    if (!authority.empty() && authority != "localhost") {
        error = "remote file URI authorities are not supported";
        return std::nullopt;
    }
#endif

    const std::u8string utf8(reinterpret_cast<const char8_t*>(decoded->data()),
                             decoded->size());
    std::filesystem::path path(utf8);
    if (!path.is_absolute()) {
        error = "file URI has no absolute path";
        return std::nullopt;
    }
    return path.lexically_normal();
}

std::string PathToFileUri(const std::filesystem::path& input) {
    std::error_code error;
    std::filesystem::path path = std::filesystem::absolute(input, error);
    if (error) path = input;
    path = path.lexically_normal();
    const std::u8string generic = path.generic_u8string();
    const std::string utf8(reinterpret_cast<const char*>(generic.data()),
                           generic.size());
#ifdef _WIN32
    if (utf8.starts_with("//")) {
        const std::size_t slash = utf8.find('/', 2);
        if (slash != std::string::npos)
            return "file://" + PercentEncodePath(utf8.substr(2, slash - 2)) +
                   PercentEncodePath(utf8.substr(slash));
    }
    return "file:///" + PercentEncodePath(utf8);
#else
    return "file://" + PercentEncodePath(utf8);
#endif
}

Position ByteOffsetToPosition(std::string_view utf8, std::size_t byteOffset) {
    byteOffset = std::min(byteOffset, utf8.size());
    Position result;
    std::size_t offset = 0;
    while (offset < byteOffset) {
        if (utf8[offset] == '\n') {
            ++result.line;
            result.character = 0;
            ++offset;
            continue;
        }
        const std::size_t before = offset;
        const std::uint32_t codepoint = DecodeUtf8(utf8, offset);
        if (offset > byteOffset) {
            offset = before + 1;
            ++result.character;
            continue;
        }
        result.character += codepoint > 0xffff ? 2 : 1;
    }
    return result;
}

std::optional<std::size_t> PositionToByteOffset(std::string_view utf8,
                                                Position position) {
    std::size_t offset = 0;
    for (std::size_t line = 0; line < position.line; ++line) {
        const std::size_t newline = utf8.find('\n', offset);
        if (newline == std::string_view::npos) return std::nullopt;
        offset = newline + 1;
    }

    std::size_t character = 0;
    while (offset < utf8.size() && utf8[offset] != '\n') {
        if (character == position.character) return offset;
        const std::size_t before = offset;
        const std::uint32_t codepoint = DecodeUtf8(utf8, offset);
        const std::size_t units = codepoint > 0xffff ? 2 : 1;
        if (character + units > position.character) return std::nullopt;
        character += units;
        if (offset <= before) return std::nullopt;
    }
    return character == position.character ? std::optional<std::size_t>(offset)
                                           : std::nullopt;
}

}  // namespace flecha::lsp
