#include <llvm/Support/Program.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const char* compiler = std::getenv("FLECHA_REAL_COMPILER");
    const char* trace = std::getenv("FLECHA_COMPILER_TRACE");
    if (!compiler || !trace) return 2;
    {
        std::ofstream output(trace, std::ios::app);
        if (!output) return 2;
        for (int index = 1; index < argc; ++index)
            output << argv[index] << '\t';
        output << '\n';
    }
    std::vector<llvm::StringRef> arguments{compiler};
    for (int index = 1; index < argc; ++index)
        arguments.emplace_back(argv[index]);
    const int status = llvm::sys::ExecuteAndWait(compiler, arguments);
    return status < 0 ? 1 : status;
}
