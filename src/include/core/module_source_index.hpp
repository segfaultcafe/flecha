#ifndef FLECHA_CORE_MODULE_SOURCE_INDEX_HPP
#define FLECHA_CORE_MODULE_SOURCE_INDEX_HPP

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/ast.hpp"

namespace flecha::core {
class SourceProvider;

struct IndexedModuleSource final {
    std::filesystem::path file;
    std::string name;
    std::optional<NamespaceAttachment> namespaceAttachment;
};

enum class NamespaceAttachmentStatus {
    Valid,
    Empty,
    NonRoot,
    MissingTarget,
    EscapesRoot,
    NotAncestor,
};

struct NamespaceAttachmentResolution final {
    NamespaceAttachmentStatus status = NamespaceAttachmentStatus::Empty;
    std::optional<std::filesystem::path> target;
};

// A source-root-scoped index shared by explicit and wildcard imports.
// Filesystem directories are indexed lazily; attachment candidates are indexed
// once per root. Individual paths can then be refreshed without rescanning the
// root.
class ModuleSourceIndex final {
   public:
    explicit ModuleSourceIndex(const SourceProvider& sources)
        : _sources(sources) {}

    [[nodiscard]] IndexedModuleSource Read(
        const std::filesystem::path& file) const;
    [[nodiscard]] std::vector<IndexedModuleSource> Discover(
        const std::filesystem::path& root,
        const std::filesystem::path& logicalDirectory);
    void Invalidate(const std::filesystem::path& file);
    void InvalidateRoot(const std::filesystem::path& root);

    [[nodiscard]] static NamespaceAttachmentResolution ResolveAttachment(
        const std::filesystem::path& file,
        const NamespaceAttachment& attachment,
        const std::filesystem::path& root);

   private:
    struct RootState final {
        std::filesystem::path root;
        bool attachmentsScanned = false;
        std::set<std::filesystem::path> scannedDirectories;
        std::map<std::filesystem::path, IndexedModuleSource> sources;
    };

    [[nodiscard]] RootState& State(const std::filesystem::path& root);
    void EnsureAttachments(RootState& state);
    void EnsureDirectory(RootState& state,
                         const std::filesystem::path& directory);
    void Update(RootState& state, const std::filesystem::path& file);
    void Remove(RootState& state, const std::filesystem::path& file);

    const SourceProvider& _sources;
    std::map<std::filesystem::path, RootState> _roots;
};

}  // namespace flecha::core

#endif  // FLECHA_CORE_MODULE_SOURCE_INDEX_HPP
