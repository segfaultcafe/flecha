#ifndef FLECHA_LONGBOW_REGISTRY_HPP
#define FLECHA_LONGBOW_REGISTRY_HPP

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "longbow/cache.hpp"
#include "longbow/resolver.hpp"

namespace flecha::longbow {

struct RegistryRelease final {
    SemanticVersion version;
    std::string checksum;
    bool yanked = false;
};

struct LockedRegistryRelease final {
    SemanticVersion version;
    std::string checksum;
};

using RegistryLock = std::map<std::string, LockedRegistryRelease>;

std::vector<RegistryRelease> ParseRegistryIndex(const std::string& contents);
RegistryLock ReadRegistryLock(const std::filesystem::path& file);

class RegistryResolver final {
   public:
    using ReadResource = std::function<std::string(const std::string& path)>;

    RegistryResolver(std::string registry, const PackageCache& cache,
                     ReadResource readResource, RegistryLock locked = {})
        : _registry(std::move(registry)),
          _cache(cache),
          _readResource(std::move(readResource)),
          _locked(std::move(locked)) {}

    AcquiredDependency Acquire(const Dependency& dependency);

   private:
    struct Selection final {
        RegistryRelease release;
        std::filesystem::path directory;
    };
    std::string ReadIndex(const std::string& identity) const;

    std::string _registry;
    const PackageCache& _cache;
    ReadResource _readResource;
    RegistryLock _locked;
    std::map<std::string, Selection> _selections;
};

}  // namespace flecha::longbow

#endif
