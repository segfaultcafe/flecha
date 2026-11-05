#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/module_loader.hpp"
#include "diagnostics/diagnostic_bag.hpp"

#ifndef FLECHA_EXECUTABLE_SUFFIX
#define FLECHA_EXECUTABLE_SUFFIX ""
#endif

namespace {
namespace fs = std::filesystem;

constexpr std::uint64_t CORPUS_SEED = 15120261105ULL;
constexpr std::string_view REPORT_FORMAT = "flecha-compiler-stress-v1";

struct Budget final {
    std::string profile;
    std::size_t declarations;
    std::size_t specializations;
    std::size_t nesting;
    std::size_t modules;
    std::size_t packages;
    std::size_t diagnosticInputs;
    std::size_t arrayElements;
    unsigned repetitions;
    unsigned timeoutSeconds;
    unsigned memoryMiB;
};

struct Result final {
    std::string name;
    std::int64_t wallMicroseconds;
    std::uint64_t peakMemoryKiB;
    int exitCode;
    std::uintmax_t stdoutBytes;
    std::uintmax_t stderrBytes;
};

struct Invocation final {
    int exitCode;
    std::string output;
    std::string error;
};

[[noreturn]] void Fail(const std::string& message) {
    throw std::runtime_error(message);
}

void Require(bool condition, const std::string& message) {
    if (!condition) Fail(message);
}

std::string Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) Fail("cannot read " + path.string());
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

void Write(const fs::path& path, std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) Fail("cannot write " + path.string());
    stream.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
    if (!stream) Fail("failed while writing " + path.string());
}

std::size_t Count(std::string_view text, std::string_view needle) {
    std::size_t result = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++result;
        position += needle.size();
    }
    return result;
}

std::string JsonEscape(std::string_view text) {
    std::ostringstream result;
    for (const unsigned char character : text) {
        switch (character) {
            case '\\':
                result << "\\\\";
                break;
            case '"':
                result << "\\\"";
                break;
            case '\n':
                result << "\\n";
                break;
            case '\r':
                result << "\\r";
                break;
            case '\t':
                result << "\\t";
                break;
            default:
                if (character < 0x20)
                    result << "\\u" << std::hex << std::setw(4)
                           << std::setfill('0') << static_cast<int>(character)
                           << std::dec;
                else
                    result << character;
        }
    }
    return result.str();
}

