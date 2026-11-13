#ifndef FLECHA_ANALYZER_SOURCE_OVERLAYS_HPP
#define FLECHA_ANALYZER_SOURCE_OVERLAYS_HPP

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace flecha::analyzer {

enum class OverlayUpdateResult {
    Applied,
    NotFound,
    StaleVersion,
};

struct SourceOverlay final {
    std::filesystem::path path;
    std::int64_t version = 0;
    std::string text;
};

class SourceOverlays final {
   public:
    // Callers provide native absolute paths. The store only performs lexical
    // normalization and never reads from or writes to the filesystem.
    [[nodiscard]] OverlayUpdateResult Open(const std::filesystem::path& path,
                                           std::string text,
                                           std::int64_t version);
    [[nodiscard]] OverlayUpdateResult Change(const std::filesystem::path& path,
                                             std::string text,
                                             std::int64_t version);
    [[nodiscard]] bool Close(const std::filesystem::path& path);

    [[nodiscard]] const SourceOverlay* Find(
        const std::filesystem::path& path) const;
    [[nodiscard]] std::vector<std::filesystem::path> Paths() const;

   private:
    [[nodiscard]] static std::filesystem::path Normalize(
        const std::filesystem::path& path);

    std::map<std::filesystem::path, SourceOverlay> _overlays;
};

}  // namespace flecha::analyzer

#endif  // FLECHA_ANALYZER_SOURCE_OVERLAYS_HPP
