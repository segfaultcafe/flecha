#ifndef FLECHA_LSP_PROTOCOL_HPP
#define FLECHA_LSP_PROTOCOL_HPP

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "lsp/analyzer_client.hpp"

namespace flecha::lsp {

enum class ProtocolState { Starting, Initialized, Shutdown, Exited };

class ProtocolServer final {
   public:
    explicit ProtocolServer(AnalyzerClient& analyzer) : _analyzer(analyzer) {}

    [[nodiscard]] std::vector<std::string> Handle(std::string_view message);
    [[nodiscard]] ProtocolState State() const { return _state; }
    [[nodiscard]] int ExitCode() const { return _exitCode; }

   private:
    struct Document final {
        std::string uri;
        std::string text;
        std::int64_t version = 0;
    };

    [[nodiscard]] std::vector<std::string> PublishDiagnostics(
        const std::filesystem::path& entry);
    [[nodiscard]] std::filesystem::path Normalize(
        const std::filesystem::path& path) const;

    AnalyzerClient& _analyzer;
    ProtocolState _state = ProtocolState::Starting;
    int _exitCode = 1;
    std::filesystem::path _workspace;
    std::map<std::filesystem::path, Document> _documents;
    std::map<std::filesystem::path, std::set<std::string>> _published;
};

}  // namespace flecha::lsp

#endif  // FLECHA_LSP_PROTOCOL_HPP