std::uint64_t StableHash(std::string_view bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

Budget SelectBudget(std::string profile) {
    if (profile == "fast")
        return {
            std::move(profile), 240, 80, 192, 24, 12, 1200, 4096, 3, 20, 1024};
    if (profile == "sanitizer")
        return {
            std::move(profile), 320, 100, 192, 32, 16, 1600, 8192, 2, 60, 0};
    if (profile == "campaign")
        return {std::move(profile),
                4000,
                800,
                1024,
                220,
                128,
                20000,
                65536,
                5,
                120,
                4096};
    Fail("unknown profile '" + profile + "'");
}

class Runner final {
   public:
    Runner(fs::path compiler, fs::path standardLibrary, fs::path output,
           Budget budget)
        : _compiler(std::move(compiler)),
          _standardLibrary(std::move(standardLibrary)),
          _output(std::move(output)),
          _budget(std::move(budget)) {}

    Invocation Run(std::string name, std::vector<std::string> arguments,
                   std::optional<int> expectedExit = 0) {
        const std::size_t invocation = _results.size();
        const fs::path stdoutPath =
            _output / "logs" /
            (std::to_string(invocation) + "-" + name + ".stdout");
        const fs::path stderrPath =
            _output / "logs" /
            (std::to_string(invocation) + "-" + name + ".stderr");
        fs::create_directories(stdoutPath.parent_path());
        fs::remove(stdoutPath);
        fs::remove(stderrPath);

        std::vector<std::string> owned;
        owned.reserve(arguments.size() + 1);
        owned.push_back(_compiler.string());
        for (auto& argument : arguments) owned.push_back(std::move(argument));
        std::vector<llvm::StringRef> refs;
        refs.reserve(owned.size());
        for (const auto& argument : owned) refs.emplace_back(argument);

        const std::string stdoutText = stdoutPath.string();
        const std::string stderrText = stderrPath.string();
        const std::array<std::optional<llvm::StringRef>, 3> redirects = {
            llvm::StringRef(""), llvm::StringRef(stdoutText),
            llvm::StringRef(stderrText)};
        std::string processError;
        bool executionFailed = false;
        std::optional<llvm::sys::ProcessStatistics> statistics;
        const auto start = std::chrono::steady_clock::now();
        const int exitCode = llvm::sys::ExecuteAndWait(
            _compiler.string(), refs, std::nullopt, redirects,
            _budget.timeoutSeconds, _budget.memoryMiB, &processError,
            &executionFailed, &statistics);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start);

        Require(!executionFailed, name + " could not execute: " + processError);
        Require(exitCode != -2, name + " crashed, exceeded " +
                                    std::to_string(_budget.timeoutSeconds) +
                                    " seconds, or exceeded " +
                                    std::to_string(_budget.memoryMiB) +
                                    " MiB: " + processError);
        if (expectedExit)
            Require(exitCode == *expectedExit,
                    name + " returned " + std::to_string(exitCode) +
                        ", expected " + std::to_string(*expectedExit) +
                        "; stderr: " + Read(stderrPath));

        const std::string output = Read(stdoutPath);
        const std::string error = Read(stderrPath);
        _results.push_back({std::move(name), elapsed.count(),
                            statistics ? statistics->PeakMemory : 0, exitCode,
                            output.size(), error.size()});
        return {exitCode, output, error};
    }

    Invocation Compile(std::string name, std::string command,
                       const fs::path& input,
                       std::vector<std::string> extra = {},
                       std::optional<int> expectedExit = 0) {
        std::vector<std::string> arguments = {std::move(command),
                                              input.string()};
        arguments.insert(arguments.end(),
                         std::make_move_iterator(extra.begin()),
                         std::make_move_iterator(extra.end()));
        return Run(std::move(name), std::move(arguments), expectedExit);
    }

    void AddResult(Result result) { _results.push_back(std::move(result)); }
    void AppendResults(const Runner& other) {
        _results.insert(_results.end(), other._results.begin(),
                        other._results.end());
    }

    const std::vector<Result>& Results() const { return _results; }
    const Budget& Limits() const { return _budget; }
    const fs::path& Output() const { return _output; }
    const fs::path& StandardLibrary() const { return _standardLibrary; }

   private:
    fs::path _compiler;
    fs::path _standardLibrary;
    fs::path _output;
    Budget _budget;
    std::vector<Result> _results;
};

fs::path GenerateLargeValid(const fs::path& root, const Budget& budget) {
    std::ostringstream source;
    source << "module main;\n";
    source << "T identity<T>(T value) { return value; }\n";
    for (std::size_t index = 0; index < budget.specializations; ++index) {
        source << "structure StressValue" << index << " { i32 value; };\n";
        source << "i32 specialize" << index << "() { StressValue" << index
               << " value = StressValue" << index
               << "::init(); value.value = " << index
               << "; copy := identity<StressValue" << index
               << ">(value); return copy.value; }\n";
    }
    for (std::size_t index = 0; index < budget.declarations; ++index) {
        source << "// deterministic tokenizer padding " << CORPUS_SEED << " "
               << index << " abcdefghijklmnopqrstuvwxyz0123456789\n";
        source << "i32 declaration_with_a_deliberately_long_name_" << index
               << "(i32 value) { return value ^ " << (index & 0x7fff)
               << "; }\n";
    }
    source << R"(
structure Owner { own *i32 pointer; };
unsafe Owner Owner::init(own *i32 pointer) override {
    return build Owner { self.pointer = pointer; };
}
unsafe void Owner::tear(&self) override { free(self->pointer); }
unsafe i32 Owner::read(&const self) { return *self->pointer; }

i32 slice_sum([]const i32 values) {
    i32 total = 0;
    loop usize index = 0; index < values.len(); index++ {
        total += values[index];
    }
    return total;
}

i32 ownership_and_borrows() {
    [4]i32 values = [10, 20, 30, 40];
    view := values[1:4];
    i32 total = slice_sum(view);
    unsafe {
        pointer, allocationError := new<i32>(total);
        if allocationError != none { return 1; }
        owner := Owner::init(pointer);
        return owner.read();
    }
}

u32 recursive_depth(u32 remaining) {
    if remaining == 0 { return 0; }
    return recursive_depth(remaining - 1) + 1;
}
)";
    source << "u8 large_array() { [" << budget.arrayElements
           << "]u8 values = [7; " << budget.arrayElements << "]; return values["
           << (budget.arrayElements - 1) << "]; }\n";
    source << R"(
