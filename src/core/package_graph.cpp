#include "core/package_graph.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string_view>
#include <variant>
#include <vector>

#include "core/tokenizer.hpp"

namespace flecha::core {
namespace fs = std::filesystem;
namespace {

struct JsonValue final {
    using Object = std::map<std::string, JsonValue>;
    using Array = std::vector<JsonValue>;
    using Storage =
        std::variant<std::nullptr_t, bool, std::string, Object, Array>;

    // JSON numbers are retained as their source spelling. The graph schema has
    // only one numeric field and validates it as an unsigned integer.
    Storage value;
    bool number = false;
};

class JsonParser final {
   public:
    explicit JsonParser(std::string_view text) : _text(text) {}

    std::optional<JsonValue> Parse(std::string& error) {
        SkipWhitespace();
        auto value = ParseValue(0);
        SkipWhitespace();
        if (!_error.empty()) {
            error = _error;
            return std::nullopt;
        }
        if (!value || _offset != _text.size()) {
            Fail("unexpected trailing input");
            error = _error;
            return std::nullopt;
        }
        return value;
    }

   private:
    std::optional<JsonValue> ParseValue(unsigned depth) {
        if (depth > 128) {
            Fail("JSON nesting exceeds 128 levels");
            return std::nullopt;
        }
        SkipWhitespace();
        if (_offset == _text.size()) {
            Fail("expected a JSON value");
            return std::nullopt;
        }
        switch (_text[_offset]) {
            case '{':
                return ParseObject(depth + 1);
            case '[':
                return ParseArray(depth + 1);
            case '"': {
                auto text = ParseString();
                if (!text) return std::nullopt;
                return JsonValue{std::move(*text), false};
            }
            case 't':
                return ParseLiteral("true", JsonValue{true, false});
            case 'f':
                return ParseLiteral("false", JsonValue{false, false});
            case 'n':
                return ParseLiteral("null", JsonValue{nullptr, false});
            default:
                if (_text[_offset] == '-' ||
                    (_text[_offset] >= '0' && _text[_offset] <= '9'))
                    return ParseNumber();
                Fail("expected a JSON value");
                return std::nullopt;
        }
    }

    std::optional<JsonValue> ParseObject(unsigned depth) {
        ++_offset;
        SkipWhitespace();
        JsonValue::Object object;
        if (Consume('}')) return JsonValue{std::move(object), false};
        while (_offset < _text.size()) {
            if (_text[_offset] != '"') {
                Fail("expected an object key");
                return std::nullopt;
            }
            auto key = ParseString();
            if (!key) return std::nullopt;
            SkipWhitespace();
            if (!Consume(':')) {
                Fail("expected ':' after object key");
                return std::nullopt;
            }
            auto value = ParseValue(depth);
            if (!value) return std::nullopt;
            if (!object.emplace(*key, std::move(*value)).second) {
                Fail("duplicate object key '" + *key + "'");
                return std::nullopt;
            }
            SkipWhitespace();
            if (Consume('}')) return JsonValue{std::move(object), false};
            if (!Consume(',')) {
                Fail("expected ',' or '}' in object");
                return std::nullopt;
            }
            SkipWhitespace();
        }
        Fail("unterminated object");
        return std::nullopt;
    }

