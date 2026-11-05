#ifndef FLECHA_TOKENIZER_HPP
#define FLECHA_TOKENIZER_HPP

#include <string>
#include <vector>

#include "diagnostics/diagnostic_bag.hpp"
#include "token.hpp"

using string = std::string;
template <typename ...Args> using vector = std::vector<Args...>;

namespace flecha {
namespace core {
    class Tokenizer {
    private:
        string _source;
        size_t _index;
        std::uint32_t _line;
        std::uint32_t _column;
        diagnostics::DiagnosticBag& _diagnostics;

        source::SourceLocation Location() const;
        source::SourceRange RangeFrom(source::SourceLocation start) const;
        char Peek(size_t offset = 0) const;
        char Advance();
        bool Match(char expected);
        bool IsAtEnd() const;
        void SkipTrivia();
        Token ScanForeignBlock(source::SourceLocation start, size_t startIndex);
        Token NextToken();

    public:
        Tokenizer(const string& src, diagnostics::DiagnosticBag& diagnostics);
        vector<Token> Tokenize();
    };
}
}

#endif  // FLECHA_TOKENIZER_HPP
