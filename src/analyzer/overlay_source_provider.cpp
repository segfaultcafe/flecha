#include "analyzer/overlay_source_provider.hpp"

namespace flecha::analyzer {

std::optional<std::string> OverlaySourceProvider::Read(
    const std::filesystem::path& path) const {
    const SourceOverlay* overlay = _overlays.Find(path);
    if (overlay) return overlay->text;
    return _fallback.Read(path);
}

std::vector<std::filesystem::path> OverlaySourceProvider::KnownSources(
    const std::filesystem::path& root) const {
    std::vector<std::filesystem::path> result;
    for (const std::filesystem::path& path : _overlays.Paths()) {
        const std::filesystem::path relative =
            path.lexically_normal().lexically_relative(root.lexically_normal());
        if (!relative.empty() && *relative.begin() != "..")
            result.push_back(path);
    }
    return result;
}

}  // namespace flecha::analyzer
