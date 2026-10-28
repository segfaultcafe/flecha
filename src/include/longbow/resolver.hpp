#ifndef FLECHA_LONGBOW_RESOLVER_HPP
#define FLECHA_LONGBOW_RESOLVER_HPP

#include <filesystem>
#include <map>
#include <optional>
#include <string>

#include "longbow/manifest.hpp"

namespace flecha::longbow {

class LocalResolver;
enum class ResolvedSourceKind { Workspace, Path, Toolchain };

struct ResolvedPackage final {
    std::string id;
    std::string identity;
    SemanticVersion version;
    std::filesystem::path directory;
    std::filesystem::path source;
    ResolvedSourceKind sourceKind = ResolvedSourceKind::Path;
    std::string checksum;
    std::map<std::string, std::string> dependencies;
};

class ResolvedGraph final {
   public:
    [[nodiscard]] const std::string& Root() const { return _root; }
    [[nodiscard]] const std::map<std::string, ResolvedPackage>& Packages()
        const {
        return _packages;
    }
    void WriteCompilerGraph(const std::filesystem::path& file) const;
    void WriteLock(const std::filesystem::path& file,
                   const std::filesystem::path& projectRoot) const;

   private:
    friend class LocalResolver;
    friend ResolvedGraph ResolveLocalPackages(
        const Manifest&, const std::optional<std::filesystem::path>&);
    std::string _root = "root";
    std::map<std::string, ResolvedPackage> _packages;
};

// Resolves path dependencies recursively. Registry dependencies with a path
// override participate through that path; unresolved registry and Git sources
// are rejected until their acquisition layers supply verified local trees.
ResolvedGraph ResolveLocalPackages(
    const Manifest& root,
    const std::optional<std::filesystem::path>& bundledStandardLibrary);

}  // namespace flecha::longbow

#endif
