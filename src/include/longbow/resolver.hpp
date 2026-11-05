#ifndef FLECHA_LONGBOW_RESOLVER_HPP
#define FLECHA_LONGBOW_RESOLVER_HPP

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>

#include "longbow/manifest.hpp"

namespace flecha::longbow {

class LocalResolver;
enum class ResolvedSourceKind { Workspace, Path, Git, Registry, Toolchain };

struct AcquiredDependency final {
    std::filesystem::path directory;
    ResolvedSourceKind sourceKind = ResolvedSourceKind::Path;
    std::string identity;
    std::string location;
    std::string revision;
    std::string checksum;
};

using DependencyAcquirer = std::function<AcquiredDependency(
    const Manifest& importer, const Dependency& dependency)>;

struct ResolvedPackage final {
    std::string id;
    std::string identity;
    SemanticVersion version;
    std::filesystem::path directory;
    std::filesystem::path source;
    ResolvedSourceKind sourceKind = ResolvedSourceKind::Path;
    std::string location;
    std::string revision;
    std::string checksum;
    std::string flecha;
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
        const Manifest&, const std::optional<std::filesystem::path>&,
        const DependencyAcquirer&);
    std::string _root = "root";
    std::map<std::string, ResolvedPackage> _packages;
};

// Resolves dependencies recursively. Registry dependencies with a path
// override participate through that path; other external sources are supplied
// as verified local trees by Longbow's acquisition layer.
ResolvedGraph ResolveLocalPackages(
    const Manifest& root,
    const std::optional<std::filesystem::path>& bundledStandardLibrary,
    const DependencyAcquirer& acquirer = {});

}  // namespace flecha::longbow

#endif
