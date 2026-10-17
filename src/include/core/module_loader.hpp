#ifndef FLECHA_CORE_MODULE_LOADER_HPP
#define FLECHA_CORE_MODULE_LOADER_HPP

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "core/ast.hpp"
#include "diagnostics/diagnostic_bag.hpp"

namespace flecha::core {
enum class ModuleLoadMode { Runtime, Test };

// Loads each canonical source once. Modules retain separate semantic scopes;
// only their declaration storage is combined for the existing lowering
// pipeline.
class ModuleLoader final {
   public:
    explicit ModuleLoader(diagnostics::DiagnosticBag& diagnostics)
        : _diagnostics(diagnostics) {}
    std::unique_ptr<CompilationUnit> Load(
        const std::filesystem::path& entry, std::filesystem::path root = {},
        std::map<std::string, std::filesystem::path> libraries = {},
        ModuleLoadMode mode = ModuleLoadMode::Runtime);

   private:
    std::string Visit(const std::filesystem::path& file,
                      const std::filesystem::path& root, bool entry,
                      source::SourceRange importRange = {});
    void Report(source::SourceRange range, const std::string& message);
    diagnostics::DiagnosticBag& _diagnostics;
    std::map<std::filesystem::path, std::string> _loaded;
    std::map<std::filesystem::path, std::filesystem::path> _roots;
    std::vector<std::filesystem::path> _active;
    std::map<std::string, std::filesystem::path> _libraries;
    std::vector<Module> _modules;
    std::vector<std::unique_ptr<Declaration>> _declarations;
    ModuleLoadMode _mode = ModuleLoadMode::Runtime;
    std::string _testingModule;
};
}  // namespace flecha::core
#endif
