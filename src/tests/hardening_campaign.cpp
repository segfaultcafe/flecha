#include <algorithm>
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

#include "core/package_graph.hpp"
#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "diagnostics/diagnostic.hpp"
#include "diagnostics/diagnostic_bag.hpp"
#include "longbow/archive.hpp"
#include "longbow/manifest.hpp"
#include "longbow/sha256.hpp"

namespace {
namespace fs = std::filesystem;

constexpr std::uint64_t CAMPAIGN_SEED = 15215320261105ULL;
constexpr std::string_view REPORT_FORMAT = "flecha-hardening-campaign-v1";

struct Budget final {
    std::string profile;
    std::size_t sourceCases;
    std::size_t versionCases;
    std::size_t manifestCases;
    std::size_t graphCases;
    std::size_t archiveCases;
    std::size_t maximumInputBytes;
};

struct Counts final {
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
};

struct Measurement final {
    std::string name;
    std::size_t cases;
    std::int64_t wallMicroseconds;
    Counts outcomes;
};

[[noreturn]] void Fail(const std::string& message) {
    throw std::runtime_error(message);
}

void Require(bool condition, const std::string& message) {
    if (!condition) Fail(message);
}

void Write(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) Fail("cannot write " + path.string());
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream) Fail("cannot finish writing " + path.string());
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

class Random final {
   public:
    explicit Random(std::uint64_t state) : _state(state) {
        if (_state == 0) _state = 0x9e3779b97f4a7c15ULL;
    }

    std::uint64_t Next() {
        _state ^= _state >> 12;
        _state ^= _state << 25;
        _state ^= _state >> 27;
        return _state * 0x2545f4914f6cdd1dULL;
    }

    std::size_t Index(std::size_t size) {
        Require(size != 0, "random selection from an empty range");
        return static_cast<std::size_t>(Next() % size);
    }

    unsigned char Byte() { return static_cast<unsigned char>(Next() & 0xff); }

   private:
    std::uint64_t _state;
};

Budget SelectBudget(std::string profile) {
    if (profile == "fast")
        return {std::move(profile), 500, 1000, 160, 160, 80, 4096};
    if (profile == "sanitizer")
        return {std::move(profile), 2500, 5000, 800, 800, 300, 16384};
    if (profile == "campaign")
        return {std::move(profile), 25000, 50000, 8000, 8000, 2500, 65536};
    Fail("unknown hardening profile '" + profile + "'");
}

std::string Mutate(std::string input, Random& random,
                   std::size_t maximumBytes) {
    static constexpr std::string_view INSERT_BYTES =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        "{}[]()<>:=,.;\\\"'/_-+*&#@!\n\r\t";
    const std::size_t operations = 1 + random.Index(8);
    for (std::size_t operation = 0; operation < operations; ++operation) {
        switch (random.Index(8)) {
            case 0:
                if (!input.empty()) input.resize(random.Index(input.size()));
                break;
            case 1:
                if (!input.empty())
                    input.erase(random.Index(input.size()),
                                1 + random.Index(std::min<std::size_t>(
                                        input.size(), 32)));
                break;
            case 2:
                if (input.size() < maximumBytes)
                    input.insert(
                        input.begin() + static_cast<std::ptrdiff_t>(
                                            random.Index(input.size() + 1)),
                        INSERT_BYTES[random.Index(INSERT_BYTES.size())]);
                break;
            case 3:
                if (!input.empty())
                    input[random.Index(input.size())] = static_cast<char>(
                        INSERT_BYTES[random.Index(INSERT_BYTES.size())]);
                break;
            case 4:
                if (!input.empty() && input.size() < maximumBytes) {
                    const std::size_t start = random.Index(input.size());
                    const std::size_t length = std::min<std::size_t>(
                        1 + random.Index(64), input.size() - start);
                    input.insert(random.Index(input.size() + 1),
                                 input.substr(start, length));
                }
                break;
            case 5:
                if (!input.empty())
                    input[random.Index(input.size())] ^=
                        static_cast<char>(1U << random.Index(8));
                break;
            case 6:
                if (input.size() < maximumBytes) {
                    const char delimiter = "()[]{}<>\"'"[random.Index(10)];
                    input.append(
                        std::min<std::size_t>(1 + random.Index(256),
                                              maximumBytes - input.size()),
                        delimiter);
                }
                break;
            case 7:
                if (!input.empty())
                    input[random.Index(input.size())] =
                        static_cast<char>(random.Byte());
                break;
        }
        if (input.size() > maximumBytes) input.resize(maximumBytes);
    }
    return input;
}