i32 main() {
    if ownership_and_borrows() != 90 { return 1; }
    if recursive_depth(128) != 128 { return 2; }
    if large_array() != 7 { return 3; }
    if specialize0() != 0 { return 4; }
    return declaration_with_a_deliberately_long_name_0(0);
}
)";
    const fs::path entry = root / "main.fl";
    Write(entry, source.str());
    return entry;
}

std::pair<fs::path, fs::path> GenerateNested(const fs::path& root,
                                             const Budget& budget) {
    std::string open(budget.nesting, '(');
    std::string close(budget.nesting, ')');
    const fs::path valid = root / "valid.fl";
    const fs::path invalid = root / "invalid.fl";
    Write(valid, "module valid; i32 value() { return " + open + "42" + close +
                     "; }\n");
    close.pop_back();
    Write(invalid, "module invalid; i32 value() { return " + open + "42" +
                       close + "; }\n");
    return {valid, invalid};
}

fs::path GenerateDiagnosticFlood(const fs::path& root, const Budget& budget) {
    std::ostringstream source;
    source << "module flood;\n";
    for (std::size_t index = 0; index < budget.diagnosticInputs; ++index)
        source << "`\n";
    const fs::path entry = root / "flood.fl";
    Write(entry, source.str());
    return entry;
}

fs::path GenerateExtremeNumerics(const fs::path& root) {
    const fs::path entry = root / "extremes.fl";
    Write(entry, R"(module extremes;
void impossible() {
    [1152921504606846976]u64 enormous = none;
    u8 outside = 256;
}
)");
    return entry;
}

fs::path GenerateModuleChain(const fs::path& root, const Budget& budget) {
    for (std::size_t index = 0; index < budget.modules; ++index) {
        std::ostringstream source;
        source << "module module" << index << ";\n";
        if (index + 1 < budget.modules)
            source << "@imports :: root::module" << (index + 1) << ";\n";
        source << "export i32 value() { return ";
        if (index + 1 < budget.modules)
            source << "module" << (index + 1) << "::value()";
        else
            source << "42";
        source << "; }\n";
        Write(root / ("module" + std::to_string(index) + ".fl"), source.str());
    }
    const fs::path entry = root / "main.fl";
    Write(entry,
          "module main; @imports :: root::module0 :: root::module0; "
          "i32 main() { return module0::value() - 42; }\n");
    return entry;
}

std::pair<fs::path, fs::path> GenerateModuleFailures(const fs::path& root) {
    const fs::path duplicateRoot = root / "duplicate";
    Write(duplicateRoot / "main.fl",
          "module main; i32 main() { return duplicate(); }\n");
    Write(duplicateRoot / "a.fl",
          "module main; i32 duplicate() { return 1; }\n");
    Write(duplicateRoot / "b.fl",
          "module main; i32 duplicate() { return 2; }\n");

    const fs::path cycleRoot = root / "cycle";
    Write(cycleRoot / "main.fl",
          "module main; @imports :: root::other; void main() {}\n");
    Write(cycleRoot / "other.fl", "module other; @imports :: root::main;\n");
    return {duplicateRoot / "main.fl", cycleRoot / "main.fl"};
}

std::pair<fs::path, fs::path> GeneratePackageGraphs(const fs::path& root,
                                                    const Budget& budget) {
    const fs::path app = root / "app";
    Write(app / "main.fl",
          "module main; @imports :: lib::package0::api; "
          "i32 main() { return api::value() - 42; }\n");
    for (std::size_t index = 0; index < budget.packages; ++index) {
        std::ostringstream source;
        source << "module api;\n";
        if (index + 1 < budget.packages)
            source << "@imports :: lib::next::api as next_api;\n";
        source << "export i32 value() { return ";
        if (index + 1 < budget.packages)
            source << "next_api::value()";
        else
            source << "42";
        source << "; }\n";
        Write(root / ("package" + std::to_string(index)) / "api.fl",
              source.str());
    }

    std::ostringstream graph;
    graph << "{\n  \"schema\": 1,\n  \"root\": \"app\",\n  \"packages\": [\n";
    graph << "    {\"id\":\"app\",\"identity\":\"local/app\","
             "\"source\":\"../app\",\"dependencies\":{"
             "\"package0\":\"package0\"}},\n";
    for (std::size_t index = 0; index < budget.packages; ++index) {
        graph << "    {\"id\":\"package" << index
              << "\",\"identity\":\"stress/package" << index
              << "\",\"version\":\"1.0.0\",\"source\":\"../package" << index
              << "\",\"dependencies\":{";
        if (index + 1 < budget.packages)
            graph << "\"next\":\"package" << (index + 1) << "\"";
        graph << "}}" << (index + 1 == budget.packages ? "\n" : ",\n");
    }
    graph << "  ]\n}\n";
    const fs::path graphPath = root / "target" / "package-graph.json";
    Write(graphPath, graph.str());

    const fs::path cyclePath = root / "target" / "cycle.json";
    Write(cyclePath, R"({
  "schema": 1,
  "root": "app",
  "packages": [
    {"id":"app","identity":"local/app","source":"../app",
     "dependencies":{"first":"first"}},
    {"id":"first","identity":"stress/first","source":"../package0",
     "dependencies":{"app":"app"}}
  ]
}
)");
    return {graphPath, cyclePath};
}

