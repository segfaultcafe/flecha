#ifndef FLECHA_CORE_OUTPUT_FORMAT_HPP
#define FLECHA_CORE_OUTPUT_FORMAT_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flecha::core {

enum class OutputFormatPartKind { Literal, Value };

struct OutputFormatPart final {
    OutputFormatPartKind kind;
    std::string text;
};

struct OutputFormat final {
    std::vector<OutputFormatPart> parts;
    std::string error;

    [[nodiscard]] bool IsValid() const { return error.empty(); }
    [[nodiscard]] std::size_t ArgumentCount() const {
        std::size_t count = 0;
        for (const auto& part : parts)
            if (part.kind != OutputFormatPartKind::Literal) ++count;
        return count;
    }
};

[[nodiscard]] inline OutputFormat ParseOutputFormat(std::string_view text) {
    OutputFormat result;
    std::string literal;
    const auto flushLiteral = [&] {
        if (literal.empty()) return;
        result.parts.push_back(
            {OutputFormatPartKind::Literal, std::move(literal)});
        literal.clear();
    };

    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '{' && text[index] != '}') {
            literal.push_back(text[index]);
            continue;
        }
        const char brace = text[index];
        if (index + 1 < text.size() && text[index + 1] == brace) {
            literal.push_back(brace);
            ++index;
            continue;
        }
        if (brace == '{' && index + 1 < text.size() && text[index + 1] == '}') {
            flushLiteral();
            result.parts.push_back({OutputFormatPartKind::Value, {}});
            ++index;
            continue;
        }
        result.error = brace == '{' ? "format string contains an unmatched '{'"
                                    : "format string contains an unmatched '}'";
        return result;
    }
    flushLiteral();
    return result;
}

}  // namespace flecha::core

#endif  // FLECHA_CORE_OUTPUT_FORMAT_HPP
