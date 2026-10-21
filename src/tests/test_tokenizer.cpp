#include <gtest/gtest.h>

#include <vector>

#include "core/tokenizer.hpp"

using namespace flecha::diagnostics;
using namespace flecha::core;

std::vector<Token> Tokenize(const std::string& source,
                            DiagnosticBag* diagnostics = nullptr) {
    DiagnosticBag localDiagnostics;
    Tokenizer tokenizer(
        source, diagnostics == nullptr ? localDiagnostics : *diagnostics);
    std::vector<Token> tokens = tokenizer.Tokenize();
    return tokens;
}

TEST(TokenizerTests, RecognizesKeywords) {
    std::vector<Token> tokens = Tokenize(
        "i32 u8 f64 char bool void string stringv func once unsafe return "
        "none");
    ASSERT_EQ(tokens.size(), 14);

    EXPECT_EQ(tokens[0].type, TokenType::I32);
    EXPECT_EQ(tokens[0].value, "i32");

    EXPECT_EQ(tokens[1].type, TokenType::U8);
    EXPECT_EQ(tokens[2].type, TokenType::F64);
    EXPECT_EQ(tokens[3].type, TokenType::Char);
    EXPECT_EQ(tokens[4].type, TokenType::Bool);
    EXPECT_EQ(tokens[5].type, TokenType::Void);
    EXPECT_EQ(tokens[6].type, TokenType::String);
    EXPECT_EQ(tokens[7].type, TokenType::StringView);
    EXPECT_EQ(tokens[8].type, TokenType::Func);
    EXPECT_EQ(tokens[9].type, TokenType::Once);
    EXPECT_EQ(tokens[10].type, TokenType::Unsafe);
    EXPECT_EQ(tokens[11].type, TokenType::Return);
    EXPECT_EQ(tokens[12].type, TokenType::None);
    EXPECT_EQ(tokens[13].type, TokenType::EOF_TOKEN);
}

TEST(TokenizerTests, RecognizesTestKeyword) {
    const std::vector<Token> tokens = Tokenize("test addition");

    ASSERT_EQ(tokens.size(), 3);
    EXPECT_EQ(tokens[0].type, TokenType::Test);
    EXPECT_EQ(tokens[1].type, TokenType::Identifier);
}

TEST(TokenizerTests, RecognizesTypeSetKeyword) {
    const std::vector<Token> tokens = Tokenize("typeset numbers");

    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::TypeSet);
    EXPECT_EQ(tokens[1].type, TokenType::Identifier);
}

TEST(TokenizerTests, RecognizesFunctionModifiersAndReservedWords) {
    std::vector<Token> tokens = Tokenize(
        "export quiver extern inline comptime yield if else while proceed "
        "break "
        "jumpto error const volatile as null true false defer");

    const std::vector<TokenType> expected = {
        TokenType::Export,  TokenType::Quiver,   TokenType::Extern,
        TokenType::Inline,  TokenType::Comptime, TokenType::Yield,
        TokenType::If,      TokenType::Else,     TokenType::While,
        TokenType::Proceed, TokenType::Break,    TokenType::JumpTo,
        TokenType::Error,   TokenType::Const,    TokenType::Volatile,
        TokenType::As,      TokenType::Null,     TokenType::True,
        TokenType::False,   TokenType::Defer,    TokenType::EOF_TOKEN,
    };
    ASSERT_EQ(tokens.size(), expected.size());
    for (size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(tokens[index].type, expected[index]);
    }
}

TEST(TokenizerTests, RecognizesMultiCharacterOperators) {
    std::vector<Token> tokens = Tokenize("= := == != === !== <= >= && ||");
    ASSERT_EQ(tokens.size(), 11);

    EXPECT_EQ(tokens[0].type, TokenType::Assign);
    EXPECT_EQ(tokens[1].type, TokenType::DeclareAssign);
    EXPECT_EQ(tokens[2].type, TokenType::Equal);
    EXPECT_EQ(tokens[3].type, TokenType::NotEqual);
    EXPECT_EQ(tokens[4].type, TokenType::StrictEqual);
    EXPECT_EQ(tokens[5].type, TokenType::StrictNotEqual);
    EXPECT_EQ(tokens[6].type, TokenType::LessEqual);
    EXPECT_EQ(tokens[7].type, TokenType::GreaterEqual);
    EXPECT_EQ(tokens[8].type, TokenType::And);
    EXPECT_EQ(tokens[9].type, TokenType::Or);
    EXPECT_EQ(tokens[10].type, TokenType::EOF_TOKEN);
}

