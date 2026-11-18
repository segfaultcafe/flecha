#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "analyzer/protocol.hpp"
#include "analyzer/server_loop.hpp"
#include "analyzer/transport.hpp"
#include "codegen/async_value_generator.hpp"
#include "codegen/llvm_code_generator.hpp"
#include "codegen/native_compiler.hpp"
#include "core/module_loader.hpp"
#include "core/package_graph.hpp"
#include "flecha/version.hpp"
#include "lowering/lowerer.hpp"
#include "semantic/semantic_analyzer.hpp"

#ifndef FLECHA_EXECUTABLE_SUFFIX
#define FLECHA_EXECUTABLE_SUFFIX ""
#endif

namespace {
int RunAnalyzerStdio() {
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
        _setmode(_fileno(stdout), _O_BINARY) == -1)
        throw std::runtime_error("cannot configure analyzer stdio");
#endif

    flecha::analyzer::Transport transport(std::cin, std::cout);
    flecha::analyzer::ProtocolServer protocol(flecha::VERSION);
    flecha::analyzer::ServerLoop loop(transport, protocol);
    loop.Run();
    return 0;
}

void PrintDiagnostic(const flecha::diagnostics::Diagnostic& diagnostic) {
    if (!diagnostic.range.start.file.empty())
        std::cerr << diagnostic.range.start.file << ":";
    std::cerr << flecha::diagnostics::DiagnosticCodeName(diagnostic.code) << ":"
              << diagnostic.range.start.line << ":"
              << diagnostic.range.start.column << ": ";
    if (diagnostic.severity == flecha::diagnostics::DiagnosticSeverity::Warning)
        std::cerr << "warning: ";
    std::cerr << diagnostic.message << "\n";
    for (const auto& related : diagnostic.related)
        std::cerr << related.range.start.file << ":" << related.range.start.line
                  << ":" << related.range.start.column
                  << ": note: " << related.message << "\n";
}
void PrintDiagnostics(const flecha::diagnostics::DiagnosticBag& diagnostics) {
    for (const auto& diagnostic : diagnostics.Diagnostics())
        PrintDiagnostic(diagnostic);
}
int Compile(const std::string& command, const std::string& input,
            const std::string& output, const std::filesystem::path& root,
            const std::map<std::string, std::filesystem::path>& libraries,
            const flecha::core::PackageGraph* packageGraph,
            const std::vector<std::string>& linkInputs, bool verboseTests,
            bool wholeTestModule) {
    flecha::diagnostics::DiagnosticBag diagnostics;
    flecha::core::ModuleLoader loader(diagnostics);
    const auto mode = command != "test" ? flecha::core::ModuleLoadMode::Runtime
                      : wholeTestModule
                          ? flecha::core::ModuleLoadMode::TestModule
                          : flecha::core::ModuleLoadMode::Test;
    auto unit = packageGraph
                    ? loader.Load(input, *packageGraph, mode, verboseTests)
                    : loader.Load(input, root, libraries, mode, verboseTests);
    if (diagnostics.HasErrors()) {
        PrintDiagnostics(diagnostics);
        return 1;
    }
    flecha::semantic::SemanticAnalyzer analyzer(diagnostics);
    auto semanticModel = analyzer.Analyze(*unit);
    if (diagnostics.HasErrors()) {
        PrintDiagnostics(diagnostics);
        return 1;
    }
    PrintDiagnostics(diagnostics);
    const flecha::semantic::FunctionSymbol* entry = nullptr;
    for (const auto& symbol : semanticModel.Symbols())
        if (symbol->Kind() == flecha::semantic::SymbolKind::Function &&
            symbol->Name() == "main")
            entry = static_cast<const flecha::semantic::FunctionSymbol*>(
                symbol.get());
    if (command == "build" || command == "test") {
        if (!entry || !entry->ParameterTypes().empty() ||
            (entry->ReturnType().Kind() !=
                 flecha::semantic::SemanticTypeKind::I32 &&
             entry->ReturnType().Kind() !=
                 flecha::semantic::SemanticTypeKind::Void)) {
            std::cerr << "an executable requires i32 main() or void main() in "
                         "its entry module\n";
            return 1;
        }
    }
    if (command == "check") return 0;
    const bool asyncEntry = entry && entry->IsAsync();
    std::unique_ptr<flecha::lowering::BoundProgram> program;
    if (!asyncEntry) {
        flecha::lowering::Lowerer lowerer;
        program = lowerer.Lower(*unit, semanticModel, diagnostics);
        if (!program) {
            PrintDiagnostics(diagnostics);
            return 1;
        }
    }
    if (command == "--emit-llvm" && !asyncEntry) {
        flecha::codegen::LlvmCodeGenerator generator;
        auto module = generator.Generate(*program, input);
        if (!module) {
            std::cerr << "LLVM generation failed: " << generator.Error()
                      << "\n";
            return 1;
        }
        if (output.empty()) {
            module->print(llvm::outs(), nullptr);
            return 0;
        }
        std::error_code error;
        llvm::raw_fd_ostream stream(output, error);
        if (error) {
            std::cerr << "cannot write '" << output << "': " << error.message()
                      << "\n";
            return 1;
        }
        module->print(stream, nullptr);
        return 0;
    }
    flecha::codegen::NativeCompiler compiler;
    if (command == "--emit-llvm") {
        if (!compiler.Ready()) {
            if (compiler.LastDiagnostic())
                PrintDiagnostic(*compiler.LastDiagnostic());
            return 1;
        }
        llvm::LLVMContext context;
        std::string error;
        auto generated = flecha::codegen::GenerateAsyncProgram(
            context, compiler.TargetDataLayout(), semanticModel, diagnostics,
            input, error);
        if (!generated) {
            std::cerr << "LLVM generation failed: " << error << "\n";
            return 1;
        }
#if LLVM_VERSION_MAJOR >= 21
        generated->module->setTargetTriple(
            llvm::Triple(compiler.TargetTriple()));
#else
        generated->module->setTargetTriple(compiler.TargetTriple());
#endif
        if (output.empty()) {
            generated->module->print(llvm::outs(), nullptr);
            return 0;
        }
        std::error_code errorCode;
        llvm::raw_fd_ostream stream(output, errorCode);
        if (errorCode) {
            std::cerr << "cannot write '" << output
                      << "': " << errorCode.message() << "\n";
            return 1;
        }
        generated->module->print(stream, nullptr);
        return 0;
    }
    std::string executable = output;
    const bool temporaryTestExecutable = command == "test" && output.empty();
    if (temporaryTestExecutable) {
        executable = (std::filesystem::temp_directory_path() /
                      ("flecha-test-" +
                       std::to_string(std::chrono::high_resolution_clock::now()
                                          .time_since_epoch()
                                          .count()) +
                       FLECHA_EXECUTABLE_SUFFIX))
                         .string();
    }
    const bool success =
        compiler.Ready() &&
        (asyncEntry
             ? (command == "--emit-object"
                    ? compiler.EmitAsyncObject(*unit, semanticModel,
                                               diagnostics, input, output)
                    : compiler.BuildAsyncExecutable(*unit, semanticModel,
                                                    diagnostics, input,
                                                    executable, linkInputs))
             : (command == "--emit-object"
                    ? compiler.EmitObject(*program, input, output)
                    : compiler.BuildExecutable(*program, input, executable,
                                               linkInputs)));
    if (!success && compiler.LastDiagnostic())
        PrintDiagnostic(*compiler.LastDiagnostic());
    if (!success) return 1;
    if (command != "test") return 0;
    const std::string executablePath =
        std::filesystem::absolute(executable).string();
    const llvm::StringRef arguments[] = {executablePath};
    const int result = llvm::sys::ExecuteAndWait(executablePath, arguments);
    if (temporaryTestExecutable) {
        std::error_code error;
        std::filesystem::remove(executable, error);
    }
    return result < 0 ? 1 : result;
}
void PrintUsage(const char* executable) {
    std::cerr
        << "usage: " << executable
        << " <check|build|test|--emit-llvm|--emit-object> <input.fl> [-o "
           "output]"
           " [--root source-directory] [--lib name=source-directory ...]"
           " [--package-graph graph.json]"
           " [--link object-or-library ...] [--verbose] [--whole-module]\n"
        << "       " << executable << " analyze --stdio\n";
}
}  // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "flecha " << flecha::VERSION << "\n";
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "analyze") {
        if (argc != 3 || std::string(argv[2]) != "--stdio") {
            PrintUsage(argv[0]);
            return 1;
        }
        try {
            return RunAnalyzerStdio();
        } catch (const std::exception& error) {
            std::cerr << "analyzer: " << error.what() << "\n";
            return 1;
        }
    }
    if (argc < 3) {
        PrintUsage(argv[0]);
        return 1;
    }
    const std::string command = argv[1];
    if (command != "check" && command != "build" && command != "test" &&
        command != "--emit-llvm" && command != "--emit-object") {
        PrintUsage(argv[0]);
        return 1;
    }
    std::string output;
    std::filesystem::path root;
    std::filesystem::path packageGraphFile;
    std::map<std::string, std::filesystem::path> libraries;
    std::vector<std::string> linkInputs;
    bool verboseTests = false;
    bool wholeTestModule = false;
    for (int index = 3; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--whole-module") {
            if (command != "test" || wholeTestModule) {
                PrintUsage(argv[0]);
                return 1;
            }
            wholeTestModule = true;
            continue;
        }
        if (option == "--verbose") {
            if (command != "test" || verboseTests) {
                PrintUsage(argv[0]);
                return 1;
            }
            verboseTests = true;
            continue;
        }
        if (++index == argc) {
            PrintUsage(argv[0]);
            return 1;
        }
        const std::string value = argv[index];
        if (option == "-o")
            output = value;
        else if (option == "--root")
            root = value;
        else if (option == "--package-graph")
            packageGraphFile = value;
        else if (option == "--lib") {
            const auto equals = value.find('=');
            if (equals == std::string::npos || equals == 0 ||
                equals + 1 == value.size() ||
                !libraries
                     .emplace(value.substr(0, equals), value.substr(equals + 1))
                     .second) {
                std::cerr << "--lib requires a unique name=source-directory\n";
                return 1;
            }
        } else if (option == "--link") {
            linkInputs.push_back(value);
        } else {
            PrintUsage(argv[0]);
            return 1;
        }
    }
    if ((command == "build" || command == "--emit-object") && output.empty()) {
        PrintUsage(argv[0]);
        return 1;
    }
    if (command != "build" && command != "test" && !linkInputs.empty()) {
        std::cerr << "--link is supported only by build and test\n";
        return 1;
    }
    if (!packageGraphFile.empty() && (!root.empty() || !libraries.empty())) {
        std::cerr
            << "--package-graph cannot be combined with --root or --lib\n";
        return 1;
    }
    try {
        std::optional<flecha::core::PackageGraph> packageGraph;
        if (!packageGraphFile.empty()) {
            std::string error;
            packageGraph =
                flecha::core::PackageGraph::Load(packageGraphFile, error);
            if (!packageGraph) {
                std::cerr << "package graph error: " << error << "\n";
                return 1;
            }
        }
        return Compile(command, argv[2], output, root, libraries,
                       packageGraph ? &*packageGraph : nullptr, linkInputs,
                       verboseTests, wholeTestModule);
    } catch (const std::filesystem::filesystem_error& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
