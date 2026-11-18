#ifndef FLECHA_CODEGEN_NATIVE_COMPILER_HPP
#define FLECHA_CODEGEN_NATIVE_COMPILER_HPP

#include <llvm/IR/DataLayout.h>
#include <llvm/Target/TargetMachine.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "diagnostics/diagnostic.hpp"
#include "lowering/bound_ir.hpp"

namespace flecha::core {
class CompilationUnit;
}
namespace flecha::diagnostics {
class DiagnosticBag;
}
namespace flecha::semantic {
class SemanticModel;
}

namespace flecha::codegen {

class NativeCompiler final {
   public:
    explicit NativeCompiler(std::string runtimeLibraryPath = {});

    [[nodiscard]] bool Ready() const { return _targetMachine != nullptr; }
    [[nodiscard]] const std::string& TargetTriple() const {
        return _targetTriple;
    }
    [[nodiscard]] const std::string& Error() const { return _error; }
    [[nodiscard]] llvm::DataLayout TargetDataLayout() const {
        return _targetMachine ? _targetMachine->createDataLayout()
                              : llvm::DataLayout("");
    }
    [[nodiscard]] const std::optional<diagnostics::Diagnostic>& LastDiagnostic()
        const {
        return _diagnostic;
    }

    bool EmitObject(const lowering::BoundProgram& program,
                    const std::string& moduleName,
                    const std::string& objectPath);
    bool LinkExecutable(const std::string& objectPath,
                        const std::string& outputPath,
                        const std::vector<std::string>& linkInputs = {});
    bool BuildExecutable(const lowering::BoundProgram& program,
                         const std::string& moduleName,
                         const std::string& outputPath,
                         const std::vector<std::string>& linkInputs = {});
    // Production async emission consumes verified semantic frame plans and
    // emits the generated synchronous wrapper for async main.
    bool EmitAsyncObject(const core::CompilationUnit& unit,
                         const semantic::SemanticModel& model,
                         const diagnostics::DiagnosticBag& diagnosticBag,
                         const std::string& moduleName,
                         const std::string& objectPath);
    bool BuildAsyncExecutable(const core::CompilationUnit& unit,
                              const semantic::SemanticModel& model,
                              const diagnostics::DiagnosticBag& diagnosticBag,
                              const std::string& moduleName,
                              const std::string& outputPath,
                              const std::vector<std::string>& linkInputs = {});

   private:
    bool Fail(diagnostics::DiagnosticCode code, std::string message);
    bool EmitFlechaObject(const lowering::BoundProgram& program,
                          const std::string& moduleName,
                          const std::string& objectPath);
    bool EmitModule(std::unique_ptr<llvm::Module> module,
                    const std::string& objectPath);
    bool EmitAsyncFlechaObject(const semantic::SemanticModel& model,
                               const diagnostics::DiagnosticBag& diagnosticBag,
                               const std::string& moduleName,
                               const std::string& objectPath);
    bool CompileForeignSources(const lowering::BoundProgram& program,
                               const std::string& compilerPath,
                               std::vector<std::string>& objectPaths,
                               std::vector<std::string>& temporaryPaths);
    bool MergeObjects(const std::string& compilerPath,
                      const std::vector<std::string>& objectPaths,
                      const std::string& outputPath);

    std::string _targetTriple;
    std::string _runtimeLibraryPath;
    std::unique_ptr<llvm::TargetMachine> _targetMachine;
    std::string _error;
    std::optional<diagnostics::Diagnostic> _diagnostic;
};

}  // namespace flecha::codegen

#endif  // FLECHA_CODEGEN_NATIVE_COMPILER_HPP
