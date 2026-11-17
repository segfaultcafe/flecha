#ifndef FLECHA_CODEGEN_ASYNC_VALUE_GENERATOR_HPP
#define FLECHA_CODEGEN_ASYNC_VALUE_GENERATOR_HPP

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "codegen/async_frame_layout.hpp"
#include "diagnostics/diagnostic_bag.hpp"
#include "semantic/semantic_model.hpp"

namespace flecha::codegen {

// Internal testing entry for end-to-end source frame execution. It is not
// wired to NativeCompiler or the CLI and does not relax FLECHA-S0089.
struct GeneratedAsyncFunction final {
    const semantic::FunctionSymbol* symbol = nullptr;
    std::string name;
    AsyncConcreteFrameLayout frame;
    std::size_t argumentSize = 0;
    std::size_t argumentAlignment = 1;
    std::vector<std::size_t> argumentOffsets;
};

struct GeneratedAsyncProgram final {
    std::unique_ptr<llvm::Module> module;
    std::vector<GeneratedAsyncFunction> functions;
};

[[nodiscard]] std::optional<GeneratedAsyncProgram>
GenerateAsyncValuesForTesting(llvm::LLVMContext& context,
                              const llvm::DataLayout& dataLayout,
                              const semantic::SemanticModel& model,
                              const diagnostics::DiagnosticBag& diagnostics,
                              std::string moduleName, std::string& error);

}  // namespace flecha::codegen
#endif
