#include "lsp/analyzer_client.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "analyzer/json.hpp"
#include "runtime/runtime_abi.h"

namespace flecha::lsp {
namespace {

using JsonObject = analyzer::json::Value::Object;

constexpr std::size_t MAX_HEADER_BYTES = 8 * 1024;
constexpr std::size_t MAX_MESSAGE_BYTES = 64 * 1024 * 1024;

class AnalyzerTransportError final : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

const JsonObject& ResponseResult(const analyzer::json::Value& response) {
    const JsonObject* envelope = analyzer::json::AsObject(response);
    if (!envelope) throw std::runtime_error("analyzer returned a non-object");
    if (const analyzer::json::Value* errorValue =
            analyzer::json::Field(*envelope, "error")) {
        const JsonObject* error = analyzer::json::AsObject(*errorValue);
        const analyzer::json::Value* messageValue =
            error ? analyzer::json::Field(*error, "message") : nullptr;
        const std::string* message =
            messageValue ? analyzer::json::AsString(*messageValue) : nullptr;
        throw std::runtime_error(message ? "analyzer: " + *message
                                         : "analyzer request failed");
    }
    const analyzer::json::Value* result =
        analyzer::json::Field(*envelope, "result");
    const JsonObject* object =
        result ? analyzer::json::AsObject(*result) : nullptr;
    if (!object) throw std::runtime_error("analyzer response has no result");
    return *object;
}

analyzer::json::Value ParseResponse(const std::string& response) {
    std::string error;
    std::optional<analyzer::json::Value> document =
        analyzer::json::Parse(response, error);
    if (!document)
        throw std::runtime_error("invalid analyzer response: " + error);
    return std::move(*document);
}

const std::string& RequiredString(const JsonObject& object,
                                  std::string_view name) {
    const analyzer::json::Value* value = analyzer::json::Field(object, name);
    const std::string* text =
        value ? analyzer::json::AsString(*value) : nullptr;
    if (!text)
        throw std::runtime_error("analyzer response requires string field '" +
                                 std::string(name) + "'");
    return *text;
}

std::int64_t RequiredInteger(const JsonObject& object, std::string_view name) {
    const analyzer::json::Value* value = analyzer::json::Field(object, name);
    const std::string* number =
        value ? analyzer::json::AsNumber(*value) : nullptr;
    std::int64_t parsed = 0;
    if (!number)
        throw std::runtime_error("analyzer response requires integer field '" +
                                 std::string(name) + "'");
    const auto [end, error] = std::from_chars(
        number->data(), number->data() + number->size(), parsed);
    if (error != std::errc{} || end != number->data() + number->size())
        throw std::runtime_error("invalid analyzer integer field '" +
                                 std::string(name) + "'");
    return parsed;
}

std::size_t RequiredOffset(const JsonObject& object, std::string_view name) {
    const std::int64_t value = RequiredInteger(object, name);
    if (value < 0) throw std::runtime_error("negative analyzer source offset");
    return static_cast<std::size_t>(value);
}

AnalyzerRange RequiredRange(const analyzer::json::Value& value) {
    const JsonObject* object = analyzer::json::AsObject(value);
    if (!object) throw std::runtime_error("invalid analyzer source range");
    return {RequiredString(*object, "file"),
            RequiredOffset(*object, "startByte"),
            RequiredOffset(*object, "endByte")};
}

const analyzer::json::Value::Array& RequiredArray(const JsonObject& object,
                                                  std::string_view name) {
    const analyzer::json::Value* value = analyzer::json::Field(object, name);
    const auto* array = value ? analyzer::json::AsArray(*value) : nullptr;
    if (!array)
        throw std::runtime_error("analyzer response requires array field '" +
                                 std::string(name) + "'");
    return *array;
}

bool RequiredBool(const JsonObject& object, std::string_view name) {
    const analyzer::json::Value* value = analyzer::json::Field(object, name);
    const bool* boolean = value ? std::get_if<bool>(&value->value) : nullptr;
    if (!boolean)
        throw std::runtime_error("analyzer response requires boolean field '" +
                                 std::string(name) + "'");
    return *boolean;
}

std::string QueryParams(const std::filesystem::path& path,
                        std::optional<std::size_t> offset = std::nullopt) {
    std::string params =
        "{\"path\":" + analyzer::json::Quote(path.generic_string());
    if (offset) params += ",\"offset\":" + std::to_string(*offset);
    params += '}';
    return params;
}

void RequireRuntimeSuccess(int32_t status, std::string_view operation) {
    if (status != FLECHA_RUNTIME_PROCESS_OK)
        throw AnalyzerTransportError(std::string(operation) +
                                     " failed with runtime status " +
                                     std::to_string(status));
}

std::string InitializeParams(
    const std::filesystem::path& workspace,
    const std::optional<std::filesystem::path>& packageGraph) {
    std::string params =
        "{\"protocolMajor\":1,\"protocolMinor\":0,\"workspace\":" +
        analyzer::json::Quote(workspace.generic_string());
    if (packageGraph)
        params += ",\"packageGraph\":" +
                  analyzer::json::Quote(packageGraph->generic_string());
    params += '}';
    return params;
}

std::string DocumentParams(const std::filesystem::path& path,
                           std::string_view text, std::int64_t version) {
    return "{\"path\":" + analyzer::json::Quote(path.generic_string()) +
           ",\"version\":" + std::to_string(version) +
           ",\"text\":" + analyzer::json::Quote(text) + '}';
}

}  // namespace

SubprocessAnalyzerClient::SubprocessAnalyzerClient(
    std::filesystem::path compiler)
    : _compiler(std::move(compiler)) {}

SubprocessAnalyzerClient::~SubprocessAnalyzerClient() {
    if (_process && !_shutdown) {
        try {
            Shutdown();
        } catch (...) {
        }
    }
    CloseHandles();
}

void SubprocessAnalyzerClient::Start() {
    if (_process) return;
    if (_shutdown)
        throw std::runtime_error("Flecha analyzer client is shut down");
    const std::string executable = _compiler.string();
    const std::string arguments = "analyze--stdio";
    const std::size_t offsets[] = {0, 7, 14};
    void* errorPipe = nullptr;
    const int32_t status = flecha_runtime_v1_process_spawn(
        &_process, &_input, &_output, &errorPipe, executable.data(),
        executable.size(),
        reinterpret_cast<const std::uint8_t*>(arguments.data()),
        arguments.size(), offsets, 2, FLECHA_RUNTIME_PROCESS_STDIO_PIPED,
        FLECHA_RUNTIME_PROCESS_STDIO_PIPED,
        FLECHA_RUNTIME_PROCESS_STDIO_INHERIT);
    RequireRuntimeSuccess(status, "starting Flecha analyzer");
}

void SubprocessAnalyzerClient::WriteFrame(std::string_view message) {
    const std::string frame =
        "Content-Length: " + std::to_string(message.size()) + "\r\n\r\n" +
        std::string(message);
    std::size_t offset = 0;
    while (offset < frame.size()) {
        std::size_t written = 0;
        const int32_t status = flecha_runtime_v1_process_pipe_write(
            _input,
            reinterpret_cast<const std::uint8_t*>(frame.data() + offset),
            frame.size() - offset, &written);
        RequireRuntimeSuccess(status, "writing to Flecha analyzer");
        if (written == 0)
            throw std::runtime_error("Flecha analyzer closed its input");
        offset += written;
    }
}

std::string SubprocessAnalyzerClient::ReadFrame() {
    while (true) {
        const std::size_t separator = _readBuffer.find("\r\n\r\n");
        if (separator != std::string::npos) {
            if (separator > MAX_HEADER_BYTES)
                throw AnalyzerTransportError(
                    "analyzer response header is too large");
            const std::string_view headers(_readBuffer.data(), separator);
            std::optional<std::size_t> contentLength;
            std::size_t begin = 0;
            while (begin <= headers.size()) {
                const std::size_t end = headers.find("\r\n", begin);
                const std::string_view line =
                    headers.substr(begin, end == std::string_view::npos
                                              ? headers.size() - begin
                                              : end - begin);
                constexpr std::string_view PREFIX = "Content-Length:";
                if (line.starts_with(PREFIX)) {
                    std::string_view value = line.substr(PREFIX.size());
                    while (!value.empty() &&
                           (value.front() == ' ' || value.front() == '\t'))
                        value.remove_prefix(1);
                    std::size_t parsed = 0;
                    const auto [last, error] = std::from_chars(
                        value.data(), value.data() + value.size(), parsed);
                    if (value.empty() || error != std::errc{} ||
                        last != value.data() + value.size())
                        throw AnalyzerTransportError(
                            "invalid analyzer Content-Length header");
                    contentLength = parsed;
                }
                if (end == std::string_view::npos) break;
                begin = end + 2;
            }
            if (!contentLength)
                throw AnalyzerTransportError(
                    "analyzer response has no Content-Length header");
            if (*contentLength > MAX_MESSAGE_BYTES)
                throw AnalyzerTransportError("analyzer response is too large");
            const std::size_t body = separator + 4;
            if (_readBuffer.size() >= body + *contentLength) {
                std::string message = _readBuffer.substr(body, *contentLength);
                _readBuffer.erase(0, body + *contentLength);
                return message;
            }
        } else if (_readBuffer.size() > MAX_HEADER_BYTES) {
            throw AnalyzerTransportError(
                "analyzer response header is too large");
        }

        std::uint8_t bytes[4096];
        std::size_t count = 0;
        const int32_t status = flecha_runtime_v1_process_pipe_read(
            _output, bytes, sizeof(bytes), &count);
        RequireRuntimeSuccess(status, "reading from Flecha analyzer");
        if (count == 0)
            throw AnalyzerTransportError("Flecha analyzer exited unexpectedly");
        _readBuffer.append(reinterpret_cast<const char*>(bytes), count);
    }
}

std::string SubprocessAnalyzerClient::Request(std::string_view method,
                                              std::string_view params) {
    try {
        return RequestOnce(method, params);
    } catch (const AnalyzerTransportError&) {
        Recover();
    }
    try {
        return RequestOnce(method, params);
    } catch (const AnalyzerTransportError&) {
        CloseHandles();
        _readBuffer.clear();
        throw;
    }
}

std::string SubprocessAnalyzerClient::RequestOnce(std::string_view method,
                                                  std::string_view params) {
    Start();
    const std::int64_t id = _nextRequestId++;
    const std::string request =
        "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
        ",\"method\":" + analyzer::json::Quote(method) +
        ",\"params\":" + std::string(params) + "}";
    WriteFrame(request);
    return ReadFrame();
}

void SubprocessAnalyzerClient::ApplyDocumentMutation(std::string_view method,
                                                     std::string_view params) {
    try {
        analyzer::json::Value response =
            ParseResponse(RequestOnce(method, params));
        (void)ResponseResult(response);
    } catch (const AnalyzerTransportError&) {
        // The desired document state is recorded before this call.  A fresh
        // analyzer receives that state through document/open replay, so
        // resending the mutation would only create a stale-version error.
        Recover();
    }
}

void SubprocessAnalyzerClient::InitializeProcess() {
    if (!_workspace)
        throw std::runtime_error("Flecha analyzer was not initialized");
    analyzer::json::Value response = ParseResponse(RequestOnce(
        "initialize", InitializeParams(*_workspace, _packageGraph)));
    (void)ResponseResult(response);
}

void SubprocessAnalyzerClient::ReplayOpenDocuments() {
    for (const auto& [path, document] : _openDocuments) {
        analyzer::json::Value response = ParseResponse(
            RequestOnce("document/open",
                        DocumentParams(path, document.text, document.version)));
        (void)ResponseResult(response);
    }
}

void SubprocessAnalyzerClient::Recover() {
    if (!_initialized)
        throw std::runtime_error("Flecha analyzer was not initialized");
    CloseHandles();
    _readBuffer.clear();
    try {
        InitializeProcess();
        ReplayOpenDocuments();
    } catch (...) {
        CloseHandles();
        _readBuffer.clear();
        throw;
    }
}

std::string SubprocessAnalyzerClient::Initialize(
    const std::filesystem::path& workspace,
    const std::optional<std::filesystem::path>& packageGraph) {
    _workspace = workspace;
    _packageGraph = packageGraph;
    analyzer::json::Value response;
    try {
        response = ParseResponse(RequestOnce(
            "initialize", InitializeParams(workspace, packageGraph)));
    } catch (const AnalyzerTransportError&) {
        CloseHandles();
        _readBuffer.clear();
        try {
            response = ParseResponse(RequestOnce(
                "initialize", InitializeParams(workspace, packageGraph)));
        } catch (const AnalyzerTransportError&) {
            CloseHandles();
            _readBuffer.clear();
            throw;
        }
    }
    const JsonObject& result = ResponseResult(response);
    std::string compilerVersion = RequiredString(result, "compilerVersion");
    _initialized = true;
    return compilerVersion;
}

void SubprocessAnalyzerClient::Open(const std::filesystem::path& path,
                                    std::string text, std::int64_t version) {
    const std::filesystem::path normalized = path.lexically_normal();
    const auto previous = _openDocuments.find(normalized);
    const std::optional<OpenDocument> saved =
        previous == _openDocuments.end()
            ? std::nullopt
            : std::optional<OpenDocument>(previous->second);
    _openDocuments.insert_or_assign(normalized, OpenDocument{text, version});
    try {
        ApplyDocumentMutation("document/open",
                              DocumentParams(path, text, version));
    } catch (...) {
        if (saved)
            _openDocuments.insert_or_assign(normalized, *saved);
        else
            _openDocuments.erase(normalized);
        throw;
    }
}

void SubprocessAnalyzerClient::Change(const std::filesystem::path& path,
                                      std::string text, std::int64_t version) {
    const std::filesystem::path normalized = path.lexically_normal();
    const auto found = _openDocuments.find(normalized);
    if (found == _openDocuments.end()) {
        analyzer::json::Value response = ParseResponse(
            Request("document/change", DocumentParams(path, text, version)));
        (void)ResponseResult(response);
        return;
    }
    const OpenDocument saved = found->second;
    found->second = {text, version};
    try {
        ApplyDocumentMutation("document/change",
                              DocumentParams(path, text, version));
    } catch (...) {
        found->second = saved;
        throw;
    }
}

void SubprocessAnalyzerClient::Close(const std::filesystem::path& path) {
    const std::filesystem::path normalized = path.lexically_normal();
    const auto found = _openDocuments.find(normalized);
    const std::optional<OpenDocument> saved =
        found == _openDocuments.end()
            ? std::nullopt
            : std::optional<OpenDocument>(found->second);
    _openDocuments.erase(normalized);
    const std::string params =
        "{\"path\":" + analyzer::json::Quote(path.generic_string()) + '}';
    try {
        ApplyDocumentMutation("document/close", params);
    } catch (...) {
        if (saved) _openDocuments.insert_or_assign(normalized, *saved);
        throw;
    }
}

AnalyzerSnapshot SubprocessAnalyzerClient::Diagnostics(
    const std::filesystem::path& path) {
    const std::string params =
        "{\"path\":" + analyzer::json::Quote(path.generic_string()) + '}';
    analyzer::json::Value response =
        ParseResponse(Request("document/diagnostics", params));
    const JsonObject& result = ResponseResult(response);

    AnalyzerSnapshot snapshot;
    snapshot.path = RequiredString(result, "path");
    if (const analyzer::json::Value* version =
            analyzer::json::Field(result, "version")) {
        if (analyzer::json::AsNumber(*version))
            snapshot.version = RequiredInteger(result, "version");
    }
    const analyzer::json::Value* diagnosticsValue =
        analyzer::json::Field(result, "diagnostics");
    const analyzer::json::Value::Array* diagnostics =
        diagnosticsValue ? analyzer::json::AsArray(*diagnosticsValue) : nullptr;
    if (!diagnostics)
        throw std::runtime_error(
            "analyzer response requires diagnostics array");
    for (const analyzer::json::Value& value : *diagnostics) {
        const JsonObject* diagnostic = analyzer::json::AsObject(value);
        if (!diagnostic)
            throw std::runtime_error("invalid analyzer diagnostic");
        snapshot.diagnostics.push_back(
            {RequiredString(*diagnostic, "code"),
             RequiredString(*diagnostic, "file"),
             RequiredOffset(*diagnostic, "startByte"),
             RequiredOffset(*diagnostic, "endByte"),
             RequiredString(*diagnostic, "severity"),
             RequiredString(*diagnostic, "message")});
    }
    return snapshot;
}

std::optional<AnalyzerHover> SubprocessAnalyzerClient::Hover(
    const std::filesystem::path& path, std::size_t offset) {
    analyzer::json::Value response =
        ParseResponse(Request("document/hover", QueryParams(path, offset)));
    const JsonObject& result = ResponseResult(response);
    const analyzer::json::Value* value = analyzer::json::Field(result, "hover");
    if (!value || std::holds_alternative<std::nullptr_t>(value->value))
        return std::nullopt;
    const JsonObject* hover = analyzer::json::AsObject(*value);
    if (!hover) throw std::runtime_error("invalid analyzer hover response");
    const analyzer::json::Value* range = analyzer::json::Field(*hover, "range");
    if (!range) throw std::runtime_error("analyzer hover has no range");
    return AnalyzerHover{RequiredRange(*range), RequiredString(*hover, "name"),
                         RequiredString(*hover, "type"),
                         RequiredString(*hover, "kind")};
}

std::optional<AnalyzerRange> SubprocessAnalyzerClient::Definition(
    const std::filesystem::path& path, std::size_t offset) {
    analyzer::json::Value response = ParseResponse(
        Request("document/definition", QueryParams(path, offset)));
    const JsonObject& result = ResponseResult(response);
    const analyzer::json::Value* value =
        analyzer::json::Field(result, "definition");
    if (!value || std::holds_alternative<std::nullptr_t>(value->value))
        return std::nullopt;
    return RequiredRange(*value);
}

std::optional<std::vector<AnalyzerRange>> SubprocessAnalyzerClient::References(
    const std::filesystem::path& path, std::size_t offset,
    bool includeDeclaration) {
    std::string params = QueryParams(path, offset);
    params.pop_back();
    params += ",\"includeDeclaration\":";
    params += includeDeclaration ? "true}" : "false}";
    analyzer::json::Value response =
        ParseResponse(Request("document/references", params));
    const JsonObject& result = ResponseResult(response);
    const analyzer::json::Value* value =
        analyzer::json::Field(result, "references");
    if (!value || std::holds_alternative<std::nullptr_t>(value->value))
        return std::nullopt;
    const auto* array = analyzer::json::AsArray(*value);
    if (!array)
        throw std::runtime_error("invalid analyzer references response");
    std::vector<AnalyzerRange> ranges;
    ranges.reserve(array->size());
    for (const analyzer::json::Value& range : *array)
        ranges.push_back(RequiredRange(range));
    return ranges;
}

std::vector<AnalyzerCompletion> SubprocessAnalyzerClient::Completion(
    const std::filesystem::path& path, std::string_view query) {
    const std::string params =
        "{\"path\":" + analyzer::json::Quote(path.generic_string()) +
        ",\"query\":" + analyzer::json::Quote(query) + '}';
    analyzer::json::Value response =
        ParseResponse(Request("document/completion", params));
    const JsonObject& result = ResponseResult(response);
    std::vector<AnalyzerCompletion> items;
    for (const analyzer::json::Value& value : RequiredArray(result, "items")) {
        const JsonObject* item = analyzer::json::AsObject(value);
        if (!item) throw std::runtime_error("invalid analyzer completion item");
        const analyzer::json::Value* declaration =
            analyzer::json::Field(*item, "declaration");
        if (!declaration)
            throw std::runtime_error("completion item has no declaration");
        items.push_back(
            {RequiredString(*item, "name"), RequiredString(*item, "type"),
             RequiredString(*item, "kind"), RequiredRange(*declaration)});
    }
    return items;
}

std::optional<AnalyzerRename> SubprocessAnalyzerClient::Rename(
    const std::filesystem::path& path, std::size_t offset) {
    analyzer::json::Value response =
        ParseResponse(Request("document/rename", QueryParams(path, offset)));
    const JsonObject& result = ResponseResult(response);
    const analyzer::json::Value* value =
        analyzer::json::Field(result, "rename");
    if (!value || std::holds_alternative<std::nullptr_t>(value->value))
        return std::nullopt;
    const JsonObject* rename = analyzer::json::AsObject(*value);
    if (!rename) throw std::runtime_error("invalid analyzer rename response");
    AnalyzerRename converted;
    converted.name = RequiredString(*rename, "name");
    for (const analyzer::json::Value& range : RequiredArray(*rename, "ranges"))
        converted.ranges.push_back(RequiredRange(range));
    return converted;
}

std::vector<AnalyzerSemanticToken> SubprocessAnalyzerClient::SemanticTokens(
    const std::filesystem::path& path) {
    analyzer::json::Value response =
        ParseResponse(Request("document/semanticTokens", QueryParams(path)));
    const JsonObject& result = ResponseResult(response);
    std::vector<AnalyzerSemanticToken> tokens;
    for (const analyzer::json::Value& value : RequiredArray(result, "tokens")) {
        const JsonObject* token = analyzer::json::AsObject(value);
        if (!token) throw std::runtime_error("invalid analyzer semantic token");
        const analyzer::json::Value* range =
            analyzer::json::Field(*token, "range");
        if (!range) throw std::runtime_error("semantic token has no range");
        tokens.push_back({RequiredRange(*range), RequiredString(*token, "kind"),
                          RequiredBool(*token, "isDeclaration")});
    }
    return tokens;
}

void SubprocessAnalyzerClient::Shutdown() {
    if (!_process || _shutdown) return;
    try {
        analyzer::json::Value response =
            ParseResponse(RequestOnce("shutdown", "{}"));
        const JsonObject* envelope = analyzer::json::AsObject(response);
        if (!envelope || analyzer::json::Field(*envelope, "error"))
            throw std::runtime_error("Flecha analyzer rejected shutdown");
    } catch (const AnalyzerTransportError&) {
        // A dead analyzer is already shut down from the client's perspective.
    }
    _shutdown = true;
    _initialized = false;
    _openDocuments.clear();
    CloseHandles();
}

void SubprocessAnalyzerClient::CloseHandles() noexcept {
    if (_input) {
        (void)flecha_runtime_v1_process_pipe_close(_input);
        _input = nullptr;
    }
    if (_output) {
        (void)flecha_runtime_v1_process_pipe_close(_output);
        _output = nullptr;
    }
    if (_process) {
        (void)flecha_runtime_v1_process_close(_process);
        _process = nullptr;
    }
}

}  // namespace flecha::lsp
