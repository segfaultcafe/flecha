#include "diagnostics/diagnostic_bag.hpp"

#include <algorithm>
#include <utility>

namespace flecha::diagnostics {

void DiagnosticBag::Report(DiagnosticCode code, source::SourceRange range,
                           std::string message, DiagnosticSeverity severity) {
    if (_diagnostics.size() >= MAX_DIAGNOSTICS) {
        if (!_limitReported) {
            _diagnostics.push_back(Diagnostic{
                DiagnosticCode::TooManyDiagnostics, DiagnosticSeverity::Error,
                "too many diagnostics; stopping after " +
                    std::to_string(MAX_DIAGNOSTICS),
                range});
            _limitReported = true;
        }
        return;
    }
    _diagnostics.push_back(
        Diagnostic{code, severity, std::move(message), range});
}

bool DiagnosticBag::HasErrors() const {
    return std::ranges::any_of(_diagnostics, [](const Diagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    });
}

}  // namespace flecha::diagnostics
