#include "longbow/manifest.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include "core/tokenizer.hpp"

namespace flecha::longbow {
namespace fs = std::filesystem;
namespace {

std::string Trim(std::string_view text) {
    const auto start = text.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(start, end - start + 1));
}

bool Identifier(std::string_view text) {
    if (text.empty() ||
        !((text.front() >= 'a' && text.front() <= 'z') ||
          (text.front() >= 'A' && text.front() <= 'Z') || text.front() == '_'))
        return false;
    return std::all_of(text.begin() + 1, text.end(), [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_';
    });
}

std::string StripComment(std::string line, unsigned number) {
    bool quoted = false, escaped = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (quoted && line[index] == '\\') {
            escaped = true;
            continue;
        }
        if (line[index] == '"') quoted = !quoted;
        if (!quoted && line[index] == '#') {
            line.resize(index);
            break;
        }
    }
    if (quoted || escaped)
        throw std::runtime_error("unterminated string at line " +
                                 std::to_string(number));
    return Trim(line);
}

std::string ParseString(std::string_view input, unsigned number) {
    const std::string text = Trim(input);
    if (text.size() < 2 || text.front() != '"' || text.back() != '"')
        throw std::runtime_error("expected a quoted string at line " +
                                 std::to_string(number));
    std::string value;
    for (std::size_t index = 1; index + 1 < text.size(); ++index) {
        char character = text[index];
        if (character != '\\') {
            if (static_cast<unsigned char>(character) < 0x20)
                throw std::runtime_error(
                    "control character in string at line " +
                    std::to_string(number));
            value += character;
            continue;
        }
        if (++index + 1 >= text.size())
            throw std::runtime_error("unterminated string escape at line " +
                                     std::to_string(number));
        switch (text[index]) {
            case '"':
                value += '"';
                break;
            case '\\':
                value += '\\';
                break;
            case 'n':
                value += '\n';
                break;
            case 'r':
                value += '\r';
                break;
            case 't':
                value += '\t';
                break;
            default:
                throw std::runtime_error("unsupported string escape at line " +
                                         std::to_string(number));
        }
    }
    return value;
}

std::pair<std::string, std::string> Assignment(const std::string& line,
                                               unsigned number) {
    bool quoted = false, escaped = false;
    int braces = 0;
    for (std::size_t index = 0; index < line.size(); ++index) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (quoted && line[index] == '\\') {
            escaped = true;
            continue;
        }
        if (line[index] == '"') quoted = !quoted;
        if (quoted) continue;
        if (line[index] == '{') ++braces;
        if (line[index] == '}') --braces;
        if (line[index] == '=' && braces == 0) {
            const std::string key =
                Trim(std::string_view(line).substr(0, index));
            const std::string value =
                Trim(std::string_view(line).substr(index + 1));
            if (!Identifier(key) || value.empty()) break;
            return {key, value};
        }
    }
    throw std::runtime_error("unsupported manifest syntax at line " +
                             std::to_string(number));
}