    std::optional<JsonValue> ParseArray(unsigned depth) {
        ++_offset;
        SkipWhitespace();
        JsonValue::Array array;
        if (Consume(']')) return JsonValue{std::move(array), false};
        while (_offset < _text.size()) {
            auto value = ParseValue(depth);
            if (!value) return std::nullopt;
            array.push_back(std::move(*value));
            SkipWhitespace();
            if (Consume(']')) return JsonValue{std::move(array), false};
            if (!Consume(',')) {
                Fail("expected ',' or ']' in array");
                return std::nullopt;
            }
            SkipWhitespace();
        }
        Fail("unterminated array");
        return std::nullopt;
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
                    const auto codepoint = ParseCodepoint();
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
        const auto first = ParseHexQuad();
        if (!first) return std::nullopt;
        if (*first < 0xd800 || *first > 0xdfff) return first;
        if (*first > 0xdbff || _offset + 2 > _text.size() ||
            _text.substr(_offset, 2) != "\\u") {
            Fail("invalid Unicode surrogate pair");
            return std::nullopt;
        }
        _offset += 2;
        const auto second = ParseHexQuad();
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

    std::optional<JsonValue> ParseNumber() {
        const std::size_t start = _offset;
        if (_text[_offset] == '-') ++_offset;
        if (_offset == _text.size()) {
            Fail("invalid number");
            return std::nullopt;
        }
        if (_text[_offset] == '0')
            ++_offset;
        else {
            if (_text[_offset] < '1' || _text[_offset] > '9') {
                Fail("invalid number");
                return std::nullopt;
            }
            while (_offset < _text.size() && _text[_offset] >= '0' &&
                   _text[_offset] <= '9')
                ++_offset;
        }
        if (_offset < _text.size() && _text[_offset] == '.') {
            ++_offset;
            if (_offset == _text.size() || _text[_offset] < '0' ||
                _text[_offset] > '9') {
                Fail("invalid number");
                return std::nullopt;
            }
            while (_offset < _text.size() && _text[_offset] >= '0' &&
                   _text[_offset] <= '9')
                ++_offset;
        }
        if (_offset < _text.size() &&
            (_text[_offset] == 'e' || _text[_offset] == 'E')) {
            ++_offset;
            if (_offset < _text.size() &&
                (_text[_offset] == '+' || _text[_offset] == '-'))
                ++_offset;
            if (_offset == _text.size() || _text[_offset] < '0' ||
                _text[_offset] > '9') {
                Fail("invalid number");
                return std::nullopt;
            }
            while (_offset < _text.size() && _text[_offset] >= '0' &&
                   _text[_offset] <= '9')
                ++_offset;
        }
        return JsonValue{std::string(_text.substr(start, _offset - start)),
                         true};
    }

    std::optional<JsonValue> ParseLiteral(std::string_view spelling,
                                          JsonValue value) {
        if (_text.substr(_offset, spelling.size()) != spelling) {
            Fail("invalid JSON literal");
            return std::nullopt;
        }
        _offset += spelling.size();
        return value;
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
        unsigned line = 1, column = 1;
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

const JsonValue::Object* Object(const JsonValue& value) {
    return std::get_if<JsonValue::Object>(&value.value);
}
const JsonValue::Array* Array(const JsonValue& value) {
    return std::get_if<JsonValue::Array>(&value.value);
}
const std::string* String(const JsonValue& value) {
    if (value.number) return nullptr;
    return std::get_if<std::string>(&value.value);
}
const JsonValue* Field(const JsonValue::Object& object,
                       const std::string& name) {
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &found->second;
}

bool TextField(const JsonValue::Object& object, const std::string& name,
               std::string& output, bool required, std::string& error,
               const std::string& context) {
    const JsonValue* value = Field(object, name);
    if (!value) {
        if (required)
            error = context + " is missing string field '" + name + "'";
        return !required;
    }
    const std::string* text = String(*value);
    if (!text) {
        error = context + " field '" + name + "' must be a string";
        return false;
    }
    output = *text;
    return true;
}

bool SafeIdentity(std::string_view value) {
    if (value.empty()) return false;
    for (const unsigned char character : value)
        if (character < 0x20 || character == 0x7f) return false;
    return true;
}

bool ValidAlias(std::string_view value) {
    if (value.empty() || value == "root" || value == "lib" || value[0] < 'a' ||
        value[0] > 'z')
        return false;
    for (const char character : value)
        if ((character < 'a' || character > 'z') &&
            (character < '0' || character > '9') && character != '_')
            return false;
    diagnostics::DiagnosticBag diagnostics;
    const auto tokens = Tokenizer(std::string(value), diagnostics).Tokenize();
    return !diagnostics.HasErrors() && tokens.size() == 2 &&
           tokens[0].type == TokenType::Identifier;
}

fs::path ResolveSource(const fs::path& graphFile, const std::string& source) {
    fs::path result(source);
    if (result.is_relative()) result = graphFile.parent_path() / result;
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(result, error);
    return error ? fs::absolute(result).lexically_normal() : canonical;
}

}  // namespace

std::optional<PackageGraph> PackageGraph::Load(const fs::path& file,
                                               std::string& error) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        error = "cannot read package graph '" + file.string() + "'";
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (!stream.good() && !stream.eof()) {
        error = "cannot read package graph '" + file.string() + "'";
        return std::nullopt;
    }
    const std::string contents = buffer.str();
    if (contents.size() > 16 * 1024 * 1024) {
        error = "package graph exceeds 16 MiB";
        return std::nullopt;
    }
    auto document = JsonParser(contents).Parse(error);
    if (!document) return std::nullopt;
    const auto* object = Object(*document);
    if (!object) {
        error = "package graph must be a JSON object";
        return std::nullopt;
    }

    const JsonValue* schemaValue = Field(*object, "schema");
    if (!schemaValue || !schemaValue->number) {
        error = "package graph field 'schema' must be an integer";
        return std::nullopt;
    }
    unsigned schema = 0;
    const auto* spelling = std::get_if<std::string>(&schemaValue->value);
    const auto parsed = std::from_chars(
        spelling->data(), spelling->data() + spelling->size(), schema);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != spelling->data() + spelling->size()) {
        error = "package graph field 'schema' must be an integer";
        return std::nullopt;
    }
    if (schema != SchemaVersion) {
        error = "unsupported package graph schema " + std::to_string(schema) +
                "; compiler supports schema " + std::to_string(SchemaVersion);
        return std::nullopt;
    }

