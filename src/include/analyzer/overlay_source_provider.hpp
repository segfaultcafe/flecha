#ifndef FLECHA_ANALYZER_OVERLAY_SOURCE_PROVIDER_HPP
#define FLECHA_ANALYZER_OVERLAY_SOURCE_PROVIDER_HPP

#include <filesystem>
#include <optional>
#include <string>

#include "analyzer/source_overlays.hpp"
#include "core/source_provider.hpp"

namespace flecha::analyzer {

class OverlaySourceProvider final : public core::SourceProvider {
   public:
    OverlaySourceProvider(const SourceOverlays& overlays,
                          const core::SourceProvider& fallback)
        : _overlays(overlays), _fallback(fallback) {}

    [[nodiscard]] std::optional<std::string> Read(
        const std::filesystem::path& path) const override;

   private:
    const SourceOverlays& _overlays;
    const core::SourceProvider& _fallback;
};

}  // namespace flecha::analyzer

#endif  // FLECHA_ANALYZER_OVERLAY_SOURCE_PROVIDER_HPP
