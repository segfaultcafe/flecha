#ifndef FLECHA_CORE_PACKAGE_GRAPH_HPP
#define FLECHA_CORE_PACKAGE_GRAPH_HPP

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace flecha::core {

struct PackageGraphNode final {
    std::string id;
    std::string identity;
    std::string version;
    std::string revision;
    std::string checksum;
    std::string flecha;
    std::filesystem::path source;
    std::map<std::string, std::string> dependencies;
};

// A resolved, generated package graph. Node ids are opaque compilation
// identities; canonical owner/package names remain metadata so the schema can
// represent parallel versions in a later resolver without changing compiler
// identity rules.
class PackageGraph final {
   public:
    static constexpr unsigned SchemaVersion = 1;

    static std::optional<PackageGraph> Load(const std::filesystem::path& file,
                                            std::string& error);

    [[nodiscard]] const PackageGraphNode& Root() const;
    [[nodiscard]] const PackageGraphNode* Find(const std::string& id) const;
    [[nodiscard]] const std::map<std::string, PackageGraphNode>& Nodes() const {
        return _nodes;
    }

   private:
    std::string _root;
    std::map<std::string, PackageGraphNode> _nodes;
};

}  // namespace flecha::core

#endif
