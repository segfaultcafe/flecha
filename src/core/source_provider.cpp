#include "core/source_provider.hpp"

#include <fstream>
#include <sstream>

namespace flecha::core {

std::optional<std::string> FilesystemSourceProvider::Read(
    const std::filesystem::path& path) const {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;

    std::ostringstream contents;
    contents << stream.rdbuf();
    if (stream.bad()) return std::nullopt;

    return contents.str();
}

}  // namespace flecha::core