std::string RepeatToSize(std::string_view pattern, std::size_t size) {
    Require(!pattern.empty(), "cannot repeat an empty pattern");
    std::string result;
    result.reserve(size);
    while (result.size() < size) {
        const std::size_t remaining = size - result.size();
        result.append(pattern.substr(0, std::min(pattern.size(), remaining)));
    }
    return result;
}

std::string NestedSource(std::size_t maximumBytes) {
    const std::size_t depth = std::min<std::size_t>(512, maximumBytes / 4);
    std::string result;
    result.reserve(maximumBytes);
    result.append(depth, '(');
    result.append(depth, ')');
    if (result.size() < maximumBytes)
        result += RepeatToSize(";", maximumBytes - result.size());
    return result;
}

std::string DiagnosticSignature(
    const flecha::diagnostics::DiagnosticBag& diagnostics) {
    std::ostringstream result;
    for (const auto& diagnostic : diagnostics.Diagnostics()) {
        result << static_cast<int>(diagnostic.code) << ':'
               << static_cast<int>(diagnostic.severity) << ':'
               << diagnostic.range.start.offset << ':'
               << diagnostic.range.end.offset << ':' << diagnostic.message
               << '\n';
    }
    return result.str();
}

std::string TokenSignature(const std::vector<flecha::core::Token>& tokens) {
    std::ostringstream result;
    for (const auto& token : tokens)
        result << static_cast<int>(token.type) << ':'
               << token.range.start.offset << ':' << token.range.end.offset
               << ':' << token.value.size() << ':'
               << flecha::longbow::Sha256(token.value) << '\n';
    return result.str();
}

struct FrontEndResult final {
    std::string tokens;
    std::string diagnostics;
    std::size_t declarations;
    std::size_t rangeEnd;
};

FrontEndResult Parse(std::string_view source) {
    flecha::diagnostics::DiagnosticBag diagnostics;
    flecha::core::Tokenizer tokenizer(std::string(source), diagnostics);
    std::vector<flecha::core::Token> tokens = tokenizer.Tokenize();
    Require(!tokens.empty(), "tokenizer returned no EOF token");
    Require(tokens.back().type == TokenType::EOF_TOKEN,
            "tokenizer did not terminate with EOF");
    std::size_t previous = 0;
    for (const auto& token : tokens) {
        Require(token.range.start.offset <= token.range.end.offset,
                "token range is reversed");
        Require(token.range.end.offset <= source.size(),
                "token range extends beyond the input");
        Require(token.range.start.offset >= previous,
                "token ranges are not monotonic");
        previous = token.range.start.offset;
    }
    const std::string tokenSignature = TokenSignature(tokens);
    flecha::core::Parser parser(std::move(tokens), diagnostics);
    std::unique_ptr<flecha::core::CompilationUnit> unit = parser.Parse();
    Require(unit != nullptr, "parser returned a null compilation unit");
    Require(diagnostics.Diagnostics().size() <=
                flecha::diagnostics::DiagnosticBag::MAX_DIAGNOSTICS + 1,
            "front end exceeded the diagnostic bound");
    return {tokenSignature, DiagnosticSignature(diagnostics),
            unit->Declarations().size(), unit->Range().end.offset};
}

