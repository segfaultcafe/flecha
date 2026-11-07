#include "analyzer/json.hpp"

#include <cstdint>
#include <utility>

namespace flecha::analyzer::json {
namespace {

class Parser final {
   public:
    explicit Parser(std::string_view text) : _text(text) {}

    std::optional<Value> ParseDocument(std::string& error) {
        SkipWhitespace();
        std::optional<Value> value = ParseValue(0);
        SkipWhitespace();
        if (_error.empty() && value && _offset != _text.size())
            Fail("unexpected trailing input");
        if (!_error.empty()) {
            error = _error;
            return std::nullopt;
        }
        return value;
    }

   private:
    std::optional<Value> ParseValue(unsigned depth) {
        if (depth > 128) return Failure("JSON nesting exceeds 128 levels");
        SkipWhitespace();
        if (_offset == _text.size()) return Failure("expected a JSON value");

        switch (_text[_offset]) {
            case '{':
                return ParseObject(depth + 1);
            case '[':
                return ParseArray(depth + 1);
            case '"': {
                std::optional<std::string> text = ParseString();
                if (!text) return std::nullopt;
                return Value{std::move(*text), false};
            }
            case 't':
                return ParseLiteral("true", Value{true, false});
            case 'f':
                return ParseLiteral("false", Value{false, false});
            case 'n':
                return ParseLiteral("null", Value{nullptr, false});
            default:
                if (_text[_offset] == '-' ||
                    (_text[_offset] >= '0' && _text[_offset] <= '9'))
                    return ParseNumber();
                return Failure("expected a JSON value");
        }
    }

    std::optional<Value> ParseObject(unsigned depth) {
        ++_offset;
        SkipWhitespace();
        Value::Object object;
        if (Consume('}')) return Value{std::move(object), false};

        while (_offset < _text.size()) {
            if (_text[_offset] != '"') return Failure("expected an object key");
            std::optional<std::string> key = ParseString();
            if (!key) return std::nullopt;
            SkipWhitespace();
            if (!Consume(':')) return Failure("expected ':' after object key");
            std::optional<Value> value = ParseValue(depth);
            if (!value) return std::nullopt;
            if (!object.emplace(*key, std::move(*value)).second)
                return Failure("duplicate object key '" + *key + "'");
            SkipWhitespace();
            if (Consume('}')) return Value{std::move(object), false};
            if (!Consume(',')) return Failure("expected ',' or '}' in object");
            SkipWhitespace();
        }
        return Failure("unterminated object");
    }

    std::optional<Value> ParseArray(unsigned depth) {
        ++_offset;
        SkipWhitespace();
        Value::Array array;
        if (Consume(']')) return Value{std::move(array), false};

        while (_offset < _text.size()) {
            std::optional<Value> value = ParseValue(depth);
            if (!value) return std::nullopt;
            array.push_back(std::move(*value));
            SkipWhitespace();
            if (Consume(']')) return Value{std::move(array), false};
            if (!Consume(',')) return Failure("expected ',' or ']' in array");
            SkipWhitespace();
        }
        return Failure("unterminated array");
    }

    std::optional<std::string> ParseString() {
        ++_offset;
        std::string result;
        while (_offset < _text.size()) {
            const unsigned char character =
                static_cast<unsigned char>(_text[_offset++]);
            if (character == '"') return result;
            if (character < 0x20) {
                Fail("control character in string");
                return std::nullopt;
            }
            if (character != '\\') {
                result += static_cast<char>(character);
                continue;
            }
            if (_offset == _text.size()) {
                Fail("unterminated string escape");
                return std::nullopt;
            }
            switch (_text[_offset++]) {
                case '"':
                    result += '"';
                    break;
                case '\\':
                    result += '\\';
                    break;
                case '/':
                    result += '/';
                    break;
                case 'b':
                    result += '\b';
                    break;
                case 'f':
                    result += '\f';
                    break;
                case 'n':
                    result += '\n';
                    break;
                case 'r':
                    result += '\r';
                    break;
                case 't':
                    result += '\t';
                    break;
                case 'u': {
                    std::optional<std::uint32_t> codepoint = ParseCodepoint();
                    if (!codepoint) return std::nullopt;
                    AppendUtf8(result, *codepoint);
                    break;
                }
                default:
                    Fail("invalid string escape");
                    return std::nullopt;
            }
        }
        Fail("unterminated string");
        return std::nullopt;
    }

    std::optional<std::uint32_t> ParseCodepoint() {
        std::optional<std::uint32_t> first = ParseHexQuad();
        if (!first) return std::nullopt;
        if (*first < 0xd800 || *first > 0xdfff) return first;
        if (*first > 0xdbff || _offset + 2 > _text.size() ||
            _text.substr(_offset, 2) != "\\u") {
            Fail("invalid Unicode surrogate pair");
            return std::nullopt;
        }
        _offset += 2;
        std::optional<std::uint32_t> second = ParseHexQuad();
        if (!second || *second < 0xdc00 || *second > 0xdfff) {
            Fail("invalid Unicode surrogate pair");
            return std::nullopt;
        }
        return 0x10000 + ((*first - 0xd800) << 10) + (*second - 0xdc00);
    }