TEST(TokenizerTests, RecognizesBorrowResultArrow) {
    std::vector<Token> tokens = Tokenize("<- < -");

    ASSERT_EQ(tokens.size(), 4);
    EXPECT_EQ(tokens[0].type, TokenType::LeftArrow);
    EXPECT_EQ(tokens[0].value, "<-");
    EXPECT_EQ(tokens[1].type, TokenType::Less);
    EXPECT_EQ(tokens[2].type, TokenType::Sub);
    EXPECT_EQ(tokens[3].type, TokenType::EOF_TOKEN);
}

TEST(TokenizerTests, RecognizesTextualLogicalOperators) {
    std::vector<Token> tokens = Tokenize("true and false or true");

    ASSERT_EQ(tokens.size(), 6);
    EXPECT_EQ(tokens[1].type, TokenType::And);
    EXPECT_EQ(tokens[1].value, "and");
    EXPECT_EQ(tokens[3].type, TokenType::Or);
    EXPECT_EQ(tokens[3].value, "or");
}

TEST(TokenizerTests, RecognizesAlsoKeyword) {
    std::vector<Token> tokens = Tokenize("also condition");

    ASSERT_EQ(tokens.size(), 3);
    EXPECT_EQ(tokens[0].type, TokenType::Also);
    EXPECT_EQ(tokens[1].type, TokenType::Identifier);
}

TEST(TokenizerTests, RecognizesEnumKeyword) {
    std::vector<Token> tokens = Tokenize("enum State");

    ASSERT_EQ(tokens.size(), 3);
    EXPECT_EQ(tokens[0].type, TokenType::Enum);
    EXPECT_EQ(tokens[1].type, TokenType::Identifier);
}

TEST(TokenizerTests, RecognizesMatchKeywordAndFatArrow) {
    std::vector<Token> tokens = Tokenize("match kind { _ => 0 }");

    ASSERT_EQ(tokens.size(), 8);
    EXPECT_EQ(tokens[0].type, TokenType::Match);
    EXPECT_EQ(tokens[3].type, TokenType::Identifier);
    EXPECT_EQ(tokens[4].type, TokenType::FatArrow);
    EXPECT_EQ(tokens[4].value, "=>");
}

TEST(TokenizerTests, RecognizesIdentifiers) {
    std::vector<Token> tokens = Tokenize("variable1 _v_ar2");
    ASSERT_EQ(tokens.size(), 3);  // 2 identifiers + EOF

    EXPECT_EQ(tokens[0].type, TokenType::Identifier);
    EXPECT_EQ(tokens[0].value, "variable1");

    EXPECT_EQ(tokens[1].type, TokenType::Identifier);
    EXPECT_EQ(tokens[1].value, "_v_ar2");

    EXPECT_EQ(tokens[2].type, TokenType::EOF_TOKEN);
}

TEST(TokenizerTests, RecognizesNumbers) {
    std::vector<Token> tokens = Tokenize("123 0 3.14");
    ASSERT_EQ(tokens.size(), 4);  // 3 numbers + EOF

    EXPECT_EQ(tokens[0].type, TokenType::NumberLiteral);
    EXPECT_EQ(tokens[0].value, "123");

    EXPECT_EQ(tokens[1].type, TokenType::NumberLiteral);
    EXPECT_EQ(tokens[1].value, "0");

    EXPECT_EQ(
        tokens[2].type,
        TokenType::FloatLiteral);  // If floating-point numbers are supported
    EXPECT_EQ(tokens[2].value, "3.14");

    EXPECT_EQ(tokens[3].type, TokenType::EOF_TOKEN);
}