std::string FrontEndSignature(const FrontEndResult& result) {
    return result.tokens + "--diagnostics--\n" + result.diagnostics +
           "--ast--\n" + std::to_string(result.declarations) + ":" +
           std::to_string(result.rangeEnd);
}

void SaveFailure(const fs::path& output, std::string_view category,
                 std::size_t index, std::string_view input,
                 std::string_view reason) {
    const fs::path root = output / "failures";
    Write(
        root / (std::string(category) + "-" + std::to_string(index) + ".input"),
        input);
    Write(root / (std::string(category) + "-" + std::to_string(index) + ".txt"),
          reason);
}

template <typename Function>
Measurement Measure(std::string name, std::size_t cases, Function function) {
    const auto start = std::chrono::steady_clock::now();
    const Counts outcomes = function();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);
    return {std::move(name), cases, elapsed.count(), outcomes};
}

Measurement ExerciseFrontEnd(const Budget& budget, const fs::path& output) {
    const std::vector<std::string> corpus = {
        "module main; i32 main() { return 0; }\n",
        "structure Box<T> { T value; };\n",
        "void f([]const u8 bytes) { loop usize i = 0; i < bytes.len(); i++ "
        "{ bytes[i]; } }\n",
        "@imports :: lib::std::text::utf8;\n",
        "unsafe void f() { pointer, allocationError := new<i32>(1); }\n",
        "test generated(&testing::UnitTest t) { return; }\n",
        "/* comment */ \"string\\nvalue\" 'x' 18446744073709551615\n",
        "{[(<unterminated\n",
    };
    return Measure("front-end", budget.sourceCases, [&] {
        Counts outcomes;
        Random random(CAMPAIGN_SEED ^ 0x46524f4e54454e44ULL);
        for (std::size_t index = 0; index < budget.sourceCases; ++index) {
            std::string input;
            if (index % 128 == 0)
                input = RepeatToSize("i32 value;\n", budget.maximumInputBytes);
            else if (index % 128 == 1)
                input = NestedSource(budget.maximumInputBytes);
            else
                input = Mutate(corpus[random.Index(corpus.size())], random,
                               budget.maximumInputBytes);
            try {
                const FrontEndResult first = Parse(input);
                const FrontEndResult second = Parse(input);
                Require(FrontEndSignature(first) == FrontEndSignature(second),
                        "front-end output is nondeterministic");
                if (first.diagnostics.empty())
                    ++outcomes.accepted;
                else
                    ++outcomes.rejected;
            } catch (const std::exception& error) {
                SaveFailure(output, "front-end", index, input, error.what());
                throw std::runtime_error("front-end case " +
                                         std::to_string(index) + ": " +
                                         error.what());
            }
        }
        return outcomes;
    });
}

std::string VersionResult(const std::string& input) {
    std::string error;
    const auto version = flecha::longbow::SemanticVersion::Parse(input, error);
    if (!version) return "error:" + error;
    std::string secondError;
    const auto reparsed =
        flecha::longbow::SemanticVersion::Parse(version->String(), secondError);
    Require(reparsed && *reparsed == *version,
            "semantic version did not round-trip");
    return "version:" + version->String();
}

std::string RequirementResult(const std::string& input,
                              const flecha::longbow::SemanticVersion& probe) {
    std::string error;
    const auto requirement =
        flecha::longbow::VersionRequirement::Parse(input, error);
    if (!requirement) return "error:" + error;
    return "requirement:" + requirement->String() +
           (requirement->Matches(probe) ? ":match" : ":no-match");
}

