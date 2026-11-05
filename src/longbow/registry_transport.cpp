#include "longbow/registry_transport.hpp"

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
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
    llvm::SmallString<128> path;
    const std::error_code error =
        llvm::sys::fs::createTemporaryFile("longbow-" + name, "tmp", path);
    if (error)
        throw std::runtime_error("cannot create a private temporary file: " +
                                 error.message());
    return std::string(path);
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

struct HttpResponse {
    int status = 0;
    std::string body;
};

std::string JsonString(const std::string& json, const std::string& key,
                       bool required = true) {
    const std::string label = "\"" + key + "\"";
    std::size_t position = json.find(label);
    if (position == std::string::npos) {
        if (!required) return {};
        throw std::runtime_error("registry returned malformed JSON");
    }
    position = json.find(':', position + label.size());
    if (position == std::string::npos)
        throw std::runtime_error("registry returned malformed JSON");
    position = json.find_first_not_of(" \t\r\n", position + 1);
    if (position == std::string::npos || json[position] != '"') {
        if (!required) return {};
        throw std::runtime_error("registry returned malformed JSON");
    }
    std::string value;
    for (++position; position < json.size(); ++position) {
        const char character = json[position];
        if (character == '"') return value;
        if (character != '\\') {
            value.push_back(character);
            continue;
        }
        if (++position >= json.size()) break;
        switch (json[position]) {
            case '"':
            case '\\':
            case '/':
                value.push_back(json[position]);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                throw std::runtime_error("registry returned unsupported JSON");
        }
    }
    throw std::runtime_error("registry returned malformed JSON");
}

long JsonInteger(const std::string& json, const std::string& key) {
    const std::string label = "\"" + key + "\"";
    std::size_t position = json.find(label);
    if (position == std::string::npos)
        throw std::runtime_error("registry returned malformed JSON");
    position = json.find(':', position + label.size());
    if (position == std::string::npos)
        throw std::runtime_error("registry returned malformed JSON");
    position = json.find_first_not_of(" \t\r\n", position + 1);
    std::size_t end = position;
    while (end < json.size() &&
           std::isdigit(static_cast<unsigned char>(json[end])))
        ++end;
    if (end == position)
        throw std::runtime_error("registry returned malformed JSON");
    try {
        return std::stol(json.substr(position, end - position));
    } catch (...) {
        throw std::runtime_error("registry returned invalid JSON number");
    }
}

std::string JsonEscape(const std::string& value) {
    std::string result;
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
                if (character < 0x20)
                    throw std::runtime_error(
                        "cannot encode JSON control character");
                result.push_back(static_cast<char>(character));
        }
    }
    return result;
}

void RequireTerminalSafe(const std::string& value, std::size_t maximum,
                         const std::string& description) {
    if (value.empty() || value.size() > maximum ||
        std::any_of(value.begin(), value.end(), [](unsigned char character) {
            return character < 0x20 || character == 0x7f;
        }))
        throw std::runtime_error("registry returned an invalid " + description);
}

fs::path CredentialsFile() {
    if (const char* configured = std::getenv("LONGBOW_TOKEN_FILE");
        configured && *configured)
        return configured;
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
        return fs::path(local) / "Flecha" / "Longbow" / "credentials";
#endif
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config)
        return fs::path(config) / "longbow" / "credentials";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".config" / "longbow" / "credentials";
    throw std::runtime_error(
        "cannot find a user configuration directory; set LONGBOW_TOKEN_FILE");
}

HttpResponse Http(const std::string& method, const std::string& url,
                  const std::optional<std::string>& body = std::nullopt,
                  const std::optional<std::string>& token = std::nullopt) {
    const auto executable = llvm::sys::findProgramByName("curl");
    if (!executable)
        throw std::runtime_error(
            "cannot contact the registry because curl is not installed");
    const fs::path responseFile = TemporaryFile("http-response");
    const fs::path statusFile = TemporaryFile("http-status");
    const fs::path requestFile = TemporaryFile("http-request");
    const fs::path headersFile = TemporaryFile("http-headers");
    RemoveFile removeResponse(responseFile), removeStatus(statusFile),
        removeRequest(requestFile), removeHeaders(headersFile);
    WritePrivateFile(responseFile, "");
    WritePrivateFile(statusFile, "");

    std::string headers = "Accept: application/json\n";
    if (body) headers += "Content-Type: application/json\n";
    if (token) headers += "Authorization: Bearer " + *token + "\n";
    WritePrivateFile(headersFile, headers);
    if (body) WritePrivateFile(requestFile, *body);

    std::vector<std::string> command{*executable,
                                     "--silent",
                                     "--show-error",
                                     "--connect-timeout",
                                     "15",
                                     "--max-time",
                                     "300",
                                     "--max-filesize",
                                     "33554432",
                                     "--proto",
                                     "=https",
                                     "--request",
                                     method,
                                     "--header",
                                     "@" + headersFile.string(),
                                     "--output",
                                     responseFile.string(),
                                     "--write-out",
                                     "%{http_code}"};
    if (body) {
        command.push_back("--data-binary");
        command.push_back("@" + requestFile.string());
    }
    command.push_back(url);
    std::vector<llvm::StringRef> refs;
    for (const auto& argument : command) refs.emplace_back(argument);
    const std::string statusName = statusFile.string();
    const std::array<std::optional<llvm::StringRef>, 3> redirects{
        std::nullopt, llvm::StringRef(statusName), std::nullopt};
    std::string error;
    bool failed = false;
    const int result = llvm::sys::ExecuteAndWait(
        command[0], refs, std::nullopt, redirects, 0, 0, &error, &failed);
    if (failed) throw std::runtime_error("cannot execute curl: " + error);
    if (result != 0)
        throw std::runtime_error("registry request failed with curl status " +
                                 std::to_string(result));
    HttpResponse response;
    const std::string status = ReadFile(statusFile);
    if (status.size() != 3 ||
        !std::all_of(status.begin(), status.end(),
                     [](unsigned char value) { return std::isdigit(value); }))
        throw std::runtime_error("registry returned an invalid HTTP status");
    try {
        response.status = std::stoi(status);
    } catch (...) {
        throw std::runtime_error("registry returned an invalid HTTP status");
    }
    response.body = ReadFile(responseFile);
    return response;
}

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
    std::string result;
    if (token && *token)
        result = token;
    else {
        const fs::path credentials = CredentialsFile();
        if (!fs::exists(credentials))
            throw std::runtime_error(
                "registry login is required; run 'longbow login' or set "
                "LONGBOW_TOKEN");
        result = ReadFile(credentials);
        while (!result.empty() &&
               (result.back() == '\n' || result.back() == '\r'))
            result.pop_back();
    }
    if (result.empty())
        throw std::runtime_error("stored registry credential is empty");
    if (std::any_of(result.begin(), result.end(), [](unsigned char value) {
            return value < 0x20 || value == 0x7f;
        }))
        throw std::runtime_error(
            "registry credential contains a control character");
    return result;
}

