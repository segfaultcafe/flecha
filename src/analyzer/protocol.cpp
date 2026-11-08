#include "analyzer/protocol.hpp"

#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#include "analyzer/json.hpp"
#include "core/package_graph.hpp"
#include "diagnostics/diagnostic.hpp"

namespace flecha::analyzer {
namespace {

using JsonObject = json::Value::Object;

constexpr int PARSE_ERROR = -32700;
constexpr int INVALID_REQUEST = -32600;
constexpr int METHOD_NOT_FOUND = -32601;
constexpr int INVALID_PARAMS = -32602;
constexpr int INTERNAL_ERROR = -32603;
constexpr int INVALID_STATE = -32001;
constexpr int STALE_VERSION = -32002;
constexpr int DOCUMENT_NOT_OPEN = -32003;
constexpr int INCOMPATIBLE_PROTOCOL = -32004;

struct Request final {
    std::string id;
    std::string method;
    const JsonObject* params = nullptr;
};

std::string ErrorResponse(std::string_view id, int code,
                          std::string_view message) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) +
           ",\"error\":{\"code\":" + std::to_string(code) +
           ",\"message\":" + json::Quote(message) + "}}";
}

std::string SuccessResponse(std::string_view id, std::string_view result) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) +
           ",\"result\":" + std::string(result) + "}";
}

bool ParseInteger(const json::Value& value, std::int64_t& result) {
    const std::string* number = json::AsNumber(value);
    if (!number) return false;
    const auto [end, error] = std::from_chars(
        number->data(), number->data() + number->size(), result);
    return error == std::errc{} && end == number->data() + number->size();
}

bool StringField(const JsonObject& object, std::string_view name,
                 std::string& result) {
    const json::Value* value = json::Field(object, name);
    if (!value) return false;
    const std::string* text = json::AsString(*value);
    if (!text) return false;
    result = *text;
    return true;
}

bool IntegerField(const JsonObject& object, std::string_view name,
                  std::int64_t& result) {
    const json::Value* value = json::Field(object, name);
    return value && ParseInteger(*value, result);
}

std::optional<Request> ParseRequest(const json::Value& document,
                                    std::string& error) {
    const JsonObject* object = json::AsObject(document);
    if (!object) {
        error = "request must be a JSON object";
        return std::nullopt;
    }

    std::string jsonrpc;
    std::string method;
    if (!StringField(*object, "jsonrpc", jsonrpc) || jsonrpc != "2.0" ||
        !StringField(*object, "method", method)) {
        error = "request requires jsonrpc '2.0' and a method";
        return std::nullopt;
    }

    const json::Value* idValue = json::Field(*object, "id");
    if (!idValue) {
        error = "analyzer protocol requests require an id";
        return std::nullopt;
    }
    std::string id;
    if (const std::string* number = json::AsNumber(*idValue)) {
        std::int64_t parsed = 0;
        if (!ParseInteger(*idValue, parsed)) {
            error = "request id must be an integer or string";
            return std::nullopt;
        }
        id = *number;
    } else if (const std::string* text = json::AsString(*idValue))
        id = json::Quote(*text);
    else {
        error = "request id must be an integer or string";
        return std::nullopt;
    }

    const json::Value* paramsValue = json::Field(*object, "params");
    const JsonObject* params =
        paramsValue ? json::AsObject(*paramsValue) : nullptr;
    if (paramsValue && !params) {
        error = "params must be a JSON object";
        return std::nullopt;
    }
    return Request{std::move(id), std::move(method), params};
}

std::string_view SeverityName(diagnostics::DiagnosticSeverity severity) {
    switch (severity) {
        case diagnostics::DiagnosticSeverity::Note:
            return "note";
        case diagnostics::DiagnosticSeverity::Warning:
            return "warning";
        case diagnostics::DiagnosticSeverity::Error:
            return "error";
    }
    return "error";
}

std::string DiagnosticsResult(const AnalysisSnapshot& snapshot) {
    std::ostringstream result;
    result << "{\"path\":" << json::Quote(snapshot.entry.generic_string())
           << ",\"version\":";
    if (snapshot.version)
        result << *snapshot.version;
    else
        result << "null";
    result << ",\"diagnostics\":[";

    bool first = true;
    for (const diagnostics::Diagnostic& diagnostic :
         snapshot.diagnostics.Diagnostics()) {
        if (!first) result << ',';
        first = false;
        result << "{\"code\":"
               << json::Quote(diagnostics::DiagnosticCodeName(diagnostic.code))
               << ",\"file\":" << json::Quote(diagnostic.range.start.file)
               << ",\"startByte\":" << diagnostic.range.start.offset
               << ",\"endByte\":" << diagnostic.range.end.offset
               << ",\"severity\":"
               << json::Quote(SeverityName(diagnostic.severity))
               << ",\"message\":" << json::Quote(diagnostic.message) << '}';
    }
    result << "]}";
    return result.str();
}

std::string UpdateError(std::string_view id, OverlayUpdateResult result) {
    if (result == OverlayUpdateResult::StaleVersion)
        return ErrorResponse(id, STALE_VERSION, "stale document version");
    return ErrorResponse(id, DOCUMENT_NOT_OPEN, "document is not open");
}

}  // namespace

