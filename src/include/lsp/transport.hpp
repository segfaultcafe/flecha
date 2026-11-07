#ifndef FLECHA_LSP_TRANSPORT_HPP
#define FLECHA_LSP_TRANSPORT_HPP

#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace flecha::lsp {

class Transport final {
public:
    explicit Transport(std::istream& input, std::ostream& output)
        : _input(input), _output(output) {}

    std::optional<std::string> ReadMessage();
    bool WriteMessage(std::string_view message);

private:
    std::istream& _input;
    std::ostream& _output;
};

}

#endif