Measurement ExerciseVersions(const Budget& budget, const fs::path& output) {
    const std::vector<std::string> versions = {
        "0.0.0",
        "0.3.0",
        "1.2.3-alpha.1+linux.x86",
        "18446744073709551615.0.0",
        "01.2.3",
        "1.2",
        "1.2.3-",
        "1.2.3+build",
        "^1.2.3",
        "~0.3",
        ">=0.3.0, <0.4.0",
        "*",
        "latest",
    };
    const flecha::longbow::SemanticVersion probe = [] {
        std::string error;
        auto parsed = flecha::longbow::SemanticVersion::Parse("0.3.7", error);
        Require(parsed.has_value(), error);
        return *parsed;
    }();
    return Measure("versions", budget.versionCases, [&] {
        Counts outcomes;
        Random random(CAMPAIGN_SEED ^ 0x56455253494f4e53ULL);
        for (std::size_t index = 0; index < budget.versionCases; ++index) {
            const std::size_t maximumBytes =
                std::min<std::size_t>(budget.maximumInputBytes, 1024);
            const std::string input =
                index % 128 == 0
                    ? RepeatToSize("18446744073709551616.", maximumBytes)
                    : Mutate(versions[random.Index(versions.size())], random,
                             maximumBytes);
            try {
                const std::string first = index % 2 == 0
                                              ? VersionResult(input)
                                              : RequirementResult(input, probe);
                const std::string second =
                    index % 2 == 0 ? VersionResult(input)
                                   : RequirementResult(input, probe);
                Require(first == second,
                        "version parser result is nondeterministic");
                if (first.starts_with("error:"))
                    ++outcomes.rejected;
                else
                    ++outcomes.accepted;
            } catch (const std::exception& error) {
                SaveFailure(output, "version", index, input, error.what());
                throw std::runtime_error("version case " +
                                         std::to_string(index) + ": " +
                                         error.what());
            }
        }
        return outcomes;
    });
}

std::string ManifestSignature(const fs::path& root, std::string_view input) {
    Write(root / "longbow.toml", input);
    try {
        const auto manifest = flecha::longbow::ReadManifest(root);
        std::ostringstream result;
        result << "manifest:" << manifest.name << ':'
               << manifest.version.String() << ':'
               << static_cast<int>(manifest.kind) << ':'
               << manifest.dependencies.size() << ':' << manifest.native.size();
        for (const auto& [alias, dependency] : manifest.dependencies)
            result << ':' << alias << ':' << static_cast<int>(dependency.kind);
        return result.str();
    } catch (const std::exception& error) {
        return "error:" + std::string(error.what());
    }
}

Measurement ExerciseManifests(const Budget& budget, const fs::path& output) {
    const std::vector<std::string> corpus = {
        "[package]\nname = \"sample\"\nversion = \"0.3.0\"\nkind = "
        "\"program\"\n\n[dependencies]\n",
        "[package]\nname = \"library\"\nregistry = \"owner/library\"\nversion "
        "= \"1.2.3\"\nkind = \"library\"\nflecha = \"^0.3\"\n\n"
        "[dependencies]\njson = { package = \"flecha/json\", version = "
        "\"^1\" }\n",
        "[package\nname = \"unterminated\n",
        "[dependencies]\na = { path = \"../a\" }\na = { path = \"../b\" }\n",
    };
    const fs::path root = output / "scratch" / "manifest";
    fs::remove_all(root);
    fs::create_directories(root);
    const Measurement measurement =
        Measure("manifests", budget.manifestCases, [&] {
            Counts outcomes;
            Random random(CAMPAIGN_SEED ^ 0x4d414e4946455354ULL);
            for (std::size_t index = 0; index < budget.manifestCases; ++index) {
                const std::string base = corpus[random.Index(corpus.size())];
                const std::string input =
                    index % 64 == 0 ? corpus[index / 64 % 2]
                    : index % 64 == 1
                        ? RepeatToSize("[dependencies]\na = { path = \"a\" }\n",
                                       budget.maximumInputBytes)
                        : Mutate(base, random, budget.maximumInputBytes);
                try {
                    const std::string first = ManifestSignature(root, input);
                    const std::string second = ManifestSignature(root, input);
                    Require(first == second,
                            "manifest parser result is nondeterministic");
                    if (first.starts_with("error:"))
                        ++outcomes.rejected;
                    else
                        ++outcomes.accepted;
                } catch (const std::exception& error) {
                    SaveFailure(output, "manifest", index, input, error.what());
                    throw std::runtime_error("manifest case " +
                                             std::to_string(index) + ": " +
                                             error.what());
                }
            }
            Require(outcomes.accepted != 0 && outcomes.rejected != 0,
                    "manifest campaign did not exercise both outcomes");
            return outcomes;
        });
    fs::remove_all(root);
    return measurement;
}

