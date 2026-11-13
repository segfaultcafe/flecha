#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

#include "analyzer/json.hpp"
#include "analyzer/protocol.hpp"
#include "analyzer/transport.hpp"

namespace {

using flecha::analyzer::json::AsObject;
using flecha::analyzer::json::AsString;
using flecha::analyzer::json::Field;
using flecha::analyzer::json::Parse;

std::optional<std::string> StringField(
    const flecha::analyzer::json::Value::Object& object,
    std::string_view name) {
    const auto* value = Field(object, name);
    const auto* text = value ? AsString(*value) : nullptr;
    return text ? std::optional<std::string>(*text) : std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 || std::string_view(argv[1]) != "analyze" ||
        std::string_view(argv[2]) != "--stdio")
        return 64;

    flecha::analyzer::Transport transport(std::cin, std::cout);
    flecha::analyzer::ProtocolServer protocol("analyzer-fixture");
    std::optional<std::filesystem::path> crashMarker;
    while (const std::optional<std::string> message = transport.ReadMessage()) {
        std::string error;
        const auto document = Parse(*message, error);
        const auto* request = document ? AsObject(*document) : nullptr;
        const auto method =
            request ? StringField(*request, "method") : std::nullopt;
        if (method && *method == "initialize" && request) {
            const auto* paramsValue = Field(*request, "params");
            const auto* params = paramsValue ? AsObject(*paramsValue) : nullptr;
            const auto workspace =
                params ? StringField(*params, "workspace") : std::nullopt;
            if (workspace)
                crashMarker = std::filesystem::path(*workspace) /
                              ".analyzer-fixture-crashed";
        }
        if (method && *method == "document/change" && crashMarker &&
            !std::filesystem::exists(*crashMarker)) {
            std::ofstream marker(*crashMarker, std::ios::binary);
            marker << "crashed\n";
            marker.close();
            std::_Exit(86);
        }

        transport.WriteMessage(protocol.Handle(*message));
        if (protocol.State() == flecha::analyzer::ProtocolState::Shutdown)
            break;
    }
    return 0;
}
