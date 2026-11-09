#include "analyzer/analysis_session.hpp"

#include <system_error>
#include <utility>

#include "core/module_loader.hpp"
#include "semantic/semantic_analyzer.hpp"

namespace flecha::analyzer {
namespace {

std::filesystem::path NormalizePath(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : normalized;
}

}  // namespace

AnalysisSession::AnalysisSession(
    std::filesystem::path workspaceRoot,
    std::map<std::string, std::filesystem::path> libraries)
    : _workspaceRoot(NormalizePath(workspaceRoot)),
      _libraries(std::move(libraries)),
      _sources(_overlays, _filesystem) {}

AnalysisSession::AnalysisSession(std::filesystem::path workspaceRoot,
                                 core::PackageGraph packageGraph)
    : _workspaceRoot(NormalizePath(workspaceRoot)),
      _packageGraph(std::move(packageGraph)),
      _sources(_overlays, _filesystem) {}

std::filesystem::path AnalysisSession::Normalize(
    const std::filesystem::path& path) const {
    const std::filesystem::path absolute =
        path.is_absolute() ? path : _workspaceRoot / path;
    return NormalizePath(absolute);
}

OverlayUpdateResult AnalysisSession::Open(const std::filesystem::path& path,
                                          std::string text,
                                          std::int64_t version) {
    return _overlays.Open(Normalize(path), std::move(text), version);
}

OverlayUpdateResult AnalysisSession::Change(const std::filesystem::path& path,
                                            std::string text,
                                            std::int64_t version) {
    return _overlays.Change(Normalize(path), std::move(text), version);
}

bool AnalysisSession::Close(const std::filesystem::path& path) {
    return _overlays.Close(Normalize(path));
}

AnalysisSnapshot AnalysisSession::Analyze(
    const std::filesystem::path& entry) const {
    const std::filesystem::path normalized = Normalize(entry);
    const SourceOverlay* overlay = _overlays.Find(normalized);
    const std::optional<std::int64_t> version =
        overlay ? std::optional<std::int64_t>(overlay->version) : std::nullopt;

    diagnostics::DiagnosticBag diagnostics;
    core::ModuleLoader loader(diagnostics, _sources);
    std::unique_ptr<core::CompilationUnit> unit =
        _packageGraph ? loader.Load(normalized, *_packageGraph)
                      : loader.Load(normalized, _workspaceRoot, _libraries);

    std::optional<semantic::SemanticModel> semanticModel;
    if (unit && !diagnostics.HasErrors()) {
        semantic::SemanticAnalyzer analyzer(diagnostics);
        semanticModel.emplace(analyzer.Analyze(*unit));
    }

    return {normalized, std::move(version), std::move(unit),
            std::move(semanticModel), std::move(diagnostics)};
}

}  // namespace flecha::analyzer