    std::optional<std::uint32_t> ParseHexQuad() {
        if (_offset + 4 > _text.size()) {
            Fail("incomplete Unicode escape");
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (unsigned index = 0; index < 4; ++index) {
            const char digit = _text[_offset++];
            value <<= 4;
            if (digit >= '0' && digit <= '9')
                value += digit - '0';
            else if (digit >= 'a' && digit <= 'f')
                value += digit - 'a' + 10;
            else if (digit >= 'A' && digit <= 'F')
                value += digit - 'A' + 10;
            else {
                Fail("invalid Unicode escape");
                return std::nullopt;
            }
        }
        return value;
    }

    static void AppendUtf8(std::string& output, std::uint32_t value) {
        if (value <= 0x7f)
            output += static_cast<char>(value);
        else if (value <= 0x7ff) {
            output += static_cast<char>(0xc0 | (value >> 6));
            output += static_cast<char>(0x80 | (value & 0x3f));
        } else if (value <= 0xffff) {
            output += static_cast<char>(0xe0 | (value >> 12));
            output += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
            output += static_cast<char>(0x80 | (value & 0x3f));
        } else {
            output += static_cast<char>(0xf0 | (value >> 18));
            output += static_cast<char>(0x80 | ((value >> 12) & 0x3f));
            output += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
            output += static_cast<char>(0x80 | (value & 0x3f));
        }
    }

    std::optional<Value> ParseNumber() {
        const std::size_t start = _offset;
        if (_text[_offset] == '-') ++_offset;
        if (_offset == _text.size()) return Failure("invalid number");
        if (_text[_offset] == '0')
            ++_offset;
        else {
            if (_text[_offset] < '1' || _text[_offset] > '9')
                return Failure("invalid number");
            while (_offset < _text.size() && _text[_offset] >= '0' &&
                   _text[_offset] <= '9')
                ++_offset;
        }
        if (_offset < _text.size() && _text[_offset] == '.') {
            ++_offset;
            if (!HasDigit()) return Failure("invalid number");
            while (HasDigit()) ++_offset;
        }
        if (_offset < _text.size() &&
            (_text[_offset] == 'e' || _text[_offset] == 'E')) {
            ++_offset;
            if (_offset < _text.size() &&
                (_text[_offset] == '+' || _text[_offset] == '-'))
                ++_offset;
            if (!HasDigit()) return Failure("invalid number");
            while (HasDigit()) ++_offset;
        }
        return Value{std::string(_text.substr(start, _offset - start)), true};
    }

    bool HasDigit() const {
        return _offset < _text.size() && _text[_offset] >= '0' &&
               _text[_offset] <= '9';
    }

    std::optional<Value> ParseLiteral(std::string_view spelling, Value value) {
        if (_text.substr(_offset, spelling.size()) != spelling)
            return Failure("invalid JSON literal");
        _offset += spelling.size();
        return value;
    }

    std::optional<Value> Failure(const std::string& message) {
        Fail(message);
        return std::nullopt;
    }

    bool Consume(char character) {
        if (_offset == _text.size() || _text[_offset] != character)
            return false;
        ++_offset;
        return true;
    }

    void SkipWhitespace() {
        while (_offset < _text.size() &&
               (_text[_offset] == ' ' || _text[_offset] == '\t' ||
                _text[_offset] == '\r' || _text[_offset] == '\n'))
            ++_offset;
    }

    void Fail(const std::string& message) {
        if (!_error.empty()) return;
        unsigned line = 1;
        unsigned column = 1;
        for (std::size_t index = 0; index < _offset && index < _text.size();
             ++index) {
            if (_text[index] == '\n') {
                ++line;
                column = 1;
            } else
                ++column;
        }
        _error = "line " + std::to_string(line) + ", column " +
                 std::to_string(column) + ": " + message;
    }

    std::string_view _text;
    std::size_t _offset = 0;
    std::string _error;
};

}  // namespace

std::optional<Value> Parse(std::string_view text, std::string& error) {
    return Parser(text).ParseDocument(error);
}

const Value::Object* AsObject(const Value& value) {
    return std::get_if<Value::Object>(&value.value);
}

const Value::Array* AsArray(const Value& value) {
    return std::get_if<Value::Array>(&value.value);
}

const std::string* AsString(const Value& value) {
    if (value.number) return nullptr;
    return std::get_if<std::string>(&value.value);
}

const std::string* AsNumber(const Value& value) {
    if (!value.number) return nullptr;
    return std::get_if<std::string>(&value.value);
}

const Value* Field(const Value::Object& object, std::string_view name) {
    const auto found = object.find(std::string(name));
    return found == object.end() ? nullptr : &found->second;
}

std::string Quote(std::string_view value) {
    constexpr char HEX[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char character : value) {
        switch (character) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\b':
                result += "\\b";
                break;
            case '\f':
                result += "\\f";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                if (character < 0x20) {
                    result += "\\u00";
                    result += HEX[character >> 4];
                    result += HEX[character & 0x0f];
                } else
                    result += static_cast<char>(character);
                break;
        }
    }
    result += '"';
    return result;
}

}  // namespace flecha::analyzer::json
