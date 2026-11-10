#include "lsp/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "analyzer/json.hpp"
#include "lsp/uri.hpp"

namespace flecha::lsp {
namespace {

using JsonObject = analyzer::json::Value::Object;

constexpr int PARSE_ERROR = -32700;
constexpr int INVALID_REQUEST = -32600;
constexpr int METHOD_NOT_FOUND = -32601;
constexpr int INVALID_PARAMS = -32602;
constexpr int INTERNAL_ERROR = -32603;
constexpr int SERVER_NOT_INITIALIZED = -32002;

struct Message final {
    std::optional<std::string> id;
    std::string method;
    const JsonObject* params = nullptr;
};

std::string ErrorResponse(std::string_view id, int code,
                          std::string_view message) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) +
           ",\"error\":{\"code\":" + std::to_string(code) +
           ",\"message\":" + analyzer::json::Quote(message) + "}}";
}

std::string SuccessResponse(std::string_view id, std::string_view result) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) +
           ",\"result\":" + std::string(result) + '}';
}

std::optional<std::string> SerializeId(const analyzer::json::Value& value) {
    if (const std::string* number = analyzer::json::AsNumber(value))
        return *number;
    if (const std::string* text = analyzer::json::AsString(value))
        return analyzer::json::Quote(*text);
    if (std::holds_alternative<std::nullptr_t>(value.value)) return "null";
    return std::nullopt;
}

std::optional<Message> ParseMessage(const analyzer::json::Value& document,
                                    std::string& error) {
    const JsonObject* object = analyzer::json::AsObject(document);
    if (!object) {
        error = "JSON-RPC message must be an object";
        return std::nullopt;
    }
    const analyzer::json::Value* version =
        analyzer::json::Field(*object, "jsonrpc");
    const analyzer::json::Value* methodValue =
        analyzer::json::Field(*object, "method");
    const std::string* versionText =
        version ? analyzer::json::AsString(*version) : nullptr;
    const std::string* method =
        methodValue ? analyzer::json::AsString(*methodValue) : nullptr;
    if (!versionText || *versionText != "2.0" || !method) {
        error = "message requires jsonrpc '2.0' and a method";
        return std::nullopt;
    }

    std::optional<std::string> id;
    if (const analyzer::json::Value* idValue =
            analyzer::json::Field(*object, "id")) {
        id = SerializeId(*idValue);
        if (!id) {
            error = "request id must be a number, string, or null";
            return std::nullopt;
        }
    }
    const analyzer::json::Value* paramsValue =
        analyzer::json::Field(*object, "params");
    const JsonObject* params =
        paramsValue ? analyzer::json::AsObject(*paramsValue) : nullptr;
    if (paramsValue && !params) {
        error = "params must be an object";
        return std::nullopt;
    }
    return Message{std::move(id), *method, params};
}

const JsonObject* ObjectField(const JsonObject& object, std::string_view name) {
    const analyzer::json::Value* value = analyzer::json::Field(object, name);
    return value ? analyzer::json::AsObject(*value) : nullptr;
}

bool StringField(const JsonObject& object, std::string_view name,
                 std::string& result) {
    const analyzer::json::Value* value = analyzer::json::Field(object, name);
    const std::string* text =
        value ? analyzer::json::AsString(*value) : nullptr;
    if (!text) return false;
    result = *text;
    return true;
}

bool IntegerField(const JsonObject& object, std::string_view name,
                  std::int64_t& result) {
    const analyzer::json::Value* value = analyzer::json::Field(object, name);
    const std::string* number =
        value ? analyzer::json::AsNumber(*value) : nullptr;
    if (!number) return false;
    const auto [end, error] = std::from_chars(
        number->data(), number->data() + number->size(), result);
    return error == std::errc{} && end == number->data() + number->size();
}

bool PositionField(const JsonObject& object, Position& result) {
    const JsonObject* position = ObjectField(object, "position");
    std::int64_t line = -1;
    std::int64_t character = -1;
    if (!position || !IntegerField(*position, "line", line) ||
        !IntegerField(*position, "character", character) || line < 0 ||
        character < 0)
        return false;
    result = {static_cast<std::size_t>(line),
              static_cast<std::size_t>(character)};
    return true;
}