fs::path GenerateCipherLoops(const fs::path& root) {
    const fs::path entry = root / "cipher_loops.fl";
    Write(entry, R"(module cipher_loops;
@imports
    :: lib::std::math::bits
    :: lib::std::math::integer;

u32 constant_array(u32 input) {
    [4]u32 values = [input, 0x9e3779b9, 0x6a09e667, 0xbb67ae85];
    return values[0] ^ values[1] ^ values[2] ^ values[3];
}

u32 fixed_variable(usize index) {
    [4]u32 values = [11, 22, 33, 44];
    return values[index];
}

u32 provably_safe_loop() {
    [16]u32 values = [3; 16];
    u32 total = 0;
    loop usize index = 0; index < values.len(); index++ {
        total += values[index];
    }
    return total;
}

u32 checked_slice([]const u32 values, usize index) {
    return values[index];
}

u32 normal_add(u32 left, u32 right) { return left + right; }
u32 wrapping_add(u32 left, u32 right) {
    return integer::wrapping_add<u32>(left, right);
}
u32 checked_add(u32 left, u32 right) {
    result, arithmeticError := integer::checked_add<u32>(left, right);
    if arithmeticError != none { return 0; }
    return result;
}
u32 saturating_add(u32 left, u32 right) {
    return integer::saturating_add<u32>(left, right);
}

u32 simon_shaped(u32 input, usize rounds) {
    [2]u32 state = [input, input ^ 0x9e3779b9];
    loop usize round = 0; round < rounds; round++ {
        mixed := bits::rotate_left<u32>(state[0], 1) ^
            bits::rotate_left<u32>(state[0], 8) ^ state[1] ^
            round.cast<u32>;
        state[1] = state[0];
        state[0] = mixed;
    }
    return state[0] ^ state[1];
}

u8 present_shaped(u8 input, usize rounds) {
    [16]u8 state = [input; 16];
    loop usize round = 0; round < rounds; round++ {
        loop usize index = 0; index < state.len(); index++ {
            state[index] = (state[index] << 1) ^
                (state[index] >> 3) ^ round.cast<u8>;
        }
    }
    return state[0] ^ state[15];
}

u64 ascon_shaped(u64 input, usize rounds) {
    [5]u64 state = [input, input ^ 1, input ^ 2, input ^ 3, input ^ 4];
    loop usize round = 0; round < rounds; round++ {
        state[2] ^= round.cast<u64>;
        state[0] ^= state[4];
        state[4] ^= state[3];
        state[2] ^= state[1];
        state[0] ^= bits::rotate_right<u64>(state[0], 19);
        state[1] ^= bits::rotate_right<u64>(state[1], 61);
        state[2] ^= bits::rotate_right<u64>(state[2], 6);
        state[3] ^= bits::rotate_right<u64>(state[3], 10);
        state[4] ^= bits::rotate_right<u64>(state[4], 7);
    }
    return state[0] ^ state[1] ^ state[2] ^ state[3] ^ state[4];
}

i32 main() {
    if normal_add(u32.max_value, 1) != 0 { return 1; }
    if wrapping_add(u32.max_value, 1) != 0 { return 2; }
    if checked_add(u32.max_value, 1) != 0 { return 3; }
    if saturating_add(u32.max_value, 1) != u32.max_value { return 4; }
    if fixed_variable(2) != 33 { return 5; }
    if provably_safe_loop() != 48 { return 6; }
    [4]u32 values = [5, 6, 7, 8];
    if checked_slice(values[:], 2) != 7 { return 7; }
    constant_array(1);
    simon_shaped(1, 32);
    present_shaped(1, 31);
    ascon_shaped(1, 12);
    return 0;
}
)");
    return entry;
}

