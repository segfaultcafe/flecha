#include "longbow/registry_transport.hpp"

#include <llvm/Support/Program.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "longbow/manifest.hpp"
#include "longbow/registry.hpp"

namespace flecha::longbow {
namespace fs = std::filesystem;
namespace {

std::string ReadFile(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read " + file.string());
    std::string result{std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>()};
    if (result.size() > 32 * 1024 * 1024)
        throw std::runtime_error("registry response exceeds 32 MiB");
    return result;
}

void WriteFile(const fs::path& file, const std::string& contents) {
    fs::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream || !stream.write(contents.data(), contents.size()))
        throw std::runtime_error("cannot write " + file.string());
}

void WritePrivateFile(const fs::path& file, const std::string& contents) {
    fs::create_directories(file.parent_path());
    {
        std::ofstream create(file, std::ios::binary | std::ios::trunc);
        if (!create) throw std::runtime_error("cannot create " + file.string());
    }
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
    WriteFile(file, contents);
}

fs::path TemporaryFile(const std::string& name) {
    return fs::temp_directory_path() /
           ("longbow-" + name + "-" +
            std::to_string(std::chrono::high_resolution_clock::now()
                               .time_since_epoch()
                               .count()));
}

class RemoveFile final {
   public:
    explicit RemoveFile(fs::path file) : _file(std::move(file)) {}
    ~RemoveFile() {
        std::error_code ignored;
        fs::remove(_file, ignored);
    }

   private:
    fs::path _file;
};

void Curl(const std::vector<std::string>& arguments,
          const std::optional<fs::path>& output = std::nullopt) {
    const auto executable = llvm::sys::findProgramByName("curl");
    if (!executable)
        throw std::runtime_error(
            "cannot contact the registry because curl is not installed");
    std::vector<std::string> command{
        *executable,         "--fail", "--silent",   "--show-error",
        "--connect-timeout", "15",     "--max-time", "300"};
    command.insert(command.end(), arguments.begin(), arguments.end());
    std::vector<llvm::StringRef> refs;
    for (const auto& argument : command) refs.emplace_back(argument);
    std::string outputName;
    std::array<std::optional<llvm::StringRef>, 3> redirects;
    llvm::ArrayRef<std::optional<llvm::StringRef>> redirectView;
    if (output) {
        outputName = output->string();
        redirects = {std::nullopt, llvm::StringRef(outputName), std::nullopt};
        redirectView = redirects;
    }
    std::string error;
    bool failed = false;
    const int status = llvm::sys::ExecuteAndWait(
        command[0], refs, std::nullopt, redirectView, 0, 0, &error, &failed);
    if (failed) throw std::runtime_error("cannot execute curl: " + error);
    if (status != 0)
        throw std::runtime_error("registry request failed with curl status " +
                                 std::to_string(status));
}

std::string IndexContents(std::vector<RegistryRelease> releases) {
    std::sort(releases.begin(), releases.end(),
              [](const auto& left, const auto& right) {
                  return left.version < right.version;
              });
    std::ostringstream output;
    output << "flecha-registry-index 1\n";
    for (const auto& release : releases)
        output << release.version.String() << '\t' << release.checksum << '\t'
               << (release.yanked ? "yanked" : "active") << '\n';
    return output.str();
}

}  // namespace

RegistryTransport::RegistryTransport() {
    const char* configured = std::getenv("LONGBOW_REGISTRY");
    _base = configured && *configured ? configured
                                      : "https://registry.flecha-lang.org/v1";
    while (_base.size() > 1 && _base.ends_with('/')) _base.pop_back();
}

bool RegistryTransport::Local() const {
    return _base.starts_with("file://") || !_base.contains("://");
}

std::string RegistryTransport::Token() const {
    const char* token = std::getenv("LONGBOW_TOKEN");
    if (!token || !*token)
        throw std::runtime_error(
            "LONGBOW_TOKEN is required for registry mutations");
    const std::string result = token;
    if (std::any_of(result.begin(), result.end(), [](unsigned char value) {
            return value < 0x20 || value == 0x7f;
        }))
        throw std::runtime_error("LONGBOW_TOKEN contains a control character");
    return result;
}

