#include "longbow/cache.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string_view>

#include "longbow/archive.hpp"
#include "longbow/sha256.hpp"

namespace flecha::longbow {
namespace fs = std::filesystem;
namespace {

std::string Digest(const std::string& checksum) {
    constexpr std::string_view Prefix = "sha256:";
    if (!checksum.starts_with(Prefix) || checksum.size() != Prefix.size() + 64)
        throw std::runtime_error("invalid SHA-256 checksum '" + checksum + "'");
    const std::string digest = checksum.substr(Prefix.size());
    for (const char character : digest)
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f')))
            throw std::runtime_error("invalid SHA-256 checksum '" + checksum +
                                     "'");
    return digest;
}

fs::path EnvironmentPath(const char* name) {
    const char* value = std::getenv(name);
    return value && *value ? fs::path(value) : fs::path{};
}

void CopyTree(const fs::path& source, const fs::path& destination) {
    if (!fs::is_directory(source))
        throw std::runtime_error("cache source is not a directory: " +
                                 source.string());
    fs::create_directories(destination);
    for (const auto& item : fs::recursive_directory_iterator(source)) {
        const fs::file_status status = item.symlink_status();
        const fs::path relative = item.path().lexically_relative(source);
        const fs::path output = destination / relative;
        if (fs::is_symlink(status))
            throw std::runtime_error("cache source contains symbolic link: " +
                                     item.path().string());
        if (fs::is_directory(status))
            fs::create_directories(output);
        else if (fs::is_regular_file(status)) {
            fs::create_directories(output.parent_path());
            fs::copy_file(item.path(), output, fs::copy_options::none);
        } else
            throw std::runtime_error(
                "cache source contains unsupported entry: " +
                item.path().string());
    }
}

}  // namespace

fs::path DefaultCacheDirectory() {
    if (const fs::path configured = EnvironmentPath("LONGBOW_CACHE_DIR");
        !configured.empty())
        return fs::absolute(configured).lexically_normal();
#ifdef _WIN32
    if (const fs::path local = EnvironmentPath("LOCALAPPDATA"); !local.empty())
        return local / "Flecha" / "Longbow" / "cache";
#elif defined(__APPLE__)
    if (const fs::path home = EnvironmentPath("HOME"); !home.empty())
        return home / "Library" / "Caches" / "flecha" / "longbow";
#else
    if (const fs::path xdg = EnvironmentPath("XDG_CACHE_HOME"); !xdg.empty())
        return xdg / "flecha" / "longbow";
    if (const fs::path home = EnvironmentPath("HOME"); !home.empty())
        return home / ".cache" / "flecha" / "longbow";
#endif
    throw std::runtime_error(
        "cannot determine Longbow cache directory; set LONGBOW_CACHE_DIR");
}

fs::path PackageCache::Entry(const std::string& checksum) const {
    return _root / "sources" / "sha256" / Digest(checksum);
}

fs::path PackageCache::GitReference(
    const std::string& url, const std::string& requestedRevision) const {
    return _root / "refs" / "git" / Sha256(url) / Sha256(requestedRevision);
}

fs::path PackageCache::RegistryIndex(const std::string& registry,
                                     const std::string& identity) const {
    return _root / "indexes" / Sha256(registry) / Sha256(identity);
}

void PackageCache::Validate(const fs::path& entry,
                            const std::string& checksum) const {
    std::ifstream marker(entry / "complete", std::ios::binary);
    std::string kind;
    std::string recorded;
    std::string sourceChecksum;
    std::getline(marker, kind);
    std::getline(marker, recorded);
    if (kind == "archive") std::getline(marker, sourceChecksum);
    if (!marker || (kind != "tree" && kind != "archive") ||
        recorded != checksum || !fs::is_directory(entry / "source"))
        throw std::runtime_error("incomplete package cache entry for " +
                                 checksum);
    std::string actual;
    if (kind == "tree")
        actual = "sha256:" + Sha256Tree(entry / "source");
    else {
        if (sourceChecksum != "sha256:" + Sha256Tree(entry / "source"))
            throw std::runtime_error(
                "corrupt extracted package cache entry for " + checksum);
        const std::string archive = [&] {
            std::ifstream stream(entry / "archive", std::ios::binary);
            if (!stream)
                throw std::runtime_error("incomplete package cache entry for " +
                                         checksum);
            return std::string{std::istreambuf_iterator<char>(stream),
                               std::istreambuf_iterator<char>()};
        }();
        actual = "sha256:" + Sha256(archive);
        if (actual == checksum &&
            BuildPackageArchive(entry / "source").checksum != checksum)
            actual = "extracted-source-mismatch";
    }
    if (actual != checksum)
        throw std::runtime_error("corrupt package cache entry for " + checksum +
                                 "; found " + actual);
}

std::optional<fs::path> PackageCache::Find(const std::string& checksum) const {
    const fs::path entry = Entry(checksum);
    if (!fs::exists(entry)) return std::nullopt;
    Validate(entry, checksum);
    return entry / "source";
}

fs::path PackageCache::Store(const fs::path& source,
                             const std::string& checksum) const {
    const std::string actual = "sha256:" + Sha256Tree(source);
    if (actual != checksum)
        throw std::runtime_error("package source checksum mismatch: expected " +
                                 checksum + ", found " + actual);
    const fs::path entry = Entry(checksum);
    if (fs::exists(entry)) {
        try {
            Validate(entry, checksum);
            return entry / "source";
        } catch (const std::runtime_error&) {
            fs::remove_all(entry);
        }
    }
    fs::create_directories(entry.parent_path());
    const fs::path temporary =
        entry.string() + ".tmp-" +
        std::to_string(std::chrono::high_resolution_clock::now()
                           .time_since_epoch()
                           .count());
    try {
        CopyTree(source, temporary / "source");
        std::ofstream marker(temporary / "complete", std::ios::binary);
        if (!marker || !(marker << "tree\n" << checksum << "\n"))
            throw std::runtime_error("cannot complete package cache entry");
        marker.close();
        std::error_code renameError;
        fs::rename(temporary, entry, renameError);
        if (renameError) {
            // Another Longbow process may have populated the same immutable
            // content address while this one was staging it.
            if (!fs::exists(entry))
                throw std::runtime_error(
                    "cannot install package cache entry: " +
                    renameError.message());
            fs::remove_all(temporary);
        }
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(temporary, ignored);
        throw;
    }
    Validate(entry, checksum);
    return entry / "source";
}

fs::path PackageCache::StoreArchive(const std::string& archive,
                                    const std::string& checksum) const {
    const std::string actual = "sha256:" + Sha256(archive);
    if (actual != checksum)
        throw std::runtime_error(
            "package archive checksum mismatch: expected " + checksum +
            ", found " + actual);
    const fs::path entry = Entry(checksum);
    if (fs::exists(entry)) {
        try {
            Validate(entry, checksum);
            return entry / "source";
        } catch (const std::runtime_error&) {
            fs::remove_all(entry);
        }
    }
    fs::create_directories(entry.parent_path());
    const fs::path temporary =
        entry.string() + ".tmp-" +
        std::to_string(std::chrono::high_resolution_clock::now()
                           .time_since_epoch()
                           .count());
    try {
        ExtractPackageArchive(archive, temporary / "source");
        {
            std::ofstream output(temporary / "archive", std::ios::binary);
            if (!output || !output.write(archive.data(), archive.size()))
                throw std::runtime_error("cannot cache package archive");
        }
        std::ofstream marker(temporary / "complete", std::ios::binary);
        if (!marker || !(marker << "archive\n"
                                << checksum << "\nsha256:"
                                << Sha256Tree(temporary / "source") << "\n"))
            throw std::runtime_error("cannot complete package cache entry");
        marker.close();
        std::error_code renameError;
        fs::rename(temporary, entry, renameError);
        if (renameError) {
            if (!fs::exists(entry))
                throw std::runtime_error(
                    "cannot install package archive cache entry: " +
                    renameError.message());
            fs::remove_all(temporary);
        }
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(temporary, ignored);
        throw;
    }
    Validate(entry, checksum);
    return entry / "source";
}

std::optional<CachedGitSource> PackageCache::FindGit(
    const std::string& url, const std::string& requestedRevision) const {
    std::ifstream stream(GitReference(url, requestedRevision),
                         std::ios::binary);
    if (!stream) return std::nullopt;
    CachedGitSource result;
    std::getline(stream, result.revision);
    std::getline(stream, result.checksum);
    std::string extra;
    if (!stream || std::getline(stream, extra) || result.revision.empty() ||
        result.checksum.empty())
        throw std::runtime_error("corrupt cached Git reference");
    const auto source = Find(result.checksum);
    if (!source)
        throw std::runtime_error("cached Git reference has no source entry");
    result.directory = *source;
    return result;
}

void PackageCache::StoreGit(const std::string& url,
                            const std::string& requestedRevision,
                            const std::string& resolvedRevision,
                            const std::string& checksum) const {
    (void)Digest(checksum);
    const fs::path reference = GitReference(url, requestedRevision);
    fs::create_directories(reference.parent_path());
    const fs::path temporary =
        reference.string() + ".tmp-" +
        std::to_string(std::chrono::high_resolution_clock::now()
                           .time_since_epoch()
                           .count());
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream || !(stream << resolvedRevision << "\n"
                                << checksum << "\n"))
            throw std::runtime_error("cannot write cached Git reference");
    }
    std::error_code error;
    fs::rename(temporary, reference, error);
    if (error) {
        fs::remove(reference, error);
        error.clear();
        fs::rename(temporary, reference, error);
    }
    if (error) {
        fs::remove(temporary);
        throw std::runtime_error("cannot install cached Git reference: " +
                                 error.message());
    }
}