std::map<std::string, std::string> InlineTable(std::string_view input,
                                               unsigned number) {
    const std::string text = Trim(input);
    if (text.size() < 2 || text.front() != '{' || text.back() != '}')
        throw std::runtime_error("expected an inline table at line " +
                                 std::to_string(number));
    std::map<std::string, std::string> result;
    std::size_t offset = 1;
    while (true) {
        while (offset + 1 < text.size() &&
               std::isspace(static_cast<unsigned char>(text[offset])))
            ++offset;
        if (offset + 1 == text.size()) return result;
        const std::size_t keyStart = offset;
        while (offset + 1 < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text[offset])) ||
                text[offset] == '_'))
            ++offset;
        const std::string key = text.substr(keyStart, offset - keyStart);
        if (!Identifier(key))
            throw std::runtime_error("invalid inline-table key at line " +
                                     std::to_string(number));
        while (offset + 1 < text.size() &&
               std::isspace(static_cast<unsigned char>(text[offset])))
            ++offset;
        if (offset + 1 >= text.size() || text[offset++] != '=')
            throw std::runtime_error("expected '=' in inline table at line " +
                                     std::to_string(number));
        while (offset + 1 < text.size() &&
               std::isspace(static_cast<unsigned char>(text[offset])))
            ++offset;
        if (offset + 1 >= text.size() || text[offset] != '"')
            throw std::runtime_error(
                "inline-table values must be strings at line " +
                std::to_string(number));
        const std::size_t valueStart = offset++;
        bool escaped = false;
        while (offset < text.size()) {
            if (escaped) {
                escaped = false;
            } else if (text[offset] == '\\') {
                escaped = true;
            } else if (text[offset] == '"') {
                ++offset;
                break;
            }
            ++offset;
        }
        if (offset > text.size() || text[offset - 1] != '"')
            throw std::runtime_error(
                "unterminated inline-table string at line " +
                std::to_string(number));
        const std::string value = ParseString(
            std::string_view(text).substr(valueStart, offset - valueStart),
            number);
        if (!result.emplace(key, value).second)
            throw std::runtime_error("duplicate inline-table key '" + key +
                                     "' at line " + std::to_string(number));
        while (offset + 1 < text.size() &&
               std::isspace(static_cast<unsigned char>(text[offset])))
            ++offset;
        if (offset + 1 == text.size()) return result;
        if (text[offset++] != ',')
            throw std::runtime_error("expected ',' in inline table at line " +
                                     std::to_string(number));
    }
}

bool NumericIdentifier(std::string_view value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](char character) {
               return character >= '0' && character <= '9';
           });
}

bool ValidVersionIdentifier(std::string_view value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') ||
                      character == '-';
           });
}

std::vector<std::string> SplitIdentifiers(std::string_view text,
                                          const std::string& description,
                                          std::string& error) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t dot = text.find('.', start);
        const std::size_t end =
            dot == std::string_view::npos ? text.size() : dot;
        const std::string value(text.substr(start, end - start));
        if (!ValidVersionIdentifier(value)) {
            error = "invalid " + description + " identifier '" + value + "'";
            return {};
        }
        result.push_back(value);
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return result;
}

bool ParseNumber(std::string_view text, std::uint64_t& value,
                 std::string& error) {
    if (!NumericIdentifier(text) || (text.size() > 1 && text.front() == '0')) {
        error = "invalid numeric version component '" + std::string(text) + "'";
        return false;
    }
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{}) {
        error = "version component is too large";
        return false;
    }
    return true;
}

struct PartialVersion final {
    SemanticVersion version;
    unsigned components = 0;
};

std::optional<PartialVersion> ParsePartial(const std::string& text,
                                           std::string& error) {
    std::string core = text;
    std::string suffix;
    const std::size_t dash = core.find('-');
    const std::size_t plus = core.find('+');
    const std::size_t split = std::min(dash, plus);
    if (split != std::string::npos) {
        suffix = core.substr(split);
        core.resize(split);
    }
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= core.size()) {
        const std::size_t dot = core.find('.', start);
        const std::size_t end = dot == std::string::npos ? core.size() : dot;
        parts.push_back(core.substr(start, end - start));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    if (parts.empty() || parts.size() > 3) {
        error = "semantic versions require one to three numeric components";
        return std::nullopt;
    }
    PartialVersion result;
    result.components = static_cast<unsigned>(parts.size());
    std::uint64_t* values[] = {&result.version.major, &result.version.minor,
                               &result.version.patch};
    for (std::size_t index = 0; index < parts.size(); ++index)
        if (!ParseNumber(parts[index], *values[index], error))
            return std::nullopt;
    if (!suffix.empty()) {
        if (parts.size() != 3) {
            error = "prerelease and build metadata require major.minor.patch";
            return std::nullopt;
        }
        auto exact = SemanticVersion::Parse(text, error);
        if (!exact) return std::nullopt;
        result.version = std::move(*exact);
    }
    return result;
}

