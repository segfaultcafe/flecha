#include "longbow/cache.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string_view>

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
            throw std::runtime_error("cache source contains unsupported entry: " +
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

void PackageCache::Validate(const fs::path& entry,
                            const std::string& checksum) const {
    std::ifstream marker(entry / "complete", std::ios::binary);
    std::string recorded;
    std::getline(marker, recorded);
    if (!marker || recorded != checksum || !fs::is_directory(entry / "source"))
        throw std::runtime_error("incomplete package cache entry for " +
                                 checksum);
    const std::string actual = "sha256:" + Sha256Tree(entry / "source");
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
        if (!marker || !(marker << checksum << "\n"))
            throw std::runtime_error("cannot complete package cache entry");
        marker.close();
        std::error_code renameError;
        fs::rename(temporary, entry, renameError);
        if (renameError) {
            // Another Longbow process may have populated the same immutable
            // content address while this one was staging it.
            if (!fs::exists(entry))
                throw std::runtime_error("cannot install package cache entry: " +
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

}  // namespace flecha::longbow