std::string GraphSignature(const fs::path& file, std::string_view input) {
    Write(file, input);
    std::string error;
    const auto graph = flecha::core::PackageGraph::Load(file, error);
    if (!graph) return "error:" + error;
    std::ostringstream result;
    result << "graph:" << graph->Root().id << ':' << graph->Nodes().size();
    for (const auto& [id, node] : graph->Nodes())
        result << ':' << id << ':' << node.identity << ':'
               << node.dependencies.size();
    return result.str();
}

Measurement ExerciseGraphs(const Budget& budget, const fs::path& output) {
    const std::vector<std::string> corpus = {
        R"({"schema":1,"root":"app","packages":[{"id":"app","identity":"local/app","source":".","dependencies":{}}]})",
        R"({"schema":1,"root":"app","packages":[{"id":"app","identity":"local/app","source":".","dependencies":{"lib":"lib"}},{"id":"lib","identity":"owner/lib","version":"1.0.0","source":"../lib","dependencies":{}}]})",
        R"({"schema":1,"root":"missing","packages":[]})",
        R"({"schema":1,"root":"app","packages":[)",
    };
    const fs::path file = output / "scratch" / "graph" / "package-graph.json";
    const Measurement measurement =
        Measure("package-graphs", budget.graphCases, [&] {
            Counts outcomes;
            Random random(CAMPAIGN_SEED ^ 0x5041434b41474553ULL);
            for (std::size_t index = 0; index < budget.graphCases; ++index) {
                const std::string base = corpus[random.Index(corpus.size())];
                const std::string input =
                    index % 64 == 0 ? corpus[index / 64 % 2]
                    : index % 64 == 1
                        ? RepeatToSize("{\"schema\":1,\"packages\":[]}",
                                       budget.maximumInputBytes)
                        : Mutate(base, random, budget.maximumInputBytes);
                try {
                    const std::string first = GraphSignature(file, input);
                    const std::string second = GraphSignature(file, input);
                    Require(first == second,
                            "package graph result is nondeterministic");
                    if (first.starts_with("error:"))
                        ++outcomes.rejected;
                    else
                        ++outcomes.accepted;
                } catch (const std::exception& error) {
                    SaveFailure(output, "package-graph", index, input,
                                error.what());
                    throw std::runtime_error("package graph case " +
                                             std::to_string(index) + ": " +
                                             error.what());
                }
            }
            Require(outcomes.accepted != 0 && outcomes.rejected != 0,
                    "package graph campaign did not exercise both outcomes");
            return outcomes;
        });
    fs::remove_all(file.parent_path());
    return measurement;
}

std::string ExtractSignature(const std::string& input, const fs::path& root) {
    fs::remove_all(root);
    try {
        flecha::longbow::ExtractPackageArchive(input, root);
        const std::string checksum = flecha::longbow::Sha256Tree(root);
        fs::remove_all(root);
        return "archive:" + checksum;
    } catch (const std::exception& error) {
        Require(!fs::exists(root),
                "failed archive extraction left partial filesystem state");
        return "error:" + std::string(error.what());
    }
}

