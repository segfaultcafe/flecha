#ifndef FLECHA_CORE_OUTPUT_FORMAT_HPP
#define FLECHA_CORE_OUTPUT_FORMAT_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flecha::core {

enum class OutputFormatPartKind { Literal, Decimal, String, Boolean };

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
        if (text[index] != '%') {
            literal.push_back(text[index]);
            continue;
        }
        if (++index == text.size()) {
            result.error = "format string ends with an incomplete '%'";
            return result;
        }
        if (text[index] == '%') {
            literal.push_back('%');
            continue;
        }
        flushLiteral();
        switch (text[index]) {
            case 'd':
                result.parts.push_back({OutputFormatPartKind::Decimal, {}});
                break;
            case 's':
                result.parts.push_back({OutputFormatPartKind::String, {}});
                break;
            case 'b':
                result.parts.push_back({OutputFormatPartKind::Boolean, {}});
                break;
            default:
                result.error = "unsupported format specifier '%";
                result.error.push_back(text[index]);
                result.error.push_back('\'');
                return result;
        }
    }
    flushLiteral();
    return result;
}

}  // namespace flecha::core

#endif  // FLECHA_CORE_OUTPUT_FORMAT_HPP
