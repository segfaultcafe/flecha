#ifndef FLECHA_LONGBOW_CACHE_HPP
#define FLECHA_LONGBOW_CACHE_HPP

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace flecha::longbow {

[[nodiscard]] std::filesystem::path DefaultCacheDirectory();

struct CachedGitSource final {
    std::filesystem::path directory;
    std::string revision;
    std::string checksum;
};

class PackageCache final {
   public:
    explicit PackageCache(std::filesystem::path root = DefaultCacheDirectory())
        : _root(std::move(root)) {}

    [[nodiscard]] const std::filesystem::path& Root() const { return _root; }
    [[nodiscard]] std::optional<std::filesystem::path> Find(
        const std::string& checksum) const;
    std::filesystem::path Store(const std::filesystem::path& source,
                                const std::string& checksum) const;
    [[nodiscard]] std::optional<CachedGitSource> FindGit(
        const std::string& url, const std::string& requestedRevision) const;
    void StoreGit(const std::string& url,
                  const std::string& requestedRevision,
                  const std::string& resolvedRevision,
                  const std::string& checksum) const;

   private:
    std::filesystem::path Entry(const std::string& checksum) const;
    std::filesystem::path GitReference(
        const std::string& url,
        const std::string& requestedRevision) const;
    void Validate(const std::filesystem::path& entry,
                  const std::string& checksum) const;
    std::filesystem::path _root;
};

}  // namespace flecha::longbow

#endif
