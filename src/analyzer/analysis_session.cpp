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

std::optional<SemanticHover> AnalysisSnapshot::Hover(
    const std::filesystem::path& file, std::size_t offset) const {
    if (!semanticModel) return std::nullopt;
    const std::filesystem::path absolute =
        file.is_absolute() ? file : workspaceRoot / file;
    return SemanticQueries(*semanticModel).Hover(absolute, offset);
}

std::optional<SemanticOccurrence> AnalysisSnapshot::OccurrenceAt(
    const std::filesystem::path& file, std::size_t offset) const {
    if (!semanticModel) return std::nullopt;
    const std::filesystem::path absolute =
        file.is_absolute() ? file : workspaceRoot / file;
    return SemanticQueries(*semanticModel).OccurrenceAt(absolute, offset);
}

std::vector<SemanticOccurrence> AnalysisSnapshot::Occurrences(
    const std::filesystem::path& file) const {
    if (!semanticModel) return {};
    const std::filesystem::path absolute =
        file.is_absolute() ? file : workspaceRoot / file;
    return SemanticQueries(*semanticModel).Occurrences(absolute);
}

std::optional<source::SourceRange> AnalysisSnapshot::Definition(
    const std::filesystem::path& file, std::size_t offset) const {
    if (!semanticModel) return std::nullopt;
    const std::filesystem::path absolute =
        file.is_absolute() ? file : workspaceRoot / file;
    return SemanticQueries(*semanticModel).Definition(absolute, offset);
}

std::optional<std::vector<source::SourceRange>> AnalysisSnapshot::References(
    const std::filesystem::path& file, std::size_t offset,
    bool includeDeclaration) const {
    if (!semanticModel) return std::nullopt;
    const std::filesystem::path absolute =
        file.is_absolute() ? file : workspaceRoot / file;
    return SemanticQueries(*semanticModel)
        .References(absolute, offset, includeDeclaration);
}

std::optional<SemanticRename> AnalysisSnapshot::Rename(
    const std::filesystem::path& file, std::size_t offset,
    bool includeDeclaration) const {
    if (!semanticModel) return std::nullopt;
    const std::filesystem::path absolute =
        file.is_absolute() ? file : workspaceRoot / file;
    return SemanticQueries(*semanticModel)
        .Rename(absolute, offset, includeDeclaration);
}

std::vector<SemanticSymbol> AnalysisSnapshot::Symbols(
    std::string_view query) const {
    if (!semanticModel) return {};
    return SemanticQueries(*semanticModel).Symbols(query);
}

AnalysisSession::AnalysisSession(
    std::filesystem::path workspaceRoot,
    std::map<std::string, std::filesystem::path> libraries)
    : _workspaceRoot(NormalizePath(workspaceRoot)),
      _libraries(std::move(libraries)),
      _sources(_overlays, _filesystem),
      _sourceIndex(_sources) {}

AnalysisSession::AnalysisSession(std::filesystem::path workspaceRoot,
                                 core::PackageGraph packageGraph)
    : _workspaceRoot(NormalizePath(workspaceRoot)),
      _packageGraph(std::move(packageGraph)),
      _sources(_overlays, _filesystem),
      _sourceIndex(_sources) {}

std::filesystem::path AnalysisSession::Normalize(
    const std::filesystem::path& path) const {
    const std::filesystem::path absolute =
        path.is_absolute() ? path : _workspaceRoot / path;
    return NormalizePath(absolute);
}

OverlayUpdateResult AnalysisSession::Open(const std::filesystem::path& path,
                                          std::string text,
                                          std::int64_t version) {
    const std::filesystem::path normalized = Normalize(path);
    const OverlayUpdateResult result =
        _overlays.Open(normalized, std::move(text), version);
    if (result == OverlayUpdateResult::Applied)
        _sourceIndex.Invalidate(normalized);
    return result;
}

OverlayUpdateResult AnalysisSession::Change(const std::filesystem::path& path,
                                            std::string text,
                                            std::int64_t version) {
    const std::filesystem::path normalized = Normalize(path);
    const OverlayUpdateResult result =
        _overlays.Change(normalized, std::move(text), version);
    if (result == OverlayUpdateResult::Applied)
        _sourceIndex.Invalidate(normalized);
    return result;
}

bool AnalysisSession::Close(const std::filesystem::path& path) {
    const std::filesystem::path normalized = Normalize(path);
    const bool closed = _overlays.Close(normalized);
    if (closed) _sourceIndex.Invalidate(normalized);
    return closed;
}

void AnalysisSession::Refresh(const std::filesystem::path& path) {
    _sourceIndex.Invalidate(Normalize(path));
}

AnalysisSnapshot AnalysisSession::Analyze(
    const std::filesystem::path& entry) const {
    const std::filesystem::path normalized = Normalize(entry);
    const SourceOverlay* overlay = _overlays.Find(normalized);
    const std::optional<std::int64_t> version =
        overlay ? std::optional<std::int64_t>(overlay->version) : std::nullopt;

    diagnostics::DiagnosticBag diagnostics;
    core::ModuleLoader loader(diagnostics, _sources, _sourceIndex);
    std::unique_ptr<core::CompilationUnit> unit =
        _packageGraph ? loader.Load(normalized, *_packageGraph,
                                    core::ModuleLoadMode::Analysis)
                      : loader.Load(normalized, _workspaceRoot, _libraries,
                                    core::ModuleLoadMode::Analysis);

    std::optional<semantic::SemanticModel> semanticModel;
    if (unit && !diagnostics.HasErrors()) {
        semantic::SemanticAnalyzer analyzer(diagnostics);
        semanticModel.emplace(analyzer.Analyze(*unit));
    }

    return {normalized,      _workspaceRoot,           std::move(version),
            std::move(unit), std::move(semanticModel), std::move(diagnostics)};
}

}  // namespace flecha::analyzer
