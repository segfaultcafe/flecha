#include "longbow/archive.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include "longbow/manifest.hpp"
#include "longbow/sha256.hpp"

namespace flecha::longbow {
namespace fs = std::filesystem;
namespace {

constexpr std::string_view Magic = "FLECHA-PACKAGE-1\n";
constexpr std::uint64_t MaximumArchiveSize = 16 * 1024 * 1024;
constexpr std::uint32_t MaximumFiles = 10000;

std::string ReadFile(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read " + file.string());
    std::string result{std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>()};
    if (result.size() > MaximumArchiveSize)
        throw std::runtime_error("package file exceeds 16 MiB: " +
                                 file.string());
    return result;
}

std::string TomlString(const std::string& value) {
    std::string result = "\"";
    for (const unsigned char character : value) {
        switch (character) {
            case '\\':
            case '"':
                result += '\\';
                result += static_cast<char>(character);
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
                if (character < 0x20 || character == 0x7f)
                    throw std::runtime_error(
                        "manifest contains an unsupported control character");
                result += static_cast<char>(character);
        }
    }
    return result + "\"";
}

std::string NormalizedManifest(const Manifest& manifest) {
    if (!manifest.registry)
        throw std::runtime_error(
            "package.registry is required for publishing");
    if (!manifest.native.empty())
        throw std::runtime_error(
            "public packages cannot contain native link inputs");
    if (manifest.kind != PackageKind::Library)
        throw std::runtime_error("only library packages can be published");
    std::ostringstream output;
    output << "[package]\nname = " << TomlString(manifest.name)
           << "\nregistry = " << TomlString(*manifest.registry)
           << "\nversion = " << TomlString(manifest.version.String())
           << "\nkind = \""
           << (manifest.kind == PackageKind::Library ? "library" : "program")
           << "\"\n";
    if (manifest.flecha)
        output << "flecha = " << TomlString(manifest.flecha->String()) << "\n";
    output << "\n[dependencies]\n";
    for (const auto& [alias, dependency] : manifest.dependencies) {
        if (dependency.kind == DependencyKind::Path)
            throw std::runtime_error("path dependency '" + alias +
                                     "' cannot be published");
        output << alias << " = { ";
        if (dependency.kind == DependencyKind::Git) {
            output << "git = " << TomlString(*dependency.git)
                   << ", rev = " << TomlString(*dependency.revision);
        } else {
            output << "package = " << TomlString(*dependency.package)
                   << ", version = "
                   << TomlString(dependency.version->String());
        }
        output << " }\n";
    }
    return output.str();
}

void ValidatePublishedManifest(const Manifest& manifest,
                               bool allowDevelopmentOverrides) {
    if (!manifest.registry || manifest.kind != PackageKind::Library ||
        !manifest.native.empty())
        throw std::runtime_error(
            "archive manifest is not a source-only public library");
    for (const auto& [alias, dependency] : manifest.dependencies) {
        if (dependency.kind == DependencyKind::Path ||
            (!allowDevelopmentOverrides && dependency.path))
            throw std::runtime_error("published dependency '" + alias +
                                     "' contains a local path");
    }
}

void AppendUnsigned(std::string& output, std::uint64_t value,
                    unsigned bytes) {
    for (unsigned shift = bytes * 8; shift != 0; shift -= 8)
        output += static_cast<char>((value >> (shift - 8)) & 0xff);
}

std::uint64_t ReadUnsigned(std::string_view bytes, std::size_t& offset,
                           unsigned count) {
    if (offset + count > bytes.size())
        throw std::runtime_error("truncated package archive");
    std::uint64_t result = 0;
    for (unsigned index = 0; index != count; ++index)
        result = (result << 8) |
                 static_cast<unsigned char>(bytes[offset++]);
    return result;
}

std::string PortableName(const std::string& path) {
    std::string result = path;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return result;
}

bool SupportedArchivePath(const std::string& path) {
    if (path == "longbow.toml" || path == "README.md" || path == "LICENSE" ||
        path.starts_with("LICENSE."))
        return !path.contains('/');
    return path.starts_with("src/") && fs::path(path).extension() == ".fl";
}

void ValidatePath(const std::string& path, std::set<std::string>& names) {
    if (path.empty() || path.size() > 4096 || path.front() == '/' ||
        path.contains('\\'))
        throw std::runtime_error("package archive contains an unsafe path");
    if (!SupportedArchivePath(path))
        throw std::runtime_error("package archive contains unsupported file '" +
                                 path + "'");
    const fs::path parsed(path);
    if (parsed.is_absolute() || parsed.has_root_name() ||
        parsed.has_root_directory())
        throw std::runtime_error("package archive contains an unsafe path");
    for (const auto& component : parsed)
        if (component.empty() || component == "." || component == "..")
            throw std::runtime_error(
                "package archive contains an unsafe path");
    if (!names.insert(PortableName(path)).second)
        throw std::runtime_error(
            "package archive contains case-colliding paths");
}

}  // namespace

PackageArchive BuildPackageArchive(const fs::path& directory) {
    const Manifest manifest = ReadManifest(directory);
    ValidatePublishedManifest(manifest, true);
    std::map<std::string, std::string> contents;
    contents.emplace("longbow.toml", NormalizedManifest(manifest));

    const fs::path source = directory / "src";
    if (!fs::is_directory(source))
        throw std::runtime_error("package source directory is missing");
    for (const auto& item : fs::recursive_directory_iterator(source)) {
        const fs::file_status status = item.symlink_status();
        if (fs::is_symlink(status))
            throw std::runtime_error("package source contains symbolic link: " +
                                     item.path().string());
        if (fs::is_directory(status)) continue;
        if (!fs::is_regular_file(status) || item.path().extension() != ".fl")
            throw std::runtime_error(
                "public package source may contain only .fl files: " +
                item.path().string());
        const std::string relative =
            (fs::path("src") / item.path().lexically_relative(source))
                .generic_string();
        contents.emplace(relative, ReadFile(item.path()));
    }

    for (const auto& item : fs::directory_iterator(directory)) {
        const std::string name = item.path().filename().string();
        if (name != "README.md" && name != "LICENSE" &&
            !name.starts_with("LICENSE."))
            continue;
        if (fs::is_symlink(item.symlink_status()) || !item.is_regular_file())
            throw std::runtime_error("package document is not a regular file: " +
                                     item.path().string());
        contents.emplace(name, ReadFile(item.path()));
    }

    if (contents.size() > MaximumFiles)
        throw std::runtime_error("package archive exceeds 10000 files");
    PackageArchive result;
    result.bytes.assign(Magic);
    std::set<std::string> names;
    for (const auto& [path, bytes] : contents) {
        ValidatePath(path, names);
        AppendUnsigned(result.bytes, path.size(), 4);
        AppendUnsigned(result.bytes, bytes.size(), 8);
        result.bytes += path;
        result.bytes += bytes;
        result.files.push_back(path);
        if (result.bytes.size() > MaximumArchiveSize)
            throw std::runtime_error("package archive exceeds 16 MiB");
    }
    AppendUnsigned(result.bytes, 0, 4);
    result.checksum = "sha256:" + Sha256(result.bytes);
    return result;
}

void ExtractPackageArchive(const std::string& bytes,
                           const fs::path& destination) {
    if (bytes.size() > MaximumArchiveSize || !bytes.starts_with(Magic))
        throw std::runtime_error("invalid Flecha package archive");
    if (fs::exists(destination))
        throw std::runtime_error("archive destination already exists: " +
                                 destination.string());
    fs::create_directories(destination);
    try {
        std::size_t offset = Magic.size();
        std::set<std::string> names;
        std::uint32_t count = 0;
        while (true) {
            const std::uint64_t pathSize = ReadUnsigned(bytes, offset, 4);
            if (pathSize == 0) break;
            const std::uint64_t contentSize = ReadUnsigned(bytes, offset, 8);
            if (++count > MaximumFiles || pathSize > 4096 ||
                contentSize > MaximumArchiveSize ||
                offset + pathSize + contentSize > bytes.size())
                throw std::runtime_error("invalid Flecha package archive");
            const std::string path = bytes.substr(offset, pathSize);
            offset += pathSize;
            ValidatePath(path, names);
            const fs::path output = destination / fs::path(path);
            fs::create_directories(output.parent_path());
            std::ofstream stream(output, std::ios::binary);
            if (!stream ||
                !stream.write(bytes.data() + offset, contentSize))
                throw std::runtime_error("cannot extract package file " + path);
            offset += contentSize;
        }
        if (offset != bytes.size())
            throw std::runtime_error("package archive has trailing bytes");
        ValidatePublishedManifest(ReadManifest(destination), false);
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(destination, ignored);
        throw;
    }
}

}  // namespace flecha::longbow
