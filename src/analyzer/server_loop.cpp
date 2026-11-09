#include "analyzer/server_loop.hpp"

#include <optional>
#include <stdexcept>
#include <string>

namespace flecha::analyzer {

void ServerLoop::Run() {
    while (_protocol.State() != ProtocolState::Shutdown) {
        std::optional<std::string> message = _transport.ReadMessage();
        if (!message) return;

        const std::string response = _protocol.Handle(*message);
        if (!_transport.WriteMessage(response))
            throw std::runtime_error("failed to write analyzer response");
    }
}

}  // namespace flecha::analyzer
