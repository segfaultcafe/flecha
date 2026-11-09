#ifndef FLECHA_ANALYZER_SERVER_LOOP_HPP
#define FLECHA_ANALYZER_SERVER_LOOP_HPP

#include "analyzer/protocol.hpp"
#include "analyzer/transport.hpp"

namespace flecha::analyzer {

class ServerLoop final {
   public:
    ServerLoop(Transport& transport, ProtocolServer& protocol)
        : _transport(transport), _protocol(protocol) {}

    void Run();

   private:
    Transport& _transport;
    ProtocolServer& _protocol;
};

}  // namespace flecha::analyzer

#endif  // FLECHA_ANALYZER_SERVER_LOOP_HPP
