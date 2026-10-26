#include <llvm/Support/Program.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "flecha/version.hpp"

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
bool ValidName(const std::string& name) {
    flecha::diagnostics::DiagnosticBag diagnostics;
    const auto tokens = flecha::core::Tokenizer(name, diagnostics).Tokenize();
    return std::regex_match(name, std::regex("[a-z][a-z0-9_]*")) &&
           name != "root" && name != "lib" && tokens.size() == 2 &&
           tokens[0].type == TokenType::Identifier;
}
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
std::string Trim(const std::string& text) {
    const auto start = text.find_first_not_of(" \t\r\n");
    return start == std::string::npos
               ? ""
               : text.substr(start,
                             text.find_last_not_of(" \t\r\n") - start + 1);
}
struct Manifest {
    std::string name;
    std::string kind = "program";
    std::map<std::string, fs::path> dependencies;
    std::map<std::string, fs::path> native;
};
Manifest ReadManifest(const fs::path& directory) {
    std::ifstream stream(directory / "longbow.toml");
    if (!stream)
        throw std::runtime_error("cannot read " +
                                 (directory / "longbow.toml").string());
    Manifest manifest;
    std::string section, line;
    std::set<std::string> keys, sections;
    unsigned number = 0;
    const std::regex stringValue(
        R"re((name|version|kind)\s*=\s*"([^"\\]*)"\s*)re");
    const std::regex dependency(
        R"re(([a-z][a-z0-9_]*)\s*=\s*\{\s*path\s*=\s*"([^"\\]+)"\s*\}\s*)re");
    while (std::getline(stream, line)) {
        ++number;
        // Comments are recognized only outside quoted strings.
        bool quoted = false;
        for (std::size_t index = 0; index < line.size(); ++index) {
            if (line[index] == '"') quoted = !quoted;
            if (line[index] == '#' && !quoted) {
                line.resize(index);
                break;
            }
        }
        line = Trim(line);
        if (line.empty()) continue;
        if (line == "[package]" || line == "[dependencies]" ||
            line == "[native]") {
            if (!sections.insert(line).second)
                throw std::runtime_error("duplicate manifest section " + line);
            section = line;
            continue;
        }
        std::smatch match;
        if (section == "[package]" &&
            std::regex_match(line, match, stringValue)) {
            if (!keys.insert(section + match[1].str()).second)
                throw std::runtime_error("duplicate package key");
            if (match[1] == "name") manifest.name = match[2];
            if (match[1] == "kind") manifest.kind = match[2];
        } else if (section == "[dependencies]" &&
                   std::regex_match(line, match, dependency)) {
            const std::string name = match[1];
            if (!ValidName(name) ||
                !manifest.dependencies
                     .emplace(name, directory / match[2].str() / "src")
                     .second)
                throw std::runtime_error("invalid or duplicate dependency '" +
                                         name + "'");
        } else if (section == "[native]" &&
                   std::regex_match(line, match, dependency)) {
            const std::string name = match[1];
            if (!ValidName(name) ||
                !manifest.native.emplace(name, directory / match[2].str())
                     .second)
                throw std::runtime_error("invalid or duplicate native input '" +
                                         name + "'");
        } else
            throw std::runtime_error("unsupported manifest syntax at line " +
                                     std::to_string(number));
    }
    if (!ValidName(manifest.name))
        throw std::runtime_error(
            "package.name must be a lowercase identifier (root and lib are "
            "reserved)");
    if (manifest.kind != "program" && manifest.kind != "library")
        throw std::runtime_error("package.kind must be program or library");
    return manifest;
}
void Write(const fs::path& file, const std::string& text) {
    std::ofstream stream(file);
    if (!stream || !(stream << text))
        throw std::runtime_error("cannot write " + file.string());
}
void Create(const fs::path& directory, bool library, bool initialize) {
    const fs::path absolute = fs::absolute(directory).lexically_normal();
    const std::string name = absolute.filename().string();
    if (!ValidName(name))
        throw std::runtime_error(
            "project directory name must be a lowercase identifier (root and "
            "lib are reserved)");
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
    Write(absolute / "src" / (library ? name + ".fl" : "main.fl"),
          library ? "module " + name +
                        ";\n\nexport i32 answer() {\n    return 42;\n}\n"
                  : "module main;\n\nvoid main() {\n    stdoutln(\"Hello from "
                    "Flecha!\");\n}\n");
    if (!fs::exists(absolute / ".gitignore") &&
        !fs::is_symlink(absolute / ".gitignore"))
        Write(absolute / ".gitignore", "/target/\n");
    std::cout << "Created " << (library ? "library " : "program ") << name
              << "\n";
}
fs::path ProjectRoot() {
    for (fs::path directory = fs::current_path();;
         directory = directory.parent_path()) {
        if (fs::is_regular_file(directory / "longbow.toml")) return directory;
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
void Usage() {
    std::cout << "usage: longbow new NAME [--lib]\n"
                 "       longbow init [--lib]\n"
                 "       longbow check|build|run|clean\n"
                 "       longbow test [--verbose]\n"
                 "       longbow run -- [program arguments]\n";
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
    const bool verboseTests =
        command == "test" && argc == 3 && std::string(argv[2]) == "--verbose";
    if (argc > 2 && !verboseTests &&
        (command != "run" || std::string(argv[2]) != "--")) {
        Usage();
        return 1;
    }
    const fs::path root = ProjectRoot();
    const Manifest manifest = ReadManifest(root);
    const fs::path target = root / "target";
    if (fs::is_symlink(target))
        throw std::runtime_error("target must not be a symbolic link");
    if (command == "clean") {
        fs::remove_all(target);
        return 0;
    }
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
                                             test.string(), "--root",
                                             sourceRoot.string()};
            if (verboseTests) compile.push_back("--verbose");
            if (!manifest.dependencies.contains("std")) {
                if (const auto standardLibrary = StandardLibrary(argv[0])) {
                    compile.push_back("--lib");
                    compile.push_back("std=" + standardLibrary->string());
                }
            }
            for (const auto& [name, source] : manifest.dependencies) {
                compile.push_back("--lib");
                compile.push_back(name + "=" + source.string());
            }
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
    const bool binary = manifest.kind == "program";
    const fs::path entry =
        root / "src" / (binary ? "main.fl" : manifest.name + ".fl");
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
                                     entry.string(), "--root",
                                     (root / "src").string()};
    if (!manifest.dependencies.contains("std")) {
        if (const auto standardLibrary = StandardLibrary(argv[0])) {
            compile.push_back("--lib");
            compile.push_back("std=" + standardLibrary->string());
        }
    }
    for (const auto& [name, source] : manifest.dependencies) {
        compile.push_back("--lib");
        compile.push_back(name + "=" + source.string());
    }
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
    for (int index = 3; index < argc; ++index) run.push_back(argv[index]);
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