    PackageGraph graph;
    if (!TextField(*object, "root", graph._root, true, error, "package graph"))
        return std::nullopt;
    if (!SafeIdentity(graph._root)) {
        error = "package graph root id is empty or contains control characters";
        return std::nullopt;
    }
    const JsonValue* packagesValue = Field(*object, "packages");
    const auto* packages = packagesValue ? Array(*packagesValue) : nullptr;
    if (!packages) {
        error = "package graph field 'packages' must be an array";
        return std::nullopt;
    }
    if (packages->size() > 10000) {
        error = "package graph exceeds 10000 package nodes";
        return std::nullopt;
    }

    for (std::size_t index = 0; index < packages->size(); ++index) {
        const auto* package = Object((*packages)[index]);
        const std::string context =
            "package graph node " + std::to_string(index);
        if (!package) {
            error = context + " must be an object";
            return std::nullopt;
        }
        PackageGraphNode node;
        std::string source;
        if (!TextField(*package, "id", node.id, true, error, context) ||
            !TextField(*package, "identity", node.identity, true, error,
                       context) ||
            !TextField(*package, "source", source, true, error, context) ||
            !TextField(*package, "version", node.version, false, error,
                       context) ||
            !TextField(*package, "revision", node.revision, false, error,
                       context) ||
            !TextField(*package, "checksum", node.checksum, false, error,
                       context) ||
            !TextField(*package, "flecha", node.flecha, false, error, context))
            return std::nullopt;
        if (!SafeIdentity(node.id) || !SafeIdentity(node.identity)) {
            error = context +
                    " id and identity must be nonempty and contain no control "
                    "characters";
            return std::nullopt;
        }
        if (source.empty()) {
            error = context + " source must not be empty";
            return std::nullopt;
        }
        node.source = ResolveSource(file, source);
        const JsonValue* dependenciesValue = Field(*package, "dependencies");
        const auto* dependencies =
            dependenciesValue ? Object(*dependenciesValue) : nullptr;
        if (!dependencies) {
            error = context + " field 'dependencies' must be an object";
            return std::nullopt;
        }
        for (const auto& [alias, targetValue] : *dependencies) {
            const std::string* target = String(targetValue);
            if (!ValidAlias(alias)) {
                error =
                    context + " has invalid dependency alias '" + alias + "'";
                return std::nullopt;
            }
            if (!target || !SafeIdentity(*target)) {
                error = context + " dependency '" + alias +
                        "' must name a package node id";
                return std::nullopt;
            }
            node.dependencies.emplace(alias, *target);
        }
        const std::string id = node.id;
        if (!graph._nodes.emplace(id, std::move(node)).second) {
            error = "duplicate package graph node id '" + id + "'";
            return std::nullopt;
        }
    }

    if (!graph._nodes.contains(graph._root)) {
        error =
            "package graph root references missing node '" + graph._root + "'";
        return std::nullopt;
    }
    for (const auto& [id, node] : graph._nodes)
        for (const auto& [alias, target] : node.dependencies) {
            (void)alias;
            if (!graph._nodes.contains(target)) {
                error = "package graph node '" + id +
                        "' references missing node '" + target + "'";
                return std::nullopt;
            }
        }

    enum class Visit { New, Active, Complete };
    std::map<std::string, Visit> visits;
    std::vector<std::string> path;
    std::function<bool(const std::string&)> visit = [&](const std::string& id) {
        if (visits[id] == Visit::Complete) return true;
        if (visits[id] == Visit::Active) {
            const auto start = std::find(path.begin(), path.end(), id);
            error = "package graph cycle:";
            for (auto item = start; item != path.end(); ++item)
                error += " " + *item + " ->";
            error += " " + id;
            return false;
        }
        visits[id] = Visit::Active;
        path.push_back(id);
        for (const auto& [alias, target] : graph._nodes.at(id).dependencies) {
            (void)alias;
            if (!visit(target)) return false;
        }
        path.pop_back();
        visits[id] = Visit::Complete;
        return true;
    };
    for (const auto& [id, node] : graph._nodes) {
        (void)node;
        if (!visit(id)) return std::nullopt;
    }
    return graph;
}

const PackageGraphNode& PackageGraph::Root() const { return _nodes.at(_root); }

const PackageGraphNode* PackageGraph::Find(const std::string& id) const {
    const auto found = _nodes.find(id);
    return found == _nodes.end() ? nullptr : &found->second;
}

}  // namespace flecha::core
