#ifndef FLECHA_LSP_SERVER_LOOP_HPP
#define FLECHA_LSP_SERVER_LOOP_HPP

#include "analyzer/transport.hpp"
#include "lsp/protocol.hpp"

namespace flecha::lsp {

class ServerLoop final {
   public:
    ServerLoop(analyzer::Transport& transport, ProtocolServer& protocol)
        : _transport(transport), _protocol(protocol) {}

    int Run();

   private:
    analyzer::Transport& _transport;
    ProtocolServer& _protocol;
};

}  // namespace flecha::lsp

#endif  // FLECHA_LSP_SERVER_LOOP_HPP
