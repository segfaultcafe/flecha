#include "analyzer/overlay_source_provider.hpp"

namespace flecha::analyzer {

std::optional<std::string> OverlaySourceProvider::Read(
    const std::filesystem::path& path) const {
    const SourceOverlay* overlay = _overlays.Find(path);
    if (overlay) return overlay->text;
    return _fallback.Read(path);
}

}  // namespace flecha::analyzer