bool CanonicalPackage(std::string_view value) {
    const std::size_t slash = value.find('/');
    if (slash == std::string_view::npos || slash == 0 ||
        slash + 1 == value.size() ||
        value.find('/', slash + 1) != std::string_view::npos)
        return false;
    const auto valid = [](std::string_view part) {
        if (part.empty() || part.front() < 'a' || part.front() > 'z')
            return false;
        return std::all_of(part.begin() + 1, part.end(), [](char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9') || character == '-' ||
                   character == '_';
        });
    };
    return valid(value.substr(0, slash)) && valid(value.substr(slash + 1));
}

}  // namespace

std::optional<SemanticVersion> SemanticVersion::Parse(const std::string& text,
                                                      std::string& error) {
    SemanticVersion version;
    std::string_view remaining(text);
    const std::size_t plus = remaining.find('+');
    if (plus != std::string_view::npos) {
        version.build =
            SplitIdentifiers(remaining.substr(plus + 1), "build", error);
        if (!error.empty()) return std::nullopt;
        remaining = remaining.substr(0, plus);
    }
    const std::size_t dash = remaining.find('-');
    if (dash != std::string_view::npos) {
        version.prerelease =
            SplitIdentifiers(remaining.substr(dash + 1), "prerelease", error);
        if (!error.empty()) return std::nullopt;
        for (const auto& identifier : version.prerelease)
            if (NumericIdentifier(identifier) && identifier.size() > 1 &&
                identifier.front() == '0') {
                error =
                    "numeric prerelease identifiers cannot have leading zeros";
                return std::nullopt;
            }
        remaining = remaining.substr(0, dash);
    }
    std::string core(remaining);
    std::size_t first = core.find('.');
    std::size_t second = first == std::string::npos ? std::string::npos
                                                    : core.find('.', first + 1);
    if (first == std::string::npos || second == std::string::npos ||
        core.find('.', second + 1) != std::string::npos ||
        !ParseNumber(std::string_view(core).substr(0, first), version.major,
                     error) ||
        !ParseNumber(
            std::string_view(core).substr(first + 1, second - first - 1),
            version.minor, error) ||
        !ParseNumber(std::string_view(core).substr(second + 1), version.patch,
                     error)) {
        if (error.empty())
            error = "semantic versions require major.minor.patch";
        return std::nullopt;
    }
    return version;
}

std::string SemanticVersion::String() const {
    std::string result = std::to_string(major) + "." + std::to_string(minor) +
                         "." + std::to_string(patch);
    if (!prerelease.empty()) {
        result += "-";
        for (std::size_t index = 0; index < prerelease.size(); ++index) {
            if (index) result += ".";
            result += prerelease[index];
        }
    }
    if (!build.empty()) {
        result += "+";
        for (std::size_t index = 0; index < build.size(); ++index) {
            if (index) result += ".";
            result += build[index];
        }
    }
    return result;
}

std::strong_ordering SemanticVersion::operator<=>(
    const SemanticVersion& other) const {
    if (const auto order = major <=> other.major; order != 0) return order;
    if (const auto order = minor <=> other.minor; order != 0) return order;
    if (const auto order = patch <=> other.patch; order != 0) return order;
    if (prerelease.empty() && other.prerelease.empty())
        return std::strong_ordering::equal;
    if (prerelease.empty()) return std::strong_ordering::greater;
    if (other.prerelease.empty()) return std::strong_ordering::less;
    for (std::size_t index = 0;
         index < prerelease.size() && index < other.prerelease.size();
         ++index) {
        const bool leftNumeric = NumericIdentifier(prerelease[index]);
        const bool rightNumeric = NumericIdentifier(other.prerelease[index]);
        if (leftNumeric && rightNumeric) {
            if (prerelease[index].size() != other.prerelease[index].size())
                return prerelease[index].size() < other.prerelease[index].size()
                           ? std::strong_ordering::less
                           : std::strong_ordering::greater;
            if (prerelease[index] != other.prerelease[index])
                return prerelease[index] < other.prerelease[index]
                           ? std::strong_ordering::less
                           : std::strong_ordering::greater;
        } else if (leftNumeric != rightNumeric)
            return leftNumeric ? std::strong_ordering::less
                               : std::strong_ordering::greater;
        else if (prerelease[index] != other.prerelease[index])
            return prerelease[index] < other.prerelease[index]
                       ? std::strong_ordering::less
                       : std::strong_ordering::greater;
    }
    return prerelease.size() <=> other.prerelease.size();
}

