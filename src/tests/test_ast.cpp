#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "core/ast.hpp"

using namespace flecha::core;
using namespace flecha::source;

namespace {

constexpr SourceRange RANGE{{0, 1, 1}, {3, 1, 4}};

std::unique_ptr<TypeNode> MakeI32Type() {
    return std::make_unique<TypeNode>(RANGE, PrimitiveType::I32);
}

std::unique_ptr<LiteralExpression> MakeInteger(std::string value) {
    return std::make_unique<LiteralExpression>(RANGE, TokenType::NumberLiteral,
                                               std::move(value));
}

}  // namespace

TEST(AstTests, EveryNodeCarriesSourceRange) {
    LiteralExpression literal(RANGE, TokenType::NumberLiteral, "42");

    EXPECT_EQ(literal.Kind(), AstNodeKind::LiteralExpression);
    EXPECT_EQ(literal.Range(), RANGE);
    EXPECT_EQ(literal.Range().Length(), 3);
}

TEST(AstTests, RepresentsPrimitiveTypesAndParameters) {
    Parameter parameter(RANGE, MakeI32Type(), "value");

    EXPECT_EQ(parameter.Kind(), AstNodeKind::Parameter);
    EXPECT_EQ(parameter.Name(), "value");
    EXPECT_EQ(parameter.Type().Type(), PrimitiveType::I32);
}

TEST(AstTests, RepresentsUnaryAndBinaryExpressions) {
    std::unique_ptr<Expression> negative = std::make_unique<UnaryExpression>(
        RANGE, TokenType::Sub, MakeInteger("1"));
    BinaryExpression expression(RANGE, std::move(negative), TokenType::Add,
                                MakeInteger("2"));

    EXPECT_EQ(expression.Kind(), AstNodeKind::BinaryExpression);
    EXPECT_EQ(expression.OperatorType(), TokenType::Add);
    EXPECT_EQ(expression.Left().Kind(), AstNodeKind::UnaryExpression);
    EXPECT_EQ(expression.Right().Kind(), AstNodeKind::LiteralExpression);
}

TEST(AstTests, RepresentsCallsWithOwnedArguments) {
    std::vector<std::unique_ptr<Expression>> arguments;
    arguments.push_back(MakeInteger("10"));
    arguments.push_back(MakeInteger("3"));

    CallExpression call(RANGE,
                        std::make_unique<NameExpression>(RANGE, "divide"),
                        std::move(arguments));

    EXPECT_EQ(call.Callee().Kind(), AstNodeKind::NameExpression);
    ASSERT_EQ(call.Arguments().size(), 2);
    EXPECT_EQ(call.Arguments()[0]->Kind(), AstNodeKind::LiteralExpression);
}

TEST(AstTests, RepresentsAssignmentWithOwnedTargetAndValue) {
    AssignmentExpression assignment(
        RANGE, std::make_unique<NameExpression>(RANGE, "result"),
        MakeInteger("42"));

    EXPECT_EQ(assignment.Target().Kind(), AstNodeKind::NameExpression);
    EXPECT_EQ(assignment.Value().Kind(), AstNodeKind::LiteralExpression);
}

TEST(AstTests, RepresentsMatchExpressionsAndWildcardArms) {
    std::vector<std::unique_ptr<MatchArm>> arms;
    arms.push_back(std::make_unique<MatchArm>(
        RANGE, false, "TokenKind", "Identifier", RANGE, MakeInteger("1")));
    arms.push_back(std::make_unique<MatchArm>(RANGE, true, "", "", RANGE,
                                              MakeInteger("0")));
    MatchExpression match(RANGE,
                          std::make_unique<NameExpression>(RANGE, "kind"),
                          std::move(arms));

    EXPECT_EQ(match.Kind(), AstNodeKind::MatchExpression);
    ASSERT_EQ(match.Arms().size(), 2);
    EXPECT_EQ(match.Arms()[0]->EnumName(), "TokenKind");
    EXPECT_EQ(match.Arms()[0]->Variant(), "Identifier");
    EXPECT_TRUE(match.Arms()[1]->IsWildcard());
}

TEST(AstTests, RepresentsOptionalReturnAndVariableInitializers) {
    ReturnStatement emptyReturn(RANGE, nullptr);
    VariableDeclarationStatement variable(RANGE, MakeI32Type(), "count",
                                          MakeInteger("0"));

    EXPECT_EQ(emptyReturn.Value(), nullptr);
    ASSERT_NE(variable.Initializer(), nullptr);
    EXPECT_EQ(variable.Name(), "count");
    ASSERT_NE(variable.ExplicitType(), nullptr);
    EXPECT_EQ(variable.ExplicitType()->Type(), PrimitiveType::I32);
}

TEST(AstTests, RepresentsFunctionAndCompilationUnit) {
    std::vector<std::unique_ptr<Parameter>> parameters;
    parameters.push_back(
        std::make_unique<Parameter>(RANGE, MakeI32Type(), "value"));

    std::vector<std::unique_ptr<Statement>> statements;
    statements.push_back(std::make_unique<ReturnStatement>(
        RANGE, std::make_unique<NameExpression>(RANGE, "value")));

    std::unique_ptr<FunctionDeclaration> function =
        std::make_unique<FunctionDeclaration>(
            RANGE, std::vector<FunctionModifier>{FunctionModifier::Inline},
            MakeI32Type(), "identity", std::move(parameters),
            std::make_unique<BlockStatement>(RANGE, std::move(statements)));

    EXPECT_EQ(function->Name(), "identity");
    EXPECT_EQ(function->Modifiers().front(), FunctionModifier::Inline);
    EXPECT_EQ(function->Parameters().size(), 1);
    EXPECT_EQ(function->Body().Statements().size(), 1);

    std::vector<std::unique_ptr<Declaration>> declarations;
    declarations.push_back(std::move(function));
    CompilationUnit unit(RANGE, std::move(declarations));

    EXPECT_EQ(unit.Kind(), AstNodeKind::CompilationUnit);
    ASSERT_EQ(unit.Declarations().size(), 1);
    EXPECT_EQ(unit.Declarations()[0]->Kind(), AstNodeKind::FunctionDeclaration);
}

TEST(AstTests, RepresentsBorrowResultContract) {
    std::vector<std::unique_ptr<Parameter>> parameters;
    parameters.push_back(
        std::make_unique<Parameter>(RANGE, MakeI32Type(), "value"));
    auto function = std::make_unique<FunctionDeclaration>(
        RANGE, std::vector<FunctionModifier>{}, MakeI32Type(), "identity",
        std::move(parameters),
        std::make_unique<BlockStatement>(
            RANGE, std::vector<std::unique_ptr<Statement>>{}),
        RANGE, "", "value", RANGE);

    EXPECT_TRUE(function->HasBorrowSource());
    EXPECT_EQ(function->BorrowSource(), "value");
    EXPECT_EQ(function->BorrowSourceRange(), RANGE);
}

TEST(AstTests, NodesAreMoveOnly) {
    static_assert(!std::is_copy_constructible_v<LiteralExpression>);
    static_assert(std::is_move_constructible_v<LiteralExpression>);
    SUCCEED();
}