std::string ProtocolServer::Handle(std::string_view message) {
    std::string parseError;
    std::optional<json::Value> document = json::Parse(message, parseError);
    if (!document) return ErrorResponse("null", PARSE_ERROR, parseError);

    std::string requestError;
    std::optional<Request> request = ParseRequest(*document, requestError);
    if (!request) return ErrorResponse("null", INVALID_REQUEST, requestError);

    try {
        if (request->method == "initialize") {
            if (_state != ProtocolState::Starting)
                return ErrorResponse(request->id, INVALID_STATE,
                                     "analyzer is already initialized");
            if (!request->params)
                return ErrorResponse(request->id, INVALID_PARAMS,
                                     "initialize params are required");

            std::int64_t major = 0;
            std::int64_t minor = 0;
            std::string workspace;
            if (!IntegerField(*request->params, "protocolMajor", major) ||
                !IntegerField(*request->params, "protocolMinor", minor) ||
                major < 0 || minor < 0 ||
                !StringField(*request->params, "workspace", workspace) ||
                !std::filesystem::path(workspace).is_absolute())
                return ErrorResponse(
                    request->id, INVALID_PARAMS,
                    "initialize requires protocolMajor, "
                    "protocolMinor, and an absolute workspace");
            if (major != PROTOCOL_MAJOR)
                return ErrorResponse(request->id, INCOMPATIBLE_PROTOCOL,
                                     "incompatible analyzer protocol major");

            const json::Value* packageGraphValue =
                json::Field(*request->params, "packageGraph");
            if (packageGraphValue) {
                const std::string* packageGraphPath =
                    json::AsString(*packageGraphValue);
                if (!packageGraphPath ||
                    !std::filesystem::path(*packageGraphPath).is_absolute())
                    return ErrorResponse(
                        request->id, INVALID_PARAMS,
                        "packageGraph must be an absolute path");
                std::string graphError;
                std::optional<core::PackageGraph> packageGraph =
                    core::PackageGraph::Load(*packageGraphPath, graphError);
                if (!packageGraph)
                    return ErrorResponse(request->id, INVALID_PARAMS,
                                         graphError);
                _session = std::make_unique<AnalysisSession>(
                    std::move(workspace), std::move(*packageGraph));
            } else {
                _session =
                    std::make_unique<AnalysisSession>(std::move(workspace));
            }
            _state = ProtocolState::Initialized;
            const std::string result =
                "{\"protocolMajor\":" + std::to_string(PROTOCOL_MAJOR) +
                ",\"protocolMinor\":" + std::to_string(PROTOCOL_MINOR) +
                ",\"compilerVersion\":" + json::Quote(_compilerVersion) +
                ",\"capabilities\":{\"diagnostics\":true,"
                "\"fullDocumentSync\":true}}";
            return SuccessResponse(request->id, result);
        }

        if (_state == ProtocolState::Starting)
            return ErrorResponse(request->id, INVALID_STATE,
                                 "analyzer is not initialized");
        if (_state == ProtocolState::Shutdown)
            return ErrorResponse(request->id, INVALID_STATE,
                                 "analyzer has shut down");

        if (request->method == "shutdown") {
            _state = ProtocolState::Shutdown;
            return SuccessResponse(request->id, "null");
        }

        if (request->method == "document/open" ||
            request->method == "document/change") {
            if (!request->params)
                return ErrorResponse(request->id, INVALID_PARAMS,
                                     "document params are required");
            std::string path;
            std::string text;
            std::int64_t version = 0;
            if (!StringField(*request->params, "path", path) ||
                !StringField(*request->params, "text", text) ||
                !IntegerField(*request->params, "version", version) ||
                version < 0)
                return ErrorResponse(request->id, INVALID_PARAMS,
                                     "document update requires path, text, and "
                                     "a nonnegative version");

            const OverlayUpdateResult update =
                request->method == "document/open"
                    ? _session->Open(path, std::move(text), version)
                    : _session->Change(path, std::move(text), version);
            if (update != OverlayUpdateResult::Applied)
                return UpdateError(request->id, update);
            return SuccessResponse(request->id, "{\"status\":\"applied\"}");
        }

        if (request->method == "document/close" ||
            request->method == "document/diagnostics") {
            if (!request->params)
                return ErrorResponse(request->id, INVALID_PARAMS,
                                     "document params are required");
            std::string path;
            if (!StringField(*request->params, "path", path))
                return ErrorResponse(request->id, INVALID_PARAMS,
                                     "document request requires a path");

            if (request->method == "document/close") {
                const bool closed = _session->Close(path);
                return SuccessResponse(request->id, closed
                                                        ? "{\"closed\":true}"
                                                        : "{\"closed\":false}");
            }
            const AnalysisSnapshot snapshot = _session->Analyze(path);
            return SuccessResponse(request->id, DiagnosticsResult(snapshot));
        }

        return ErrorResponse(request->id, METHOD_NOT_FOUND,
                             "unknown analyzer method");
    } catch (const std::exception& error) {
        return ErrorResponse(request->id, INTERNAL_ERROR, error.what());
    }
}

}  // namespace flecha::analyzer
