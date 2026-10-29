#include "core/module_loader.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>

#include "core/parser.hpp"
#include "core/tokenizer.hpp"

namespace flecha::core {
namespace fs = std::filesystem;
namespace {
struct ModuleHeader final {
    std::string name;
    bool privateModule = false;
};

struct ParsedSource final {
    fs::path file;
    std::string sourceId;
    std::unique_ptr<CompilationUnit> unit;
};

std::optional<std::string> ReadSource(const fs::path& file) {
    std::ifstream stream(file);
    if (!stream) return std::nullopt;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

ModuleHeader ReadHeader(const fs::path& file) {
    const auto source = ReadSource(file);
    if (!source) return {};
    diagnostics::DiagnosticBag ignored;
    Parser parser(Tokenizer(*source, ignored).Tokenize(), ignored);
    parser.ParseHeader();
    return {parser.ModuleName(), parser.PrivateModule()};
}

bool IsTestModule(std::string_view name) { return name.ends_with("_test"); }

bool IsStandardTestingImport(const Import& import) {
    return import.path ==
           std::vector<std::string>{"lib", "std", "tests", "testing"};
}

std::string LogicalKey(const std::string& sourceIdentity, const fs::path& root,
                       const fs::path& directory, const std::string& name) {
    return sourceIdentity + "\n" + root.generic_string() + "\n" +
           directory.generic_string() + "\n" + name;
}

std::string DotLeader(std::string label) {
    label += " ";
    constexpr std::size_t statusColumn = 52;
    label.append(statusColumn > label.size() ? statusColumn - label.size() : 3,
                 '.');
    return label + " ";
}

std::string DisplayPath(const fs::path& file) {
    std::error_code error;
    const fs::path relative = fs::relative(file, fs::current_path(), error);
    if (!error && !relative.empty() && *relative.begin() != "..")
        return relative.generic_string();
    return file.generic_string();
}

std::string StringLiteral(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        if (character == '\\' || character == '"') escaped += '\\';
        escaped += character;
    }
    return escaped;
}

std::string TestHarnessSource(
    const std::vector<const FunctionDeclaration*>& tests,
    const fs::path& entryFile, bool verbose) {
    const std::string fileName = entryFile.filename().string();
    const std::string displayPath = DisplayPath(entryFile);
    const std::string escapedFileName = StringLiteral(fileName);
    const std::string escapedDisplayPath = StringLiteral(displayPath);

    std::ostringstream source;
    source << "i32 __flecha_test_main() { usize __flecha_passed = 0; "
              "usize __flecha_failed = 0; ";
    struct TestContext final {
        const FunctionDeclaration* function;
        std::string variable;
    };
    std::vector<TestContext> contexts;
    for (std::size_t index = 0; index < tests.size(); ++index) {
        const auto& function = *tests[index];
        const std::string context =
            "__flecha_test_context_" + std::to_string(index);
        contexts.push_back({&function, context});
        source << "testing::UnitTest " << context
               << " = testing::UnitTest::init(\"" << function.Name() << "\", "
               << (verbose ? "true" : "false") << "); " << function.Name()
               << "(&" << context << "); if " << context
               << ".failed() { __flecha_failed++; } else { "
                  "__flecha_passed++; } ";
    }
    source << "if __flecha_failed != 0 { ";
    source << "stdoutln(\"" << escapedFileName << "\"); ";
    for (const auto& context : contexts) {
        const auto& function = *context.function;
        const std::string functionPath =
            function.Range().start.file.empty()
                ? escapedDisplayPath
                : StringLiteral(DisplayPath(function.Range().start.file));
        source << "if " << context.variable << ".failed() { "
               << "stdoutln(\"" << DotLeader("  " + function.Name())
               << "fail\"); stdoutf(\"    test: " << functionPath << ":"
               << function.NameRange().start.line << ":"
               << function.NameRange().start.column << "\\n\"); "
               << context.variable << ".printOutput(); } ";
        if (verbose)
            source << "else { stdoutln(\"" << DotLeader("  " + function.Name())
                   << "ok\"); " << context.variable << ".printOutput(); } ";
    }
    source << "stdoutln(\"\"); ";
    source << "stdoutf(\"FAIL [{}/" << tests.size()
           << "] tests passed, but [{}/" << tests.size()
           << "] failed\\n\", __flecha_passed, __flecha_failed); return 1; } ";
    if (verbose) {
        source << "stdoutln(\"" << escapedFileName << "\"); ";
        for (const auto& context : contexts)
            source << "stdoutln(\""
                   << DotLeader("  " + context.function->Name()) << "ok\"); "
                   << context.variable << ".printOutput(); ";
        source << "stdoutln(\"\"); ";
    } else
        source << "stdout(\"" << StringLiteral(DotLeader(fileName)) << "\"); ";
    source << "stdoutf(\"OK [{}/" << tests.size()
           << "] tests passed\\n\", __flecha_passed); return 0; }";
    return source.str();
}
}  // namespace

void ModuleLoader::Report(source::SourceRange range,
                          const std::string& message) {
    _diagnostics.Report(diagnostics::DiagnosticCode::UnknownName, range,
                        message);
}

std::unique_ptr<CompilationUnit> ModuleLoader::Load(
    const fs::path& entry, fs::path root,
    std::map<std::string, fs::path> libraries, ModuleLoadMode mode,
    bool verboseTests) {
    if (root.empty()) {
        root = fs::absolute(entry).parent_path();
        for (fs::path directory = root; !directory.empty();
             directory = directory.parent_path()) {
            if (directory.filename() == "src") {
                root = directory;
                break;
            }
            if (directory == directory.parent_path()) break;
        }
    }
    return LoadConfigured(entry, std::move(root), std::move(libraries), nullptr,
                          "root", mode, verboseTests);
}

std::unique_ptr<CompilationUnit> ModuleLoader::Load(const fs::path& entry,
                                                    const PackageGraph& graph,
                                                    ModuleLoadMode mode,
                                                    bool verboseTests) {
    return LoadConfigured(entry, graph.Root().source, {}, &graph,
                          graph.Root().id, mode, verboseTests);
}

std::unique_ptr<CompilationUnit> ModuleLoader::LoadConfigured(
    const fs::path& entry, fs::path root,
    std::map<std::string, fs::path> libraries, const PackageGraph* graph,
    std::string rootIdentity, ModuleLoadMode mode, bool verboseTests) {
    _loaded.clear();
    _privateModules.clear();
    _roots.clear();
    _active.clear();
    _modules.clear();
    _declarations.clear();
    _mode = mode;
    _verboseTests = verboseTests;
    _testingModule.clear();
    _libraries = std::move(libraries);
    _packageGraph = graph;
    root = fs::weakly_canonical(root);
    const fs::path file = fs::weakly_canonical(entry);
    const fs::path relativeEntry = file.lexically_relative(root);
    if (relativeEntry.empty() || *relativeEntry.begin() == "..") {
        const source::SourceLocation origin{0, 1, 1, file.string()};
        Report({origin, origin},
               "entry source escapes the root package source");
        return std::make_unique<CompilationUnit>(
            source::SourceRange{}, std::vector<std::unique_ptr<Declaration>>{});
    }
    const ModuleHeader header = ReadHeader(file);
    if (header.name.empty())
        LoadLegacyEntry(file, root, rootIdentity);
    else
        VisitModule(file.parent_path(), header.name, root, true, file, {},
                    std::move(rootIdentity));

    auto unit = std::make_unique<CompilationUnit>(source::SourceRange{},
                                                  std::move(_declarations));
    unit->modules = std::move(_modules);
    unit->testingModule = std::move(_testingModule);
    return unit;
}

std::optional<ModuleLoader::LibrarySource> ModuleLoader::ResolveLibrary(
    const std::string& sourceIdentity, const std::string& alias,
    source::SourceRange range, bool includeLegacyHint) {
    if (_packageGraph) {
        const PackageGraphNode* source = _packageGraph->Find(sourceIdentity);
        if (!source) {
            Report(range, "package graph has no source identity '" +
                              sourceIdentity + "'");
            return std::nullopt;
        }
        const auto dependency = source->dependencies.find(alias);
        if (dependency == source->dependencies.end()) {
            Report(range, "package '" + source->identity +
                              "' has no dependency alias '" + alias + "'");
            return std::nullopt;
        }
        const PackageGraphNode* target =
            _packageGraph->Find(dependency->second);
        if (!target) {
            Report(range, "package graph dependency '" + alias +
                              "' references missing node '" +
                              dependency->second + "'");
            return std::nullopt;
        }
        return LibrarySource{target->id, target->source};
    }
    const auto library = _libraries.find(alias);
    if (library == _libraries.end()) {
        Report(range, "library '" + alias + "' is not configured" +
                          (includeLegacyHint ? "; supply --lib name=source-root"
                                             : ""));
        return std::nullopt;
    }
    return LibrarySource{"lib::" + alias,
                         fs::weakly_canonical(library->second)};
}

std::string ModuleLoader::SourceName(const std::string& sourceIdentity) const {
    if (!_packageGraph) return sourceIdentity;
    const PackageGraphNode* node = _packageGraph->Find(sourceIdentity);
    if (!node) return sourceIdentity;
    return node->identity + (node->version.empty() ? "" : "@" + node->version);
}

void ModuleLoader::LoadLegacyEntry(const fs::path& file, const fs::path& root,
                                   const std::string& sourceIdentity) {
    const auto source = ReadSource(file);
    const source::SourceLocation origin{0, 1, 1, file.string()};
    const source::SourceRange fileRange{origin, origin};
    if (!source) {
        Report(fileRange, "cannot open module '" + file.string() + "'");
        return;
    }
    diagnostics::DiagnosticBag lexical;
    auto tokens = Tokenizer(*source, lexical).Tokenize();
    const auto locate = [&](source::SourceRange range) {
        range.start.file = file.string();
        range.end.file = file.string();
        return range;
    };
    for (auto& token : tokens) token.range = locate(token.range);
    for (const auto& diagnostic : lexical.Diagnostics())
        _diagnostics.Report(diagnostic.code, locate(diagnostic.range),
                            diagnostic.message, diagnostic.severity);
    Parser parser(std::move(tokens), _diagnostics);
    auto unit = parser.Parse();
    if (_mode == ModuleLoadMode::Test)
        Report(fileRange,
               "'flecha test' requires a declared '<name>_test' module");
    const std::string sourceId = "$legacy$";
    _modules.push_back(
        Module{"",
               sourceId,
               "",
               file.string(),
               sourceIdentity + "\n" + root.generic_string() + "\n.",
               SourceName(sourceIdentity),
               false,
               true,
               false,
               {},
               {}});
    for (auto& declaration : unit->TakeDeclarations()) {
        declaration->sourceId = sourceId;
        _declarations.push_back(std::move(declaration));
    }
}

std::string ModuleLoader::VisitModule(const fs::path& inputDirectory,
                                      const std::string& name,
                                      const fs::path& root, bool entry,
                                      const fs::path& entrySource,
                                      source::SourceRange importRange,
                                      std::string sourceIdentity,
                                      std::string importingQuiverId) {
    const fs::path directory = fs::weakly_canonical(inputDirectory);
    const fs::path relativeDirectory = directory.lexically_relative(root);
    const std::string quiverId = sourceIdentity + "\n" + root.generic_string() +
                                 "\n" + relativeDirectory.generic_string();
    std::string quiverName = SourceName(sourceIdentity);
    for (const auto& segment : relativeDirectory)
        if (segment != ".") quiverName += "::" + segment.string();
    const std::string key = LogicalKey(sourceIdentity, root, directory, name);
    const auto reportPrivateImport = [&] {
        Report(importRange,
               "private module '" + name +
                   "' can only be imported from modules in quiver '" +
                   quiverName + "'");
    };

    if (std::find(_active.begin(), _active.end(), key) != _active.end()) {
        Report(importRange, "module import cycle involving '" + name + "'");
        return {};
    }
    if (const auto found = _loaded.find(key); found != _loaded.end()) {
        const auto privacy = _privateModules.find(key);
        if (!entry && privacy != _privateModules.end() && privacy->second &&
            importingQuiverId != quiverId)
            reportPrivateImport();
        return found->second;
    }
    if (_active.size() >= 256) {
        Report(importRange, "module import depth exceeds 256");
        return {};
    }
    if (!fs::is_directory(directory)) {
        Report(importRange,
               "cannot open module directory '" + directory.string() + "'");
        return {};
    }

    std::vector<fs::path> sourceFiles;
    for (const auto& item : fs::directory_iterator(directory)) {
        if (!item.is_regular_file() || item.path().extension() != ".fl" ||
            ReadHeader(item.path()).name != name)
            continue;
        const fs::path sourceFile = fs::weakly_canonical(item.path());
        const fs::path relative = sourceFile.lexically_relative(root);
        if (relative.empty() || *relative.begin() == "..") {
            Report(importRange, "module source escapes its source root: " +
                                    sourceFile.string());
            continue;
        }
        sourceFiles.push_back(sourceFile);
    }
    std::sort(sourceFiles.begin(), sourceFiles.end());
    if (sourceFiles.empty()) {
        Report(importRange, "cannot open module '" + name + "' in namespace '" +
                                quiverName + "'");
        return {};
    }

    const bool testModule = IsTestModule(name);
    const source::SourceLocation namespaceOrigin{0, 1, 1, directory.string()};
    const source::SourceRange namespaceRange{namespaceOrigin, namespaceOrigin};
    if (testModule && (_mode != ModuleLoadMode::Test || !entry))
        Report(
            namespaceRange,
            "test modules can only be compiled as the entry of 'flecha test'");
    if (entry && _mode == ModuleLoadMode::Test && !testModule)
        Report(namespaceRange,
               "'flecha test' requires a declared '<name>_test' module");

    const std::string id = "$m" + std::to_string(_loaded.size()) + "$";
    _loaded.emplace(key, id);
    _active.push_back(key);

    std::vector<ParsedSource> sources;
    std::optional<bool> privateModule;
    for (std::size_t sourceIndex = 0; sourceIndex < sourceFiles.size();
         ++sourceIndex) {
        const fs::path& file = sourceFiles[sourceIndex];
        if (const auto previousRoot = _roots.find(file);
            previousRoot != _roots.end() && previousRoot->second != root) {
            Report(namespaceRange,
                   "module source is loaded with conflicting source roots: " +
                       file.string());
            continue;
        }
        _roots.emplace(file, root);
        const auto source = ReadSource(file);
        if (!source) {
            Report(namespaceRange,
                   "cannot open module source '" + file.string() + "'");
            continue;
        }
        diagnostics::DiagnosticBag lexical;
        auto tokens = Tokenizer(*source, lexical).Tokenize();
        const auto locate = [&](source::SourceRange range) {
            range.start.file = file.string();
            range.end.file = file.string();
            return range;
        };
        const source::SourceLocation fileOrigin{0, 1, 1, file.string()};
        const source::SourceRange fileRange{fileOrigin, fileOrigin};
        for (auto& token : tokens) token.range = locate(token.range);
        for (const auto& diagnostic : lexical.Diagnostics())
            _diagnostics.Report(diagnostic.code, locate(diagnostic.range),
                                diagnostic.message, diagnostic.severity);
        Parser parser(std::move(tokens), _diagnostics,
                      "s" + std::to_string(sourceIndex) + "$");
        parser.ParseHeader();

        std::vector<Import> expanded;
        for (const auto& import : parser.Imports()) {
            if (!import.wildcard) {
                expanded.push_back(import);
                continue;
            }
            fs::path sourceRoot = root;
            std::size_t start = 1;
            if (import.path[0] == "lib") {
                if (import.path.size() < 2) continue;
                const auto library = ResolveLibrary(
                    sourceIdentity, import.path[1], import.range);
                if (!library) continue;
                sourceRoot = library->root;
                start = 2;
            }
            fs::path selectedDirectory = sourceRoot;
            for (std::size_t i = start; i < import.path.size(); ++i)
                selectedDirectory /= import.path[i];
            selectedDirectory = fs::weakly_canonical(selectedDirectory);
            if (!fs::is_directory(selectedDirectory)) {
                Report(import.range, "cannot open module directory '" +
                                         selectedDirectory.string() + "'");
                continue;
            }
            std::set<std::string> names;
            for (const auto& item : fs::directory_iterator(selectedDirectory)) {
                if (!item.is_regular_file() || item.path().extension() != ".fl")
                    continue;
                const std::string selected = ReadHeader(item.path()).name;
                if (!selected.empty() && !IsTestModule(selected))
                    names.insert(selected);
            }
            for (const auto& selected : names) {
                Import choice = import;
                choice.wildcard = false;
                choice.path.push_back(selected);
                choice.alias = selected;
                expanded.push_back(std::move(choice));
            }
        }
        parser.SetImports(std::move(expanded));
        auto unit = parser.Parse();
        if (unit->moduleName != name)
            Report(fileRange, "source declares module '" + unit->moduleName +
                                  "' while loading logical module '" + name +
                                  "'");
        if (!privateModule)
            privateModule = unit->privateModule;
        else if (*privateModule != unit->privateModule)
            Report(unit->Range(),
                   "logical module '" + name +
                       "' mixes ordinary and private module declarations");
        sources.push_back({file, id + "s" + std::to_string(sourceIndex) + "$",
                           std::move(unit)});
    }

    const bool isPrivate = privateModule.value_or(false);
    _privateModules.emplace(key, isPrivate);
    if (!entry && isPrivate && importingQuiverId != quiverId)
        reportPrivateImport();
    if (isPrivate && !sources.empty())
        _diagnostics.Report(
            diagnostics::DiagnosticCode::UnexpectedToken,
            sources.front().unit->Range(),
            "'private module' is deprecated; use an ordinary module with "
            "quiver declarations",
            diagnostics::DiagnosticSeverity::Warning);

    std::vector<Module> sourceModules;
    std::string testPair;
    const std::string productionName =
        testModule
            ? name.substr(0, name.size() - std::string_view("_test").size())
            : "";
    for (auto& source : sources) {
        Module module{id,
                      source.sourceId,
                      name,
                      source.file.string(),
                      quiverId,
                      quiverName,
                      isPrivate,
                      entry,
                      testModule,
                      {},
                      std::move(source.unit->imports)};
        for (auto& import : module.imports) {
            if (import.path.empty()) continue;
            fs::path sourceRoot = root;
            std::string targetIdentity = sourceIdentity;
            std::size_t start = 1;
            if (import.path[0] == "lib") {
                if (import.path.size() < 3) continue;
                const auto library = ResolveLibrary(
                    sourceIdentity, import.path[1], import.range, true);
                if (!library) continue;
                sourceRoot = library->root;
                targetIdentity = library->identity;
                start = 2;
            } else if (import.path[0] != "root" || import.path.size() < 2)
                continue;
            const std::string targetName = import.path.back();
            fs::path targetDirectory = sourceRoot;
            for (std::size_t index = start; index + 1 < import.path.size();
                 ++index)
                targetDirectory /= import.path[index];
            targetDirectory = fs::weakly_canonical(targetDirectory);
            const auto relative =
                targetDirectory.lexically_relative(sourceRoot);
            if (relative.empty() || *relative.begin() == "..") {
                Report(import.range, "module path escapes its source root");
                continue;
            }
            import.resolvedModule =
                VisitModule(targetDirectory, targetName, sourceRoot, false, {},
                            import.range, targetIdentity, quiverId);
            if (IsStandardTestingImport(import))
                _testingModule = import.resolvedModule;
            if (testModule && targetIdentity == sourceIdentity &&
                targetDirectory == directory && targetName == productionName)
                testPair = import.resolvedModule;
        }
        sourceModules.push_back(std::move(module));
    }

    if (entry && testModule && testPair.empty())
        Report(namespaceRange,
               "test module must explicitly import its paired production "
               "module '" +
                   productionName + "'");
    for (auto& module : sourceModules) {
        module.testPair = testPair;
        _modules.push_back(std::move(module));
    }

    if (entry && _mode == ModuleLoadMode::Test) {
        std::vector<const FunctionDeclaration*> tests;
        for (const auto& source : sources) {
            if (source.file != entrySource) continue;
            for (const auto& declaration : source.unit->Declarations())
                if (declaration->Kind() == AstNodeKind::FunctionDeclaration &&
                    static_cast<const FunctionDeclaration&>(*declaration)
                        .IsTest())
                    tests.push_back(static_cast<const FunctionDeclaration*>(
                        declaration.get()));
        }
        diagnostics::DiagnosticBag generatedDiagnostics;
        auto generatedTokens =
            Tokenizer(TestHarnessSource(tests, entrySource, _verboseTests),
                      generatedDiagnostics)
                .Tokenize();
        Parser generatedParser(std::move(generatedTokens),
                               generatedDiagnostics);
        Import testingImport;
        testingImport.path = {"lib", "std", "tests", "testing"};
        testingImport.alias = "testing";
        testingImport.resolvedModule = _testingModule;
        generatedParser.SetImports({testingImport});
        auto generated = generatedParser.Parse();
        for (const auto& diagnostic : generatedDiagnostics.Diagnostics())
            _diagnostics.Report(diagnostic.code, diagnostic.range,
                                diagnostic.message, diagnostic.severity);
        const std::string harnessSource = id + "harness$";
        _modules.push_back(Module{id,
                                  harnessSource,
                                  name,
                                  entrySource.string(),
                                  quiverId,
                                  quiverName,
                                  isPrivate,
                                  true,
                                  true,
                                  testPair,
                                  {std::move(testingImport)}});
        for (auto& declaration : generated->TakeDeclarations()) {
            if (declaration->Kind() == AstNodeKind::FunctionDeclaration)
                static_cast<FunctionDeclaration&>(*declaration)
                    .MarkTestHarness();
            declaration->moduleId = id;
            declaration->sourceId = harnessSource;
            declaration->quiverId = quiverId;
            _declarations.push_back(std::move(declaration));
        }
    }

    for (auto& source : sources)
        for (auto& declaration : source.unit->TakeDeclarations()) {
            declaration->moduleId = id;
            declaration->sourceId = source.sourceId;
            declaration->quiverId = quiverId;
            _declarations.push_back(std::move(declaration));
        }
    _active.pop_back();
    return id;
}
}  // namespace flecha::core
