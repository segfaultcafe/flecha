#include "longbow/resolver.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "core/package_graph.hpp"
#include "longbow/sha256.hpp"

namespace flecha::longbow {
namespace fs = std::filesystem;
namespace {

fs::path CanonicalDirectory(const fs::path& input,
                            const std::string& description) {
    std::error_code error;
    const fs::path result = fs::weakly_canonical(input, error);
    if (error || !fs::is_directory(result))
        throw std::runtime_error(description +
                                 " is not a directory: " + input.string());
    return result;
}

std::string JsonString(const std::string& value) {
    std::string result = "\"";
    for (const unsigned char character : value) {
        switch (character) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\b':
                result += "\\b";
                break;
            case '\f':
                result += "\\f";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                if (character < 0x20) {
                    constexpr char Hex[] = "0123456789abcdef";
                    result += "\\u00";
                    result += Hex[character >> 4];
                    result += Hex[character & 0x0f];
                } else
                    result += static_cast<char>(character);
        }
    }
    return result + "\"";
}

std::string SourcePath(const fs::path& source, const fs::path& graphFile) {
    std::error_code error;
    const fs::path relative =
        fs::relative(source, graphFile.parent_path(), error);
    return (error || relative.empty() ? source : relative).generic_string();
}

std::string SourceKind(ResolvedSourceKind kind) {
    switch (kind) {
        case ResolvedSourceKind::Workspace:
            return "workspace";
        case ResolvedSourceKind::Path:
            return "path";
        case ResolvedSourceKind::Git:
            return "git";
        case ResolvedSourceKind::Registry:
            return "registry";
        case ResolvedSourceKind::Toolchain:
            return "toolchain";
    }
    return {};
}

void WriteIfChanged(const fs::path& file, const std::string& contents) {
    std::ifstream existing(file, std::ios::binary);
    if (existing) {
        std::ostringstream current;
        current << existing.rdbuf();
        if (current.str() == contents) return;
    }
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream || !(stream << contents))
        throw std::runtime_error("cannot write " + file.string());
}

}  // namespace

class LocalResolver final {
   public:
    LocalResolver(const Manifest& root,
                  const std::optional<fs::path>& bundledStandardLibrary,
                  const DependencyAcquirer& acquirer)
        : _root(root),
          _bundledStandardLibrary(bundledStandardLibrary),
          _acquirer(acquirer) {}

    ResolvedGraph Resolve() {
        _graph._packages.emplace(
            "root", Package(_root, "root", "local/" + ModuleName(_root.name),
                            ResolvedSourceKind::Workspace));
        _paths.emplace(CanonicalDirectory(_root.directory, "project root"),
                       "root");
        _active.push_back("root");
        ResolveDependencies(_root, _graph._packages.at("root"));
        _active.pop_back();
        AddStandardLibrary();
        return std::move(_graph);
    }

   private:
    ResolvedPackage Package(const Manifest& manifest, std::string id,
                            std::string identity, ResolvedSourceKind kind,
                            const AcquiredDependency* acquired = nullptr) {
        const fs::path directory =
            CanonicalDirectory(manifest.directory, "package directory");
        const fs::path source =
            CanonicalDirectory(directory / "src", "package source");
        ResolvedPackage package;
        package.id = std::move(id);
        package.identity = std::move(identity);
        package.version = manifest.version;
        package.directory = directory;
        package.source = source;
        package.sourceKind = kind;
        if (manifest.flecha) package.flecha = manifest.flecha->String();
        if (acquired) {
            package.location = acquired->location;
            package.revision = acquired->revision;
            package.checksum = acquired->checksum;
        } else if (kind == ResolvedSourceKind::Path)
            package.checksum = "sha256:" + Sha256Tree(source);
        return package;
    }

