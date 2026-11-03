#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>

namespace flecha::core {

inline bool ParseUnsignedIntegerLiteral(std::string_view literal,
                                        std::uint64_t& value) {
    int radix = 10;
    if (literal.size() >= 2 && literal[0] == '0' &&
        (literal[1] == 'x' || literal[1] == 'X')) {
        radix = 16;
        literal.remove_prefix(2);
    }
    if (literal.empty()) return false;

    const auto parsed = std::from_chars(
        literal.data(), literal.data() + literal.size(), value, radix);
    return parsed.ec == std::errc{} &&
           parsed.ptr == literal.data() + literal.size();
}

inline bool IsZeroIntegerLiteral(std::string_view literal) {
    std::uint64_t value = 0;
    return ParseUnsignedIntegerLiteral(literal, value) && value == 0;
}

}  // namespace flecha::core