std::string RegistryTransport::Read(const std::string& resource) const {
    if (resource.empty() || resource.front() == '/' || resource.contains(".."))
        throw std::runtime_error("invalid registry resource path");
    if (Local()) {
        const fs::path root = _base.starts_with("file://")
                                  ? fs::path(_base.substr(7))
                                  : fs::path(_base);
        return ReadFile(root / fs::path(resource));
    }
    if (!_base.starts_with("https://"))
        throw std::runtime_error("public registry URL must use HTTPS");
    const fs::path output = TemporaryFile("registry-response");
    RemoveFile remove(output);
    Curl({"--proto", "=https", _base + "/" + resource}, output);
    return ReadFile(output);
}

void RegistryTransport::Publish(const std::string& identity,
                                const std::string& version,
                                const std::string& checksum,
                                const std::string& archive) const {
    if (!ValidPackageIdentity(identity))
        throw std::runtime_error("invalid registry package identity");
    std::string versionError;
    if (!SemanticVersion::Parse(version, versionError))
        throw std::runtime_error("invalid registry package version");
    const std::string resource =
        "packages/" + identity + "/" + version + "/archive";
    if (Local()) {
        (void)Token();
        const fs::path root = _base.starts_with("file://")
                                  ? fs::path(_base.substr(7))
                                  : fs::path(_base);
        const fs::path archiveFile = root / fs::path(resource);
        if (fs::exists(archiveFile))
            throw std::runtime_error(
                "published package versions are immutable");
        std::vector<RegistryRelease> releases;
        const fs::path indexFile =
            root / "packages" / fs::path(identity) / "index";
        if (fs::exists(indexFile))
            releases = ParseRegistryIndex(ReadFile(indexFile));
        WriteFile(archiveFile, archive);
        releases.push_back(
            {*SemanticVersion::Parse(version, versionError), checksum, false});
        WriteFile(indexFile, IndexContents(std::move(releases)));
        return;
    }
    if (!_base.starts_with("https://"))
        throw std::runtime_error("public registry URL must use HTTPS");
    const fs::path body = TemporaryFile("publish-body");
    const fs::path headers = TemporaryFile("publish-headers");
    RemoveFile removeBody(body), removeHeaders(headers);
    WriteFile(body, archive);
    WritePrivateFile(headers, "Authorization: Bearer " + Token() + "\n");
    Curl({"--proto", "=https", "--request", "PUT", "--header",
          "@" + headers.string(), "--header",
          "Content-Type: application/vnd.flecha.package.v1", "--header",
          "X-Flecha-Checksum: " + checksum, "--data-binary",
          "@" + body.string(), _base + "/" + resource});
}

void RegistryTransport::Yank(const std::string& identity,
                             const std::string& version) const {
    if (!ValidPackageIdentity(identity))
        throw std::runtime_error("invalid registry package identity");
    std::string versionError;
    if (!SemanticVersion::Parse(version, versionError))
        throw std::runtime_error("invalid registry package version");
    if (Local()) {
        (void)Token();
        const fs::path root = _base.starts_with("file://")
                                  ? fs::path(_base.substr(7))
                                  : fs::path(_base);
        const fs::path indexFile =
            root / "packages" / fs::path(identity) / "index";
        auto releases = ParseRegistryIndex(ReadFile(indexFile));
        const auto found = std::find_if(
            releases.begin(), releases.end(), [&](const auto& release) {
                return release.version.String() == version;
            });
        if (found == releases.end())
            throw std::runtime_error("cannot yank missing package version");
        found->yanked = true;
        WriteFile(indexFile, IndexContents(std::move(releases)));
        return;
    }
    if (!_base.starts_with("https://"))
        throw std::runtime_error("public registry URL must use HTTPS");
    const fs::path headers = TemporaryFile("yank-headers");
    RemoveFile remove(headers);
    WritePrivateFile(headers, "Authorization: Bearer " + Token() + "\n");
    Curl({"--proto", "=https", "--request", "POST", "--header",
          "@" + headers.string(),
          _base + "/packages/" + identity + "/" + version + "/yank"});
}

}  // namespace flecha::longbow
