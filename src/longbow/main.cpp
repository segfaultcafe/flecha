#include <llvm/Support/Program.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "flecha/version.hpp"
#include "longbow/archive.hpp"
#include "longbow/cache.hpp"
#include "longbow/manifest.hpp"
#include "longbow/registry.hpp"
#include "longbow/registry_transport.hpp"
#include "longbow/resolver.hpp"
#include "longbow/sha256.hpp"

#ifndef FLECHA_EXECUTABLE_SUFFIX
#define FLECHA_EXECUTABLE_SUFFIX ""
#endif

#ifndef FLECHA_OBJECT_SUFFIX
#define FLECHA_OBJECT_SUFFIX ".o"
#endif

#ifndef FLECHA_INSTALL_DATADIR
#define FLECHA_INSTALL_DATADIR "share"
#endif

namespace fs = std::filesystem;
namespace {
std::optional<std::string> DeclaredModule(const fs::path& file) {
    std::ifstream stream(file);
    if (!stream) return std::nullopt;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    flecha::diagnostics::DiagnosticBag diagnostics;
    flecha::core::Parser parser(
        flecha::core::Tokenizer(buffer.str(), diagnostics).Tokenize(),
        diagnostics);
    parser.ParseHeader();
    if (diagnostics.HasErrors() || parser.ModuleName().empty())
        return std::nullopt;
    return parser.ModuleName();
}
void Write(const fs::path& file, const std::string& text) {
    std::ofstream stream(file);
    if (!stream || !(stream << text))
        throw std::runtime_error("cannot write " + file.string());
}
void Create(const fs::path& directory, bool library, bool initialize) {
    const fs::path absolute = fs::absolute(directory).lexically_normal();
    const std::string name = absolute.filename().string();
    if (!flecha::longbow::ValidProjectName(name))
        throw std::runtime_error(
            "project directory name must start with a letter or digit and "
            "contain only letters, digits, spaces, hyphens, underscores, or "
            "dots");
    if (!initialize && fs::exists(absolute))
        throw std::runtime_error("destination already exists: " +
                                 absolute.string());
    if (initialize && !fs::is_directory(absolute))
        throw std::runtime_error("init requires an existing directory");
    // Preflight every generated path before writing any files. Existing source
    // trees are never adopted or overwritten implicitly by init.
    for (const auto& file : {absolute / "longbow.toml", absolute / "src"})
        if (fs::exists(file) || fs::is_symlink(file))
            throw std::runtime_error("refusing to overwrite " + file.string());
    fs::create_directories(absolute / "src");
    Write(absolute / "longbow.toml",
          "[package]\nname = \"" + name + "\"\nversion = \"0.1.0\"\nkind = \"" +
              (library ? "library" : "program") + "\"\n\n[dependencies]\n");
    const std::string module = flecha::longbow::ModuleName(name);
    Write(absolute / "src" / (library ? module + ".fl" : "main.fl"),
          library ? "module " + module +
                        ";\n\nexport i32 answer() {\n    return 42;\n}\n"
                  : "module main;\n\nvoid main() {\n    stdoutln(\"Hello from "
                    "Flecha!\");\n}\n");
    if (!fs::exists(absolute / ".gitignore") &&
        !fs::is_symlink(absolute / ".gitignore"))
        Write(absolute / ".gitignore", "/target/\n");
    std::cout << "Created " << (library ? "library " : "program ") << name
              << "\n";
}
fs::path ProjectRoot(const std::optional<fs::path>& requested) {
    fs::path start = requested ? fs::absolute(*requested).lexically_normal()
                               : fs::current_path();
    if (requested && !fs::is_directory(start))
        throw std::runtime_error("project path is not a directory: " +
                                 start.string());
    for (fs::path directory = start;; directory = directory.parent_path()) {
        if (fs::is_regular_file(directory / "longbow.toml")) return directory;
        if (requested) break;
        if (directory == directory.parent_path()) break;
    }
    throw std::runtime_error(
        "no longbow.toml found in this directory or its parents");
}
int Execute(const std::vector<std::string>& arguments,
            const std::optional<fs::path>& output = std::nullopt) {
    std::vector<llvm::StringRef> refs;
    for (const auto& argument : arguments) refs.emplace_back(argument);
    std::string error;
    bool failed = false;
    std::string outputName;
    std::array<std::optional<llvm::StringRef>, 3> redirects;
    llvm::ArrayRef<std::optional<llvm::StringRef>> redirectView;
    if (output) {
        outputName = output->string();
        redirects = {std::nullopt, llvm::StringRef(outputName), std::nullopt};
        redirectView = redirects;
    }
    const int status = llvm::sys::ExecuteAndWait(
        arguments[0], refs, std::nullopt, redirectView, 0, 0, &error, &failed);
    if (failed)
        throw std::runtime_error("cannot execute " + arguments[0] + ": " +
                                 error);
    return status < 0 ? 1 : status;
}
std::string ReadFile(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read " + file.string());
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}
class TemporaryDirectory final {
   public:
    explicit TemporaryDirectory(const fs::path& parent) {
        fs::create_directories(parent);
        for (unsigned attempt = 0; attempt != 100; ++attempt) {
            _path = parent /
                    ("git-" +
                     std::to_string(std::chrono::high_resolution_clock::now()
                                        .time_since_epoch()
                                        .count()) +
                     "-" + std::to_string(attempt));
            std::error_code error;
            if (fs::create_directory(_path, error)) return;
            if (error && error != std::errc::file_exists)
                throw std::runtime_error(
                    "cannot create Git staging directory: " + error.message());
        }
        throw std::runtime_error("cannot allocate Git staging directory");
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(_path, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    [[nodiscard]] const fs::path& Path() const { return _path; }

   private:
    fs::path _path;
};
std::string GitExecutable() {
    const auto git = llvm::sys::findProgramByName("git");
    if (!git)
        throw std::runtime_error(
            "cannot fetch Git dependency because git is not installed");
    return *git;
}
void RequireGit(const std::vector<std::string>& arguments,
                const std::optional<fs::path>& output = std::nullopt) {
    if (Execute(arguments, output) != 0)
        throw std::runtime_error("Git dependency command failed: " +
                                 arguments.at(1));
}
std::vector<std::string> GitTreeFiles(const std::string& listing) {
    std::vector<std::string> files;
    std::set<std::string> portableNames;
    std::size_t offset = 0;
    while (offset < listing.size()) {
        const std::size_t end = listing.find('\0', offset);
        if (end == std::string::npos)
            throw std::runtime_error("Git produced a malformed tree listing");
        const std::string_view entry(listing.data() + offset, end - offset);
        const std::size_t first = entry.find(' ');
        if (first == std::string_view::npos)
            throw std::runtime_error("Git produced a malformed tree listing");
        const std::size_t second = entry.find(' ', first + 1);
        if (second == std::string_view::npos)
            throw std::runtime_error("Git produced a malformed tree listing");
        const std::size_t tab = entry.find('\t', second + 1);
        const std::string_view mode = entry.substr(0, first);
        if (tab == std::string_view::npos ||
            entry.substr(first + 1, second - first - 1) != "blob" ||
            (mode != "100644" && mode != "100755"))
            throw std::runtime_error(
                "Git package contains a submodule, symbolic link, or "
                "unsupported tree entry");
        const std::string path(entry.substr(tab + 1));
        if (path.empty() || path.front() == '/' || path.contains('\\'))
            throw std::runtime_error("Git package contains an unsafe path");
        const fs::path parsed(path);
        if (parsed.is_absolute() || parsed.has_root_name() ||
            parsed.has_root_directory())
            throw std::runtime_error("Git package contains an unsafe path");
        for (const auto& component : parsed) {
            std::string name = component.string();
            std::transform(
                name.begin(), name.end(), name.begin(),
                [](unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            if (component.empty() || component == "." || component == ".." ||
                name == ".git")
                throw std::runtime_error("Git package contains an unsafe path");
        }
        std::string portable = path;
        std::transform(portable.begin(), portable.end(), portable.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        if (!portableNames.insert(portable).second)
            throw std::runtime_error(
                "Git package contains case-colliding paths");
        files.push_back(path);
        offset = end + 1;
    }
    return files;
}
flecha::longbow::AcquiredDependency AcquireGit(
    const flecha::longbow::Manifest& importer,
    const flecha::longbow::Dependency& dependency,
    const flecha::longbow::PackageCache& cache) {
    const std::string& declaredUrl = *dependency.git;
    std::string url = declaredUrl;
    if (declaredUrl.contains("::"))
        throw std::runtime_error(
            "Git remote-helper transports are not allowed in dependencies");
    bool remote = false;
    if (const std::size_t scheme = declaredUrl.find("://");
        scheme != std::string::npos) {
        const std::string protocol = declaredUrl.substr(0, scheme);
        if (protocol != "https" && protocol != "ssh" && protocol != "git" &&
            protocol != "file")
            throw std::runtime_error("unsupported Git dependency protocol '" +
                                     protocol + "'");
        remote = true;
    } else if (const std::size_t colon = declaredUrl.find(':');
               colon != std::string::npos && !declaredUrl.starts_with("./") &&
               !declaredUrl.starts_with("../")) {
        if (colon == 0 || declaredUrl.substr(0, colon).contains('/'))
            throw std::runtime_error("invalid Git dependency location");
        remote = true;
    }
    if (!remote && fs::path(declaredUrl).is_relative())
        url = fs::absolute(importer.directory / declaredUrl)
                  .lexically_normal()
                  .string();
    const std::string& requested = *dependency.revision;
    try {
        if (const auto cached = cache.FindGit(url, requested))
            return {cached->directory,
                    flecha::longbow::ResolvedSourceKind::Git,
                    "git/" + flecha::longbow::Sha256(url).substr(0, 16),
                    declaredUrl,
                    cached->revision,
                    cached->checksum};
    } catch (const std::runtime_error&) {
        // A reachable origin can repair an incomplete reference or corrupt
        // content entry. The fetch below still has to pass full verification.
    }

    const std::string git = GitExecutable();
    TemporaryDirectory staging(cache.Root() / "staging");
    const fs::path repository = staging.Path() / "repository.git";
    const fs::path exported = staging.Path() / "source";
    RequireGit(
        {git, "clone", "--bare", "--quiet", "--", url, repository.string()});

    const fs::path revisionOutput = staging.Path() / "revision";
    RequireGit({git, "-C", repository.string(), "rev-parse", "--verify",
                requested + "^{commit}"},
               revisionOutput);
    std::string revision = ReadFile(revisionOutput);
    while (!revision.empty() &&
           (revision.back() == '\n' || revision.back() == '\r'))
        revision.pop_back();
    if ((revision.size() != 40 && revision.size() != 64) ||
        !std::all_of(revision.begin(), revision.end(), [](char value) {
            return (value >= '0' && value <= '9') ||
                   (value >= 'a' && value <= 'f');
        }))
        throw std::runtime_error("Git did not resolve an exact commit hash");

    const fs::path treeOutput = staging.Path() / "tree";
    RequireGit({git, "-C", repository.string(), "ls-tree", "-r", "-z",
                "--full-tree", revision},
               treeOutput);
    fs::create_directories(exported);
    const std::string listing = ReadFile(treeOutput);
    for (const std::string& path : GitTreeFiles(listing)) {
        // Resolve the blob by commit and path. No checkout means repository
        // hooks, submodules, and clean/smudge filters are never executed.
        const fs::path destination = exported / fs::path(path);
        fs::create_directories(destination.parent_path());
        RequireGit(
            {git, "-C", repository.string(), "show", revision + ":" + path},
            destination);
    }
    if (!fs::is_regular_file(exported / "longbow.toml") ||
        !fs::is_directory(exported / "src"))
        throw std::runtime_error(
            "Git dependency commit is not a Longbow package");
    const std::string checksum =
        "sha256:" + flecha::longbow::Sha256Tree(exported);
    const fs::path stored = cache.Store(exported, checksum);
    cache.StoreGit(url, requested, revision, checksum);
    return {stored,
            flecha::longbow::ResolvedSourceKind::Git,
            "git/" + flecha::longbow::Sha256(url).substr(0, 16),
            declaredUrl,
            revision,
            checksum};
}
std::string Compiler(const char* executable) {
    if (const char* configured = std::getenv("FLECHA_COMPILER"))
        return configured;
    auto resolved = llvm::sys::findProgramByName(executable);
    fs::path binary = resolved ? fs::path(*resolved) : fs::absolute(executable);
    const fs::path sibling =
        binary.parent_path() / ("flecha" FLECHA_EXECUTABLE_SUFFIX);
    if (fs::is_regular_file(sibling)) return sibling.string();
    auto compiler =
        llvm::sys::findProgramByName("flecha" FLECHA_EXECUTABLE_SUFFIX);
    if (compiler) return *compiler;
    throw std::runtime_error(
        "cannot find flecha; place it beside longbow or set FLECHA_COMPILER");
}
std::optional<fs::path> StandardLibrary(const char* executable) {
    if (const char* configured = std::getenv("FLECHA_STANDARD_LIBRARY")) {
        fs::path source = fs::absolute(configured).lexically_normal();
        if (!fs::is_directory(source))
            throw std::runtime_error(
                "FLECHA_STANDARD_LIBRARY is not a source directory: " +
                source.string());
        return source;
    }

    auto resolved = llvm::sys::findProgramByName(executable);
    fs::path binary = resolved ? fs::path(*resolved) : fs::absolute(executable);
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(binary, error);
    if (!error) binary = canonical;

    const fs::path dataDirectory = FLECHA_INSTALL_DATADIR;
    const fs::path source =
        ((dataDirectory.is_absolute()
              ? dataDirectory
              : binary.parent_path().parent_path() / dataDirectory) /
         "flecha" / "std" / "src")
            .lexically_normal();
    if (fs::is_directory(source)) return source;
    return std::nullopt;
}
class CompilerGraph final {
   public:
    CompilerGraph(const flecha::longbow::ResolvedGraph& graph,
                  const fs::path& target, bool persistent) {
        if (persistent)
            _file = target / "package-graph.json";
        else {
            _temporary = true;
            _file = fs::temp_directory_path() /
                    ("longbow-package-graph-" +
                     std::to_string(std::chrono::high_resolution_clock::now()
                                        .time_since_epoch()
                                        .count()) +
                     ".json");
        }
        graph.WriteCompilerGraph(_file);
    }
    ~CompilerGraph() {
        if (!_temporary) return;
        std::error_code error;
        fs::remove(_file, error);
    }
    CompilerGraph(const CompilerGraph&) = delete;
    CompilerGraph& operator=(const CompilerGraph&) = delete;
    [[nodiscard]] const fs::path& File() const { return _file; }

   private:
    fs::path _file;
    bool _temporary = false;
};
void Usage() {
    std::cout << "usage: longbow new NAME [--lib]\n"
                 "       longbow init [--lib]\n"
                 "       longbow add OWNER/PACKAGE@REQUIREMENT [--as ALIAS]\n"
                 "       longbow remove ALIAS\n"
                 "       longbow fetch [PROJECT]\n"
                 "       longbow update [ALIAS]\n"
                 "       longbow publish [PROJECT] [--dry-run]\n"
                 "       longbow yank VERSION\n"
                 "       longbow check|build|clean [PROJECT]\n"
                 "       longbow test [PROJECT] [--verbose]\n"
                 "       longbow run [PROJECT] [-- program arguments]\n";
}
int Run(int argc, char** argv) {
    if (argc < 2) {
        Usage();
        return 1;
    }
    const std::string command = argv[1];
    if (command == "--version") {
        if (argc != 2) {
            Usage();
            return 1;
        }
        std::cout << "longbow " << flecha::VERSION << "\n";
        return 0;
    }
    if (command == "--help" || command == "help") {
        Usage();
        return 0;
    }
    if (command == "new" || command == "init") {
        const bool initialize = command == "init";
        const int flag = initialize ? 2 : 3;
        if (argc < flag || argc > flag + 1 ||
            (argc == flag + 1 && std::string(argv[flag]) != "--lib")) {
            Usage();
            return 1;
        }
        Create(initialize ? fs::current_path() : fs::path(argv[2]),
               argc == flag + 1, initialize);
        return 0;
    }
    if (command == "add") {
        if (argc != 3 && argc != 5) {
            Usage();
            return 1;
        }
        const std::string specification = argv[2];
        const std::size_t separator = specification.rfind('@');
        if (separator == std::string::npos || separator == 0 ||
            separator + 1 == specification.size() ||
            (argc == 5 && std::string(argv[3]) != "--as"))
            throw std::runtime_error(
                "add requires OWNER/PACKAGE@REQUIREMENT [--as ALIAS]");
        const std::string identity = specification.substr(0, separator);
        std::string alias =
            argc == 5 ? argv[4] : identity.substr(identity.find('/') + 1);
        std::replace(alias.begin(), alias.end(), '-', '_');
        const fs::path root = ProjectRoot(std::nullopt);
        flecha::longbow::AddRegistryDependency(
            root, identity, specification.substr(separator + 1), alias);
        std::cout << "Added " << alias << " -> " << identity << " "
                  << specification.substr(separator + 1) << "\n";
        return 0;
    }
    if (command == "remove") {
        if (argc != 3) {
            Usage();
            return 1;
        }
        const fs::path root = ProjectRoot(std::nullopt);
        flecha::longbow::RemoveDependency(root, argv[2]);
        std::cout << "Removed " << argv[2] << "\n";
        return 0;
    }
    if (command == "publish") {
        bool dryRun = false;
        std::optional<fs::path> project;
        for (int index = 2; index < argc; ++index) {
            if (std::string(argv[index]) == "--dry-run") {
                if (dryRun) throw std::runtime_error("duplicate --dry-run");
                dryRun = true;
            } else if (!project)
                project = fs::path(argv[index]);
            else {
                Usage();
                return 1;
            }
        }
        const fs::path root = ProjectRoot(project);
        const auto manifest = flecha::longbow::ReadManifest(root);
        const auto archive = flecha::longbow::BuildPackageArchive(root);
        std::cout << *manifest.registry << "@" << manifest.version.String()
                  << "\n";
        for (const auto& file : archive.files)
            std::cout << "  " << file << "\n";
        std::cout << archive.checksum << "\n";
        if (!dryRun) {
            const flecha::longbow::RegistryTransport transport;
            transport.Publish(*manifest.registry, manifest.version.String(),
                              archive.checksum, archive.bytes);
            std::cout << "Published\n";
        }
        return 0;
    }
    if (command == "yank") {
        if (argc != 3) {
            Usage();
            return 1;
        }
        const fs::path root = ProjectRoot(std::nullopt);
        const auto manifest = flecha::longbow::ReadManifest(root);
        if (!manifest.registry)
            throw std::runtime_error(
                "package.registry is required for yanking");
        const flecha::longbow::RegistryTransport transport;
        transport.Yank(*manifest.registry, argv[2]);
        std::cout << "Yanked " << *manifest.registry << "@" << argv[2] << "\n";
        return 0;
    }
    if (command != "check" && command != "build" && command != "run" &&
        command != "test" && command != "clean" && command != "fetch" &&
        command != "update") {
        Usage();
        return 1;
    }
    int argument = 2;
    std::optional<fs::path> project;
    std::optional<std::string> updateAlias;
    if (command == "update" && argument < argc)
        updateAlias = argv[argument++];
    else if (argument < argc && std::string(argv[argument]) != "--" &&
             std::string(argv[argument]) != "--verbose")
        project = fs::path(argv[argument++]);
    bool verboseTests = false;
    if (command == "test" && argument < argc &&
        std::string(argv[argument]) == "--verbose") {
        verboseTests = true;
        ++argument;
    }
    int runArguments = argc;
    if (command == "run" && argument < argc &&
        std::string(argv[argument]) == "--") {
        runArguments = ++argument;
        argument = argc;
    }
    if (argument != argc) {
        Usage();
        return 1;
    }
    const fs::path root = ProjectRoot(project);
    const flecha::longbow::Manifest manifest =
        flecha::longbow::ReadManifest(root);
    if (updateAlias) {
        const auto found = manifest.dependencies.find(*updateAlias);
        if (found == manifest.dependencies.end() ||
            found->second.kind != flecha::longbow::DependencyKind::Registry)
            throw std::runtime_error(
                "update alias is not a registry dependency: " + *updateAlias);
    }
    const fs::path target = root / "target";
    if (fs::is_symlink(target))
        throw std::runtime_error("target must not be a symbolic link");
    if (command == "clean") {
        fs::remove_all(target);
        return 0;
    }
    const flecha::longbow::PackageCache cache;
    const flecha::longbow::RegistryTransport registryTransport;
    auto registryLock =
        flecha::longbow::ReadRegistryLock(root / "longbow.lock");
    if (command == "update") {
        if (updateAlias)
            registryLock.erase(*manifest.dependencies.at(*updateAlias).package);
        else
            registryLock.clear();
    }
    flecha::longbow::RegistryResolver registryResolver(
        registryTransport.Base(), cache,
        [&](const std::string& resource) {
            return registryTransport.Read(resource);
        },
        std::move(registryLock));
    const auto packages = flecha::longbow::ResolveLocalPackages(
        manifest, StandardLibrary(argv[0]),
        [&](const flecha::longbow::Manifest& importer,
            const flecha::longbow::Dependency& dependency) {
            if (dependency.kind == flecha::longbow::DependencyKind::Git)
                return AcquireGit(importer, dependency, cache);
            return registryResolver.Acquire(dependency);
        });
    std::string toolchainVersionError;
    const auto toolchainVersion = flecha::longbow::SemanticVersion::Parse(
        flecha::VERSION, toolchainVersionError);
    if (!toolchainVersion)
        throw std::runtime_error("invalid Flecha toolchain version '" +
                                 std::string(flecha::VERSION) +
                                 "': " + toolchainVersionError);
    for (const auto& [id, package] : packages.Packages()) {
        (void)id;
        if (package.flecha.empty()) continue;
        std::string requirementError;
        const auto requirement = flecha::longbow::VersionRequirement::Parse(
            package.flecha, requirementError);
        if (!requirement || !requirement->Matches(*toolchainVersion))
            throw std::runtime_error(
                "package '" + package.identity + "' requires Flecha " +
                package.flecha + ", but the toolchain is " + flecha::VERSION);
    }
    if (command != "test") packages.WriteLock(root / "longbow.lock", root);
    if (command == "fetch" || command == "update") return 0;
    const CompilerGraph compilerGraph(packages, target,
                                      command == "build" || command == "run");
    if (command == "test") {
        std::vector<fs::path> tests;
        const fs::path sourceRoot = root / "src";
        if (fs::is_directory(sourceRoot))
            for (const auto& item :
                 fs::recursive_directory_iterator(sourceRoot)) {
                if (!item.is_regular_file() || item.path().extension() != ".fl")
                    continue;
                const auto module = DeclaredModule(item.path());
                if (!module || !module->ends_with("_test")) continue;
                tests.push_back(fs::weakly_canonical(item.path()));
            }
        std::sort(tests.begin(), tests.end());
        std::size_t passed = 0;
        std::size_t failed = 0;
        for (const fs::path& test : tests) {
            std::vector<std::string> compile{Compiler(argv[0]), "test",
                                             test.string(), "--package-graph",
                                             compilerGraph.File().string()};
            if (verboseTests) compile.push_back("--verbose");
            for (const auto& [name, input] : manifest.native) {
                (void)name;
                compile.push_back("--link");
                compile.push_back(input.lexically_normal().string());
            }
            if (Execute(compile) == 0)
                ++passed;
            else
                ++failed;
        }
        if (tests.size() > 1) {
            const std::size_t total = passed + failed;
            if (failed == 0)
                std::cout << "\nOK [" << passed << "/" << total
                          << "] test files passed\n";
            else
                std::cout << "\nFAIL [" << passed << "/" << total
                          << "] test files passed, but [" << failed << "/"
                          << total << "] test files failed\n";
        } else if (tests.empty())
            std::cout << "no test files found\n";
        return failed == 0 ? 0 : 1;
    }
    const bool binary = manifest.kind == flecha::longbow::PackageKind::Program;
    const fs::path entry =
        root / "src" /
        (binary ? "main.fl"
                : flecha::longbow::ModuleName(manifest.name) + ".fl");
    if (!fs::is_regular_file(entry))
        throw std::runtime_error("missing entry module " + entry.string());
    if (command == "run" && !binary)
        throw std::runtime_error("a library cannot be run");
    const fs::path output =
        target / (manifest.name +
                  (binary ? FLECHA_EXECUTABLE_SUFFIX : FLECHA_OBJECT_SUFFIX));
    std::vector<std::string> compile{Compiler(argv[0]),
                                     command == "check" ? "check"
                                     : binary           ? "build"
                                                        : "--emit-object",
                                     entry.string(), "--package-graph",
                                     compilerGraph.File().string()};
    if (command != "check") {
        fs::create_directories(target);
        compile.push_back("-o");
        compile.push_back(output.string());
    }
    if (binary && command != "check") {
        for (const auto& [name, input] : manifest.native) {
            (void)name;
            compile.push_back("--link");
            compile.push_back(input.lexically_normal().string());
        }
    }
    const int result = Execute(compile);
    if (result != 0 || command != "run") return result;
    std::vector<std::string> run{output.string()};
    for (int index = runArguments; index < argc; ++index)
        run.push_back(argv[index]);
    return Execute(run);
}
}  // namespace
int main(int argc, char** argv) {
    try {
        return Run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "longbow: " << error.what() << "\n";
        return 1;
    }
}