    void ResolveDependencies(const Manifest& manifest, ResolvedPackage& node) {
        for (const auto& [alias, dependency] : manifest.dependencies) {
            if (alias == "std") {
                if (node.id != "root")
                    throw std::runtime_error(
                        "only the root package may override the standard "
                        "library");
                _standardLibrary = CanonicalDirectory(
                    *dependency.path / "src", "standard-library source");
                continue;
            }
            std::optional<AcquiredDependency> acquired;
            if (!dependency.path && _acquirer)
                acquired = _acquirer(manifest, dependency);
            if (!dependency.path && !acquired) {
                const std::string source =
                    dependency.kind == DependencyKind::Git ? "Git" : "registry";
                throw std::runtime_error(source + " dependency '" + alias +
                                         "' has not been fetched");
            }
            const fs::path directory = CanonicalDirectory(
                acquired ? acquired->directory : *dependency.path,
                "dependency '" + alias + "'");
            const auto existing = _paths.find(directory);
            if (existing != _paths.end()) {
                if (std::find(_active.begin(), _active.end(),
                              existing->second) != _active.end()) {
                    std::string cycle = "package dependency cycle:";
                    const auto start = std::find(_active.begin(), _active.end(),
                                                 existing->second);
                    for (auto item = start; item != _active.end(); ++item)
                        cycle += " " + *item + " ->";
                    throw std::runtime_error(cycle + " " + existing->second);
                }
                if (dependency.package &&
                    _graph._packages.at(existing->second).identity !=
                        *dependency.package)
                    throw std::runtime_error(
                        "dependency path is used with conflicting package "
                        "identities");
                node.dependencies.emplace(alias, existing->second);
                continue;
            }

            Manifest child = ReadManifest(directory);
            if (dependency.version &&
                !dependency.version->Matches(child.version))
                throw std::runtime_error(
                    "path override for '" + alias + "' has version " +
                    child.version.String() + ", which does not match " +
                    dependency.version->String());
            const ResolvedSourceKind sourceKind =
                acquired ? acquired->sourceKind : ResolvedSourceKind::Path;
            const std::string prefix =
                sourceKind == ResolvedSourceKind::Git       ? "git-"
                : sourceKind == ResolvedSourceKind::Registry ? "registry-"
                                                            : "path-";
            const std::string id =
                prefix + std::to_string(_graph._packages.size());
            const std::string identity = acquired
                                             ? acquired->identity
                                             : dependency.package.value_or(
                                                   "local/" + ModuleName(child.name));
            _graph._packages.emplace(
                id, Package(child, id, identity, sourceKind,
                            acquired ? &*acquired : nullptr));
            _paths.emplace(directory, id);
            node.dependencies.emplace(alias, id);
            _active.push_back(id);
            ResolveDependencies(child, _graph._packages.at(id));
            _active.pop_back();
        }
    }

    void AddStandardLibrary() {
        const bool overridden = _standardLibrary.has_value();
        const std::optional<fs::path> source =
            _standardLibrary ? _standardLibrary : _bundledStandardLibrary;
        if (!source) return;
        const fs::path canonical =
            CanonicalDirectory(*source, "standard-library source");
        ResolvedPackage standard;
        standard.id = "toolchain-std";
        standard.identity = "flecha/std";
        standard.source = canonical;
        standard.directory = canonical.parent_path();
        standard.sourceKind = overridden ? ResolvedSourceKind::Path
                                         : ResolvedSourceKind::Toolchain;
        if (overridden)
            standard.checksum = "sha256:" + Sha256Tree(canonical);
        _graph._packages.emplace(standard.id, std::move(standard));
        for (auto& [id, package] : _graph._packages)
            if (id != "toolchain-std")
                package.dependencies.emplace("std", "toolchain-std");
    }

    const Manifest& _root;
    std::optional<fs::path> _bundledStandardLibrary;
    const DependencyAcquirer& _acquirer;
    std::optional<fs::path> _standardLibrary;
    ResolvedGraph _graph;
    std::map<fs::path, std::string> _paths;
    std::vector<std::string> _active;
};

ResolvedGraph ResolveLocalPackages(
    const Manifest& root,
    const std::optional<fs::path>& bundledStandardLibrary,
    const DependencyAcquirer& acquirer) {
    return LocalResolver(root, bundledStandardLibrary, acquirer).Resolve();
}