std::optional<std::filesystem::path> UriPath(std::string_view uri,
                                             std::string& error) {
    return FileUriToPath(uri, error);
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

int DiagnosticSeverity(std::string_view severity) {
    if (severity == "error") return 1;
    if (severity == "warning") return 2;
    return 3;
}

int CompletionKind(std::string_view kind) {
    if (kind == "function") return 3;
    if (kind == "field") return 5;
    if (kind == "parameter" || kind == "localVariable" ||
        kind == "moduleConstant")
        return 6;
    if (kind == "structureType") return 7;
    if (kind == "enumType") return 13;
    return 25;
}

int SemanticTokenKind(std::string_view kind) {
    if (kind == "function") return 0;
    if (kind == "parameter") return 1;
    if (kind == "field") return 3;
    if (kind == "enumType") return 4;
    if (kind == "structureType") return 5;
    if (kind == "errorType" || kind == "callableType") return 6;
    return 2;
}

bool IsIdentifier(std::string_view name) {
    if (name.empty()) return false;
    const auto start = static_cast<unsigned char>(name.front());
    if (std::isalpha(start) == 0 && name.front() != '_') return false;
    for (const unsigned char character : name.substr(1))
        if (std::isalnum(character) == 0 && character != '_') return false;
    return true;
}

std::string PublishNotification(std::string_view uri,
                                std::optional<std::int64_t> version,
                                std::string_view diagnostics) {
    std::string params = "{\"uri\":" + analyzer::json::Quote(uri);
    if (version) params += ",\"version\":" + std::to_string(*version);
    params += ",\"diagnostics\":" + std::string(diagnostics) + '}';
    return "{\"jsonrpc\":\"2.0\",\"method\":"
           "\"textDocument/publishDiagnostics\",\"params\":" +
           params + '}';
}

std::optional<std::filesystem::path> WorkspaceFromInitialize(
    const JsonObject& params, std::string& error) {
    if (const analyzer::json::Value* foldersValue =
            analyzer::json::Field(params, "workspaceFolders")) {
        if (const analyzer::json::Value::Array* folders =
                analyzer::json::AsArray(*foldersValue);
            folders && !folders->empty()) {
            const JsonObject* first =
                analyzer::json::AsObject(folders->front());
            std::string uri;
            if (first && StringField(*first, "uri", uri))
                return UriPath(uri, error);
        }
    }
    if (const analyzer::json::Value* rootUri =
            analyzer::json::Field(params, "rootUri")) {
        if (const std::string* uri = analyzer::json::AsString(*rootUri))
            return UriPath(*uri, error);
    }
    std::string rootPath;
    if (StringField(params, "rootPath", rootPath)) {
        std::filesystem::path path(rootPath);
        if (!path.is_absolute()) {
            error = "rootPath must be absolute";
            return std::nullopt;
        }
        return path.lexically_normal();
    }
    std::error_code currentError;
    std::filesystem::path current = std::filesystem::current_path(currentError);
    if (currentError) {
        error = "cannot determine workspace root";
        return std::nullopt;
    }
    return current;
}

std::optional<std::filesystem::path> PackageGraphFromInitialize(
    const JsonObject& params, const std::filesystem::path& workspace,
    std::string& error) {
    if (const JsonObject* options =
            ObjectField(params, "initializationOptions")) {
        std::string configured;
        if (StringField(*options, "packageGraph", configured)) {
            if (configured.starts_with("file://"))
                return UriPath(configured, error);
            std::filesystem::path path(configured);
            if (!path.is_absolute()) path = workspace / path;
            return path.lexically_normal();
        }
    }
    const std::filesystem::path detected =
        workspace / "target" / "package-graph.json";
    std::error_code existsError;
    if (std::filesystem::is_regular_file(detected, existsError))
        return detected;
    return std::nullopt;
}

}  // namespace

std::filesystem::path ProtocolServer::Normalize(
    const std::filesystem::path& path) const {
    const std::filesystem::path absolute =
        path.is_absolute() ? path : _workspace / path;
    std::error_code error;
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(absolute, error);
    return error ? absolute.lexically_normal() : canonical;
}

