#ifndef FLECHA_ANALYZER_ANALYSIS_SESSION_HPP
#define FLECHA_ANALYZER_ANALYSIS_SESSION_HPP

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "analyzer/overlay_source_provider.hpp"
#include "analyzer/source_overlays.hpp"
#include "core/ast.hpp"
#include "core/source_provider.hpp"
#include "diagnostics/diagnostic_bag.hpp"
#include "semantic/semantic_model.hpp"

namespace flecha::analyzer {

struct AnalysisSnapshot final {
    std::filesystem::path entry;
    std::optional<std::int64_t> version;
    std::unique_ptr<core::CompilationUnit> unit;
    std::optional<semantic::SemanticModel> semanticModel;
    diagnostics::DiagnosticBag diagnostics;

    [[nodiscard]] bool HasErrors() const { return diagnostics.HasErrors(); }
};

class AnalysisSession final {
   public:
    explicit AnalysisSession(
        std::filesystem::path workspaceRoot,
        std::map<std::string, std::filesystem::path> libraries = {});

    AnalysisSession(const AnalysisSession&) = delete;
    AnalysisSession& operator=(const AnalysisSession&) = delete;
    AnalysisSession(AnalysisSession&&) = delete;
    AnalysisSession& operator=(AnalysisSession&&) = delete;

    [[nodiscard]] OverlayUpdateResult Open(const std::filesystem::path& path,
                                           std::string text,
                                           std::int64_t version);
    [[nodiscard]] OverlayUpdateResult Change(const std::filesystem::path& path,
                                             std::string text,
                                             std::int64_t version);
    [[nodiscard]] bool Close(const std::filesystem::path& path);

    [[nodiscard]] AnalysisSnapshot Analyze(
        const std::filesystem::path& entry) const;

    [[nodiscard]] const std::filesystem::path& WorkspaceRoot() const {
        return _workspaceRoot;
    }

   private:
    [[nodiscard]] std::filesystem::path Normalize(
        const std::filesystem::path& path) const;

    std::filesystem::path _workspaceRoot;
    std::map<std::string, std::filesystem::path> _libraries;
    SourceOverlays _overlays;
    core::FilesystemSourceProvider _filesystem;
    OverlaySourceProvider _sources;
};

}  // namespace flecha::analyzer

#endif  // FLECHA_ANALYZER_ANALYSIS_SESSION_HPP
