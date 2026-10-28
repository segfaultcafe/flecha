#include "longbow/registry.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "longbow/manifest.hpp"

namespace flecha::longbow {
namespace {

bool ValidChecksum(const std::string& checksum) {
    if (!checksum.starts_with("sha256:") || checksum.size() != 71) return false;
    return std::all_of(checksum.begin() + 7, checksum.end(), [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
    });
}

}  // namespace

RegistryLock ReadRegistryLock(const std::filesystem::path& file) {
    std::ifstream stream(file);
    if (!stream) return {};
    RegistryLock result;
    std::string identity, kind, version, checksum, line;
    const auto value = [](const std::string& input,
                          const std::string& field) -> std::string {
        const std::string prefix = "\"" + field + "\": \"";
        const std::size_t start = input.find(prefix);
        if (start == std::string::npos) return {};
        const std::size_t begin = start + prefix.size();
        const std::size_t end = input.find('"', begin);
        return end == std::string::npos ? std::string{}
                                        : input.substr(begin, end - begin);
    };
    const auto finish = [&] {
        if (kind == "registry") {
            std::string error;
            const auto parsed = SemanticVersion::Parse(version, error);
            if (!ValidPackageIdentity(identity) || !parsed ||
                !ValidChecksum(checksum) ||
                !result
                     .emplace(identity,
                              LockedRegistryRelease{*parsed, checksum})
                     .second)
                throw std::runtime_error("invalid registry entry in lockfile");
        }
        identity.clear();
        kind.clear();
        version.clear();
        checksum.clear();
    };
    while (std::getline(stream, line)) {
        if (line == "    {" && (!identity.empty() || !kind.empty() ||
                                !version.empty() || !checksum.empty()))
            throw std::runtime_error("malformed longbow.lock package entry");
        if (const std::string parsed = value(line, "identity"); !parsed.empty())
            identity = parsed;
        if (const std::string parsed = value(line, "kind"); !parsed.empty())
            kind = parsed;
        if (const std::string parsed = value(line, "version"); !parsed.empty())
            version = parsed;
        if (const std::string parsed = value(line, "checksum"); !parsed.empty())
            checksum = parsed;
        if (line == "    }," || line == "    }") finish();
    }
    if (!identity.empty() || !kind.empty() || !version.empty() ||
        !checksum.empty())
        throw std::runtime_error("truncated longbow.lock package entry");
    return result;
}

std::vector<RegistryRelease> ParseRegistryIndex(const std::string& contents) {
    if (contents.size() > 4 * 1024 * 1024)
        throw std::runtime_error("registry index exceeds 4 MiB");
    std::istringstream stream(contents);
    std::string line;
    if (!std::getline(stream, line))
        throw std::runtime_error("unsupported registry index format");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != "flecha-registry-index 1")
        throw std::runtime_error("unsupported registry index format");
    std::vector<RegistryRelease> releases;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (releases.size() == 100000)
            throw std::runtime_error("registry index exceeds 100000 releases");
        const std::size_t first = line.find('\t');
        const std::size_t second =
            first == std::string::npos ? first : line.find('\t', first + 1);
        if (first == std::string::npos || second == std::string::npos ||
            line.find('\t', second + 1) != std::string::npos)
            throw std::runtime_error("malformed registry index entry");
        std::string error;
        const auto version =
            SemanticVersion::Parse(line.substr(0, first), error);
        const std::string checksum = line.substr(first + 1, second - first - 1);
        const std::string state = line.substr(second + 1);
        if (!version || !ValidChecksum(checksum) ||
            (state != "active" && state != "yanked"))
            throw std::runtime_error("invalid registry index entry");
        if (std::any_of(
                releases.begin(), releases.end(),
                [&](const auto& item) { return item.version == *version; }))
            throw std::runtime_error("duplicate registry package version " +
                                     version->String());
        releases.push_back({*version, checksum, state == "yanked"});
    }
    return releases;
}

std::string RegistryResolver::ReadIndex(const std::string& identity) const {
    std::string contents;
    try {
        contents = _readResource("packages/" + identity + "/index");
    } catch (const std::exception& onlineError) {
        const auto cached = _cache.FindRegistryIndex(_registry, identity);
        if (!cached)
            throw std::runtime_error("cannot fetch registry index for '" +
                                     identity + "': " + onlineError.what());
        (void)ParseRegistryIndex(*cached);
        return *cached;
    }
    (void)ParseRegistryIndex(contents);
    _cache.StoreRegistryIndex(_registry, identity, contents);
    return contents;
}

AcquiredDependency RegistryResolver::Acquire(const Dependency& dependency) {
    const std::string& identity = *dependency.package;
    const auto selected = _selections.find(identity);
    if (selected != _selections.end()) {
        if (!dependency.version->Matches(selected->second.release.version))
            throw std::runtime_error(
                "incompatible version requirements for registry package '" +
                identity + "'");
        return {selected->second.directory,
                ResolvedSourceKind::Registry,
                identity,
                _registry,
                {},
                selected->second.release.checksum};
    }

    const auto releases = ParseRegistryIndex(ReadIndex(identity));
    const RegistryRelease* choice = nullptr;
    const auto locked = _locked.find(identity);
    if (locked != _locked.end()) {
        if (!dependency.version->Matches(locked->second.version))
            throw std::runtime_error(
                "locked registry package '" + identity +
                "' no longer matches manifest requirement " +
                dependency.version->String());
        const auto found = std::find_if(
            releases.begin(), releases.end(), [&](const auto& release) {
                return release.version == locked->second.version;
            });
        if (found == releases.end() ||
            found->checksum != locked->second.checksum)
            throw std::runtime_error(
                "registry metadata changed for locked package '" + identity +
                "'");
        choice = &*found;
    } else {
        for (const auto& release : releases)
            if (!release.yanked &&
                dependency.version->Matches(release.version) &&
                (!choice || choice->version < release.version))
                choice = &release;
    }
    if (!choice)
        throw std::runtime_error("no active version of registry package '" +
                                 identity + "' matches " +
                                 dependency.version->String());

    std::filesystem::path directory;
    try {
        if (const auto cached = _cache.Find(choice->checksum))
            directory = *cached;
    } catch (const std::runtime_error&) {
        // Verified reacquisition below repairs a corrupt online cache entry.
    }
    if (directory.empty()) {
        const std::string archive =
            _readResource("packages/" + identity + "/" +
                          choice->version.String() + "/archive");
        directory = _cache.StoreArchive(archive, choice->checksum);
    }
    const Manifest manifest = ReadManifest(directory);
    if (manifest.registry != identity || manifest.version != choice->version)
        throw std::runtime_error(
            "registry archive identity or version mismatch");
    const Selection selection{*choice, directory};
    _selections.emplace(identity, selection);
    return {directory, ResolvedSourceKind::Registry,
            identity,  _registry,
            {},        choice->checksum};
}

}  // namespace flecha::longbow