bool SemanticVersion::operator==(const SemanticVersion& other) const {
    return (*this <=> other) == 0;
}

std::optional<VersionRequirement> VersionRequirement::Parse(
    const std::string& text, std::string& error) {
    VersionRequirement result;
    result._text = Trim(text);
    if (result._text.empty()) {
        error = "version requirement must not be empty";
        return std::nullopt;
    }
    if (result._text == "*") {
        result._any = true;
        return result;
    }
    const auto add = [&](Operator operation, SemanticVersion version) {
        result._comparators.push_back({operation, std::move(version)});
    };
    std::size_t start = 0;
    while (start <= result._text.size()) {
        const std::size_t comma = result._text.find(',', start);
        const std::size_t end =
            comma == std::string::npos ? result._text.size() : comma;
        std::string part =
            Trim(std::string_view(result._text).substr(start, end - start));
        if (part.empty()) {
            error = "empty comparator in version requirement";
            return std::nullopt;
        }
        std::string operation;
        for (const std::string candidate :
             {">=", "<=", ">", "<", "=", "^", "~"})
            if (part.starts_with(candidate)) {
                operation = candidate;
                part = Trim(std::string_view(part).substr(candidate.size()));
                break;
            }
        auto parsed = ParsePartial(part, error);
        if (!parsed) return std::nullopt;
        const SemanticVersion lower = parsed->version;
        if (operation == ">=")
            add(Operator::GreaterEqual, lower);
        else if (operation == ">")
            add(Operator::Greater, lower);
        else if (operation == "<=")
            add(Operator::LessEqual, lower);
        else if (operation == "<")
            add(Operator::Less, lower);
        else if (operation == "=" ||
                 (operation.empty() && parsed->components == 3))
            add(Operator::Equal, lower);
        else {
            add(Operator::GreaterEqual, lower);
            SemanticVersion upper = lower;
            upper.prerelease.clear();
            upper.build.clear();
            if (operation == "~") {
                if (parsed->components == 1) {
                    ++upper.major;
                    upper.minor = upper.patch = 0;
                } else {
                    ++upper.minor;
                    upper.patch = 0;
                }
            } else if (operation == "^") {
                if (upper.major > 0 || parsed->components == 1) {
                    ++upper.major;
                    upper.minor = upper.patch = 0;
                } else if (upper.minor > 0 || parsed->components == 2) {
                    ++upper.minor;
                    upper.patch = 0;
                } else
                    ++upper.patch;
            } else if (parsed->components == 1) {
                ++upper.major;
                upper.minor = upper.patch = 0;
            } else {
                ++upper.minor;
                upper.patch = 0;
            }
            add(Operator::Less, upper);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return result;
}

bool VersionRequirement::Matches(const SemanticVersion& version) const {
    if (_any) return version.prerelease.empty();
    if (!version.prerelease.empty()) {
        const bool explicitlyIncluded =
            std::any_of(_comparators.begin(), _comparators.end(),
                        [&](const Comparator& comparator) {
                            return !comparator.version.prerelease.empty() &&
                                   comparator.version.major == version.major &&
                                   comparator.version.minor == version.minor &&
                                   comparator.version.patch == version.patch;
                        });
        if (!explicitlyIncluded) return false;
    }
    for (const auto& comparator : _comparators) {
        const auto order = version <=> comparator.version;
        if ((comparator.operation == Operator::Equal && order != 0) ||
            (comparator.operation == Operator::Less && order >= 0) ||
            (comparator.operation == Operator::LessEqual && order > 0) ||
            (comparator.operation == Operator::Greater && order <= 0) ||
            (comparator.operation == Operator::GreaterEqual && order < 0))
            return false;
    }
    return true;
}

bool ValidIdentifier(const std::string& name) {
    diagnostics::DiagnosticBag diagnostics;
    const auto tokens = core::Tokenizer(name, diagnostics).Tokenize();
    return std::regex_match(name, std::regex("[a-z][a-z0-9_]*")) &&
           name != "root" && name != "lib" && tokens.size() == 2 &&
           tokens[0].type == TokenType::Identifier;
}

bool ValidProjectName(const std::string& name) {
    return std::regex_match(name, std::regex("[A-Za-z0-9][A-Za-z0-9 _.-]*"));
}

bool ValidPackageIdentity(const std::string& identity) {
    return CanonicalPackage(identity);
}

std::string ModuleName(const std::string& projectName) {
    std::string module;
    bool separator = false;
    for (char character : projectName) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
        if ((character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') || character == '_') {
            if (separator && !module.empty() && module.back() != '_')
                module += '_';
            module += character;
            separator = false;
        } else
            separator = true;
    }
    if (module.empty()) module = "project";
    if (module[0] < 'a' || module[0] > 'z') module = "project_" + module;
    if (!ValidIdentifier(module)) module += "_project";
    return module;
}

Manifest ReadManifest(const fs::path& directory) {
    const fs::path file = directory / "longbow.toml";
    std::ifstream stream(file);
    if (!stream) throw std::runtime_error("cannot read " + file.string());
    Manifest manifest;
    manifest.directory = fs::absolute(directory).lexically_normal();
    std::string section, line;
    std::set<std::string> packageKeys, sections;
    unsigned number = 0;
    bool hasName = false, hasVersion = false, hasKind = false;
    while (std::getline(stream, line)) {
        ++number;
        line = StripComment(std::move(line), number);
        if (line.empty()) continue;
        if (line == "[package]" || line == "[dependencies]" ||
            line == "[native]") {
            if (!sections.insert(line).second)
                throw std::runtime_error("duplicate manifest section " + line);
            section = line;
            continue;
        }
        const auto [key, rawValue] = Assignment(line, number);
        if (section == "[package]") {
            if (!packageKeys.insert(key).second)
                throw std::runtime_error("duplicate package key '" + key + "'");
            const std::string value = ParseString(rawValue, number);
            if (key == "name") {
                manifest.name = value;
                hasName = true;
            } else if (key == "registry") {
                if (!CanonicalPackage(value))
                    throw std::runtime_error(
                        "package.registry must be a valid owner/package");
                manifest.registry = value;
            } else if (key == "version") {
                std::string error;
                const auto version = SemanticVersion::Parse(value, error);
                if (!version)
                    throw std::runtime_error("invalid package.version: " +
                                             error);
                manifest.version = *version;
                hasVersion = true;
            } else if (key == "kind") {
                if (value == "program")
                    manifest.kind = PackageKind::Program;
                else if (value == "library")
                    manifest.kind = PackageKind::Library;
                else
                    throw std::runtime_error(
                        "package.kind must be program or library");
                hasKind = true;
            } else if (key == "flecha") {
                std::string error;
                manifest.flecha = VersionRequirement::Parse(value, error);
                if (!manifest.flecha)
                    throw std::runtime_error("invalid package.flecha: " +
                                             error);
            } else
                throw std::runtime_error("unknown package key '" + key + "'");
            continue;
        }
        if (section == "[dependencies]") {
            if (!ValidIdentifier(key) || manifest.dependencies.contains(key))
                throw std::runtime_error("invalid or duplicate dependency '" +
                                         key + "'");
            const auto values = InlineTable(rawValue, number);
            Dependency dependency;
            dependency.alias = key;
            const auto path = values.find("path");
            const auto package = values.find("package");
            const auto version = values.find("version");
            const auto git = values.find("git");
            const auto revision = values.find("rev");
            for (const auto& [field, value] : values) {
                (void)value;
                if (field != "path" && field != "package" &&
                    field != "version" && field != "git" && field != "rev")
                    throw std::runtime_error("unknown dependency field '" +
                                             field + "' for '" + key + "'");
            }
            if (path != values.end()) {
                if (path->second.empty())
                    throw std::runtime_error(
                        "dependency path must not be empty");
                dependency.path = manifest.directory / path->second;
            }
            if (package != values.end()) dependency.package = package->second;
            if (git != values.end()) dependency.git = git->second;
            if (revision != values.end())
                dependency.revision = revision->second;
            if (version != values.end()) {
                std::string error;
                dependency.version =
                    VersionRequirement::Parse(version->second, error);
                if (!dependency.version)
                    throw std::runtime_error(
                        "invalid dependency version for '" + key +
                        "': " + error);
            }
            if (dependency.git || dependency.revision) {
                if (!dependency.git || dependency.git->empty() ||
                    !dependency.revision || dependency.revision->empty() ||
                    dependency.path || dependency.package || dependency.version)
                    throw std::runtime_error(
                        "Git dependency '" + key +
                        "' requires only nonempty git and rev fields");
                if ((dependency.revision->size() != 40 &&
                     dependency.revision->size() != 64) ||
                    !std::all_of(dependency.revision->begin(),
                                 dependency.revision->end(), [](char value) {
                                     return (value >= '0' && value <= '9') ||
                                            (value >= 'a' && value <= 'f') ||
                                            (value >= 'A' && value <= 'F');
                                 }))
                    throw std::runtime_error(
                        "Git dependency '" + key +
                        "' rev must be a full 40- or 64-digit commit hash");
                dependency.kind = DependencyKind::Git;
            } else if (dependency.package || dependency.version) {
                if (!dependency.package || !dependency.version ||
                    !CanonicalPackage(*dependency.package))
                    throw std::runtime_error(
                        "registry dependency '" + key +
                        "' requires a valid owner/package and version");
                if (*dependency.package == "flecha/std")
                    throw std::runtime_error(
                        "the bundled standard library cannot be a registry "
                        "dependency");
                dependency.kind = DependencyKind::Registry;
            } else if (dependency.path) {
                dependency.kind = DependencyKind::Path;
            } else
                throw std::runtime_error("dependency '" + key +
                                         "' has no source");
            if (key == "std" && dependency.kind != DependencyKind::Path)
                throw std::runtime_error(
                    "std may only be a local toolchain-development override");
            manifest.dependencies.emplace(key, std::move(dependency));
            continue;
        }
        if (section == "[native]") {
            if (!ValidIdentifier(key) || manifest.native.contains(key))
                throw std::runtime_error("invalid or duplicate native input '" +
                                         key + "'");
            const auto values = InlineTable(rawValue, number);
            if (values.size() != 1 || !values.contains("path") ||
                values.at("path").empty())
                throw std::runtime_error("native input '" + key +
                                         "' requires only a path field");
            manifest.native.emplace(key,
                                    manifest.directory / values.at("path"));
            continue;
        }
        throw std::runtime_error(
            "manifest key outside a supported section at line " +
            std::to_string(number));
    }
    if (!hasName || !ValidProjectName(manifest.name))
        throw std::runtime_error(
            "package.name must start with a letter or digit and contain only "
            "letters, digits, spaces, hyphens, underscores, or dots");
    if (!hasVersion) throw std::runtime_error("package.version is required");
    if (!hasKind) throw std::runtime_error("package.kind is required");
    return manifest;
}

}  // namespace flecha::longbow