TEST(TokenizerTests, RecognizesSpecialCharacters) {
    std::vector<Token> tokens = Tokenize("! ( ) [ ] { } , . : ; *");
    ASSERT_EQ(tokens.size(), 13);

    EXPECT_EQ(tokens[0].type, TokenType::Bang);
    EXPECT_EQ(tokens[0].value, "!");

    EXPECT_EQ(tokens[1].type, TokenType::LParen);
    EXPECT_EQ(tokens[2].type, TokenType::RParen);
    EXPECT_EQ(tokens[3].type, TokenType::LBracket);
    EXPECT_EQ(tokens[4].type, TokenType::RBracket);
    EXPECT_EQ(tokens[5].type, TokenType::LCurly);
    EXPECT_EQ(tokens[6].type, TokenType::RCurly);
    EXPECT_EQ(tokens[7].type, TokenType::Comma);
    EXPECT_EQ(tokens[8].type, TokenType::Dot);
    EXPECT_EQ(tokens[9].type, TokenType::Colon);
    EXPECT_EQ(tokens[10].type, TokenType::SemiColon);
    EXPECT_EQ(tokens[11].type, TokenType::Mul);
    EXPECT_EQ(tokens[12].type, TokenType::EOF_TOKEN);
}

TEST(TokenizerTests, RecognizesComplexStatements) {
    std::vector<Token> tokens = Tokenize("unsafe i32 main() { return none; }");
    ASSERT_EQ(tokens.size(), 11);

    EXPECT_EQ(tokens[0].type, TokenType::Unsafe);
    EXPECT_EQ(tokens[1].type, TokenType::I32);
    EXPECT_EQ(tokens[2].type, TokenType::Identifier);
    EXPECT_EQ(tokens[3].type, TokenType::LParen);
    EXPECT_EQ(tokens[4].type, TokenType::RParen);
    EXPECT_EQ(tokens[5].type, TokenType::LCurly);
    EXPECT_EQ(tokens[6].type, TokenType::Return);
    EXPECT_EQ(tokens[7].type, TokenType::None);
    EXPECT_EQ(tokens[8].type, TokenType::SemiColon);
    EXPECT_EQ(tokens[9].type, TokenType::RCurly);
    EXPECT_EQ(tokens[10].type, TokenType::EOF_TOKEN);
}

TEST(TokenizerTests, LegacyAllocationWordsAreIdentifiers) {
    std::vector<Token> tokens = Tokenize("allot dellot");

    ASSERT_EQ(tokens.size(), 3);
    EXPECT_EQ(tokens[0].type, TokenType::Identifier);
    EXPECT_EQ(tokens[1].type, TokenType::Identifier);
}

TEST(TokenizerTests, SkipsLineAndBlockComments) {
    std::vector<Token> tokens = Tokenize(
        "i32 // result type\n"
        "main /* no parameters yet */ ()");

    ASSERT_EQ(tokens.size(), 5);
    EXPECT_EQ(tokens[0].type, TokenType::I32);
    EXPECT_EQ(tokens[1].type, TokenType::Identifier);
    EXPECT_EQ(tokens[2].type, TokenType::LParen);
    EXPECT_EQ(tokens[3].type, TokenType::RParen);
    EXPECT_EQ(tokens[4].type, TokenType::EOF_TOKEN);
}

TEST(TokenizerTests, RejectsUnterminatedBlockComment) {
    DiagnosticBag diagnostics;
    std::vector<Token> tokens = Tokenize("i32 /* unfinished", &diagnostics);

    ASSERT_EQ(diagnostics.Diagnostics().size(), 1);
    EXPECT_EQ(diagnostics.Diagnostics()[0].code,
              DiagnosticCode::UnterminatedBlockComment);
    EXPECT_EQ(tokens.back().type, TokenType::EOF_TOKEN);
}

TEST(TokenizerTests, RecognizesStringLiterals) {
    std::vector<Token> tokens = Tokenize(
        "\"Hello, World!\" \"This is a test.\" \"Escape \\\"quote\\\"\"");
    ASSERT_EQ(tokens.size(), 4);  // 3 string literals + EOF

    EXPECT_EQ(tokens[0].type, TokenType::StringLiteral);
    EXPECT_EQ(tokens[0].value, "Hello, World!");

    EXPECT_EQ(tokens[1].type, TokenType::StringLiteral);
    EXPECT_EQ(tokens[1].value, "This is a test.");

    EXPECT_EQ(tokens[2].type, TokenType::StringLiteral);
    EXPECT_EQ(tokens[2].value, "Escape \"quote\"");

    EXPECT_EQ(tokens[3].type, TokenType::EOF_TOKEN);
}

