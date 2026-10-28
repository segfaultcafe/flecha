#include <llvm/Support/Program.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "flecha/version.hpp"
#include "longbow/manifest.hpp"
#include "longbow/resolver.hpp"

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
    for (fs::path directory = start;;
         directory = directory.parent_path()) {
        if (fs::is_regular_file(directory / "longbow.toml")) return directory;
        if (requested) break;
        if (directory == directory.parent_path()) break;
    }
    throw std::runtime_error(
        "no longbow.toml found in this directory or its parents");
}
int Execute(const std::vector<std::string>& arguments) {
    std::vector<llvm::StringRef> refs;
    for (const auto& argument : arguments) refs.emplace_back(argument);
    std::string error;
    bool failed = false;
    const int status = llvm::sys::ExecuteAndWait(
        arguments[0], refs, std::nullopt, {}, 0, 0, &error, &failed);
    if (failed)
        throw std::runtime_error("cannot execute " + arguments[0] + ": " +
                                 error);
    return status < 0 ? 1 : status;
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
    if (command != "check" && command != "build" && command != "run" &&
        command != "test" && command != "clean") {
        Usage();
        return 1;
    }
    int argument = 2;
    std::optional<fs::path> project;
    if (argument < argc && std::string(argv[argument]) != "--" &&
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
    const fs::path target = root / "target";
    if (fs::is_symlink(target))
        throw std::runtime_error("target must not be a symbolic link");
    if (command == "clean") {
        fs::remove_all(target);
        return 0;
    }
    const auto packages = flecha::longbow::ResolveLocalPackages(
        manifest, StandardLibrary(argv[0]));
    const CompilerGraph compilerGraph(packages, target,
                                      command == "build" || command == "run");
    if (command == "test") {
        std::vector<fs::path> tests;
        const fs::path sourceRoot = root / "src";
        if (fs::is_directory(sourceRoot))
            for (const auto& item :
                 fs::recursive_directory_iterator(sourceRoot)) {
                if (!item.is_regular_file() ||
                    item.path().extension() != ".fl")
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
