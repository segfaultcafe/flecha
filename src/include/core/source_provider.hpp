#ifndef FLECHA_CORE_SOURCE_PROVIDER_HPP
#define FLECHA_CORE_SOURCE_PROVIDER_HPP

#include <filesystem>
#include <optional>
#include <string>

namespace flecha::core {

class SourceProvider {
   public:
    virtual ~SourceProvider() = default;

    [[nodiscard]] virtual std::optional<std::string> Read(
        const std::filesystem::path& path) const = 0;
};

class FilesystemSourceProvider final : public SourceProvider {
   public:
    [[nodiscard]] std::optional<std::string> Read(
        const std::filesystem::path& path) const override;
};

}  // namespace flecha::core

#endif  // FLECHA_CORE_SOURCE_PROVIDER_HPP
