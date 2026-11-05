#ifndef FLECHA_LONGBOW_ARCHIVE_HPP
#define FLECHA_LONGBOW_ARCHIVE_HPP

#include <filesystem>
#include <string>
#include <vector>

namespace flecha::longbow {

struct PackageArchive final {
    std::string bytes;
    std::string checksum;
    std::vector<std::string> files;
};

PackageArchive BuildPackageArchive(const std::filesystem::path& directory);
void ExtractPackageArchive(const std::string& bytes,
                           const std::filesystem::path& destination);

}  // namespace flecha::longbow

#endif
