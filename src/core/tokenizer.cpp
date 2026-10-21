#include "core/tokenizer.hpp"

#include <cctype>
#include <string_view>
#include <unordered_map>

namespace flecha::core {
namespace {

using KeywordMap = std::unordered_map<std::string_view, TokenType>;

const KeywordMap KEYWORDS = {
    {"i8", TokenType::I8},
    {"i16", TokenType::I16},
    {"i32", TokenType::I32},
    {"i64", TokenType::I64},
    {"isize", TokenType::ISize},
    {"u8", TokenType::U8},
    {"u16", TokenType::U16},
    {"u32", TokenType::U32},
    {"u64", TokenType::U64},
    {"usize", TokenType::USize},
    {"f32", TokenType::F32},
    {"f64", TokenType::F64},
    {"bool", TokenType::Bool},
    {"char", TokenType::Char},
    {"void", TokenType::Void},
    {"string", TokenType::String},
    {"stringv", TokenType::StringView},
    {"module", TokenType::Module},
    {"test", TokenType::Test},
    {"enum", TokenType::Enum},
    {"typeset", TokenType::TypeSet},
    {"structure", TokenType::Structure},
    {"private", TokenType::Private},
    {"override", TokenType::Override},
    {"build", TokenType::Build},
    {"func", TokenType::Func},
    {"once", TokenType::Once},
    {"unsafe", TokenType::Unsafe},
    {"export", TokenType::Export},
    {"quiver", TokenType::Quiver},
    {"extern", TokenType::Extern},
    {"inline", TokenType::Inline},
    {"comptime", TokenType::Comptime},
    {"return", TokenType::Return},
    {"defer", TokenType::Defer},
    {"yield", TokenType::Yield},
    {"if", TokenType::If},
    {"match", TokenType::Match},
    {"else", TokenType::Else},
    {"also", TokenType::Also},
    {"while", TokenType::While},
    {"for", TokenType::For},
    {"loop", TokenType::Loop},
    {"range", TokenType::Range},
    {"proceed", TokenType::Proceed},
    {"break", TokenType::Break},
    {"jumpto", TokenType::JumpTo},
    {"error", TokenType::Error},
    {"const", TokenType::Const},
    {"own", TokenType::Own},
    {"lend", TokenType::Lend},
    {"constptr", TokenType::ConstPtr},
    {"constref", TokenType::ConstRef},
    {"volatile", TokenType::Volatile},
    {"as", TokenType::As},
    {"none", TokenType::None},
    {"null", TokenType::Null},
    {"true", TokenType::True},
    {"false", TokenType::False},
    {"and", TokenType::And},
    {"or", TokenType::Or},
};

bool IsIdentifierStart(char character) {
    const unsigned char value = static_cast<unsigned char>(character);
    return std::isalpha(value) != 0 || character == '_';
}

bool IsIdentifierContinue(char character) {
    const unsigned char value = static_cast<unsigned char>(character);
    return std::isalnum(value) != 0 || character == '_';
}

}  // namespace

source::SourceLocation Tokenizer::Location() const {
    return {_index, _line, _column};
}

source::SourceRange Tokenizer::RangeFrom(source::SourceLocation start) const {
    return {start, Location()};
}

char Tokenizer::Peek(size_t offset) const {
    const size_t position = _index + offset;
    return position < _source.size() ? _source[position] : '\0';
}

char Tokenizer::Advance() {
    const char character = Peek();
    if (IsAtEnd()) {
        return '\0';
    }

    ++_index;
    if (character == '\n') {
        ++_line;
        _column = 1;
    } else {
        ++_column;
    }
    return character;
}

bool Tokenizer::Match(char expected) {
    if (Peek() != expected) {
        return false;
    }
    Advance();
    return true;
}

bool Tokenizer::IsAtEnd() const { return _index >= _source.size(); }

void Tokenizer::SkipTrivia() {
    while (!IsAtEnd()) {
        const unsigned char current = static_cast<unsigned char>(Peek());
        if (std::isspace(current) != 0) {
            Advance();
            continue;
        }

        if (Peek() == '/' && Peek(1) == '/') {
            while (!IsAtEnd() && Peek() != '\n') {
                Advance();
            }
            continue;
        }

        if (Peek() == '/' && Peek(1) == '*') {
            const source::SourceLocation start = Location();
            Advance();
            Advance();
            while (!IsAtEnd() && !(Peek() == '*' && Peek(1) == '/')) {
                Advance();
            }
            if (IsAtEnd()) {
                _diagnostics.Report(
                    diagnostics::DiagnosticCode::UnterminatedBlockComment,
                    RangeFrom(start), "unterminated block comment");
                return;
            }
            Advance();
            Advance();
            continue;
        }

        break;
    }
}

Token Tokenizer::NextToken() {
    SkipTrivia();
    if (IsAtEnd()) {
        const source::SourceLocation location = Location();
        return Token(TokenType::EOF_TOKEN, "", {location, location});
    }

    const source::SourceLocation start = Location();
    const size_t startIndex = start.offset;
    const char character = Advance();

    const auto makeToken = [&](TokenType type) {
        return Token(type, _source.substr(startIndex, _index - startIndex),
                     RangeFrom(start));
    };

    switch (character) {
        case ';':
            return makeToken(TokenType::SemiColon);
        case ',':
            return makeToken(TokenType::Comma);
        case '.':
            return makeToken(TokenType::Dot);
        case '(':
            return makeToken(TokenType::LParen);
        case ')':
            return makeToken(TokenType::RParen);
        case '[':
            return makeToken(TokenType::LBracket);
        case ']':
            return makeToken(TokenType::RBracket);
        case '{':
            return makeToken(TokenType::LCurly);
        case '}':
            return makeToken(TokenType::RCurly);
        case '+':
            return makeToken(Match('+') ? TokenType::Increment
                                        : TokenType::Add);
        case '-':
            return makeToken(Match('>') ? TokenType::Arrow : TokenType::Sub);
        case '*':
            return makeToken(TokenType::Mul);
        case '/':
            return makeToken(TokenType::Div);
        case '%':
            return makeToken(TokenType::Mod);
        case '^':
            return makeToken(TokenType::Xor);
        case '@':
            return makeToken(TokenType::At);
        case '#':
            return makeToken(TokenType::Hash);
        case ':':
            if (Match(':')) return makeToken(TokenType::Scope);
            return makeToken(Match('=') ? TokenType::DeclareAssign
                                        : TokenType::Colon);
        case '=':
            if (Match('>')) return makeToken(TokenType::FatArrow);
            if (Match('=')) {
                return makeToken(Match('=') ? TokenType::StrictEqual
                                            : TokenType::Equal);
            }
            return makeToken(TokenType::Assign);
        case '!':
            if (Match('=')) {
                return makeToken(Match('=') ? TokenType::StrictNotEqual
                                            : TokenType::NotEqual);
            }
            return makeToken(TokenType::Bang);
        case '<':
            if (Match('-')) return makeToken(TokenType::LeftArrow);
            return makeToken(Match('=') ? TokenType::LessEqual
                                        : TokenType::Less);
        case '>':
            return makeToken(Match('=') ? TokenType::GreaterEqual
                                        : TokenType::Greater);
        case '&':
            if (Match('&')) return makeToken(TokenType::And);
            return makeToken(TokenType::Ampersand);
        case '|':
            if (Match('|')) return makeToken(TokenType::Or);
            break;
        case '"': {
            string value;
            bool valid = true;
            while (!IsAtEnd() && Peek() != '"') {
                if (Peek() == '\n') {
                    _diagnostics.Report(
                        diagnostics::DiagnosticCode::UnterminatedStringLiteral,
                        RangeFrom(start), "unterminated string literal");
                    return Token(
                        TokenType::Invalid,
                        _source.substr(startIndex, _index - startIndex),
                        RangeFrom(start));
                }
                if (Peek() != '\\') {
                    value += Advance();
                    continue;
                }

                const source::SourceLocation escapeStart = Location();
                Advance();
                if (IsAtEnd()) break;
                switch (Advance()) {
                    case 'n':
                        value += '\n';
                        break;
                    case 't':
                        value += '\t';
                        break;
                    case 'r':
                        value += '\r';
                        break;
                    case '\\':
                        value += '\\';
                        break;
                    case '"':
                        value += '"';
                        break;
                    default:
                        valid = false;
                        _diagnostics.Report(
                            diagnostics::DiagnosticCode::InvalidStringEscape,
                            RangeFrom(escapeStart),
                            "invalid escape sequence in string literal");
                        break;
                }
            }
            if (!Match('"')) {
                _diagnostics.Report(
                    diagnostics::DiagnosticCode::UnterminatedStringLiteral,
                    RangeFrom(start), "unterminated string literal");
                valid = false;
            }
            return Token(valid ? TokenType::StringLiteral : TokenType::Invalid,
                         value, RangeFrom(start));
        }
        case '\'': {
            if (IsAtEnd() || Peek() == '\'' || Peek() == '\n') {
                if (Peek() == '\'') Advance();
                _diagnostics.Report(
                    diagnostics::DiagnosticCode::InvalidCharacterLiteral,
                    RangeFrom(start),
                    "character literal must contain exactly one character");
                return Token(TokenType::Invalid,
                             _source.substr(startIndex, _index - startIndex),
                             RangeFrom(start));
            }

            char value = Advance();
            if (value == '\\') {
                if (IsAtEnd()) {
                    _diagnostics.Report(
                        diagnostics::DiagnosticCode::InvalidCharacterLiteral,
                        RangeFrom(start), "unterminated character literal");
                    return Token(TokenType::Invalid, "", RangeFrom(start));
                }
                const source::SourceLocation escapeStart{_index - 1, _line,
                                                         _column - 1};
                switch (Advance()) {
                    case 'n':
                        value = '\n';
                        break;
                    case 't':
                        value = '\t';
                        break;
                    case 'r':
                        value = '\r';
                        break;
                    case '0':
                        value = '\0';
                        break;
                    case '\\':
                        value = '\\';
                        break;
                    case '\'':
                        value = '\'';
                        break;
                    default:
                        _diagnostics.Report(
                            diagnostics::DiagnosticCode::InvalidCharacterEscape,
                            RangeFrom(escapeStart),
                            "invalid escape sequence in character literal");
                        while (!IsAtEnd() && Peek() != '\'' && Peek() != '\n') {
                            Advance();
                        }
                        Match('\'');
                        return Token(TokenType::Invalid, "", RangeFrom(start));
                }
            }
            if (!Match('\'')) {
                while (!IsAtEnd() && Peek() != '\'' && Peek() != '\n') {
                    Advance();
                }
                Match('\'');
                _diagnostics.Report(
                    diagnostics::DiagnosticCode::InvalidCharacterLiteral,
                    RangeFrom(start),
                    "character literal must contain exactly one character");
                return Token(TokenType::Invalid, "", RangeFrom(start));
            }
            return Token(TokenType::CharLiteral, string(1, value),
                         RangeFrom(start));
        }
        default:
            break;
    }

    const unsigned char unsignedCharacter =
        static_cast<unsigned char>(character);
    if (std::isdigit(unsignedCharacter) != 0) {
        while (std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
            Advance();
        }

        TokenType type = TokenType::NumberLiteral;
        if (Peek() == '.' &&
            std::isdigit(static_cast<unsigned char>(Peek(1))) != 0) {
            type = TokenType::FloatLiteral;
            Advance();
            while (std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                Advance();
            }
        }
        return makeToken(type);
    }

    if (IsIdentifierStart(character)) {
        while (IsIdentifierContinue(Peek())) {
            Advance();
        }
        const string word = _source.substr(startIndex, _index - startIndex);
        const KeywordMap::const_iterator keyword = KEYWORDS.find(word);
        return Token(
            keyword == KEYWORDS.end() ? TokenType::Identifier : keyword->second,
            word, RangeFrom(start));
    }

    _diagnostics.Report(diagnostics::DiagnosticCode::UnexpectedCharacter,
                        RangeFrom(start),
                        "unexpected character '" + string(1, character) + "'");
    return makeToken(TokenType::Invalid);
}

Tokenizer::Tokenizer(const string& source,
                     diagnostics::DiagnosticBag& diagnostics)
    : _source(source),
      _index(0),
      _line(1),
      _column(1),
      _diagnostics(diagnostics) {}

vector<Token> Tokenizer::Tokenize() {
    vector<Token> tokens;
    while (tokens.empty() || tokens.back().type != TokenType::EOF_TOKEN) {
        tokens.push_back(NextToken());
    }
    return tokens;
}

}  // namespace flecha::core
