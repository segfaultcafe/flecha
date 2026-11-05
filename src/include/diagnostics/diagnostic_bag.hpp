#ifndef FLECHA_DIAGNOSTICS_DIAGNOSTIC_BAG_HPP
#define FLECHA_DIAGNOSTICS_DIAGNOSTIC_BAG_HPP

#include <string>
#include <vector>

#include "diagnostic.hpp"

namespace flecha::diagnostics {

class DiagnosticBag {
   public:
    static constexpr std::size_t MAX_DIAGNOSTICS = 100;

    void Report(DiagnosticCode code, source::SourceRange range,
                std::string message,
                DiagnosticSeverity severity = DiagnosticSeverity::Error);

    [[nodiscard]] bool Empty() const { return _diagnostics.empty(); }
    [[nodiscard]] bool HasErrors() const;
    [[nodiscard]] const std::vector<Diagnostic>& Diagnostics() const {
        return _diagnostics;
    }

   private:
    std::vector<Diagnostic> _diagnostics;
    bool _limitReported = false;
};

}  // namespace flecha::diagnostics

#endif  // FLECHA_DIAGNOSTICS_DIAGNOSTIC_BAG_HPP