std::vector<std::string> ProtocolServer::PublishDiagnostics(
    const std::filesystem::path& requestedEntry) {
    const std::filesystem::path entry = Normalize(requestedEntry);
    const AnalyzerSnapshot snapshot = _analyzer.Diagnostics(entry);
    std::map<std::filesystem::path, std::vector<const AnalyzerDiagnostic*>>
        groups;
    groups[entry];
    for (const AnalyzerDiagnostic& diagnostic : snapshot.diagnostics) {
        const std::filesystem::path file =
            diagnostic.file.empty() ? entry : Normalize(diagnostic.file);
        groups[file].push_back(&diagnostic);
    }

    std::vector<std::string> notifications;
    std::set<std::string> currentUris;
    for (const auto& [path, diagnostics] : groups) {
        const std::string uri = PathToFileUri(path);
        currentUris.insert(uri);
        const auto document = _documents.find(path);
        const std::string source = document == _documents.end()
                                       ? ReadFile(path)
                                       : document->second.text;
        std::ostringstream serialized;
        serialized << '[';
        bool first = true;
        for (const AnalyzerDiagnostic* diagnostic : diagnostics) {
            if (!first) serialized << ',';
            first = false;
            const Position start =
                ByteOffsetToPosition(source, diagnostic->startByte);
            const Position end = ByteOffsetToPosition(
                source, std::max(diagnostic->startByte, diagnostic->endByte));
            serialized << "{\"range\":{\"start\":{\"line\":" << start.line
                       << ",\"character\":" << start.character
                       << "},\"end\":{\"line\":" << end.line
                       << ",\"character\":" << end.character
                       << "}},\"severity\":"
                       << DiagnosticSeverity(diagnostic->severity)
                       << ",\"code\":"
                       << analyzer::json::Quote(diagnostic->code)
                       << ",\"source\":\"flecha\",\"message\":"
                       << analyzer::json::Quote(diagnostic->message) << '}';
        }
        serialized << ']';
        const std::optional<std::int64_t> version =
            document == _documents.end()
                ? std::nullopt
                : std::optional<std::int64_t>(document->second.version);
        notifications.push_back(
            PublishNotification(uri, version, serialized.str()));
    }

    for (const std::string& oldUri : _published[entry]) {
        if (!currentUris.contains(oldUri))
            notifications.push_back(
                PublishNotification(oldUri, std::nullopt, "[]"));
    }
    _published[entry] = std::move(currentUris);
    return notifications;
}

