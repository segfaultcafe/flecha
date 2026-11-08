#include "analyzer/source_overlays.hpp"

#include <utility>

namespace flecha::analyzer {

std::filesystem::path SourceOverlays::Normalize(
    const std::filesystem::path& path) {
    return path.lexically_normal();
}

OverlayUpdateResult SourceOverlays::Open(const std::filesystem::path& path,
                                         std::string text,
                                         std::int64_t version) {
    const std::filesystem::path normalized = Normalize(path);
    const auto existing = _overlays.find(normalized);

    if (existing != _overlays.end() && version <= existing->second.version)
        return OverlayUpdateResult::StaleVersion;

    _overlays.insert_or_assign(
        normalized, SourceOverlay{normalized, version, std::move(text)});
    return OverlayUpdateResult::Applied;
}

OverlayUpdateResult SourceOverlays::Change(const std::filesystem::path& path,
                                           std::string text,
                                           std::int64_t version) {
    const std::filesystem::path normalized = Normalize(path);
    const auto existing = _overlays.find(normalized);

    if (existing == _overlays.end()) return OverlayUpdateResult::NotFound;
    if (version <= existing->second.version)
        return OverlayUpdateResult::StaleVersion;

    existing->second.version = version;
    existing->second.text = std::move(text);
    return OverlayUpdateResult::Applied;
}

bool SourceOverlays::Close(const std::filesystem::path& path) {
    return _overlays.erase(Normalize(path)) != 0;
}

const SourceOverlay* SourceOverlays::Find(
    const std::filesystem::path& path) const {
    const auto overlay = _overlays.find(Normalize(path));
    return overlay == _overlays.end() ? nullptr : &overlay->second;
}

}  // namespace flecha::analyzer
