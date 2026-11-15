#pragma once
#include <functional>

#include "semantic/semantic_model.hpp"
namespace flecha::semantic {
using MetadataResolver =
    std::function<std::optional<CompileTimeValue>(const core::Expression&)>;
std::optional<CompileTimeValue> ConvertCompileTimeValue(CompileTimeValue value,
                                                        SemanticType type);
std::optional<CompileTimeValue> EvaluateCompileTimeValue(
    const core::Expression& expression, const SemanticModel& model,
    const MetadataResolver& resolver = {});
}  // namespace flecha::semantic