std::vector<std::string> ProtocolServer::Handle(std::string_view messageText) {
    std::string parseError;
    std::optional<analyzer::json::Value> document =
        analyzer::json::Parse(messageText, parseError);
    if (!document) return {ErrorResponse("null", PARSE_ERROR, parseError)};

    std::string requestError;
    std::optional<Message> message = ParseMessage(*document, requestError);
    if (!message) return {ErrorResponse("null", INVALID_REQUEST, requestError)};

    const auto Error = [&](int code, std::string_view text) {
        return message->id ? std::vector<std::string>{ErrorResponse(
                                 *message->id, code, text)}
                           : std::vector<std::string>{};
    };
    try {
        if (message->method == "exit") {
            if (_state == ProtocolState::Initialized) {
                try {
                    _analyzer.Shutdown();
                } catch (...) {
                }
            }
            _exitCode = _state == ProtocolState::Shutdown ? 0 : 1;
            _state = ProtocolState::Exited;
            return {};
        }

        if (message->method == "initialize") {
            if (!message->id) return {};
            if (_state != ProtocolState::Starting)
                return Error(INVALID_REQUEST, "server is already initialized");
            if (!message->params)
                return Error(INVALID_PARAMS, "initialize params are required");
            std::string error;
            std::optional<std::filesystem::path> workspace =
                WorkspaceFromInitialize(*message->params, error);
            if (!workspace)
                return Error(INVALID_PARAMS,
                             error.empty() ? "invalid workspace root" : error);
            _workspace = Normalize(*workspace);
            std::optional<std::filesystem::path> packageGraph =
                PackageGraphFromInitialize(*message->params, _workspace, error);
            if (!error.empty()) return Error(INVALID_PARAMS, error);
            const std::string compilerVersion =
                _analyzer.Initialize(_workspace, packageGraph);
            _state = ProtocolState::Initialized;
            const std::string result =
                "{\"capabilities\":{\"positionEncoding\":\"utf-16\","
                "\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
                "\"hoverProvider\":true,\"definitionProvider\":true,"
                "\"referencesProvider\":true,"
                "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\"]},"
                "\"renameProvider\":true,"
                "\"semanticTokensProvider\":{\"legend\":{"
                "\"tokenTypes\":[\"function\",\"parameter\",\"variable\","
                "\"property\",\"enum\",\"struct\",\"type\"],"
                "\"tokenModifiers\":[\"declaration\"]},\"full\":true}},"
                "\"serverInfo\":{\"name\":\"flecha-lsp\",\"version\":" +
                analyzer::json::Quote(compilerVersion) + "}}";
            return {SuccessResponse(*message->id, result)};
        }

        if (_state == ProtocolState::Starting)
            return Error(SERVER_NOT_INITIALIZED,
                         "language server is not initialized");
        if (_state == ProtocolState::Shutdown ||
            _state == ProtocolState::Exited)
            return Error(INVALID_REQUEST, "language server has shut down");

        if (message->method == "initialized" ||
            message->method == "$/setTrace" ||
            message->method == "$/cancelRequest")
            return {};

        if (message->method == "shutdown") {
            if (!message->id) return {};
            _analyzer.Shutdown();
            _state = ProtocolState::Shutdown;
            return {SuccessResponse(*message->id, "null")};
        }

        if (message->method == "textDocument/didOpen") {
            if (!message->params) return {};
            const JsonObject* textDocument =
                ObjectField(*message->params, "textDocument");
            std::string uri;
            std::string text;
            std::int64_t version = -1;
            if (!textDocument || !StringField(*textDocument, "uri", uri) ||
                !StringField(*textDocument, "text", text) ||
                !IntegerField(*textDocument, "version", version) || version < 0)
                return {};
            std::string error;
            std::optional<std::filesystem::path> path = UriPath(uri, error);
            if (!path) return {};
            const std::filesystem::path normalized = Normalize(*path);
            _analyzer.Open(normalized, text, version);
            _documents[normalized] = {uri, std::move(text), version};
            return PublishDiagnostics(normalized);
        }

        if (message->method == "textDocument/didChange") {
            if (!message->params) return {};
            const JsonObject* textDocument =
                ObjectField(*message->params, "textDocument");
            std::string uri;
            std::int64_t version = -1;
            const analyzer::json::Value* changesValue =
                analyzer::json::Field(*message->params, "contentChanges");
            const analyzer::json::Value::Array* changes =
                changesValue ? analyzer::json::AsArray(*changesValue) : nullptr;
            if (!textDocument || !StringField(*textDocument, "uri", uri) ||
                !IntegerField(*textDocument, "version", version) ||
                version < 0 || !changes || changes->empty())
                return {};
            const JsonObject* last = analyzer::json::AsObject(changes->back());
            std::string text;
            if (!last || !StringField(*last, "text", text) ||
                analyzer::json::Field(*last, "range"))
                return {};
            std::string error;
            std::optional<std::filesystem::path> path = UriPath(uri, error);
            if (!path) return {};
            const std::filesystem::path normalized = Normalize(*path);
            if (!_documents.contains(normalized)) return {};
            _analyzer.Change(normalized, text, version);
            _documents[normalized] = {uri, std::move(text), version};
            return PublishDiagnostics(normalized);
        }

        if (message->method == "textDocument/didClose") {
            if (!message->params) return {};
            const JsonObject* textDocument =
                ObjectField(*message->params, "textDocument");
            std::string uri;
            if (!textDocument || !StringField(*textDocument, "uri", uri))
                return {};
            std::string error;
            std::optional<std::filesystem::path> path = UriPath(uri, error);
            if (!path) return {};
            const std::filesystem::path normalized = Normalize(*path);
            _analyzer.Close(normalized);
            _documents.erase(normalized);
            std::vector<std::string> notifications;
            std::set<std::string> uris = std::move(_published[normalized]);
            uris.insert(uri);
            _published.erase(normalized);
            for (const std::string& publishedUri : uris)
                notifications.push_back(
                    PublishNotification(publishedUri, std::nullopt, "[]"));
            return notifications;
        }

        const bool semanticRequest =
            message->method == "textDocument/hover" ||
            message->method == "textDocument/definition" ||
            message->method == "textDocument/references" ||
            message->method == "textDocument/completion" ||
            message->method == "textDocument/rename" ||
            message->method == "textDocument/semanticTokens/full";
        if (semanticRequest) {
            if (!message->id || !message->params)
                return Error(INVALID_PARAMS, "request params are required");
            const JsonObject* textDocument =
                ObjectField(*message->params, "textDocument");
            std::string uri;
            if (!textDocument || !StringField(*textDocument, "uri", uri))
                return Error(INVALID_PARAMS, "textDocument.uri is required");
            std::string uriError;
            std::optional<std::filesystem::path> parsedPath =
                UriPath(uri, uriError);
            if (!parsedPath) return Error(INVALID_PARAMS, uriError);
            const std::filesystem::path path = Normalize(*parsedPath);
            const auto sourceFor = [&](const std::filesystem::path& input) {
                const std::filesystem::path normalized = Normalize(input);
                const auto found = _documents.find(normalized);
                return found == _documents.end() ? ReadFile(normalized)
                                                 : found->second.text;
            };
            const auto rangeJson = [&](const AnalyzerRange& range) {
                const std::filesystem::path file = Normalize(range.file);
                const std::string source = sourceFor(file);
                const Position start =
                    ByteOffsetToPosition(source, range.startByte);
                const Position end = ByteOffsetToPosition(
                    source, std::max(range.startByte, range.endByte));
                return "{\"start\":{\"line\":" + std::to_string(start.line) +
                       ",\"character\":" + std::to_string(start.character) +
                       "},\"end\":{\"line\":" + std::to_string(end.line) +
                       ",\"character\":" + std::to_string(end.character) + "}}";
            };
            const auto locationJson = [&](const AnalyzerRange& range) {
                return "{\"uri\":" +
                       analyzer::json::Quote(
                           PathToFileUri(Normalize(range.file))) +
                       ",\"range\":" + rangeJson(range) + '}';
            };

            if (message->method == "textDocument/semanticTokens/full") {
                const std::string source = sourceFor(path);
                const std::vector<AnalyzerSemanticToken> tokens =
                    _analyzer.SemanticTokens(path);
                std::ostringstream data;
                data << "{\"data\":[";
                bool first = true;
                std::size_t previousLine = 0;
                std::size_t previousCharacter = 0;
                for (const AnalyzerSemanticToken& token : tokens) {
                    if (Normalize(token.range.file) != path) continue;
                    const Position start =
                        ByteOffsetToPosition(source, token.range.startByte);
                    const Position end =
                        ByteOffsetToPosition(source, token.range.endByte);
                    if (start.line != end.line ||
                        end.character <= start.character)
                        continue;
                    const std::size_t deltaLine = start.line - previousLine;
                    const std::size_t deltaStart =
                        deltaLine == 0 ? start.character - previousCharacter
                                       : start.character;
                    if (!first) data << ',';
                    first = false;
                    data << deltaLine << ',' << deltaStart << ','
                         << end.character - start.character << ','
                         << SemanticTokenKind(token.kind) << ','
                         << (token.isDeclaration ? 1 : 0);
                    previousLine = start.line;
                    previousCharacter = start.character;
                }
                data << "]}";
                return {SuccessResponse(*message->id, data.str())};
            }

            Position position;
            if (!PositionField(*message->params, position))
                return Error(INVALID_PARAMS, "position is required");
            const std::string source = sourceFor(path);
            const std::optional<std::size_t> offset =
                PositionToByteOffset(source, position);
            if (!offset)
                return Error(INVALID_PARAMS,
                             "position is outside the document");

            if (message->method == "textDocument/hover") {
                const std::optional<AnalyzerHover> hover =
                    _analyzer.Hover(path, *offset);
                if (!hover) return {SuccessResponse(*message->id, "null")};
                std::string display = hover->name.empty()
                                          ? hover->type
                                          : hover->name + ": " + hover->type;
                const std::string result =
                    "{\"contents\":{\"kind\":\"plaintext\",\"value\":" +
                    analyzer::json::Quote(display) +
                    "},\"range\":" + rangeJson(hover->range) + '}';
                return {SuccessResponse(*message->id, result)};
            }

            if (message->method == "textDocument/definition") {
                const std::optional<AnalyzerRange> definition =
                    _analyzer.Definition(path, *offset);
                return {SuccessResponse(
                    *message->id,
                    definition ? locationJson(*definition) : "null")};
            }

            if (message->method == "textDocument/references") {
                bool includeDeclaration = true;
                if (const JsonObject* context =
                        ObjectField(*message->params, "context")) {
                    if (const analyzer::json::Value* include =
                            analyzer::json::Field(*context,
                                                  "includeDeclaration")) {
                        const bool* value = std::get_if<bool>(&include->value);
                        if (!value)
                            return Error(INVALID_PARAMS,
                                         "includeDeclaration must be boolean");
                        includeDeclaration = *value;
                    }
                }
                const auto references =
                    _analyzer.References(path, *offset, includeDeclaration);
                if (!references) return {SuccessResponse(*message->id, "null")};
                std::string result = "[";
                for (std::size_t index = 0; index < references->size();
                     ++index) {
                    if (index != 0) result += ',';
                    result += locationJson((*references)[index]);
                }
                result += ']';
                return {SuccessResponse(*message->id, result)};
            }

            if (message->method == "textDocument/completion") {
                std::size_t begin = *offset;
                while (begin > 0) {
                    const unsigned char character = source[begin - 1];
                    if (std::isalnum(character) == 0 && character != '_') break;
                    --begin;
                }
                const std::vector<AnalyzerCompletion> completions =
                    _analyzer.Completion(path, std::string_view(source).substr(
                                                   begin, *offset - begin));
                std::string items = "{\"isIncomplete\":false,\"items\":[";
                for (std::size_t index = 0; index < completions.size();
                     ++index) {
                    if (index != 0) items += ',';
                    const AnalyzerCompletion& item = completions[index];
                    items += "{\"label\":" + analyzer::json::Quote(item.name) +
                             ",\"kind\":" +
                             std::to_string(CompletionKind(item.kind)) +
                             ",\"detail\":" + analyzer::json::Quote(item.type) +
                             '}';
                }
                items += "]}";
                return {SuccessResponse(*message->id, items)};
            }

            std::string newName;
            if (!StringField(*message->params, "newName", newName) ||
                !IsIdentifier(newName))
                return Error(INVALID_PARAMS,
                             "newName must be a valid Flecha identifier");
            const std::optional<AnalyzerRename> rename =
                _analyzer.Rename(path, *offset);
            if (!rename) return {SuccessResponse(*message->id, "null")};
            std::map<std::string, std::vector<const AnalyzerRange*>> changes;
            for (const AnalyzerRange& range : rename->ranges)
                changes[PathToFileUri(Normalize(range.file))].push_back(&range);
            std::string edit = "{\"changes\":{";
            bool firstUri = true;
            for (const auto& [changedUri, ranges] : changes) {
                if (!firstUri) edit += ',';
                firstUri = false;
                edit += analyzer::json::Quote(changedUri) + ":[";
                for (std::size_t index = 0; index < ranges.size(); ++index) {
                    if (index != 0) edit += ',';
                    edit += "{\"range\":" + rangeJson(*ranges[index]) +
                            ",\"newText\":" + analyzer::json::Quote(newName) +
                            '}';
                }
                edit += ']';
            }
            edit += "}}";
            return {SuccessResponse(*message->id, edit)};
        }

        return Error(METHOD_NOT_FOUND, "method is not supported");
    } catch (const std::exception& error) {
        return Error(INTERNAL_ERROR, error.what());
    }
}

}  // namespace flecha::lsp
