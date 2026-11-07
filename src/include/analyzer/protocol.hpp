#ifndef FLECHA_ANALYZER_PROTOCOL_HPP
#define FLECHA_ANALYZER_PROTOCOL_HPP

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "analyzer/analysis_session.hpp"

namespace flecha::analyzer {

enum class ProtocolState { Starting, Initialized, Shutdown };

class ProtocolServer final {
   public:
    static constexpr int PROTOCOL_MAJOR = 1;
    static constexpr int PROTOCOL_MINOR = 0;

    explicit ProtocolServer(std::string compilerVersion)
        : _compilerVersion(std::move(compilerVersion)) {}

    [[nodiscard]] std::string Handle(std::string_view message);
    [[nodiscard]] ProtocolState State() const { return _state; }

   private:
    std::string _compilerVersion;
    ProtocolState _state = ProtocolState::Starting;
    std::unique_ptr<AnalysisSession> _session;
};

}  // namespace flecha::analyzer

#endif  // FLECHA_ANALYZER_PROTOCOL_HPP