void ResolvedGraph::WriteCompilerGraph(const fs::path& file) const {
    if (file.empty())
        throw std::runtime_error(
            "compiler package graph path must not be empty");
    if (!file.parent_path().empty()) fs::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream)
        throw std::runtime_error("cannot write package graph " + file.string());
    stream << "{\n  \"schema\": " << core::PackageGraph::SchemaVersion
           << ",\n  \"root\": " << JsonString(_root)
           << ",\n  \"packages\": [\n";
    std::size_t packageIndex = 0;
    for (const auto& [id, package] : _packages) {
        stream << "    {\n      \"id\": " << JsonString(id)
               << ",\n      \"identity\": " << JsonString(package.identity)
               << ",\n      \"source\": "
               << JsonString(SourcePath(package.source, file));
        if (package.id != "toolchain-std")
            stream << ",\n      \"version\": "
                   << JsonString(package.version.String());
        if (!package.checksum.empty())
            stream << ",\n      \"checksum\": " << JsonString(package.checksum);
        if (!package.revision.empty())
            stream << ",\n      \"revision\": " << JsonString(package.revision);
        if (!package.flecha.empty())
            stream << ",\n      \"flecha\": " << JsonString(package.flecha);
        stream << ",\n      \"dependencies\": {";
        std::size_t dependencyIndex = 0;
        for (const auto& [alias, target] : package.dependencies) {
            if (dependencyIndex++) stream << ",";
            stream << "\n        " << JsonString(alias) << ": "
                   << JsonString(target);
        }
        if (!package.dependencies.empty()) stream << "\n      ";
        stream << "}\n    }";
        if (++packageIndex != _packages.size()) stream << ",";
        stream << "\n";
    }
    stream << "  ]\n}\n";
    if (!stream)
        throw std::runtime_error("cannot write package graph " + file.string());
}

void ResolvedGraph::WriteLock(const fs::path& file,
                              const fs::path& projectRoot) const {
    std::ostringstream stream;
    stream << "{\n  \"schema\": 1,\n  \"root\": " << JsonString(_root)
           << ",\n  \"packages\": [\n";
    std::size_t packageIndex = 0;
    for (const auto& [id, package] : _packages) {
        stream << "    {\n      \"id\": " << JsonString(id)
               << ",\n      \"identity\": " << JsonString(package.identity)
               << ",\n      \"source\": {\n        \"kind\": "
               << JsonString(SourceKind(package.sourceKind));
        if (package.sourceKind != ResolvedSourceKind::Toolchain) {
            if (package.sourceKind == ResolvedSourceKind::Git ||
                package.sourceKind == ResolvedSourceKind::Registry) {
                stream << ",\n        \"location\": "
                       << JsonString(package.location);
                if (!package.revision.empty())
                    stream << ",\n        \"revision\": "
                           << JsonString(package.revision);
            } else {
                std::error_code error;
                fs::path relative =
                    fs::relative(package.directory, projectRoot, error);
                if (error || relative.empty()) relative = package.directory;
                stream << ",\n        \"path\": "
                       << JsonString(relative.generic_string());
            }
        }
        stream << "\n      }";
        if (package.id != "toolchain-std")
            stream << ",\n      \"version\": "
                   << JsonString(package.version.String());
        if (!package.checksum.empty())
            stream << ",\n      \"checksum\": " << JsonString(package.checksum);
        if (!package.flecha.empty())
            stream << ",\n      \"flecha\": " << JsonString(package.flecha);
        stream << ",\n      \"dependencies\": {";
        std::size_t dependencyIndex = 0;
        for (const auto& [alias, target] : package.dependencies) {
            if (dependencyIndex++) stream << ",";
            stream << "\n        " << JsonString(alias) << ": "
                   << JsonString(target);
        }
        if (!package.dependencies.empty()) stream << "\n      ";
        stream << "}\n    }";
        if (++packageIndex != _packages.size()) stream << ",";
        stream << "\n";
    }
    stream << "  ]\n}\n";
    WriteIfChanged(file, stream.str());
}

}  // namespace flecha::longbow