Measurement ExerciseArchives(const Budget& budget, const fs::path& fixture,
                             const fs::path& output) {
    const auto valid = flecha::longbow::BuildPackageArchive(fixture);
    return Measure("archives", budget.archiveCases, [&] {
        Counts outcomes;
        Random random(CAMPAIGN_SEED ^ 0x4152434849564553ULL);
        for (std::size_t index = 0; index < budget.archiveCases; ++index) {
            std::string input = valid.bytes;
            if (index % 64 == 1)
                input.assign(budget.maximumInputBytes, '\0');
            else if (index != 0)
                input = Mutate(std::move(input), random,
                               std::min<std::size_t>(budget.maximumInputBytes,
                                                     valid.bytes.size() * 2));
            try {
                const std::string first =
                    ExtractSignature(input, output / "scratch/archive/first");
                const std::string second =
                    ExtractSignature(input, output / "scratch/archive/second");
                Require(first == second,
                        "archive extraction result is nondeterministic");
                if (first.starts_with("error:"))
                    ++outcomes.rejected;
                else
                    ++outcomes.accepted;
            } catch (const std::exception& error) {
                SaveFailure(output, "archive", index, input, error.what());
                throw std::runtime_error("archive case " +
                                         std::to_string(index) + ": " +
                                         error.what());
            }
        }
        fs::remove_all(output / "scratch/archive");
        Require(outcomes.accepted != 0 && outcomes.rejected != 0,
                "archive campaign did not exercise both outcomes");
        return outcomes;
    });
}

void WriteReport(const fs::path& output, const Budget& budget,
                 const std::vector<Measurement>& measurements) {
    std::ostringstream report;
    report << "{\n  \"format\": \"" << REPORT_FORMAT << "\",\n"
           << "  \"profile\": \"" << JsonEscape(budget.profile) << "\",\n"
           << "  \"seed\": " << CAMPAIGN_SEED << ",\n"
           << "  \"budget\": {\n"
           << "    \"source_cases\": " << budget.sourceCases << ",\n"
           << "    \"version_cases\": " << budget.versionCases << ",\n"
           << "    \"manifest_cases\": " << budget.manifestCases << ",\n"
           << "    \"graph_cases\": " << budget.graphCases << ",\n"
           << "    \"archive_cases\": " << budget.archiveCases << ",\n"
           << "    \"maximum_input_bytes\": " << budget.maximumInputBytes
           << "\n  },\n  \"measurements\": [\n";
    for (std::size_t index = 0; index < measurements.size(); ++index) {
        const auto& measurement = measurements[index];
        report << "    {\"name\": \"" << JsonEscape(measurement.name)
               << "\", \"cases\": " << measurement.cases
               << ", \"wall_us\": " << measurement.wallMicroseconds
               << ", \"accepted\": " << measurement.outcomes.accepted
               << ", \"rejected\": " << measurement.outcomes.rejected << "}"
               << (index + 1 == measurements.size() ? "\n" : ",\n");
    }
    report << "  ]\n}\n";
    Write(output / "hardening-campaign-report.json", report.str());
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4)
            Fail(
                "usage: hardening_campaign <archive-fixture> <output> "
                "<fast|sanitizer|campaign>");
        const fs::path fixture = fs::absolute(argv[1]);
        const fs::path output = fs::absolute(argv[2]);
        const Budget budget = SelectBudget(argv[3]);
        Require(fs::is_directory(fixture),
                "archive fixture is not a directory: " + fixture.string());
        fs::remove_all(output / "scratch");
        fs::remove_all(output / "failures");
        fs::create_directories(output);

        std::vector<Measurement> measurements;
        measurements.push_back(ExerciseFrontEnd(budget, output));
        measurements.push_back(ExerciseVersions(budget, output));
        measurements.push_back(ExerciseManifests(budget, output));
        measurements.push_back(ExerciseGraphs(budget, output));
        measurements.push_back(ExerciseArchives(budget, fixture, output));
        WriteReport(output, budget, measurements);
        fs::remove_all(output / "scratch");
        std::cout << "hardening campaign " << budget.profile << " passed; seed "
                  << CAMPAIGN_SEED << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hardening campaign failed: " << error.what() << '\n';
        return 1;
    }
}
