#ifndef FLECHA_CODEGEN_NATIVE_COMPILER_HPP
#define FLECHA_CODEGEN_NATIVE_COMPILER_HPP

#include <llvm/Target/TargetMachine.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "diagnostics/diagnostic.hpp"
#include "lowering/bound_ir.hpp"

namespace flecha::codegen {

class NativeCompiler final {
   public:
    explicit NativeCompiler(std::string runtimeLibraryPath = {});

    [[nodiscard]] bool Ready() const { return _targetMachine != nullptr; }
    [[nodiscard]] const std::string& TargetTriple() const {
        return _targetTriple;
    }
    [[nodiscard]] const std::string& Error() const { return _error; }
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

   private:
    bool Fail(diagnostics::DiagnosticCode code, std::string message);
    bool EmitFlechaObject(const lowering::BoundProgram& program,
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
