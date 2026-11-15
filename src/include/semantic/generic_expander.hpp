#ifndef FLECHA_SEMANTIC_GENERIC_EXPANDER_HPP
#define FLECHA_SEMANTIC_GENERIC_EXPANDER_HPP
#include "core/ast.hpp"
#include "diagnostics/diagnostic_bag.hpp"
namespace flecha::semantic {
// Materializes concrete declarations before ordinary semantic analysis.
// Template syntax stays owned by the unit for diagnostics; lowering sees
// concrete ASTs.
void ExpandGenerics(core::CompilationUnit& unit,
                    diagnostics::DiagnosticBag& diagnostics);
void ExpandGeneratedGenerics(core::CompilationUnit& unit,
                             core::FunctionDeclaration& function,
                             diagnostics::DiagnosticBag& diagnostics);
void ReportGenericOrigins(const core::CompilationUnit& unit,
                          diagnostics::DiagnosticBag& diagnostics);
}  // namespace flecha::semantic
#endif