TEST(TokenizerTests, UnterminatedStringLiteral) {
    DiagnosticBag diagnostics;
    std::vector<Token> tokens =
        Tokenize("\"This string is not terminated", &diagnostics);

    ASSERT_TRUE(diagnostics.HasErrors());
    EXPECT_EQ(diagnostics.Diagnostics()[0].code,
              DiagnosticCode::UnterminatedStringLiteral);
    EXPECT_EQ(tokens[0].type, TokenType::Invalid);
}

TEST(TokenizerTests, RecognizesEmptyStringLiteral) {
    std::vector<Token> tokens = Tokenize("\"\"");
    ASSERT_EQ(tokens.size(), 2);  // 1 empty string + EOF

    EXPECT_EQ(tokens[0].type, TokenType::StringLiteral);
    EXPECT_EQ(tokens[0].value, "");

    EXPECT_EQ(tokens[1].type, TokenType::EOF_TOKEN);
}

TEST(TokenizerTests, StringWithNewlineEscape) {
    std::vector<Token> tokens = Tokenize("\"Line1\\nLine2\"");
    ASSERT_EQ(tokens.size(), 2);  // 1 string + EOF

    EXPECT_EQ(tokens[0].type, TokenType::StringLiteral);
    EXPECT_EQ(tokens[0].value, "Line1\nLine2");

    EXPECT_EQ(tokens[1].type, TokenType::EOF_TOKEN);
}

TEST(TokenizerTests, RecognizesCharacterLiterals) {
    std::vector<Token> tokens = Tokenize("'a' '\\n' '\\''");
    ASSERT_EQ(tokens.size(), 4);  // 3 char literals + EOF

    EXPECT_EQ(tokens[0].type, TokenType::CharLiteral);
    EXPECT_EQ(tokens[0].value, "a");

    EXPECT_EQ(tokens[1].type, TokenType::CharLiteral);
    EXPECT_EQ(tokens[1].value, "\n");

    EXPECT_EQ(tokens[2].type, TokenType::CharLiteral);
    EXPECT_EQ(tokens[2].value, "'");

    EXPECT_EQ(tokens[3].type, TokenType::EOF_TOKEN);
}

TEST(TokenizerTests, UnterminatedCharacterLiteral) {
    DiagnosticBag diagnostics;
    std::vector<Token> tokens = Tokenize("'a", &diagnostics);

    EXPECT_TRUE(diagnostics.HasErrors());
    EXPECT_EQ(tokens[0].type, TokenType::Invalid);
}

TEST(TokenizerTests, InvalidMultiCharacterLiteral) {
    DiagnosticBag diagnostics;
    std::vector<Token> tokens = Tokenize("'ab'", &diagnostics);

    EXPECT_TRUE(diagnostics.HasErrors());
    EXPECT_EQ(tokens[0].type, TokenType::Invalid);
}

TEST(TokenizerTests, EmptyCharacterLiteral) {
    DiagnosticBag diagnostics;
    std::vector<Token> tokens = Tokenize("''", &diagnostics);

    EXPECT_TRUE(diagnostics.HasErrors());
    EXPECT_EQ(tokens[0].type, TokenType::Invalid);
}

TEST(TokenizerTests, StringWithEscapedCharacters) {
    std::vector<Token> tokens =
        Tokenize("\"This is a \\\"test\\\" with \\n and \\t.\"");
    ASSERT_EQ(tokens.size(), 2);  // 1 string + EOF

    EXPECT_EQ(tokens[0].type, TokenType::StringLiteral);
    EXPECT_EQ(tokens[0].value, "This is a \"test\" with \n and \t.");

    EXPECT_EQ(tokens[1].type, TokenType::EOF_TOKEN);
}

TEST(TokenizerTests, CharacterWithInvalidEscapeSequence) {
    DiagnosticBag diagnostics;
    std::vector<Token> tokens = Tokenize("'\\x'", &diagnostics);

    ASSERT_TRUE(diagnostics.HasErrors());
    EXPECT_EQ(diagnostics.Diagnostics()[0].code,
              DiagnosticCode::InvalidCharacterEscape);
    EXPECT_EQ(tokens[0].type, TokenType::Invalid);
}