std::string RegistryTransport::Login() const {
    if (Local())
        throw std::runtime_error(
            "interactive login is unavailable for a local file registry");
    if (!_base.starts_with("https://"))
        throw std::runtime_error("public registry URL must use HTTPS");
    const HttpResponse started =
        Http("POST", _base + "/auth/device", "{\"client_id\":\"longbow-cli\"}");
    if (started.status != 200)
        throw std::runtime_error("registry refused to start device login");
    const std::string deviceCode = JsonString(started.body, "device_code");
    const std::string userCode = JsonString(started.body, "user_code");
    const std::string verification =
        JsonString(started.body, "verification_uri_complete");
    const long expires = JsonInteger(started.body, "expires_in");
    long interval = JsonInteger(started.body, "interval");
    if (expires <= 0 || expires > 3600 || interval <= 0 || interval > 60)
        throw std::runtime_error(
            "registry returned invalid device login timing");
    RequireTerminalSafe(deviceCode, 512, "device code");
    RequireTerminalSafe(userCode, 32, "user code");
    RequireTerminalSafe(verification, 2048, "verification URL");
    if (!verification.starts_with("https://"))
        throw std::runtime_error(
            "registry returned a non-HTTPS verification URL");

    std::cout << "Open " << verification << "\n"
              << "Enter code: " << userCode << "\n\n"
              << "Waiting for authorization..." << std::endl;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(expires);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(interval));
        const std::string request =
            "{\"client_id\":\"longbow-cli\",\"device_code\":\"" +
            JsonEscape(deviceCode) + "\"}";
        const HttpResponse exchanged =
            Http("POST", _base + "/auth/device/token", request);
        if (exchanged.status == 200) {
            const std::string accessToken =
                JsonString(exchanged.body, "access_token");
            if (!accessToken.starts_with("flp_") || accessToken.size() > 512)
                throw std::runtime_error(
                    "registry returned an invalid credential");
            WritePrivateFile(CredentialsFile(), accessToken + "\n");
            std::string handle = JsonString(exchanged.body, "handle");
            RequireTerminalSafe(handle, 200, "account handle");
            return handle;
        }
        const std::string error = JsonString(exchanged.body, "error", false);
        if (exchanged.status == 400 && error == "authorization_pending")
            continue;
        if (exchanged.status == 400 && error == "slow_down") {
            interval = std::min(60L, interval + 5);
            continue;
        }
        if (error == "access_denied")
            throw std::runtime_error("registry login was denied");
        if (error == "expired_token")
            throw std::runtime_error("registry login code expired");
        throw std::runtime_error(
            "registry login failed" +
            (error.empty() ? std::string{} : ": " + error));
    }
    throw std::runtime_error("registry login code expired");
}

std::string RegistryTransport::WhoAmI() const {
    if (Local())
        throw std::runtime_error(
            "identity is unavailable for a local file registry");
    const HttpResponse response =
        Http("GET", _base + "/auth/whoami", std::nullopt, Token());
    if (response.status == 401)
        throw std::runtime_error("registry credential is invalid or expired");
    if (response.status != 200)
        throw std::runtime_error("registry identity request failed");
    const std::string handle = JsonString(response.body, "handle", false);
    std::string result =
        handle.empty() ? JsonString(response.body, "subject") : handle;
    RequireTerminalSafe(result, 200, "account handle");
    return result;
}

void RegistryTransport::Logout() const {
    if (Local())
        throw std::runtime_error(
            "logout is unavailable for a local file registry");
    const std::string token = Token();
    const HttpResponse response =
        Http("POST", _base + "/auth/revoke", "{}", token);
    if (response.status != 204 && response.status != 401)
        throw std::runtime_error("registry logout failed");
    if (const char* configured = std::getenv("LONGBOW_TOKEN");
        configured && *configured)
        return;
    std::error_code error;
    fs::remove(CredentialsFile(), error);
    if (error)
        throw std::runtime_error(
            "registry token was revoked, but the local "
            "credential could not be removed: " +
            error.message());
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
