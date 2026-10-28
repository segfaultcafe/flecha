#ifndef FLECHA_LONGBOW_MANIFEST_HPP
#define FLECHA_LONGBOW_MANIFEST_HPP

#include <compare>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace flecha::longbow {

struct SemanticVersion final {
    std::uint64_t major = 0;
    std::uint64_t minor = 0;
    std::uint64_t patch = 0;
    std::vector<std::string> prerelease;
    std::vector<std::string> build;

    static std::optional<SemanticVersion> Parse(const std::string& text,
                                                std::string& error);
    [[nodiscard]] std::string String() const;
    [[nodiscard]] std::strong_ordering operator<=>(
        const SemanticVersion& other) const;
    [[nodiscard]] bool operator==(const SemanticVersion& other) const;
};

class VersionRequirement final {
   public:
    static std::optional<VersionRequirement> Parse(const std::string& text,
                                                   std::string& error);
    [[nodiscard]] bool Matches(const SemanticVersion& version) const;
    [[nodiscard]] const std::string& String() const { return _text; }

   private:
    enum class Operator { Equal, Less, LessEqual, Greater, GreaterEqual };
    struct Comparator final {
        Operator operation;
        SemanticVersion version;
    };
    std::string _text;
    std::vector<Comparator> _comparators;
    bool _any = false;
};

enum class PackageKind { Program, Library };
enum class DependencyKind { Path, Git, Registry };

struct Dependency final {
    std::string alias;
    DependencyKind kind = DependencyKind::Path;
    std::optional<std::filesystem::path> path;
    std::optional<std::string> package;
    std::optional<VersionRequirement> version;
    std::optional<std::string> git;
    std::optional<std::string> revision;
};

struct Manifest final {
    std::filesystem::path directory;
    std::string name;
    SemanticVersion version;
    PackageKind kind = PackageKind::Program;
    std::optional<VersionRequirement> flecha;
    std::map<std::string, Dependency> dependencies;
    std::map<std::string, std::filesystem::path> native;
};

[[nodiscard]] bool ValidIdentifier(const std::string& name);
[[nodiscard]] bool ValidProjectName(const std::string& name);
[[nodiscard]] std::string ModuleName(const std::string& projectName);
[[nodiscard]] Manifest ReadManifest(const std::filesystem::path& directory);

}  // namespace flecha::longbow

#endif
