#ifndef FLECHA_CORE_PARSER_HPP
#define FLECHA_CORE_PARSER_HPP

#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

#include "ast.hpp"
#include "diagnostics/diagnostic_bag.hpp"
#include "token.hpp"

namespace flecha::core {

class Parser {
   public:
    Parser(std::vector<Token> tokens, diagnostics::DiagnosticBag& diagnostics);

    void ParseHeader();
    const std::vector<Import>& Imports() const { return _imports; }
    void SetImports(std::vector<Import> imports);
    [[nodiscard]] std::unique_ptr<CompilationUnit> Parse();

   private:
    bool _headerParsed = false;
    std::string _moduleName;
    std::vector<Import> _imports;
    std::set<std::string> _importAliases;
    [[nodiscard]] const Token& Current() const;
    [[nodiscard]] const Token& Peek(std::size_t offset) const;
    [[nodiscard]] bool IsAtEnd() const;
    [[nodiscard]] bool Check(TokenType type) const;
    [[nodiscard]] bool IsInferredVariableDeclaration() const;
    const Token& Advance();
    bool Match(TokenType type);
    const Token* Consume(TokenType type, std::string_view expectation);
    [[nodiscard]] bool StartsType() const;
    [[nodiscard]] bool IsTypeToken(TokenType type) const;
    bool ScanType(std::size_t& offset) const;
    bool ScanTypeArguments(std::size_t& offset) const;
    std::vector<std::unique_ptr<TypeNode>> ParseTypeArguments();
    std::vector<std::string> ParseGenericParameters();
    std::unique_ptr<StructureDeclaration> ParseStructureDeclaration();
    std::unique_ptr<ErrorDeclaration> ParseErrorDeclaration();
    std::unique_ptr<EnumDeclaration> ParseEnumDeclaration();
    std::unique_ptr<Declaration> ParseDeclaration();
    std::unique_ptr<FunctionDeclaration> ParseTestDeclaration();
    std::unique_ptr<CallableTypeDeclaration> ParseCallableTypeDeclaration();
    std::unique_ptr<FunctionDeclaration> ParseFunctionDeclaration();
    std::unique_ptr<TypeNode> ParseType(bool allowOwn = false,
                                        bool allowLend = false,
                                        bool allowTupleOwners = false);
    std::optional<std::uint64_t> ParseArrayLength();
    std::unique_ptr<Expression> ParseArrayExpression();
    std::unique_ptr<Expression> ParseMatchExpression();
    std::unique_ptr<Parameter> ParseParameter();
    std::unique_ptr<Parameter> ParseCallableParameter();
    std::unique_ptr<BlockStatement> ParseBlockStatement(bool isUnsafe = false);
    std::unique_ptr<Statement> ParseStatement();
    bool HasParenthesizedBindings() const;
    bool IsDestructureStatement() const;
    std::unique_ptr<Statement> ParseDestructureStatement();
    std::unique_ptr<VariableDeclarationStatement>
    ParseVariableDeclarationStatement();
    std::unique_ptr<ReturnStatement> ParseReturnStatement();
    std::unique_ptr<IfStatement> ParseIfStatement();
    std::unique_ptr<WhileStatement> ParseWhileStatement();
    std::unique_ptr<Statement> ParseLoopStatement();
    std::unique_ptr<RangeStatement> ParseRangeStatement();
    std::unique_ptr<ProceedStatement> ParseProceedStatement();
    std::unique_ptr<BreakStatement> ParseBreakStatement();
    std::unique_ptr<JumpToStatement> ParseJumpToStatement();
    std::unique_ptr<PositionStatement> ParsePositionStatement();
    std::unique_ptr<ConditionalBranch> ParseConditionalBranch(
        source::SourceLocation start);
    std::unique_ptr<AlsoClause> ParseAlsoClause();
    std::unique_ptr<Expression> ParseExpression();
    std::unique_ptr<Expression> ParseAssignmentExpression();
    std::unique_ptr<Expression> ParseBinaryExpression(int minimumPrecedence);
    std::unique_ptr<Expression> ParseUnaryExpression();
    std::unique_ptr<Expression> ParsePostfixExpression();
    std::vector<std::unique_ptr<Expression>> ParseArgumentList(
        source::SourceLocation& end);
    std::unique_ptr<Expression> ParsePrimaryExpression();
    [[nodiscard]] int BinaryPrecedence(TokenType type) const;
    [[nodiscard]] int UnaryPrecedence(TokenType type) const;
    void ReportUnexpectedToken(std::string_view context);
    void SynchronizeTopLevel();
    void SynchronizeStatement();

    std::vector<Token> _tokens;
    std::size_t _position = 0;
    std::size_t _rangeId = 0;
    diagnostics::DiagnosticBag& _diagnostics;
};

}  // namespace flecha::core

#endif  // FLECHA_CORE_PARSER_HPP
