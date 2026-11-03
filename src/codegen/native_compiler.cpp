#include "codegen/native_compiler.hpp"

#include <llvm/ADT/SmallString.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FileUtilities.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "codegen/llvm_code_generator.hpp"

#ifndef FLECHA_RUNTIME_LIBRARY_PATH
#define FLECHA_RUNTIME_LIBRARY_PATH ""
#endif

#ifndef FLECHA_RUNTIME_LIBRARY_NAME
#define FLECHA_RUNTIME_LIBRARY_NAME ""
#endif

#ifndef FLECHA_RUNTIME_INSTALL_DIRECTORY
#define FLECHA_RUNTIME_INSTALL_DIRECTORY "lib/flecha"
#endif

#ifndef FLECHA_OBJECT_SUFFIX
#define FLECHA_OBJECT_SUFFIX ".o"
#endif

namespace {

void MainExecutableAnchor() {}

std::string DefaultRuntimeLibraryPath() {
    if (const char* configured = std::getenv("FLECHA_RUNTIME_LIBRARY")) {
        if (*configured != '\0') return configured;
    }

    const std::filesystem::path executable = llvm::sys::fs::getMainExecutable(
        nullptr, reinterpret_cast<void*>(&MainExecutableAnchor));
    if (!executable.empty()) {
        const std::filesystem::path installed =
            executable.parent_path().parent_path() /
            FLECHA_RUNTIME_INSTALL_DIRECTORY / FLECHA_RUNTIME_LIBRARY_NAME;
        if (std::filesystem::is_regular_file(installed))
            return installed.string();
    }

    const std::filesystem::path buildPath = FLECHA_RUNTIME_LIBRARY_PATH;
    if (!buildPath.empty() && std::filesystem::is_regular_file(buildPath))
        return buildPath.string();

    return buildPath.string();
}

struct LinkerSearchResult final {
    std::optional<std::string> path;
    std::vector<std::string> attempted;
};

LinkerSearchResult FindLinkerDriver() {
    LinkerSearchResult result;
    if (const char* configured = std::getenv("FLECHA_LINKER")) {
        if (*configured != '\0') {
            result.attempted.emplace_back(configured);
            const llvm::ErrorOr<std::string> path =
                llvm::sys::findProgramByName(configured);
            if (path) result.path = *path;
            return result;
        }
    }

#ifdef _WIN32
    constexpr const char* CANDIDATES[] = {"clang++.exe", "clang++", "c++.exe",
                                          "c++"};
#else
    constexpr const char* CANDIDATES[] = {"c++", "clang++", "g++"};
#endif
    for (const char* candidate : CANDIDATES) {
        result.attempted.emplace_back(candidate);
        const llvm::ErrorOr<std::string> path =
            llvm::sys::findProgramByName(candidate);
        if (path) {
            result.path = *path;
            return result;
        }
    }
    return result;
}

std::string AttemptedLinkers(const std::vector<std::string>& attempted) {
    std::string text;
    for (const auto& candidate : attempted) {
        if (!text.empty()) text += ", ";
        text += candidate;
    }
    return text;
}

std::string PlatformToolchainHint() {
#ifdef _WIN32
    return "Install the MSYS2 UCRT64 clang toolchain and add its bin directory "
           "to PATH, or set FLECHA_LINKER to the full clang++.exe path. Verify "
           "that the UCRT SDK and startup libraries are installed.";
#elif defined(__APPLE__)
    return "Install the Xcode command-line tools with 'xcode-select "
           "--install', "
           "verify that 'xcrun --show-sdk-path' succeeds, or set FLECHA_LINKER "
           "to the full clang++ path.";
#else
    return "Install a C++ toolchain and the system development/startup files, "
           "or set FLECHA_LINKER to the full clang++ or g++ path.";
#endif
}

std::string ReadLinkerOutput(const llvm::SmallString<256>& path) {
    if (path.empty()) return {};
    std::ifstream stream(path.str().str(), std::ios::binary);
    std::string output{std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>()};
    constexpr std::size_t MAX_OUTPUT = 8192;
    if (output.size() > MAX_OUTPUT) {
        output.resize(MAX_OUTPUT);
        output += "\n[linker output truncated]";
    }
    while (!output.empty() &&
           std::isspace(static_cast<unsigned char>(output.back())))
        output.pop_back();
    return output;
}

int ExecuteTool(const std::string& executable,
                const std::vector<std::string>& ownedArguments,
                std::string& executionError, bool& executionFailed,
                std::string& standardError) {
    std::vector<llvm::StringRef> arguments;
    arguments.reserve(ownedArguments.size());
    for (const auto& argument : ownedArguments)
        arguments.emplace_back(argument);

    llvm::SmallString<256> errorPath;
    const std::error_code captureError =
        llvm::sys::fs::createTemporaryFile("flecha-tool", "stderr", errorPath);
    std::optional<llvm::StringRef> redirects[3];
    llvm::ArrayRef<std::optional<llvm::StringRef>> redirectRefs;
    if (!captureError) {
        redirects[2] = errorPath.str();
        redirectRefs = redirects;
    }
    const int result = llvm::sys::ExecuteAndWait(
        executable, arguments, std::nullopt, redirectRefs, 0, 0,
        &executionError, &executionFailed);
    standardError = ReadLinkerOutput(errorPath);
    if (!errorPath.empty()) std::filesystem::remove(errorPath.c_str());
    return result;
}

std::string EscapeLineFile(std::string file) {
    if (file.empty()) return "<flecha foreign c>";
    std::string escaped;
    escaped.reserve(file.size());
    for (const char character : file) {
        if (character == '\\' || character == '"') escaped += '\\';
        escaped += character;
    }
    return escaped;
}

void RemoveFiles(const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
}

class ScopedFileCleanup final {
   public:
    explicit ScopedFileCleanup(const std::vector<std::string>& paths)
        : _paths(paths) {}
    ~ScopedFileCleanup() { RemoveFiles(_paths); }

