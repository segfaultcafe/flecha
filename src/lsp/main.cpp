#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "analyzer/transport.hpp"
#include "lsp/analyzer_client.hpp"
#include "lsp/protocol.hpp"
#include "lsp/server_loop.hpp"

#ifndef FLECHA_EXECUTABLE_SUFFIX
#define FLECHA_EXECUTABLE_SUFFIX ""
#endif

namespace {

std::filesystem::path CompilerPath(const char* executable) {
    if (const char* configured = std::getenv("FLECHA_COMPILER");
        configured && *configured)
        return configured;

    std::filesystem::path server(executable);
    std::error_code error;
    if (!server.is_absolute() && server.has_parent_path())
        server = std::filesystem::absolute(server, error);
    else if (!server.is_absolute() &&
             std::filesystem::is_regular_file(server, error))
        server = std::filesystem::absolute(server, error);
    if (server.is_absolute()) {
        const std::filesystem::path sibling =
            server.parent_path() / ("flecha" FLECHA_EXECUTABLE_SUFFIX);
        if (std::filesystem::is_regular_file(sibling, error)) return sibling;
    }
    return "flecha" FLECHA_EXECUTABLE_SUFFIX;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 1) {
        std::cerr << "usage: " << argv[0] << "\n";
        return 1;
    }
    try {
#ifdef _WIN32
        if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
            _setmode(_fileno(stdout), _O_BINARY) == -1)
            throw std::runtime_error("cannot configure LSP stdio");
#endif
        flecha::analyzer::Transport transport(std::cin, std::cout);
        flecha::lsp::SubprocessAnalyzerClient analyzer(CompilerPath(argv[0]));
        flecha::lsp::ProtocolServer protocol(analyzer);
        flecha::lsp::ServerLoop loop(transport, protocol);
        return loop.Run();
    } catch (const std::exception& error) {
        std::cerr << "flecha-lsp: " << error.what() << '\n';
        return 1;
    }
}