std::optional<std::string> PackageCache::FindRegistryIndex(
    const std::string& registry, const std::string& identity) const {
    std::ifstream stream(RegistryIndex(registry, identity), std::ios::binary);
    if (!stream) return std::nullopt;
    std::string contents{std::istreambuf_iterator<char>(stream),
                         std::istreambuf_iterator<char>()};
    if (contents.empty() || contents.size() > 4 * 1024 * 1024)
        throw std::runtime_error("cached registry index is empty or oversized");
    return contents;
}

void PackageCache::StoreRegistryIndex(const std::string& registry,
                                      const std::string& identity,
                                      const std::string& contents) const {
    if (contents.empty() || contents.size() > 4 * 1024 * 1024)
        throw std::runtime_error("registry index is empty or oversized");
    const fs::path file = RegistryIndex(registry, identity);
    fs::create_directories(file.parent_path());
    const fs::path temporary =
        file.string() + ".tmp-" +
        std::to_string(std::chrono::high_resolution_clock::now()
                           .time_since_epoch()
                           .count());
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream || !stream.write(contents.data(), contents.size()))
            throw std::runtime_error("cannot cache registry index");
    }
    std::error_code error;
    fs::rename(temporary, file, error);
    if (error) {
        fs::remove(file, error);
        error.clear();
        fs::rename(temporary, file, error);
    }
    if (error) {
        fs::remove(temporary);
        throw std::runtime_error("cannot install cached registry index: " +
                                 error.message());
    }
}

}  // namespace flecha::longbow
