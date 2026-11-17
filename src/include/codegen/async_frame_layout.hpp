#ifndef FLECHA_CODEGEN_ASYNC_FRAME_LAYOUT_HPP
#define FLECHA_CODEGEN_ASYNC_FRAME_LAYOUT_HPP

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/LLVMContext.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "lowering/async_lowerer.hpp"

namespace flecha::codegen {

struct AsyncTemporaryLayout final {
    std::size_t state;
    std::size_t argument;
    semantic::SemanticType type;
    std::size_t offset;
    std::size_t size;
    std::size_t alignment;
};

// Concrete, target-data-layout storage for one compiler-generated frame. Slot
// offsets are stable for the frame's lifetime. Computations are in-frame C++
// handles that own separately allocated, stable callee frames, so recursive
// direct awaits never recursively embed their layouts.
struct AsyncConcreteFrameLayout final {
    std::size_t size = 0;
    std::size_t alignment = 1;
    std::size_t controlOffset = 0;
    std::size_t initializedOffset = 0;
    std::size_t activeOperationsOffset = 0;
    std::size_t resourceOffset = 0;
    std::vector<std::optional<std::size_t>> slotOffsets;
    std::vector<std::size_t> operationOffsets;
    std::vector<AsyncTemporaryLayout> temporaries;
};

[[nodiscard]] std::optional<AsyncConcreteFrameLayout> PlanAsyncFrameLayout(
    llvm::LLVMContext& context, const llvm::DataLayout& dataLayout,
    const semantic::AsyncFramePlan& plan, const lowering::AsyncMachine& machine,
    std::string& error);

}  // namespace flecha::codegen
#endif