   private:
    const std::vector<std::string>& _paths;
};

}  // namespace

namespace flecha::codegen {

NativeCompiler::NativeCompiler(std::string runtimeLibraryPath)
    : _runtimeLibraryPath(runtimeLibraryPath.empty()
                              ? DefaultRuntimeLibraryPath()
                              : std::move(runtimeLibraryPath)) {
    if (llvm::InitializeNativeTarget() ||
        llvm::InitializeNativeTargetAsmPrinter()) {
        Fail(diagnostics::DiagnosticCode::NativeTargetUnavailable,
             "failed to initialize the native LLVM target");
        return;
    }

    _targetTriple = llvm::sys::getDefaultTargetTriple();
    const llvm::Triple triple(_targetTriple);
    std::string lookupError;
#if LLVM_VERSION_MAJOR >= 21
    const llvm::Target* target =
        llvm::TargetRegistry::lookupTarget(triple, lookupError);
#else
    const llvm::Target* target =
        llvm::TargetRegistry::lookupTarget(_targetTriple, lookupError);
#endif
    if (target == nullptr) {
        Fail(diagnostics::DiagnosticCode::NativeTargetUnavailable,
             "unable to find LLVM target for '" + _targetTriple +
                 "': " + lookupError);
        return;
    }

    llvm::TargetOptions options;
#if LLVM_VERSION_MAJOR >= 21
    _targetMachine.reset(target->createTargetMachine(
        triple, "generic", "", options, llvm::Reloc::PIC_));
#else
    _targetMachine.reset(target->createTargetMachine(
        _targetTriple, "generic", "", options, llvm::Reloc::PIC_));
#endif
    if (_targetMachine == nullptr) {
        Fail(
            diagnostics::DiagnosticCode::NativeTargetUnavailable,
            "unable to create LLVM target machine for '" + _targetTriple + "'");
    }
}

bool NativeCompiler::Fail(diagnostics::DiagnosticCode code,
                          std::string message) {
    _error = std::move(message);
    _diagnostic =
        diagnostics::Diagnostic{code, diagnostics::DiagnosticSeverity::Error,
                                _error, source::SourceRange{}};
    return false;
}

bool NativeCompiler::EmitFlechaObject(const lowering::BoundProgram& program,
                                      const std::string& moduleName,
                                      const std::string& objectPath) {
    _error.clear();
    _diagnostic.reset();
    if (_targetMachine == nullptr) {
        return Fail(diagnostics::DiagnosticCode::NativeTargetUnavailable,
                    "native LLVM target is not available");
    }

    LlvmCodeGenerator generator;
    generator.ConfigureTarget(_targetTriple,
                              _targetMachine->createDataLayout());
    std::unique_ptr<llvm::Module> module =
        generator.Generate(program, moduleName);
    if (module == nullptr) {
        return Fail(diagnostics::DiagnosticCode::ObjectEmissionFailed,
                    generator.Error());
    }

    // The target emission pass manager only performs instruction selection and
    // machine-level work. Run LLVM's target-aware scalar/vector optimization
    // pipeline first so native builds do not emit the generator's deliberately
    // straightforward IR verbatim.
    llvm::LoopAnalysisManager loopAnalyses;
    llvm::FunctionAnalysisManager functionAnalyses;
    llvm::CGSCCAnalysisManager cgsccAnalyses;
    llvm::ModuleAnalysisManager moduleAnalyses;
    llvm::PassBuilder passBuilder(_targetMachine.get());
    passBuilder.registerModuleAnalyses(moduleAnalyses);
    passBuilder.registerCGSCCAnalyses(cgsccAnalyses);
    passBuilder.registerFunctionAnalyses(functionAnalyses);
    passBuilder.registerLoopAnalyses(loopAnalyses);
    passBuilder.crossRegisterProxies(loopAnalyses, functionAnalyses,
                                     cgsccAnalyses, moduleAnalyses);
    llvm::ModulePassManager optimizationPipeline =
        passBuilder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
    optimizationPipeline.run(*module, moduleAnalyses);

    std::error_code fileError;
    llvm::raw_fd_ostream output(objectPath, fileError, llvm::sys::fs::OF_None);
    if (fileError) {
        return Fail(diagnostics::DiagnosticCode::ObjectEmissionFailed,
                    "unable to open object file '" + objectPath +
                        "': " + fileError.message());
    }

    llvm::legacy::PassManager passManager;
#if LLVM_VERSION_MAJOR >= 18
    constexpr auto objectFileType = llvm::CodeGenFileType::ObjectFile;
#else
    constexpr auto objectFileType = llvm::CGFT_ObjectFile;
#endif
    if (_targetMachine->addPassesToEmitFile(passManager, output, nullptr,
                                            objectFileType, false)) {
        return Fail(diagnostics::DiagnosticCode::ObjectEmissionFailed,
                    "LLVM target cannot emit object files");
    }
    passManager.run(*module);
    output.flush();
    if (output.has_error()) {
        return Fail(diagnostics::DiagnosticCode::ObjectEmissionFailed,
                    "failed while writing object file '" + objectPath + "'");
    }
    return true;
}

bool NativeCompiler::CompileForeignSources(
    const lowering::BoundProgram& program, const std::string& compilerPath,
    std::vector<std::string>& objectPaths,
    std::vector<std::string>& temporaryPaths) {
    for (std::size_t index = 0; index < program.ForeignSources().size();
         ++index) {
        const auto& foreign = program.ForeignSources()[index];
        const bool isC = foreign.language == lowering::BoundForeignLanguage::C;
        llvm::SmallString<256> sourcePath;
        std::error_code error = llvm::sys::fs::createTemporaryFile(
            "flecha-foreign", isC ? "c" : "s", sourcePath);
        if (error) {
            return Fail(diagnostics::DiagnosticCode::ForeignCompilationFailed,
                        "unable to create temporary foreign source: " +
                            error.message());
        }
        temporaryPaths.push_back(sourcePath.str().str());

        std::string sourceText;
        if (isC) {
            sourceText = "#line " + std::to_string(foreign.bodyLine) + " \"" +
                         EscapeLineFile(foreign.range.start.file) + "\"\n";
        }
        sourceText += foreign.source;
        if (sourceText.empty() || sourceText.back() != '\n') sourceText += '\n';
        {
            std::error_code writeError;
            llvm::raw_fd_ostream sourceOutput(sourcePath, writeError,
                                              llvm::sys::fs::OF_None);
            if (writeError) {
                return Fail(
                    diagnostics::DiagnosticCode::ForeignCompilationFailed,
                    "unable to write temporary foreign source: " +
                        writeError.message());
            }
            sourceOutput << sourceText;
            sourceOutput.flush();
            if (sourceOutput.has_error()) {
                return Fail(
                    diagnostics::DiagnosticCode::ForeignCompilationFailed,
                    "failed while writing temporary foreign source");
            }
        }

        llvm::SmallString<256> objectPath;
        error = llvm::sys::fs::createTemporaryFile(
            "flecha-foreign", FLECHA_OBJECT_SUFFIX, objectPath);
        if (error) {
            return Fail(diagnostics::DiagnosticCode::ForeignCompilationFailed,
                        "unable to create temporary foreign object: " +
                            error.message());
        }
        temporaryPaths.push_back(objectPath.str().str());
        objectPaths.push_back(objectPath.str().str());

        std::vector<std::string> arguments{compilerPath};
#ifndef _WIN32
        arguments.emplace_back("-fPIC");
#endif
#ifdef FLECHA_SANITIZED_RUNTIME
        arguments.emplace_back("-fsanitize=address,undefined");
        arguments.emplace_back("-fno-omit-frame-pointer");
#endif
        arguments.insert(arguments.end(), {"-x", isC ? "c" : "assembler", "-c",
                                           sourcePath.str().str(), "-o",
                                           objectPath.str().str()});
        std::string executionError;
        std::string compilerOutput;
        bool executionFailed = false;
        const int result = ExecuteTool(compilerPath, arguments, executionError,
                                       executionFailed, compilerOutput);
        if (executionFailed || result != 0) {
            std::string message =
                std::string(isC ? "C" : "assembly") + " foreign block " +
                std::to_string(index + 1) + " failed to compile with '" +
                compilerPath + "' (exit code " + std::to_string(result) + ")";
            if (!executionError.empty()) message += ": " + executionError;
            if (!compilerOutput.empty())
                message += "\ncompiler output:\n" + compilerOutput;
            return Fail(diagnostics::DiagnosticCode::ForeignCompilationFailed,
                        std::move(message));
        }
    }
    return true;
}

bool NativeCompiler::MergeObjects(const std::string& compilerPath,
                                  const std::vector<std::string>& objectPaths,
                                  const std::string& outputPath) {
    // A relocatable merge must contain only the supplied objects. In
    // particular, MinGW compiler drivers otherwise inject CRT startup objects
    // even with -r, causing duplicate entry points during the final link.
    std::vector<std::string> arguments{compilerPath, "-nostdlib", "-r"};
    arguments.insert(arguments.end(), objectPaths.begin(), objectPaths.end());
    arguments.emplace_back("-o");
    arguments.push_back(outputPath);
    std::string executionError;
    std::string linkerOutput;
    bool executionFailed = false;
    const int result = ExecuteTool(compilerPath, arguments, executionError,
                                   executionFailed, linkerOutput);
    if (executionFailed || result != 0) {
        std::string message =
            "unable to merge Flecha and foreign objects with '" + compilerPath +
            "' (exit code " + std::to_string(result) + ")";
        if (!executionError.empty()) message += ": " + executionError;
        if (!linkerOutput.empty())
            message += "\nlinker output:\n" + linkerOutput;
        return Fail(diagnostics::DiagnosticCode::ObjectEmissionFailed,
                    std::move(message));
    }
    return true;
}

bool NativeCompiler::EmitObject(const lowering::BoundProgram& program,
                                const std::string& moduleName,
                                const std::string& objectPath) {
    if (program.ForeignSources().empty())
        return EmitFlechaObject(program, moduleName, objectPath);

    _error.clear();
    _diagnostic.reset();
    const LinkerSearchResult compiler = FindLinkerDriver();
    if (!compiler.path.has_value()) {
        return Fail(
            diagnostics::DiagnosticCode::ForeignCompilationFailed,
            "unable to find a native compiler for foreign source blocks; "
            "tried: " +
                AttemptedLinkers(compiler.attempted) + ". " +
                PlatformToolchainHint());
    }

    std::vector<std::string> temporaryPaths;
    ScopedFileCleanup cleanup(temporaryPaths);
    llvm::SmallString<256> flechaObject;
    const std::error_code temporaryError = llvm::sys::fs::createTemporaryFile(
        "flecha-generated", FLECHA_OBJECT_SUFFIX, flechaObject);
    if (temporaryError) {
        return Fail(diagnostics::DiagnosticCode::ObjectEmissionFailed,
                    "unable to create temporary Flecha object: " +
                        temporaryError.message());
    }
    temporaryPaths.push_back(flechaObject.str().str());
    if (!EmitFlechaObject(program, moduleName, flechaObject.str().str()))
        return false;

    std::vector<std::string> objectPaths{flechaObject.str().str()};
    if (!CompileForeignSources(program, *compiler.path, objectPaths,
                               temporaryPaths))
        return false;
    return MergeObjects(*compiler.path, objectPaths, objectPath);
}

bool NativeCompiler::LinkExecutable(
    const std::string& objectPath, const std::string& outputPath,
    const std::vector<std::string>& linkInputs) {
    _error.clear();
    _diagnostic.reset();
    if (_runtimeLibraryPath.empty() ||
        !llvm::sys::fs::exists(_runtimeLibraryPath)) {
        return Fail(diagnostics::DiagnosticCode::RuntimeUnavailable,
                    "Flecha runtime library was not found at '" +
                        _runtimeLibraryPath + "'");
    }
    const LinkerSearchResult linker = FindLinkerDriver();
    if (!linker.path.has_value()) {
        return Fail(diagnostics::DiagnosticCode::LinkerFailed,
                    "unable to find a supported system C++ linker driver for "
                    "target '" +
                        _targetTriple +
                        "'; tried: " + AttemptedLinkers(linker.attempted) +
                        ". " + PlatformToolchainHint());
    }

    std::vector<std::string> ownedArguments{linker.path.value(), objectPath};
    ownedArguments.insert(ownedArguments.end(), linkInputs.begin(),
                          linkInputs.end());
    ownedArguments.push_back(_runtimeLibraryPath);
#if defined(__linux__)
    // Crash reports use the executable's dynamic symbol table to name Flecha
    // frames in their native stack trace.
    ownedArguments.emplace_back("-rdynamic");
#endif
#ifdef _WIN32
    // The static runtime uses BCryptGenRandom for secure randomness and
    // temporary-file names. Consumers must link the Windows import library
    // after the runtime archive so its unresolved symbols are satisfied.
    ownedArguments.emplace_back("-lbcrypt");
    ownedArguments.emplace_back("-lws2_32");
#endif
#ifdef FLECHA_SANITIZED_RUNTIME
    ownedArguments.emplace_back("-fsanitize=address,undefined");
#endif
    ownedArguments.emplace_back("-o");
    ownedArguments.push_back(outputPath);
    std::vector<llvm::StringRef> arguments;
    arguments.reserve(ownedArguments.size());
    for (const auto& argument : ownedArguments)
        arguments.emplace_back(argument);

    llvm::SmallString<256> linkerOutputPath;
    const std::error_code captureError = llvm::sys::fs::createTemporaryFile(
        "flecha-linker", "stderr", linkerOutputPath);
    std::optional<llvm::StringRef> redirects[3];
    llvm::ArrayRef<std::optional<llvm::StringRef>> redirectRefs;
    if (!captureError) {
        redirects[2] = linkerOutputPath.str();
        redirectRefs = redirects;
    }
    std::string executionError;
    bool executionFailed = false;
    const int result = llvm::sys::ExecuteAndWait(
        linker.path.value(), arguments, std::nullopt, redirectRefs, 0, 0,
        &executionError, &executionFailed);
    const std::string linkerOutput = ReadLinkerOutput(linkerOutputPath);
    if (!linkerOutputPath.empty())
        std::filesystem::remove(linkerOutputPath.c_str());
    if (executionFailed || result != 0) {
        std::string message = "system linker '" + linker.path.value() +
                              "' failed for target '" + _targetTriple +
                              "' with exit code " + std::to_string(result);
        if (!executionError.empty()) message += ": " + executionError;
        if (!linkerOutput.empty())
            message += "\nlinker output:\n" + linkerOutput;
        message += "\n" + PlatformToolchainHint();
        return Fail(diagnostics::DiagnosticCode::LinkerFailed,
                    std::move(message));
    }
    return true;
}

bool NativeCompiler::BuildExecutable(
    const lowering::BoundProgram& program, const std::string& moduleName,
    const std::string& outputPath, const std::vector<std::string>& linkInputs) {
    llvm::SmallString<256> objectPath;
    const std::error_code temporaryError = llvm::sys::fs::createUniqueFile(
        outputPath + ".flecha-%%%%%%" FLECHA_OBJECT_SUFFIX, objectPath);
    if (temporaryError) {
        return Fail(diagnostics::DiagnosticCode::ObjectEmissionFailed,
                    "unable to create temporary object for '" + outputPath +
                        "': " + temporaryError.message());
    }
    llvm::FileRemover removeObject(objectPath);
    const std::string objectPathText = objectPath.str().str();
    if (!EmitObject(program, moduleName, objectPathText)) return false;
    return LinkExecutable(objectPathText, outputPath, linkInputs);
}

}  // namespace flecha::codegen