void ExerciseReusedLoader(Runner& runner, const fs::path& largeRoot,
                          const fs::path& largeEntry) {
    const fs::path freshRoot = runner.Output() / "corpus" / "fresh-state";
    const fs::path freshEntry = freshRoot / "fresh.fl";
    Write(freshEntry, "module fresh; void fresh_only() {}\n");
    flecha::diagnostics::DiagnosticBag diagnostics;
    flecha::core::ModuleLoader loader(diagnostics);
    const auto start = std::chrono::steady_clock::now();
    auto first = loader.Load(largeEntry, largeRoot);
    Require(!diagnostics.HasErrors(),
            "in-process large module load produced diagnostics");
    Require(first->modules.size() == runner.Limits().modules + 1,
            "in-process large module load omitted modules");
    auto second = loader.Load(freshEntry, freshRoot);
    Require(!diagnostics.HasErrors(),
            "reused module loader produced stale diagnostics");
    Require(second->modules.size() == 1 && second->Declarations().size() == 1 &&
                second->modules.front().name == "fresh",
            "reused module loader retained stale compiler state");
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);
    runner.AddResult({"reused-module-loader", elapsed.count(), 0, 0, 0, 0});
}

std::string FunctionBody(std::string_view ir, std::string_view name) {
    std::size_t start = ir.find(name);
    Require(start != std::string_view::npos,
            "optimized IR does not define " + std::string(name));
    start = ir.rfind("define ", start);
    Require(start != std::string_view::npos,
            "cannot locate definition for " + std::string(name));
    const std::size_t end = ir.find("\n}\n", start);
    Require(end != std::string_view::npos,
            "cannot locate end of definition for " + std::string(name));
    return std::string(ir.substr(start, end + 3 - start));
}

void OptimizeIr(const fs::path& input, const fs::path& output) {
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    std::unique_ptr<llvm::Module> module =
        llvm::parseIRFile(input.string(), diagnostic, context);
    if (!module) {
        std::string message;
        llvm::raw_string_ostream stream(message);
        diagnostic.print("compiler_stress", stream);
        Fail("cannot parse generated LLVM: " + stream.str());
    }

    llvm::LoopAnalysisManager loopAnalyses;
    llvm::FunctionAnalysisManager functionAnalyses;
    llvm::CGSCCAnalysisManager cgsccAnalyses;
    llvm::ModuleAnalysisManager moduleAnalyses;
    llvm::PassBuilder builder;
    builder.registerModuleAnalyses(moduleAnalyses);
    builder.registerCGSCCAnalyses(cgsccAnalyses);
    builder.registerFunctionAnalyses(functionAnalyses);
    builder.registerLoopAnalyses(loopAnalyses);
    builder.crossRegisterProxies(loopAnalyses, functionAnalyses, cgsccAnalyses,
                                 moduleAnalyses);
    llvm::ModulePassManager pipeline =
        builder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
    pipeline.run(*module, moduleAnalyses);

    std::error_code error;
    llvm::raw_fd_ostream stream(output.string(), error);
    if (error) Fail("cannot write optimized LLVM: " + error.message());
    module->print(stream, nullptr);
}

