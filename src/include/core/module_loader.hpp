#ifndef FLECHA_CORE_MODULE_LOADER_HPP
#define FLECHA_CORE_MODULE_LOADER_HPP

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "core/ast.hpp"
#include "core/package_graph.hpp"
#include "diagnostics/diagnostic_bag.hpp"

namespace flecha::core {
enum class ModuleLoadMode { Runtime, Test };

// Loads source units into logical modules. Source units retain independent
// import scopes while declarations with the same logical module identity share
// one semantic scope and native namespace.
class ModuleLoader final {
   public:
    explicit ModuleLoader(diagnostics::DiagnosticBag& diagnostics)
        : _diagnostics(diagnostics) {}
    std::unique_ptr<CompilationUnit> Load(
        const std::filesystem::path& entry, std::filesystem::path root = {},
        std::map<std::string, std::filesystem::path> libraries = {},
        ModuleLoadMode mode = ModuleLoadMode::Runtime,
        bool verboseTests = false);
    std::unique_ptr<CompilationUnit> Load(
        const std::filesystem::path& entry, const PackageGraph& graph,
        ModuleLoadMode mode = ModuleLoadMode::Runtime,
        bool verboseTests = false);

   private:
    struct LibrarySource final {
        std::string identity;
        std::filesystem::path root;
    };
    std::unique_ptr<CompilationUnit> LoadConfigured(
        const std::filesystem::path& entry, std::filesystem::path root,
        std::map<std::string, std::filesystem::path> libraries,
        const PackageGraph* graph, std::string rootIdentity,
        ModuleLoadMode mode, bool verboseTests);
    std::optional<LibrarySource> ResolveLibrary(
        const std::string& sourceIdentity, const std::string& alias,
        source::SourceRange range, bool includeLegacyHint = false);
    std::string SourceName(const std::string& sourceIdentity) const;
    std::string VisitModule(const std::filesystem::path& directory,
                            const std::string& name,
                            const std::filesystem::path& root, bool entry,
                            const std::filesystem::path& entrySource = {},
                            source::SourceRange importRange = {},
                            std::string sourceIdentity = "root",
                            std::string importingQuiverId = {});
    void LoadLegacyEntry(const std::filesystem::path& file,
                         const std::filesystem::path& root,
                         const std::string& sourceIdentity);
    void Report(source::SourceRange range, const std::string& message);
    diagnostics::DiagnosticBag& _diagnostics;
    std::map<std::string, std::string> _loaded;
    std::map<std::string, bool> _privateModules;
    std::map<std::filesystem::path, std::filesystem::path> _roots;
    std::vector<std::string> _active;
    std::map<std::string, std::filesystem::path> _libraries;
    const PackageGraph* _packageGraph = nullptr;
    std::vector<Module> _modules;
    std::vector<std::unique_ptr<Declaration>> _declarations;
    ModuleLoadMode _mode = ModuleLoadMode::Runtime;
    bool _verboseTests = false;
    std::string _testingModule;
};
}  // namespace flecha::core
#endif
