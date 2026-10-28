#ifndef FLECHA_LONGBOW_SHA256_HPP
#define FLECHA_LONGBOW_SHA256_HPP

#include <filesystem>
#include <string>
#include <string_view>

namespace flecha::longbow {

[[nodiscard]] std::string Sha256(std::string_view bytes);
[[nodiscard]] std::string Sha256File(const std::filesystem::path& file);
[[nodiscard]] std::string Sha256Tree(const std::filesystem::path& directory);

}  // namespace flecha::longbow

#endif