void WriteReport(
    const Runner& runner, std::size_t rawChecks, std::size_t optimizedChecks,
    const std::vector<std::pair<std::string, std::uint64_t>>& artifactHashes) {
    const Budget& budget = runner.Limits();
    std::ostringstream report;
    report << "{\n"
           << "  \"format\": \"" << REPORT_FORMAT << "\",\n"
           << "  \"profile\": \"" << JsonEscape(budget.profile) << "\",\n"
           << "  \"seed\": " << CORPUS_SEED << ",\n"
           << "  \"budget\": {\n"
           << "    \"timeout_seconds_per_process\": " << budget.timeoutSeconds
           << ",\n"
           << "    \"memory_mib_per_process\": " << budget.memoryMiB << ",\n"
           << "    \"declarations\": " << budget.declarations << ",\n"
           << "    \"generic_specializations\": " << budget.specializations
           << ",\n"
           << "    \"nesting_depth\": " << budget.nesting << ",\n"
           << "    \"logical_modules\": " << budget.modules << ",\n"
           << "    \"packages\": " << budget.packages << ",\n"
           << "    \"array_elements\": " << budget.arrayElements << "\n"
           << "  },\n"
           << "  \"bounds_checks\": {\"raw_llvm\": " << rawChecks
           << ", \"optimized_llvm\": " << optimizedChecks << "},\n"
           << "  \"artifacts\": [\n";
    for (std::size_t index = 0; index < artifactHashes.size(); ++index) {
        report << "    {\"name\": \"" << JsonEscape(artifactHashes[index].first)
               << "\", \"fnv1a64\": \"0x" << std::hex
               << artifactHashes[index].second << std::dec << "\"}"
               << (index + 1 == artifactHashes.size() ? "\n" : ",\n");
    }
    report << "  ],\n  \"invocations\": [\n";
    const auto& results = runner.Results();
    for (std::size_t index = 0; index < results.size(); ++index) {
        const Result& result = results[index];
        report << "    {\"name\": \"" << JsonEscape(result.name)
               << "\", \"wall_us\": " << result.wallMicroseconds
               << ", \"peak_memory_kib\": " << result.peakMemoryKiB
               << ", \"exit_code\": " << result.exitCode
               << ", \"stdout_bytes\": " << result.stdoutBytes
               << ", \"stderr_bytes\": " << result.stderrBytes << "}"
               << (index + 1 == results.size() ? "\n" : ",\n");
    }
    report << "  ]\n}\n";
    Write(runner.Output() / "compiler-stress-report.json", report.str());
}