TEST(TokenizerTests, RecordsExactHalfOpenSourceRanges) {
    std::vector<Token> tokens = Tokenize("i32 value\n  = 42;");

    EXPECT_EQ(tokens[0].range.start.offset, 0);
    EXPECT_EQ(tokens[0].range.start.line, 1);
    EXPECT_EQ(tokens[0].range.start.column, 1);
    EXPECT_EQ(tokens[0].range.end.offset, 3);
    EXPECT_EQ(tokens[0].range.end.line, 1);
    EXPECT_EQ(tokens[0].range.end.column, 4);

    EXPECT_EQ(tokens[2].range.start.offset, 12);
    EXPECT_EQ(tokens[2].range.start.line, 2);
    EXPECT_EQ(tokens[2].range.start.column, 3);
    EXPECT_EQ(tokens[2].range.end.offset, 13);
    EXPECT_EQ(tokens[2].range.end.column, 4);

    EXPECT_TRUE(tokens.back().range.Empty());
    EXPECT_EQ(tokens.back().range.start.offset, 17);
}

TEST(TokenizerTests, DiagnosticRangesIdentifyInvalidSource) {
    DiagnosticBag diagnostics;
    std::vector<Token> tokens = Tokenize("i32 ` value", &diagnostics);

    ASSERT_EQ(diagnostics.Diagnostics().size(), 1);
    const Diagnostic& diagnostic = diagnostics.Diagnostics()[0];
    EXPECT_EQ(diagnostic.code, DiagnosticCode::UnexpectedCharacter);
    EXPECT_EQ(diagnostic.range.start.offset, 4);
    EXPECT_EQ(diagnostic.range.start.column, 5);
    EXPECT_EQ(diagnostic.range.end.offset, 5);
    EXPECT_EQ(tokens[1].type, TokenType::Invalid);
    EXPECT_EQ(tokens[2].type, TokenType::Identifier);
}

TEST(TokenizerTests, RecognizesConstAliasesAndReferenceMarker) {
    DiagnosticBag diagnostics;
    const std::string source = "const constptr constref constval & && and";
    const std::vector<Token> tokens = Tokenize(source, &diagnostics);
    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(tokens.size(), 8);
    const std::vector<TokenType> expected{
        TokenType::Const,      TokenType::ConstPtr,  TokenType::ConstRef,
        TokenType::Identifier, TokenType::Ampersand, TokenType::And,
        TokenType::And,        TokenType::EOF_TOKEN};
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        EXPECT_EQ(tokens[index].type, expected[index]);
        EXPECT_EQ(source.substr(tokens[index].range.start.offset,
                                tokens[index].range.Length()),
                  tokens[index].value);
    }
}

TEST(TokenizerTests, RecognizesLoopKeywordsAndAdjacentIncrement) {
    const std::vector<Token> tokens = Tokenize("for loop range x++ x + +1");
    const std::vector<TokenType> expected{
        TokenType::For,        TokenType::Loop,      TokenType::Range,
        TokenType::Identifier, TokenType::Increment, TokenType::Identifier,
        TokenType::Add,        TokenType::Add,       TokenType::NumberLiteral,
        TokenType::EOF_TOKEN};
    ASSERT_EQ(tokens.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index)
        EXPECT_EQ(tokens[index].type, expected[index]);
    EXPECT_EQ(tokens[4].value, "++");
    EXPECT_EQ(tokens[4].range.end.offset - tokens[4].range.start.offset, 2);
}

TEST(TokenizerTests, RecognizesOwnershipContractsAsKeywords) {
    const auto tokens = Tokenize("copy own lend copier owner lender");
    ASSERT_EQ(tokens.size(), 7);
    EXPECT_EQ(tokens[0].type, TokenType::Identifier);
    EXPECT_EQ(tokens[1].type, TokenType::Own);
    EXPECT_EQ(tokens[2].type, TokenType::Lend);
    EXPECT_EQ(tokens[3].type, TokenType::Identifier);
    EXPECT_EQ(tokens[4].type, TokenType::Identifier);
    EXPECT_EQ(tokens[5].type, TokenType::Identifier);
}

TEST(TokenizerTests, RecognizesModuleAndImportDirectiveTokens) {
    const auto tokens = Tokenize("module example; @imports :: root::math;");
    ASSERT_EQ(tokens.size(), 11);
    EXPECT_EQ(tokens[0].type, TokenType::Module);
    EXPECT_EQ(tokens[3].type, TokenType::At);
    EXPECT_EQ(tokens[4].type, TokenType::Identifier);
    EXPECT_EQ(tokens[5].type, TokenType::Scope);
}
