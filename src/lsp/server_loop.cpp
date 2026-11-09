#include "lsp/server_loop.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace flecha::lsp {

int ServerLoop::Run() {
    while (_protocol.State() != ProtocolState::Exited) {
        std::optional<std::string> message = _transport.ReadMessage();
        if (!message) break;
        const std::vector<std::string> outgoing = _protocol.Handle(*message);
        for (const std::string& response : outgoing) {
            if (!_transport.WriteMessage(response))
                throw std::runtime_error("failed to write LSP response");
        }
    }
    return _protocol.State() == ProtocolState::Exited ? _protocol.ExitCode()
                                                      : 0;
}

}  // namespace flecha::lsp