void Exercise(Runner& runner) {
    const Budget& budget = runner.Limits();
    const fs::path corpus = runner.Output() / "corpus";
    fs::create_directories(corpus);

    const fs::path large = GenerateLargeValid(corpus / "large-valid", budget);
    const auto [nestedValid, nestedInvalid] =
        GenerateNested(corpus / "nested", budget);
    const fs::path flood =
        GenerateDiagnosticFlood(corpus / "diagnostic-flood", budget);
    const fs::path extremes =
        GenerateExtremeNumerics(corpus / "extreme-numerics");
    const fs::path modules = GenerateModuleChain(corpus / "modules", budget);
    const auto [duplicate, importCycle] =
        GenerateModuleFailures(corpus / "module-failures");
    const auto [packageGraph, packageCycle] =
        GeneratePackageGraphs(corpus / "packages", budget);
    const fs::path cipher = GenerateCipherLoops(corpus / "cipher");

    ExerciseReusedLoader(runner, modules.parent_path(), modules);

    std::optional<Invocation> firstCheck;
    for (unsigned repetition = 0; repetition < budget.repetitions;
         ++repetition) {
        Invocation result =
            runner.Compile("large-check-" + std::to_string(repetition), "check",
                           large, {"--root", large.parent_path().string()});
        if (!firstCheck)
            firstCheck = result;
        else
            Require(result.output == firstCheck->output &&
                        result.error == firstCheck->error,
                    "valid repeated checks produced different output");
    }

    runner.Compile("nested-valid", "check", nestedValid,
                   {"--root", nestedValid.parent_path().string()});
    const Invocation invalidFirst = runner.Compile(
        "nested-invalid-0", "check", nestedInvalid,
        {"--root", nestedInvalid.parent_path().string()}, std::nullopt);
    Require(invalidFirst.exitCode != 0 &&
                invalidFirst.error.find("FLECHA-P") != std::string::npos,
            "deep invalid syntax did not produce a parser diagnostic");
    const Invocation invalidSecond = runner.Compile(
        "nested-invalid-1", "check", nestedInvalid,
        {"--root", nestedInvalid.parent_path().string()}, std::nullopt);
    Require(invalidFirst.error == invalidSecond.error,
            "deep invalid diagnostics are nondeterministic");

    const Invocation floodFirst =
        runner.Compile("diagnostic-flood-0", "check", flood,
                       {"--root", flood.parent_path().string()}, std::nullopt);
    Require(floodFirst.exitCode != 0, "diagnostic flood unexpectedly compiled");
    Require(Count(floodFirst.error, "FLECHA-") <= 101,
            "diagnostic flood exceeded the 101-line compiler bound");
    Require(Count(floodFirst.error, "FLECHA-C0006") == 1,
            "diagnostic flood did not report the compiler limit once");
    const Invocation floodSecond =
        runner.Compile("diagnostic-flood-1", "check", flood,
                       {"--root", flood.parent_path().string()}, std::nullopt);
    Require(floodFirst.error == floodSecond.error,
            "bounded flood diagnostics are nondeterministic");

    const Invocation numeric = runner.Compile(
        "extreme-numerics", "check", extremes,
        {"--root", extremes.parent_path().string()}, std::nullopt);
    Require(numeric.exitCode != 0 &&
                numeric.error.find("FLECHA-S0033") != std::string::npos &&
                numeric.error.find("FLECHA-S0032") != std::string::npos,
            "extreme numeric diagnostics were incomplete");

    const Invocation duplicateResult = runner.Compile(
        "duplicate-declarations", "check", duplicate,
        {"--root", duplicate.parent_path().string()}, std::nullopt);
    Require(duplicateResult.exitCode != 0 &&
                duplicateResult.error.find("FLECHA-S0001") != std::string::npos,
            "duplicate declarations did not produce FLECHA-S0001");
    const Invocation cycleResult = runner.Compile(
        "import-cycle", "check", importCycle,
        {"--root", importCycle.parent_path().string()}, std::nullopt);
    Require(cycleResult.exitCode != 0 &&
                cycleResult.error.find("import cycle") != std::string::npos,
            "module import cycle was not diagnosed");

    const fs::path moduleExecutable =
        runner.Output() / ("module-chain" FLECHA_EXECUTABLE_SUFFIX);
    runner.Compile("module-build", "build", modules,
                   {"-o", moduleExecutable.string(), "--root",
                    modules.parent_path().string()});

    const fs::path packageEntry =
        packageGraph.parent_path().parent_path() / "app" / "main.fl";
    runner.Compile("package-graph", "check", packageEntry,
                   {"--package-graph", packageGraph.string()});
    const Invocation packageCycleResult = runner.Compile(
        "package-cycle", "check", packageEntry,
        {"--package-graph", packageCycle.string()}, std::nullopt);
    Require(packageCycleResult.exitCode != 0 &&
                packageCycleResult.error.find("package graph cycle") !=
                    std::string::npos,
            "package cycle was not diagnosed");

    const fs::path llvmOne = runner.Output() / "large-1.ll";
    const fs::path llvmTwo = runner.Output() / "large-2.ll";
    runner.Compile(
        "large-llvm-0", "--emit-llvm", large,
        {"-o", llvmOne.string(), "--root", large.parent_path().string()});
    runner.Compile(
        "large-llvm-1", "--emit-llvm", large,
        {"-o", llvmTwo.string(), "--root", large.parent_path().string()});
    Require(Read(llvmOne) == Read(llvmTwo), "repeated LLVM artifacts differ");

    const fs::path objectOne = runner.Output() / "large-1.o";
    const fs::path objectTwo = runner.Output() / "large-2.o";
    runner.Compile(
        "large-object-0", "--emit-object", large,
        {"-o", objectOne.string(), "--root", large.parent_path().string()});
    runner.Compile(
        "large-object-1", "--emit-object", large,
        {"-o", objectTwo.string(), "--root", large.parent_path().string()});
    Require(Read(objectOne) == Read(objectTwo),
            "repeated native objects differ");

    const fs::path executableOne =
        runner.Output() / ("large-1" FLECHA_EXECUTABLE_SUFFIX);
    const fs::path executableTwo =
        runner.Output() / ("large-2" FLECHA_EXECUTABLE_SUFFIX);
    runner.Compile(
        "large-link-0", "build", large,
        {"-o", executableOne.string(), "--root", large.parent_path().string()});
    runner.Compile(
        "large-link-1", "build", large,
        {"-o", executableTwo.string(), "--root", large.parent_path().string()});
    Require(Read(executableOne) == Read(executableTwo),
            "repeated linked executables differ");

    // Execute generated programs directly while retaining the same timeout and
    // memory accounting as compiler subprocesses.
    const fs::path originalCompiler = executableOne;
    Runner executableRunner(originalCompiler, runner.StandardLibrary(),
                            runner.Output() / "execution", budget);
    executableRunner.Run("large-execute-0", {}, 0);
    runner.AppendResults(executableRunner);
    Runner moduleRunner(moduleExecutable, runner.StandardLibrary(),
                        runner.Output() / "module-execution", budget);
    moduleRunner.Run("module-execute", {}, 0);
    runner.AppendResults(moduleRunner);

    const fs::path cipherRaw = runner.Output() / "cipher-raw.ll";
    const fs::path cipherOptimized = runner.Output() / "cipher-o2.ll";
    runner.Compile(
        "cipher-llvm", "--emit-llvm", cipher,
        {"-o", cipherRaw.string(), "--root", cipher.parent_path().string(),
         "--lib", "std=" + runner.StandardLibrary().string()});
    const auto optimizeStart = std::chrono::steady_clock::now();
    OptimizeIr(cipherRaw, cipherOptimized);
    const auto optimizeElapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - optimizeStart);
    runner.AddResult(
        {"cipher-optimize-o2", optimizeElapsed.count(), 0, 0, 0, 0});
    const std::string rawIr = Read(cipherRaw);
    const std::string optimizedIr = Read(cipherOptimized);
    const std::size_t rawChecks = Count(rawIr, "call void @llvm.trap()");
    const std::size_t optimizedChecks =
        Count(optimizedIr, "call void @llvm.trap()");
    Require(rawChecks > optimizedChecks,
            "O2 did not eliminate any redundant bounds checks");
    Require(optimizedChecks > 0,
            "O2 unexpectedly removed every dynamic bounds check");
    Require(FunctionBody(optimizedIr, "constant_array").find("llvm.trap") ==
                std::string::npos,
            "constant fixed-array indices retain a redundant O2 check");
    Require(FunctionBody(optimizedIr, "provably_safe_loop").find("llvm.trap") ==
                std::string::npos,
            "provably safe fixed-array loop retains a redundant O2 check");
    Require(FunctionBody(optimizedIr, "fixed_variable").find("llvm.trap") !=
                std::string::npos,
            "variable fixed-array index lost its required check");
    Require(FunctionBody(optimizedIr, "checked_slice").find("llvm.trap") !=
                std::string::npos,
            "checked-slice index lost its required check");
    Require(rawIr.find("llvm.uadd.with.overflow.i32") != std::string::npos,
            "checked arithmetic did not lower to LLVM overflow intrinsics");
    Require(FunctionBody(rawIr, "normal_add").find("add i32") !=
                    std::string::npos &&
                FunctionBody(rawIr, "normal_add").find("add nuw") ==
                    std::string::npos &&
                FunctionBody(rawIr, "normal_add").find("add nsw") ==
                    std::string::npos,
            "normal integer arithmetic is not explicit wrapping LLVM add");

    const fs::path cipherExecutable =
        runner.Output() / ("cipher-loops" FLECHA_EXECUTABLE_SUFFIX);
    runner.Compile("cipher-native-o2", "build", cipher,
                   {"-o", cipherExecutable.string(), "--root",
                    cipher.parent_path().string(), "--lib",
                    "std=" + runner.StandardLibrary().string()});
    Runner cipherRunner(cipherExecutable, runner.StandardLibrary(),
                        runner.Output() / "cipher-execution", budget);
    cipherRunner.Run("cipher-execute", {}, 0);
    runner.AppendResults(cipherRunner);

    const std::vector<std::pair<std::string, std::uint64_t>> hashes = {
        {"large-llvm", StableHash(Read(llvmOne))},
        {"large-object", StableHash(Read(objectOne))},
        {"large-executable", StableHash(Read(executableOne))},
        {"cipher-raw-llvm", StableHash(rawIr)},
        {"cipher-o2-llvm", StableHash(optimizedIr)}};
    WriteReport(runner, rawChecks, optimizedChecks, hashes);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 5) {
            std::cerr << "usage: " << argv[0]
                      << " COMPILER STANDARD_LIBRARY OUTPUT "
                         "<fast|sanitizer|campaign>\n";
            return 2;
        }
        const fs::path compiler = fs::absolute(argv[1]);
        const fs::path standardLibrary = fs::absolute(argv[2]);
        const fs::path output = fs::absolute(argv[3]);
        const Budget budget = SelectBudget(argv[4]);
        Require(fs::is_regular_file(compiler),
                "compiler does not exist: " + compiler.string());
        Require(fs::is_directory(standardLibrary),
                "standard library does not exist: " + standardLibrary.string());
        fs::create_directories(output);
        Runner runner(compiler, standardLibrary, output, budget);
        Exercise(runner);
        std::cout << "compiler stress " << budget.profile << " passed; seed "
                  << CORPUS_SEED << "; report "
                  << (output / "compiler-stress-report.json").string() << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "compiler stress failed: " << error.what() << "\n";
        return 1;
    }
}
