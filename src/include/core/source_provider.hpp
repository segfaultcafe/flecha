#ifndef FLECHA_CORE_SOURCE_PROVIDER_HPP
#define FLECHA_CORE_SOURCE_PROVIDER_HPP

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace flecha::core {

class SourceProvider {
   public:
    virtual ~SourceProvider() = default;

    [[nodiscard]] virtual std::optional<std::string> Read(
        const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual std::vector<std::filesystem::path> KnownSources(
        const std::filesystem::path& root) const {
        (void)root;
        return {};
    }
};

class FilesystemSourceProvider final : public SourceProvider {
   public:
    [[nodiscard]] std::optional<std::string> Read(
        const std::filesystem::path& path) const override;
};

}  // namespace flecha::core

#endif  // FLECHA_CORE_SOURCE_PROVIDER_HPP
