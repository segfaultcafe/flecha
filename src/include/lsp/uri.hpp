#ifndef FLECHA_LSP_URI_HPP
#define FLECHA_LSP_URI_HPP

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace flecha::lsp {

[[nodiscard]] std::optional<std::filesystem::path> FileUriToPath(
    std::string_view uri, std::string& error);
[[nodiscard]] std::string PathToFileUri(const std::filesystem::path& path);

struct Position final {
    std::size_t line = 0;
    std::size_t character = 0;
};

// Analyzer offsets are UTF-8 byte offsets. LSP positions use zero-based lines
// and UTF-16 code units, including for clients whose host strings are UTF-16.
[[nodiscard]] Position ByteOffsetToPosition(std::string_view utf8,
                                            std::size_t byteOffset);
[[nodiscard]] std::optional<std::size_t> PositionToByteOffset(
    std::string_view utf8, Position position);

}  // namespace flecha::lsp

#endif  // FLECHA_LSP_URI_HPP
