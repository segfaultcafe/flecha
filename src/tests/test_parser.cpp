#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "core/parser.hpp"
#include "core/tokenizer.hpp"

using namespace flecha::core;
using namespace flecha::diagnostics;
using namespace flecha::source;

namespace {

std::vector<Token> Lex(const std::string& source, DiagnosticBag& diagnostics) {
    Tokenizer tokenizer(source, diagnostics);
    return tokenizer.Tokenize();
}

std::unique_ptr<CompilationUnit> ParseSource(const std::string& source,
                                             DiagnosticBag& diagnostics) {
    std::vector<Token> tokens = Lex(source, diagnostics);
    Parser parser(std::move(tokens), diagnostics);
    return parser.Parse();
}

const Expression& ReturnedExpression(const CompilationUnit& unit) {
    const FunctionDeclaration& function =
        static_cast<const FunctionDeclaration&>(*unit.Declarations()[0]);
    const ReturnStatement& statement =
        static_cast<const ReturnStatement&>(*function.Body().Statements()[0]);
    return *statement.Value();
}

}  // namespace

TEST(ParserTests, ParsesEmptyCompilationUnit) {
    DiagnosticBag diagnostics;
    Parser parser({}, diagnostics);

    std::unique_ptr<CompilationUnit> unit = parser.Parse();

    ASSERT_NE(unit, nullptr);
    EXPECT_EQ(unit->Kind(), AstNodeKind::CompilationUnit);
    EXPECT_TRUE(unit->Declarations().empty());
    EXPECT_TRUE(unit->Range().Empty());
    EXPECT_TRUE(diagnostics.Empty());
}

TEST(ParserTests, UsesTokenRangesForCompilationUnit) {
    DiagnosticBag diagnostics;
    std::vector<Token> tokens = Lex("i32", diagnostics);
    Parser parser(std::move(tokens), diagnostics);

    std::unique_ptr<CompilationUnit> unit = parser.Parse();

    EXPECT_EQ(unit->Range().start.offset, 0);
    EXPECT_EQ(unit->Range().end.offset, 3);
}

TEST(ParserTests, ReportsUnexpectedTopLevelTokensWithoutThrowing) {
    DiagnosticBag diagnostics;
    std::vector<Token> tokens = Lex("main + other", diagnostics);
    Parser parser(std::move(tokens), diagnostics);

    std::unique_ptr<CompilationUnit> unit;
    EXPECT_NO_THROW(unit = parser.Parse());

    ASSERT_NE(unit, nullptr);
    ASSERT_EQ(diagnostics.Diagnostics().size(), 1);
    EXPECT_EQ(diagnostics.Diagnostics()[0].code,
              DiagnosticCode::UnexpectedToken);
    EXPECT_EQ(diagnostics.Diagnostics()[0].range.start.offset, 0);
}

TEST(ParserTests, ParsesCompleteFunctionWithReturnStatement) {
    DiagnosticBag diagnostics;
    std::vector<Token> tokens = Lex("i32 main() { return 0; }", diagnostics);
    Parser parser(std::move(tokens), diagnostics);

    std::unique_ptr<CompilationUnit> unit = parser.Parse();

    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 1);
    ASSERT_EQ(unit->Declarations()[0]->Kind(),
              AstNodeKind::FunctionDeclaration);
    const FunctionDeclaration& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    EXPECT_EQ(function.Name(), "main");
    EXPECT_EQ(function.ReturnType().Type(), PrimitiveType::I32);
    EXPECT_TRUE(function.Parameters().empty());
    ASSERT_EQ(function.Body().Statements().size(), 1);
    ASSERT_EQ(function.Body().Statements()[0]->Kind(),
              AstNodeKind::ReturnStatement);
    const ReturnStatement& returnStatement =
        static_cast<const ReturnStatement&>(*function.Body().Statements()[0]);
    ASSERT_NE(returnStatement.Value(), nullptr);
    EXPECT_EQ(returnStatement.Value()->Kind(), AstNodeKind::LiteralExpression);
    EXPECT_EQ(unit->Range().start.offset, 0);
    EXPECT_EQ(unit->Range().end.offset, 24);
}

TEST(ParserTests, ParsesModuleConstantsWithoutConfusingConstFunctions) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(
        "export const usize LIMIT = 4 * 2; const i32 answer() { return 42; }",
        diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 2);
    ASSERT_EQ(unit->Declarations()[0]->Kind(),
              AstNodeKind::ModuleConstantDeclaration);
    const auto& constant =
        static_cast<const ModuleConstantDeclaration&>(*unit->Declarations()[0]);
    EXPECT_EQ(constant.Name(), "LIMIT");
    EXPECT_TRUE(constant.Type().IsConst());
    EXPECT_TRUE(constant.exported);
    EXPECT_EQ(constant.Initializer().Kind(), AstNodeKind::BinaryExpression);
    EXPECT_EQ(unit->Declarations()[1]->Kind(),
              AstNodeKind::FunctionDeclaration);
}

TEST(ParserTests, ParsesTypedParametersAndBareReturn) {
    DiagnosticBag diagnostics;
    std::vector<Token> tokens =
        Lex("void reset(i32 value, bool enabled) { return; }", diagnostics);
    Parser parser(std::move(tokens), diagnostics);

    std::unique_ptr<CompilationUnit> unit = parser.Parse();

    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 1);
    const FunctionDeclaration& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    ASSERT_EQ(function.Parameters().size(), 2);
    EXPECT_EQ(function.Parameters()[0]->Name(), "value");
    EXPECT_EQ(function.Parameters()[0]->Type().Type(), PrimitiveType::I32);
    EXPECT_EQ(function.Parameters()[1]->Name(), "enabled");
    EXPECT_EQ(function.Parameters()[1]->Type().Type(), PrimitiveType::Bool);
    const ReturnStatement& returnStatement =
        static_cast<const ReturnStatement&>(*function.Body().Statements()[0]);
    EXPECT_EQ(returnStatement.Value(), nullptr);
}

TEST(ParserTests, ParsesNamedCallableSignatures) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(
        "func Mapper(i32, stringv text) => (i32); "
        "unsafe func Reader(lend *const u8 source) => (own *u8, error);",
        diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 2);
    ASSERT_EQ(unit->Declarations()[0]->Kind(),
              AstNodeKind::CallableTypeDeclaration);
    const auto& mapper =
        static_cast<const CallableTypeDeclaration&>(*unit->Declarations()[0]);
    EXPECT_EQ(mapper.Name(), "Mapper");
    EXPECT_FALSE(mapper.IsUnsafe());
    ASSERT_EQ(mapper.Parameters().size(), 2);
    EXPECT_TRUE(mapper.Parameters()[0]->Name().empty());
    EXPECT_EQ(mapper.Parameters()[1]->Name(), "text");
    ASSERT_EQ(mapper.Results().size(), 1);
    EXPECT_EQ(mapper.Results()[0]->Type(), PrimitiveType::I32);

    const auto& reader =
        static_cast<const CallableTypeDeclaration&>(*unit->Declarations()[1]);
    EXPECT_TRUE(reader.IsUnsafe());
    EXPECT_EQ(reader.Parameters()[0]->Type().Contract(),
              OwnershipContract::Lend);
    ASSERT_EQ(reader.Results().size(), 2);
    EXPECT_EQ(reader.Results()[0]->Contract(), OwnershipContract::Own);
}

TEST(ParserTests, ParsesOnceCallableSignaturesAndLiterals) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(R"(
        unsafe once func Task(i32) => (i32);
        void main() {
            Task task = once unsafe [copy value](i32 input) => (i32) {
                return input + value;
            };
        }
    )",
                            diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 2);
    const auto& task =
        static_cast<const CallableTypeDeclaration&>(*unit->Declarations()[0]);
    EXPECT_TRUE(task.IsUnsafe());
    EXPECT_TRUE(task.IsOnce());
    const auto& main =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[1]);
    const auto& declaration = static_cast<const VariableDeclarationStatement&>(
        *main.Body().Statements()[0]);
    const auto& literal = static_cast<const AnonymousFunctionExpression&>(
        *declaration.Initializer());
    EXPECT_TRUE(literal.Function().HasModifier(FunctionModifier::Unsafe));
    EXPECT_TRUE(literal.Function().HasModifier(FunctionModifier::Once));
}

TEST(ParserTests, ReportsDuplicateCallableModifiersAtExactRanges) {
    struct Case final {
        std::string source;
        std::string message;
        std::string duplicate;
    };
    const std::vector<Case> cases{
        {"once once func Task() => ();", "duplicate callable modifier 'once'",
         "once"},
        {"unsafe unsafe func Task() => ();",
         "duplicate callable modifier 'unsafe'", "unsafe"},
        {"func Task() => (); void main() { "
         "Task task = once once []() => () {}; }",
         "duplicate anonymous function modifier 'once'", "once"},
        {"unsafe func Task() => (); void main() { "
         "Task task = unsafe unsafe []() => () {}; }",
         "duplicate anonymous function modifier 'unsafe'", "unsafe"},
    };

    for (const Case& test : cases) {
        DiagnosticBag diagnostics;
        ParseSource(test.source, diagnostics);

        ASSERT_EQ(diagnostics.Diagnostics().size(), 1) << test.source;
        const Diagnostic& diagnostic = diagnostics.Diagnostics().front();
        EXPECT_EQ(diagnostic.code, DiagnosticCode::UnexpectedToken);
        EXPECT_EQ(diagnostic.message, test.message);
        EXPECT_EQ(diagnostic.range.start.offset,
                  test.source.rfind(test.duplicate));
        EXPECT_EQ(diagnostic.range.Length(), test.duplicate.size());
    }
}

TEST(ParserTests, ParsesGenericCallableSignatures) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(
        "export func Predicate<T>(&const T value) => (bool); "
        "func Transform<T, U>(T) => (U);",
        diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 2);
    const auto& predicate =
        static_cast<const CallableTypeDeclaration&>(*unit->Declarations()[0]);
    EXPECT_TRUE(predicate.exported);
    ASSERT_EQ(predicate.genericParameters.size(), 1);
    EXPECT_EQ(predicate.genericParameters[0], "T");
    EXPECT_EQ(predicate.Parameters()[0]->Type().Target()->Name(), "T");
    EXPECT_EQ(predicate.Parameters()[0]->Name(), "value");
    EXPECT_FALSE(predicate.templateTokens.empty());
    ASSERT_EQ(predicate.parameterTokenRanges.size(), 1);

    const auto& transform =
        static_cast<const CallableTypeDeclaration&>(*unit->Declarations()[1]);
    ASSERT_EQ(transform.genericParameters.size(), 2);
    EXPECT_EQ(transform.genericParameters[0], "T");
    EXPECT_EQ(transform.genericParameters[1], "U");
    EXPECT_EQ(transform.Parameters()[0]->Type().Name(), "T");
    EXPECT_EQ(transform.Results()[0]->Name(), "U");
}

TEST(ParserTests, ParsesTypeSetsAndGenericConstraints) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(R"(
        typeset indices { u8, u16, usize, };
        structure Box<T: indices> { T value; };
        bool accepts<T: numeric, U: indices>(T value, U index) {
            return T.isof<floating_point>;
        }
    )",
                            diagnostics);

    for (const auto& diagnostic : diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 3u);
    const auto& set =
        static_cast<const TypeSetDeclaration&>(*unit->Declarations()[0]);
    EXPECT_EQ(set.Name(), "indices");
    ASSERT_EQ(set.Members().size(), 3u);
    EXPECT_EQ(set.Members()[2]->Type(), PrimitiveType::USize);

    const auto& box =
        static_cast<const StructureDeclaration&>(*unit->Declarations()[1]);
    EXPECT_EQ(box.genericConstraints.at("T"), "indices");
    const auto& accepts =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[2]);
    EXPECT_EQ(accepts.genericConstraints.at("T"), "numeric");
    EXPECT_EQ(accepts.genericConstraints.at("U"), "indices");
    const auto& returned = static_cast<const ReturnStatement&>(
        *accepts.Body().Statements().front());
    EXPECT_EQ(returned.Value()->Kind(), AstNodeKind::IsOfExpression);
}

TEST(ParserTests, ParsesCapturelessAnonymousFunctionForms) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(R"(
        func Mapper(i32) => (i32);
        void main() {
            Mapper explicit = [](i32 value) => (i32) { return value; };
            Mapper inferred = (value) => { return value; };
        }
    )",
                            diagnostics);

    for (const auto& diagnostic : diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 2);
    const auto& main =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[1]);
    ASSERT_EQ(main.Body().Statements().size(), 2);

    const auto& explicitVariable =
        static_cast<const VariableDeclarationStatement&>(
            *main.Body().Statements()[0]);
    const auto& explicitFunction =
        static_cast<const AnonymousFunctionExpression&>(
            *explicitVariable.Initializer());
    EXPECT_TRUE(explicitFunction.HasExplicitCaptureList());
    EXPECT_FALSE(explicitFunction.HasInferredSignature());
    ASSERT_EQ(explicitFunction.Function().Parameters().size(), 1);
    EXPECT_EQ(explicitFunction.Function().Parameters()[0]->Name(), "value");
    EXPECT_EQ(explicitFunction.Function().Parameters()[0]->Type().Type(),
              PrimitiveType::I32);
    EXPECT_EQ(explicitFunction.Function().ReturnType().Type(),
              PrimitiveType::I32);

    const auto& inferredVariable =
        static_cast<const VariableDeclarationStatement&>(
            *main.Body().Statements()[1]);
    const auto& inferredFunction =
        static_cast<const AnonymousFunctionExpression&>(
            *inferredVariable.Initializer());
    EXPECT_FALSE(inferredFunction.HasExplicitCaptureList());
    EXPECT_TRUE(inferredFunction.HasInferredSignature());
    EXPECT_EQ(inferredFunction.Function().Parameters()[0]->Name(), "value");
}

TEST(ParserTests, ParsesCopyCaptureLists) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(R"(
        func Mapper(i32) => (i32);
        void main() {
            Mapper callback = [copy factor, copy offset](value) => {
                return value + factor + offset;
            };
        }
    )",
                            diagnostics);

    for (const auto& diagnostic : diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(diagnostics.HasErrors());
    const auto& main =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[1]);
    const auto& variable = static_cast<const VariableDeclarationStatement&>(
        *main.Body().Statements()[0]);
    const auto& function = static_cast<const AnonymousFunctionExpression&>(
        *variable.Initializer());
    ASSERT_EQ(function.Captures().size(), 2);
    EXPECT_EQ(function.Captures()[0]->Name(), "factor");
    EXPECT_EQ(function.Captures()[1]->Name(), "offset");
    EXPECT_EQ(function.Captures()[0]->Mode(),
              AnonymousFunctionExpression::CaptureMode::Copy);
}

TEST(ParserTests, ParsesCheckedReferenceCaptureLists) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(R"(
        func Mapper() => (i32);
        void main() {
            Mapper callback = [&value, &const limit]() => {
                return value + limit;
            };
        }
    )",
                            diagnostics);

    for (const auto& diagnostic : diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(diagnostics.HasErrors());
    const auto& main =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[1]);
    const auto& variable = static_cast<const VariableDeclarationStatement&>(
        *main.Body().Statements()[0]);
    const auto& function = static_cast<const AnonymousFunctionExpression&>(
        *variable.Initializer());
    ASSERT_EQ(function.Captures().size(), 2);
    EXPECT_EQ(function.Captures()[0]->Name(), "value");
    EXPECT_EQ(function.Captures()[0]->Mode(),
              AnonymousFunctionExpression::CaptureMode::Reference);
    EXPECT_EQ(function.Captures()[1]->Name(), "limit");
    EXPECT_EQ(function.Captures()[1]->Mode(),
              AnonymousFunctionExpression::CaptureMode::ConstReference);
}

TEST(ParserTests, ParsesRawResourceCaptureLists) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(R"(
        unsafe func Reader() => (i32);
        void main() {
            Reader callback = unsafe [lend borrowed, own transferred]() => {
                return *borrowed + *transferred;
            };
        }
    )",
                            diagnostics);

    for (const auto& diagnostic : diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(diagnostics.HasErrors());
    const auto& main =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[1]);
    const auto& variable = static_cast<const VariableDeclarationStatement&>(
        *main.Body().Statements()[0]);
    const auto& function = static_cast<const AnonymousFunctionExpression&>(
        *variable.Initializer());
    ASSERT_EQ(function.Captures().size(), 2);
    EXPECT_EQ(function.Captures()[0]->Mode(),
              AnonymousFunctionExpression::CaptureMode::Lend);
    EXPECT_EQ(function.Captures()[1]->Mode(),
              AnonymousFunctionExpression::CaptureMode::Own);
}

TEST(ParserTests, ParsesTestDeclarationsWithMutableContexts) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(
        "module sample; @imports :: root::testing; "
        "test addition(&testing::UnitTest t) { t->fail(); }",
        diagnostics);

    for (const auto& diagnostic : diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 1);
    const auto& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    EXPECT_TRUE(function.IsTest());
    EXPECT_EQ(function.Name(), "addition");
    EXPECT_EQ(function.ReturnType().Type(), PrimitiveType::Void);
    ASSERT_EQ(function.Parameters().size(), 1);
    EXPECT_EQ(function.Parameters()[0]->Type().TypeForm(), TypeKind::Reference);
    EXPECT_FALSE(function.Parameters()[0]->Type().Target()->IsConst());
    EXPECT_EQ(function.Parameters()[0]->Type().Target()->Name(),
              "testing::UnitTest");
}

TEST(ParserTests, ParsesBorrowResultSourceAfterParameters) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(
        "&const i32 identity(&const i32 value) <- value { return value; }",
        diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 1);
    const auto& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    EXPECT_TRUE(function.HasBorrowSource());
    EXPECT_EQ(function.BorrowSource(), "value");
    EXPECT_EQ(function.BorrowSourceRange().start.offset, 41);
    EXPECT_EQ(function.BorrowSourceRange().end.offset, 46);
}

TEST(ParserTests, RequiresBorrowSourceAfterLeftArrow) {
    DiagnosticBag diagnostics;
    (void)ParseSource("&i32 invalid(&i32 value) <- { return value; }",
                      diagnostics);

    EXPECT_TRUE(diagnostics.HasErrors());
    ASSERT_FALSE(diagnostics.Diagnostics().empty());
    EXPECT_EQ(diagnostics.Diagnostics().front().code,
              DiagnosticCode::ExpectedToken);
}

TEST(ParserTests, ParsesOwningSliceTypesInDeclarationsAndAllocations) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(
        "structure Buffer { own []const u8 bytes; }; "
        "own []i32 allocate(usize count) { values, failure := "
        "new<own []i32>(count); return values; }",
        diagnostics);

    for (const auto& diagnostic : diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 2);
    const auto& structure =
        static_cast<const StructureDeclaration&>(*unit->Declarations()[0]);
    ASSERT_EQ(structure.Fields().size(), 1);
    EXPECT_EQ(structure.Fields()[0]->Type().TypeForm(), TypeKind::Slice);
    EXPECT_EQ(structure.Fields()[0]->Type().Contract(), OwnershipContract::Own);
    EXPECT_TRUE(structure.Fields()[0]->Type().Target()->IsConst());

    const auto& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[1]);
    EXPECT_EQ(function.ReturnType().TypeForm(), TypeKind::Slice);
    EXPECT_EQ(function.ReturnType().Contract(), OwnershipContract::Own);
    const auto& declaration = static_cast<const DestructureStatement&>(
        *function.Body().Statements()[0]);
    const auto& call = static_cast<const CallExpression&>(declaration.Value());
    ASSERT_NE(call.TypeArgument(), nullptr);
    EXPECT_EQ(call.TypeArgument()->TypeForm(), TypeKind::Slice);
    EXPECT_EQ(call.TypeArgument()->Contract(), OwnershipContract::Own);
}

TEST(ParserTests, ParsesExternDeclarationWithSemicolonAndModifiers) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(
        "extern unsafe i32 native_add(i32 left, i32 right);", diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 1);
    const auto& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations().front());
    EXPECT_TRUE(function.IsExtern());
    EXPECT_TRUE(function.IsUnsafe());
    EXPECT_EQ(function.Name(), "native_add");
    EXPECT_EQ(function.Parameters().size(), 2);
    EXPECT_TRUE(function.Body().Statements().empty());
}

TEST(ParserTests, ExternDeclarationRequiresSemicolonInsteadOfBody) {
    DiagnosticBag diagnostics;
    (void)ParseSource("extern i32 native_add(i32 value) { return value; }",
                      diagnostics);

    ASSERT_TRUE(diagnostics.HasErrors());
    EXPECT_EQ(diagnostics.Diagnostics().front().code,
              DiagnosticCode::ExpectedToken);
}

TEST(ParserTests, ReportsMissingSemicolonAndPreservesFunction) {
    DiagnosticBag diagnostics;
    std::vector<Token> tokens = Lex("i32 main() { return 0 }", diagnostics);
    Parser parser(std::move(tokens), diagnostics);

    std::unique_ptr<CompilationUnit> unit = parser.Parse();

    ASSERT_EQ(diagnostics.Diagnostics().size(), 1);
    EXPECT_EQ(diagnostics.Diagnostics()[0].code, DiagnosticCode::ExpectedToken);
    EXPECT_EQ(diagnostics.Diagnostics()[0].range.start.offset, 22);
    ASSERT_EQ(unit->Declarations().size(), 1);
}

TEST(ParserTests, AppendsMissingEndOfFileTokenSafely) {
    DiagnosticBag diagnostics;
    const SourceRange range{{0, 1, 1}, {3, 1, 4}};
    std::vector<Token> tokens;
    tokens.emplace_back(TokenType::I32, "i32", range);
    Parser parser(std::move(tokens), diagnostics);

    std::unique_ptr<CompilationUnit> unit = parser.Parse();

    ASSERT_NE(unit, nullptr);
    EXPECT_EQ(unit->Range().end.offset, 3);
    EXPECT_EQ(diagnostics.Diagnostics().size(), 1);
}

TEST(ParserExpressionTests, AppliesCompletePrecedenceTable) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit = ParseSource(
        "bool check() { return a or b and c | d ^ e & f == g < h << i + j * "
        "k; }",
        diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const BinaryExpression& logicalOr =
        static_cast<const BinaryExpression&>(ReturnedExpression(*unit));
    EXPECT_EQ(logicalOr.OperatorType(), TokenType::Or);

    const BinaryExpression& logicalAnd =
        static_cast<const BinaryExpression&>(logicalOr.Right());
    EXPECT_EQ(logicalAnd.OperatorType(), TokenType::And);

    const BinaryExpression& bitwiseOr =
        static_cast<const BinaryExpression&>(logicalAnd.Right());
    EXPECT_EQ(bitwiseOr.OperatorType(), TokenType::BitOr);

    const BinaryExpression& xorExpression =
        static_cast<const BinaryExpression&>(bitwiseOr.Right());
    EXPECT_EQ(xorExpression.OperatorType(), TokenType::Xor);

    const BinaryExpression& bitwiseAnd =
        static_cast<const BinaryExpression&>(xorExpression.Right());
    EXPECT_EQ(bitwiseAnd.OperatorType(), TokenType::Ampersand);

    const BinaryExpression& equality =
        static_cast<const BinaryExpression&>(bitwiseAnd.Right());
    EXPECT_EQ(equality.OperatorType(), TokenType::Equal);

    const BinaryExpression& comparison =
        static_cast<const BinaryExpression&>(equality.Right());
    EXPECT_EQ(comparison.OperatorType(), TokenType::Less);

    const BinaryExpression& shift =
        static_cast<const BinaryExpression&>(comparison.Right());
    EXPECT_EQ(shift.OperatorType(), TokenType::ShiftLeft);

    const BinaryExpression& addition =
        static_cast<const BinaryExpression&>(shift.Right());
    EXPECT_EQ(addition.OperatorType(), TokenType::Add);

    const BinaryExpression& multiplication =
        static_cast<const BinaryExpression&>(addition.Right());
    EXPECT_EQ(multiplication.OperatorType(), TokenType::Mul);
}

TEST(ParserExpressionTests, BinaryOperatorsAreLeftAssociative) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit =
        ParseSource("i32 check() { return 10 - 3 - 2; }", diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const BinaryExpression& outer =
        static_cast<const BinaryExpression&>(ReturnedExpression(*unit));
    EXPECT_EQ(outer.OperatorType(), TokenType::Sub);
    ASSERT_EQ(outer.Left().Kind(), AstNodeKind::BinaryExpression);
    const BinaryExpression& inner =
        static_cast<const BinaryExpression&>(outer.Left());
    EXPECT_EQ(inner.OperatorType(), TokenType::Sub);
}

TEST(ParserExpressionTests, UnaryOperatorsBindTighterAndAssociateRight) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit =
        ParseSource("i32 check() { return !-*value; }", diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const UnaryExpression& logicalNot =
        static_cast<const UnaryExpression&>(ReturnedExpression(*unit));
    EXPECT_EQ(logicalNot.OperatorType(), TokenType::Bang);
    const UnaryExpression& negation =
        static_cast<const UnaryExpression&>(logicalNot.Operand());
    EXPECT_EQ(negation.OperatorType(), TokenType::Sub);
    const UnaryExpression& dereference =
        static_cast<const UnaryExpression&>(negation.Operand());
    EXPECT_EQ(dereference.OperatorType(), TokenType::Mul);
}

TEST(ParserExpressionTests, ParenthesesOverridePrecedenceAndKeepRange) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit =
        ParseSource("i32 check() { return (1 + 2) * 3; }", diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const BinaryExpression& multiplication =
        static_cast<const BinaryExpression&>(ReturnedExpression(*unit));
    EXPECT_EQ(multiplication.OperatorType(), TokenType::Mul);
    ASSERT_EQ(multiplication.Left().Kind(),
              AstNodeKind::ParenthesizedExpression);
    const ParenthesizedExpression& parentheses =
        static_cast<const ParenthesizedExpression&>(multiplication.Left());
    EXPECT_EQ(parentheses.Inner().Kind(), AstNodeKind::BinaryExpression);
    EXPECT_EQ(parentheses.Range().Length(), 7);
}

TEST(ParserExpressionTests, ReportsMissingRightOperandWithoutThrowing) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit =
        ParseSource("i32 check() { return 1 +; }", diagnostics);

    ASSERT_NE(unit, nullptr);
    ASSERT_EQ(diagnostics.Diagnostics().size(), 1);
    EXPECT_EQ(diagnostics.Diagnostics()[0].code, DiagnosticCode::ExpectedToken);
    EXPECT_EQ(diagnostics.Diagnostics()[0].message, "expected an expression");
}

TEST(ParserAssignmentTests, AssignmentsHaveLowestPrecedence) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit =
        ParseSource("i32 check() { return result = a or b + c; }", diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const AssignmentExpression& assignment =
        static_cast<const AssignmentExpression&>(ReturnedExpression(*unit));
    EXPECT_EQ(assignment.Target().Kind(), AstNodeKind::NameExpression);
    ASSERT_EQ(assignment.Value().Kind(), AstNodeKind::BinaryExpression);
    EXPECT_EQ(
        static_cast<const BinaryExpression&>(assignment.Value()).OperatorType(),
        TokenType::Or);
}

TEST(ParserAssignmentTests, AssignmentsAssociateRight) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit =
        ParseSource("i32 check() { return a = b = 0; }", diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const AssignmentExpression& outer =
        static_cast<const AssignmentExpression&>(ReturnedExpression(*unit));
    EXPECT_EQ(outer.Target().Kind(), AstNodeKind::NameExpression);
    EXPECT_EQ(outer.Value().Kind(), AstNodeKind::AssignmentExpression);
}

TEST(ParserAssignmentTests, ParsesCompoundAssignmentsAndPostfixDecrement) {
    for (const auto& [text, operation] :
         std::vector<std::pair<std::string, TokenType>>{
             {"+=", TokenType::AddAssign},
             {"-=", TokenType::SubAssign},
             {"*=", TokenType::MulAssign},
             {"/=", TokenType::DivAssign},
             {"^=", TokenType::XorAssign}}) {
        DiagnosticBag diagnostics;
        auto unit = ParseSource("i32 check() { return value " + text + " 2; }",
                                diagnostics);
        ASSERT_FALSE(diagnostics.HasErrors()) << text;
        const auto& assignment =
            static_cast<const AssignmentExpression&>(ReturnedExpression(*unit));
        EXPECT_EQ(assignment.OperatorType(), operation) << text;
    }

    DiagnosticBag diagnostics;
    auto unit = ParseSource("i32 check() { return value--; }", diagnostics);
    ASSERT_FALSE(diagnostics.HasErrors());
    const auto& decrement =
        static_cast<const UnaryExpression&>(ReturnedExpression(*unit));
    EXPECT_EQ(decrement.OperatorType(), TokenType::Decrement);
}

TEST(ParserAssignmentTests, ReportsInvalidAssignmentTarget) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit =
        ParseSource("i32 check() { return (a + b) = 3; }", diagnostics);

    ASSERT_NE(unit, nullptr);
    ASSERT_EQ(diagnostics.Diagnostics().size(), 1);
    EXPECT_EQ(diagnostics.Diagnostics()[0].code,
              DiagnosticCode::InvalidAssignmentTarget);
    EXPECT_EQ(diagnostics.Diagnostics()[0].message,
              "expression is not a valid assignment target");
}

TEST(ParserVariableDeclarationTests, ParsesTypedInitializedVariables) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit = ParseSource(
        "void check() { i32 x = 32; bool y = false; }", diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const FunctionDeclaration& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    ASSERT_EQ(function.Body().Statements().size(), 2);

    const VariableDeclarationStatement& x =
        static_cast<const VariableDeclarationStatement&>(
            *function.Body().Statements()[0]);
    ASSERT_NE(x.ExplicitType(), nullptr);
    EXPECT_EQ(x.ExplicitType()->Type(), PrimitiveType::I32);
    EXPECT_EQ(x.Name(), "x");
    ASSERT_NE(x.Initializer(), nullptr);
    EXPECT_EQ(x.Initializer()->Kind(), AstNodeKind::LiteralExpression);

    const VariableDeclarationStatement& y =
        static_cast<const VariableDeclarationStatement&>(
            *function.Body().Statements()[1]);
    ASSERT_NE(y.ExplicitType(), nullptr);
    EXPECT_EQ(y.ExplicitType()->Type(), PrimitiveType::Bool);
    EXPECT_EQ(y.Name(), "y");
    ASSERT_NE(y.Initializer(), nullptr);
    EXPECT_EQ(
        static_cast<const LiteralExpression&>(*y.Initializer()).LiteralType(),
        TokenType::False);
}

TEST(ParserVariableDeclarationTests, ParsesTypedDeclarationWithoutInitializer) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit =
        ParseSource("void check() { i32 value; }", diagnostics);

    ASSERT_NE(unit, nullptr);
    ASSERT_FALSE(diagnostics.HasErrors());
    const FunctionDeclaration& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    const VariableDeclarationStatement& variable =
        static_cast<const VariableDeclarationStatement&>(
            *function.Body().Statements()[0]);
    ASSERT_NE(variable.ExplicitType(), nullptr);
    EXPECT_EQ(variable.ExplicitType()->Type(), PrimitiveType::I32);
    EXPECT_EQ(variable.Name(), "value");
    EXPECT_EQ(variable.Initializer(), nullptr);
}

TEST(ParserVariableDeclarationTests, ParsesInferredVariables) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit =
        ParseSource("void check() { x := 32; y := false; }", diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const FunctionDeclaration& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    ASSERT_EQ(function.Body().Statements().size(), 2);

    const VariableDeclarationStatement& x =
        static_cast<const VariableDeclarationStatement&>(
            *function.Body().Statements()[0]);
    EXPECT_FALSE(x.HasExplicitType());
    EXPECT_EQ(x.ExplicitType(), nullptr);
    EXPECT_EQ(x.Name(), "x");
    ASSERT_NE(x.Initializer(), nullptr);
    EXPECT_EQ(x.Initializer()->Kind(), AstNodeKind::LiteralExpression);

    const VariableDeclarationStatement& y =
        static_cast<const VariableDeclarationStatement&>(
            *function.Body().Statements()[1]);
    EXPECT_FALSE(y.HasExplicitType());
    EXPECT_EQ(y.Name(), "y");
    ASSERT_NE(y.Initializer(), nullptr);
    EXPECT_EQ(
        static_cast<const LiteralExpression&>(*y.Initializer()).LiteralType(),
        TokenType::False);
}

TEST(ParserVariableDeclarationTests, InferredVariablesRequireInitializer) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit =
        ParseSource("void check() { value :=; }", diagnostics);

    ASSERT_NE(unit, nullptr);
    ASSERT_EQ(diagnostics.Diagnostics().size(), 1);
    EXPECT_EQ(diagnostics.Diagnostics()[0].message, "expected an expression");
}

TEST(ParserCallTests, ParsesNestedFunctionCalls) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit = ParseSource(
        "i32 check() { return add(1, multiply(2, 3)); }", diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const CallExpression& add =
        static_cast<const CallExpression&>(ReturnedExpression(*unit));
    ASSERT_EQ(add.Arguments().size(), 2);
    EXPECT_EQ(add.Callee().Kind(), AstNodeKind::NameExpression);
    EXPECT_EQ(static_cast<const NameExpression&>(add.Callee()).Name(), "add");
    ASSERT_EQ(add.Arguments()[1]->Kind(), AstNodeKind::CallExpression);
    const CallExpression& multiply =
        static_cast<const CallExpression&>(*add.Arguments()[1]);
    EXPECT_EQ(multiply.Arguments().size(), 2);
}

TEST(ParserCallTests, ParsesEmptyAndChainedCalls) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit =
        ParseSource("i32 check() { return factory()(value); }", diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const CallExpression& outer =
        static_cast<const CallExpression&>(ReturnedExpression(*unit));
    ASSERT_EQ(outer.Arguments().size(), 1);
    ASSERT_EQ(outer.Callee().Kind(), AstNodeKind::CallExpression);
    const CallExpression& inner =
        static_cast<const CallExpression&>(outer.Callee());
    EXPECT_TRUE(inner.Arguments().empty());
    EXPECT_EQ(outer.Range().Length(), 16);
}

TEST(ParserCallTests, CallsBindTighterThanUnaryAndBinaryOperators) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit = ParseSource(
        "i32 check() { return -value(1) * other(2); }", diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const BinaryExpression& multiplication =
        static_cast<const BinaryExpression&>(ReturnedExpression(*unit));
    ASSERT_EQ(multiplication.Left().Kind(), AstNodeKind::UnaryExpression);
    const UnaryExpression& unary =
        static_cast<const UnaryExpression&>(multiplication.Left());
    EXPECT_EQ(unary.Operand().Kind(), AstNodeKind::CallExpression);
    EXPECT_EQ(multiplication.Right().Kind(), AstNodeKind::CallExpression);
}

TEST(ParserCallTests, ReportsMissingArgumentSeparatorAndRecovers) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit =
        ParseSource("i32 check() { return add(1 2); }", diagnostics);

    ASSERT_NE(unit, nullptr);
    ASSERT_EQ(diagnostics.Diagnostics().size(), 1);
    EXPECT_EQ(diagnostics.Diagnostics()[0].message,
              "expected ',' or ')' after argument");
}

TEST(ParserCallTests, ReportsTrailingComma) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit =
        ParseSource("i32 check() { return add(1,); }", diagnostics);

    ASSERT_NE(unit, nullptr);
    ASSERT_EQ(diagnostics.Diagnostics().size(), 1);
    EXPECT_EQ(diagnostics.Diagnostics()[0].message,
              "expected an argument after ','");
}

TEST(ParserStringViewTests, ParsesMembersIndexingAndPostfixChains) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit = ParseSource(
        "stringv check(stringv text) { return text.substr(0, 3); } "
        "char first(stringv text) { return text.at(0); } "
        "char indexed(stringv text) { return text.substr(1, 2)[0]; }",
        diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const FunctionDeclaration& substringFunction =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    const ReturnStatement& substringReturn =
        static_cast<const ReturnStatement&>(
            *substringFunction.Body().Statements()[0]);
    const MemberCallExpression& substring =
        static_cast<const MemberCallExpression&>(*substringReturn.Value());
    EXPECT_EQ(substring.Member(), "substr");
    EXPECT_EQ(substring.Arguments().size(), 2);

    const FunctionDeclaration& atFunction =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[1]);
    const ReturnStatement& atReturn =
        static_cast<const ReturnStatement&>(*atFunction.Body().Statements()[0]);
    EXPECT_EQ(
        static_cast<const MemberCallExpression&>(*atReturn.Value()).Member(),
        "at");

    const FunctionDeclaration& indexFunction =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[2]);
    const ReturnStatement& indexReturn = static_cast<const ReturnStatement&>(
        *indexFunction.Body().Statements()[0]);
    const IndexExpression& index =
        static_cast<const IndexExpression&>(*indexReturn.Value());
    EXPECT_EQ(index.Receiver().Kind(), AstNodeKind::MemberCallExpression);
}

TEST(ParserConditionalTests, ParsesConditionalAlsoClauses) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit = ParseSource(
        "void check() { if x {} also y {} else if z {} also w {} else {} }",
        diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const FunctionDeclaration& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    ASSERT_EQ(function.Body().Statements().size(), 1);
    const IfStatement& ifStatement =
        static_cast<const IfStatement&>(*function.Body().Statements()[0]);

    ASSERT_NE(ifStatement.PrimaryBranch().Also(), nullptr);
    const AlsoClause& primaryAlso = *ifStatement.PrimaryBranch().Also();
    EXPECT_EQ(primaryAlso.Condition().Kind(), AstNodeKind::NameExpression);
    EXPECT_EQ(
        static_cast<const NameExpression&>(primaryAlso.Condition()).Name(),
        "y");

    ASSERT_EQ(ifStatement.ElseIfBranches().size(), 1);
    ASSERT_NE(ifStatement.ElseIfBranches()[0]->Also(), nullptr);
    const AlsoClause& secondaryAlso = *ifStatement.ElseIfBranches()[0]->Also();
    EXPECT_EQ(
        static_cast<const NameExpression&>(secondaryAlso.Condition()).Name(),
        "w");
    ASSERT_NE(ifStatement.ElseBody(), nullptr);
}

TEST(ParserConditionalTests, RejectsMultipleAlsoClausesOnOneBranch) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit = ParseSource(
        "void check() { if ready {} also warm {} also authorized {} }",
        diagnostics);

    ASSERT_EQ(diagnostics.Diagnostics().size(), 1);
    EXPECT_EQ(diagnostics.Diagnostics()[0].code,
              DiagnosticCode::UnexpectedToken);
    EXPECT_EQ(diagnostics.Diagnostics()[0].message,
              "a branch may have only one 'also' clause");
    const FunctionDeclaration& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    const IfStatement& ifStatement =
        static_cast<const IfStatement&>(*function.Body().Statements()[0]);
    ASSERT_NE(ifStatement.PrimaryBranch().Also(), nullptr);
    EXPECT_EQ(static_cast<const NameExpression&>(
                  ifStatement.PrimaryBranch().Also()->Condition())
                  .Name(),
              "warm");
}

TEST(ParserConditionalTests, RejectsBareAlsoClause) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit =
        ParseSource("void check() { if ready {} also {} }", diagnostics);

    ASSERT_NE(unit, nullptr);
    ASSERT_EQ(diagnostics.Diagnostics().size(), 1);
    EXPECT_EQ(diagnostics.Diagnostics()[0].code, DiagnosticCode::ExpectedToken);
    EXPECT_EQ(diagnostics.Diagnostics()[0].message,
              "expected a condition after 'also'");
}

TEST(ParserWhileTests, ParsesConditionalAlsoClause) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit = ParseSource(
        "void Test() { while running {} also ready {} }", diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const FunctionDeclaration& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    ASSERT_EQ(function.Body().Statements().size(), 1);
    const WhileStatement& whileStatement =
        static_cast<const WhileStatement&>(*function.Body().Statements()[0]);
    EXPECT_EQ(whileStatement.Condition().Kind(), AstNodeKind::NameExpression);
    ASSERT_NE(whileStatement.Also(), nullptr);
    EXPECT_EQ(
        static_cast<const NameExpression&>(whileStatement.Also()->Condition())
            .Name(),
        "ready");
}

TEST(ParserWhileTests, RejectsMultipleAlsoClauses) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit = ParseSource(
        "void Test() { while running {} also first {} also second {} }",
        diagnostics);

    ASSERT_NE(unit, nullptr);
    ASSERT_EQ(diagnostics.Diagnostics().size(), 1);
    EXPECT_EQ(diagnostics.Diagnostics()[0].code,
              DiagnosticCode::UnexpectedToken);
    EXPECT_EQ(diagnostics.Diagnostics()[0].message,
              "a while loop may have only one 'also' clause");
}

TEST(ParserLoopControlTests, ParsesProceedBreakJumpAndPosition) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit = ParseSource(
        "void Test() { while true { proceed; break; jumpto retry; "
        "#retry } }",
        diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const FunctionDeclaration& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    const WhileStatement& loop =
        static_cast<const WhileStatement&>(*function.Body().Statements()[0]);
    ASSERT_EQ(loop.Body().Statements().size(), 4);
    EXPECT_EQ(loop.Body().Statements()[0]->Kind(),
              AstNodeKind::ProceedStatement);
    EXPECT_EQ(loop.Body().Statements()[1]->Kind(), AstNodeKind::BreakStatement);
    EXPECT_EQ(loop.Body().Statements()[2]->Kind(),
              AstNodeKind::JumpToStatement);
    EXPECT_EQ(loop.Body().Statements()[3]->Kind(),
              AstNodeKind::PositionStatement);
}

TEST(ParserCastTests, ParsesBothSpellingsAndChainsLeftToRight) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit = ParseSource(
        "i32 Test() { value := (32 as i64).cast<u8> as i32; "
        "return value; }",
        diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const FunctionDeclaration& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    const VariableDeclarationStatement& declaration =
        static_cast<const VariableDeclarationStatement&>(
            *function.Body().Statements()[0]);
    ASSERT_EQ(declaration.Initializer()->Kind(), AstNodeKind::CastExpression);
    const CastExpression& outer =
        static_cast<const CastExpression&>(*declaration.Initializer());
    EXPECT_EQ(outer.TargetType().Type(), PrimitiveType::I32);
    ASSERT_EQ(outer.Value().Kind(), AstNodeKind::CastExpression);
    const CastExpression& middle =
        static_cast<const CastExpression&>(outer.Value());
    EXPECT_EQ(middle.TargetType().Type(), PrimitiveType::U8);
    EXPECT_EQ(middle.Value().Kind(), AstNodeKind::ParenthesizedExpression);
}

TEST(ParserCastTests, CastBindsTighterThanBinaryOperators) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit =
        ParseSource("i64 Test() { return 10 + 32 as i64; }", diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const FunctionDeclaration& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    const ReturnStatement& statement =
        static_cast<const ReturnStatement&>(*function.Body().Statements()[0]);
    const BinaryExpression& binary =
        static_cast<const BinaryExpression&>(*statement.Value());
    EXPECT_EQ(binary.Right().Kind(), AstNodeKind::CastExpression);
}

TEST(ParserStringTests, ParsesOwningStringTypesAndLiteralConversions) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit = ParseSource(
        "string Build() { string value = \"hi mom\".string(); "
        "return value.cast<string>; }",
        diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const FunctionDeclaration& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    EXPECT_EQ(function.ReturnType().Type(), PrimitiveType::String);
    const VariableDeclarationStatement& variable =
        static_cast<const VariableDeclarationStatement&>(
            *function.Body().Statements()[0]);
    EXPECT_EQ(variable.ExplicitType()->Type(), PrimitiveType::String);
    EXPECT_EQ(variable.Initializer()->Kind(),
              AstNodeKind::MemberCallExpression);
}

TEST(ParserTests, PreservesRecursiveTypeQualifiersAndRanges) {
    const std::string source =
        "const *&const u8 f(constref *const i32 arg) { constptr u8 p; }";
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit = ParseSource(source, diagnostics);
    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 1);
    const FunctionDeclaration& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    const TypeNode& type = function.ReturnType();
    EXPECT_EQ(type.TypeForm(), TypeKind::Pointer);
    EXPECT_TRUE(type.IsConst());
    EXPECT_EQ(type.Range().start.offset, 0);
    EXPECT_EQ(type.Range().end.offset, source.find(" f("));
    EXPECT_EQ(type.QualifierRange().Length(), 5);
    ASSERT_NE(type.Target(), nullptr);
    EXPECT_EQ(type.Target()->TypeForm(), TypeKind::Reference);
    EXPECT_FALSE(type.Target()->IsConst());
    ASSERT_NE(type.Target()->Target(), nullptr);
    const TypeNode& leaf = *type.Target()->Target();
    EXPECT_EQ(leaf.Type(), PrimitiveType::U8);
    EXPECT_TRUE(leaf.IsConst());
    EXPECT_EQ(leaf.Range().start.offset, source.find("const u8"));
    EXPECT_EQ(leaf.QualifierRange().start.offset, source.find("const u8"));
    EXPECT_EQ(leaf.QualifierRange().Length(), 5);
    const TypeNode& parameter = function.Parameters()[0]->Type();
    EXPECT_EQ(parameter.TypeForm(), TypeKind::Reference);
    EXPECT_TRUE(parameter.IsConst());
    EXPECT_EQ(parameter.QualifierRange().Length(), 8);
    ASSERT_NE(parameter.Target(), nullptr);
    EXPECT_EQ(parameter.Target()->TypeForm(), TypeKind::Pointer);
    EXPECT_FALSE(parameter.Target()->IsConst());
    EXPECT_TRUE(parameter.Target()->Target()->IsConst());
    const VariableDeclarationStatement& variable =
        static_cast<const VariableDeclarationStatement&>(
            *function.Body().Statements()[0]);
    ASSERT_NE(variable.ExplicitType(), nullptr);
    EXPECT_EQ(variable.ExplicitType()->TypeForm(), TypeKind::Pointer);
    EXPECT_TRUE(variable.ExplicitType()->IsConst());
    EXPECT_EQ(variable.ExplicitType()->Range().start.offset,
              source.find("constptr"));
    EXPECT_EQ(variable.ExplicitType()->Range().Length(), 11);
}

TEST(ParserTests, DistinguishesCompoundDeclarationsFromUnaryExpressions) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit =
        ParseSource("void f() { *u8 p; &u8 r; *p; &r; 2 * 3; true && false; }",
                    diagnostics);
    ASSERT_FALSE(diagnostics.HasErrors());
    const FunctionDeclaration& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    ASSERT_EQ(function.Body().Statements().size(), 6);
    for (std::size_t index = 0; index < 2; ++index) {
        EXPECT_EQ(function.Body().Statements()[index]->Kind(),
                  AstNodeKind::VariableDeclarationStatement);
    }
    for (std::size_t index = 2; index < 6; ++index) {
        EXPECT_EQ(function.Body().Statements()[index]->Kind(),
                  AstNodeKind::ExpressionStatement);
    }
}

TEST(ParserTests, RejectsMalformedOrDuplicateQualifiersAndRecovers) {
    const std::vector<std::string> declarations{"const const i32 x = 1;",
                                                "const constptr u8 x;",
                                                "const constref u8 x;",
                                                "const * x;",
                                                "const &;",
                                                "constptr;",
                                                "constref;"};
    for (const std::string& declaration : declarations) {
        SCOPED_TRACE(declaration);
        DiagnosticBag diagnostics;
        std::unique_ptr<CompilationUnit> unit = ParseSource(
            "void f() { " + declaration + " } i32 g() { return 42; }",
            diagnostics);
        EXPECT_TRUE(diagnostics.HasErrors());
        ASSERT_EQ(unit->Declarations().size(), 2);
        EXPECT_EQ(
            static_cast<const FunctionDeclaration&>(*unit->Declarations()[1])
                .Name(),
            "g");
    }
}

TEST(ParserUnsafeTests, ParsesUnsafeFunctionsBlocksAndRawAddresses) {
    DiagnosticBag diagnostics;
    const std::string source =
        "unsafe void set(*i32 p) { (*p) = 42; } "
        "i32 main() { i32 value = 0; unsafe { { ptr := &raw value; set(ptr); } "
        "} return value; }";
    std::unique_ptr<CompilationUnit> unit = ParseSource(source, diagnostics);
    ASSERT_FALSE(diagnostics.HasErrors());
    const auto& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    EXPECT_TRUE(function.IsUnsafe());
    EXPECT_EQ(function.Range().start.offset, 0);
    const auto& mainFunction =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[1]);
    EXPECT_FALSE(mainFunction.IsUnsafe());
    const auto& block = static_cast<const BlockStatement&>(
        *mainFunction.Body().Statements()[1]);
    EXPECT_TRUE(block.IsUnsafe());
    EXPECT_EQ(block.Range().start.offset, source.find("unsafe {"));
    const auto& nested =
        static_cast<const BlockStatement&>(*block.Statements()[0]);
    EXPECT_FALSE(nested.IsUnsafe());
    const auto& declaration = static_cast<const VariableDeclarationStatement&>(
        *nested.Statements()[0]);
    const auto& address =
        static_cast<const UnaryExpression&>(*declaration.Initializer());
    EXPECT_EQ(address.OperatorType(), TokenType::RawAddress);
    EXPECT_EQ(address.Range().Length(), 10);
}

TEST(ParserUnsafeTests, RejectsStandaloneUnsafeCallAndDuplicateModifiers) {
    for (const std::string& source :
         {"void f() { unsafe g(); }", "unsafe unsafe void f() {}"}) {
        DiagnosticBag diagnostics;
        ParseSource(source, diagnostics);
        EXPECT_TRUE(diagnostics.HasErrors());
    }
}

TEST(ParserArrayTests, ParsesNestedTypesListsAndRepeatsWithRanges) {
    const std::string source =
        "const [2][3]u8 f() { return [[1, 2, 3], [4; 3]]; }";
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit = ParseSource(source, diagnostics);
    ASSERT_FALSE(diagnostics.HasErrors());
    const auto& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    const TypeNode& type = function.ReturnType();
    EXPECT_EQ(type.TypeForm(), TypeKind::Array);
    EXPECT_TRUE(type.IsConst());
    EXPECT_EQ(type.Length(), 2);
    EXPECT_EQ(type.Target()->Length(), 3);
    EXPECT_EQ(type.Target()->Target()->Type(), PrimitiveType::U8);
    EXPECT_EQ(source.substr(type.Range().start.offset, type.Range().Length()),
              "const [2][3]u8");
    const auto& outer =
        static_cast<const ArrayExpression&>(ReturnedExpression(*unit));
    EXPECT_EQ(outer.Length(), 2);
    EXPECT_FALSE(outer.IsRepeat());
    const auto& repeat =
        static_cast<const ArrayExpression&>(*outer.Elements()[1]);
    EXPECT_TRUE(repeat.IsRepeat());
    EXPECT_EQ(repeat.Length(), 3);
    EXPECT_EQ(repeat.Elements().size(), 1);
    EXPECT_EQ(
        source.substr(repeat.Range().start.offset, repeat.Range().Length()),
        "[4; 3]");
}

TEST(ParserArrayTests, DistinguishesArrayTypesFromExpressionStatements) {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit = ParseSource(
        "void f() { [2]u8 a = [1, 2,]; a[0] = 3; [1, 2][0]; [];} "
        "void g(&[2]u8 a, *[2]u8 b) {}",
        diagnostics);
    EXPECT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 2);
}

TEST(ParserArrayTests, RejectsMalformedLengthsAndInitializersWithoutHanging) {
    for (const std::string& source :
         {"void f([-1]u8 a) {}", "void f([count]u8 a) {}",
          "void f([1 + 1]u8 a) {}", "void f([:]u8 a) {}",
          "void f([18446744073709551616]u8 a) {}", "void f() { a := [1; -2]; }",
          "void f() { a := [1; count]; }", "void f() { a := [1; 2, 3]; }",
          "void f() { a := [1, ; }", "void f() { a := [1, 2; }",
          "void f() { a := [1; 18446744073709551616]; }"}) {
        SCOPED_TRACE(source);
        DiagnosticBag diagnostics;
        EXPECT_NE(ParseSource(source, diagnostics), nullptr);
        EXPECT_TRUE(diagnostics.HasErrors());
    }
}

TEST(ParserTests, NormalizesCountedLoopsWithASeparateUpdateAndHeaderScope) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(
        "void f() { loop u32 i = 1; i <= 4; i++ { proceed; } }", diagnostics);
    ASSERT_FALSE(diagnostics.HasErrors());
    const auto& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    const auto& scope =
        static_cast<const BlockStatement&>(*function.Body().Statements()[0]);
    ASSERT_EQ(scope.Statements().size(), 2);
    EXPECT_EQ(scope.Statements()[0]->Kind(),
              AstNodeKind::VariableDeclarationStatement);
    const auto& loop =
        static_cast<const WhileStatement&>(*scope.Statements()[1]);
    ASSERT_NE(loop.Update(), nullptr);
    EXPECT_EQ(
        static_cast<const UnaryExpression&>(*loop.Update()).OperatorType(),
        TokenType::Increment);
    EXPECT_EQ(loop.Body().Statements()[0]->Kind(),
              AstNodeKind::ProceedStatement);
}

TEST(ParserTests, ParsesTypedInferredAndDiscardedRangeBindings) {
    for (const std::string& header :
         {"for i := range a", "for (i, v) := range a",
          "for (u32 i, i32 v) = range a", "for (_, v) := range a",
          "for (_, _) := range a"}) {
        SCOPED_TRACE(header);
        DiagnosticBag diagnostics;
        auto unit = ParseSource("void f() { a := [1, 2]; " + header + " {} }",
                                diagnostics);
        ASSERT_FALSE(diagnostics.HasErrors());
        const auto& function =
            static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
        EXPECT_EQ(function.Body().Statements()[1]->Kind(),
                  AstNodeKind::RangeStatement);
    }
}

TEST(ParserTests, RejectsMalformedLoopHeadersAndRecovers) {
    for (const std::string& header :
         {"for (i v) := range a {}", "for (i, v) = range a {}",
          "for (u32 i, i32 v) := range a {}", "for i := a {}",
          "for (i, v, w) := range a {}", "loop u32 i = 0; i < 2 i++ {}",
          "loop i := 0; i < 2; {}"}) {
        SCOPED_TRACE(header);
        DiagnosticBag diagnostics;
        auto unit =
            ParseSource("void f() { " + header + " } void g() {}", diagnostics);
        ASSERT_NE(unit, nullptr);
        // A counted loop may omit its update clause.
        if (header == "loop i := 0; i < 2; {}")
            EXPECT_FALSE(diagnostics.HasErrors());
        else
            EXPECT_TRUE(diagnostics.HasErrors());
    }
}

TEST(ParserSliceTests, ParsesTypesBoundsAndWritableAccessors) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(R"(
        void f([]const i32 s, const []u8 fixed, [2][]u8 nested) {
            a := s[:]; b := s[1:]; c := s[:2]; d := s[1:2];
            fixed.front() = 1; fixed.back()++;
        }
    )",
                            diagnostics);
    ASSERT_FALSE(diagnostics.HasErrors());
    const auto& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    EXPECT_EQ(function.Parameters()[0]->Type().TypeForm(), TypeKind::Slice);
    EXPECT_TRUE(function.Parameters()[0]->Type().Target()->IsConst());
    EXPECT_TRUE(function.Parameters()[1]->Type().IsConst());
    for (std::size_t index = 0; index < 4; ++index) {
        const auto& declaration =
            static_cast<const VariableDeclarationStatement&>(
                *function.Body().Statements()[index]);
        const auto& slice =
            static_cast<const SliceExpression&>(*declaration.Initializer());
        EXPECT_EQ(slice.Kind(), AstNodeKind::SliceExpression);
        EXPECT_EQ(slice.Start() != nullptr, index == 1 || index == 3);
        EXPECT_EQ(slice.End() != nullptr, index == 2 || index == 3);
    }
}

TEST(ParserSliceTests, RecoversFromMalformedSliceExpressions) {
    for (const std::string& expression :
         {"a[]", "a[::]", "a[1:2:3]", "a[1:", "a[:;", "a[1:2"}) {
        SCOPED_TRACE(expression);
        DiagnosticBag diagnostics;
        EXPECT_NE(ParseSource("void f() { " + expression + "; } void g() {}",
                              diagnostics),
                  nullptr);
        EXPECT_TRUE(diagnostics.HasErrors());
    }
}

TEST(ParserDeferTests, ParsesDeferredCallsAndTheirSourceRanges) {
    const std::string source = "void f() { defer stdoutln(42); defer (f()); }";
    DiagnosticBag diagnostics;
    auto unit = ParseSource(source, diagnostics);
    ASSERT_FALSE(diagnostics.HasErrors());
    const auto& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    ASSERT_EQ(function.Body().Statements().size(), 2);
    const auto& deferred =
        static_cast<const DeferStatement&>(*function.Body().Statements()[0]);
    EXPECT_EQ(deferred.Kind(), AstNodeKind::DeferStatement);
    EXPECT_EQ(deferred.Call().Kind(), AstNodeKind::CallExpression);
    EXPECT_EQ(source.substr(
                  deferred.Range().start.offset,
                  deferred.Range().end.offset - deferred.Range().start.offset),
              "defer stdoutln(42);");
}

TEST(ParserDeferTests, RecoversFromMissingCallsAndTerminators) {
    for (const std::string& statement :
         {"defer;", "defer stdoutln(1)", "defer { stdoutln(1); }",
          "defer defer f();"}) {
        SCOPED_TRACE(statement);
        DiagnosticBag diagnostics;
        EXPECT_NE(ParseSource("void f() { " + statement + " } void g() {}",
                              diagnostics),
                  nullptr);
        EXPECT_TRUE(diagnostics.HasErrors());
    }
}

TEST(ParserTupleTests, ParsesNestedTypesValuesAndBindingForms) {
    const std::string source =
        "(i32, (bool, u8)) f() { return (42, (true, 3)); } "
        "void g() { p := (1, 2); (a, b) := p; (i32 c, a) = p; (a, _) = p; }";
    DiagnosticBag diagnostics;
    auto unit = ParseSource(source, diagnostics);
    ASSERT_FALSE(diagnostics.HasErrors());
    const auto& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    EXPECT_EQ(function.ReturnType().TypeForm(), TypeKind::Tuple);
    EXPECT_EQ(function.ReturnType().Elements().size(), 2);
    EXPECT_EQ(function.ReturnType().Elements()[1]->TypeForm(), TypeKind::Tuple);
    const auto& tuple =
        static_cast<const TupleExpression&>(ReturnedExpression(*unit));
    EXPECT_EQ(tuple.Elements().size(), 2);
    EXPECT_EQ(
        source.substr(tuple.Range().start.offset,
                      tuple.Range().end.offset - tuple.Range().start.offset),
        "(42, (true, 3))");
    const auto& body =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[1])
            .Body();
    const auto& inferred =
        static_cast<const DestructureStatement&>(*body.Statements()[1]);
    EXPECT_TRUE(inferred.IsInferred());
    EXPECT_EQ(inferred.Bindings()[0]->Name(), "a");
    const auto& mixed =
        static_cast<const DestructureStatement&>(*body.Statements()[2]);
    EXPECT_FALSE(mixed.IsInferred());
    EXPECT_NE(mixed.Bindings()[0]->ExplicitType(), nullptr);
    EXPECT_EQ(mixed.Bindings()[1]->ExplicitType(), nullptr);
}

TEST(ParserTupleTests, KeepsGroupingAndRejectsMalformedTuples) {
    DiagnosticBag valid;
    ParseSource(
        "void f() { x := 1; r := &x; (*r) = (2); ((x)); p := (1, 2,); (a, b,) "
        ":= p; }",
        valid);
    EXPECT_FALSE(valid.HasErrors());
    for (const std::string& source :
         {"void f() { loop (a, b) := (1, 2); a < 3; a++ {} }",
          "void f() { t := (); }", "void f() { t := (1,); }",
          "(i32) f() { return 1; }", "() f() {}",
          "void f() { (a, b) := (1,); }",
          "void f() { (i32, bool) t = (1, true; }",
          "void f() { (a[0], b) = (1, 2); }"}) {
        SCOPED_TRACE(source);
        DiagnosticBag diagnostics;
        ParseSource(source, diagnostics);
        EXPECT_TRUE(diagnostics.HasErrors());
    }
}

TEST(ParserTupleTests, ParsesBareBindingsAndSharedRecursiveTypes) {
    const std::string source = R"(
        void f() {
            q, r := (3, 2);
            i32 a, i32 b = (3, 2);
            i32 c, d = (3, 2);
            bool first, second, i32 number = (true, false, 42);
            const *u8 p, other = (none, none);
            (i32, bool) left, right = ((1, true), (2, false));
            q, r = (r, q);
            (i32 fresh, q) = (1, 2);
        }
    )";
    DiagnosticBag diagnostics;
    auto unit = ParseSource(source, diagnostics);
    ASSERT_FALSE(diagnostics.HasErrors());
    const auto& body =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0])
            .Body();
    const auto& inferred =
        static_cast<const DestructureStatement&>(*body.Statements()[0]);
    EXPECT_TRUE(inferred.IsInferred());
    EXPECT_EQ(inferred.Bindings()[1]->ExplicitType(), nullptr);
    EXPECT_EQ(source.substr(
                  inferred.Range().start.offset,
                  inferred.Range().end.offset - inferred.Range().start.offset),
              "q, r := (3, 2);");
    const auto& shared =
        static_cast<const DestructureStatement&>(*body.Statements()[2]);
    ASSERT_NE(shared.Bindings()[1]->ExplicitType(), nullptr);
    EXPECT_EQ(shared.Bindings()[1]->ExplicitType()->Type(), PrimitiveType::I32);
    const auto& pointer =
        static_cast<const DestructureStatement&>(*body.Statements()[4]);
    EXPECT_TRUE(pointer.Bindings()[1]->ExplicitType()->IsConst());
    EXPECT_EQ(pointer.Bindings()[1]->ExplicitType()->TypeForm(),
              TypeKind::Pointer);
    const auto& tuple =
        static_cast<const DestructureStatement&>(*body.Statements()[5]);
    EXPECT_EQ(tuple.Bindings()[1]->ExplicitType()->TypeForm(), TypeKind::Tuple);
    const auto& mixed =
        static_cast<const DestructureStatement&>(*body.Statements()[7]);
    EXPECT_EQ(mixed.Bindings()[1]->ExplicitType(), nullptr);
}

TEST(ParserTupleTests, DistinguishesBareBindingsFromCallsTypesAndGrouping) {
    DiagnosticBag valid;
    ParseSource(
        "void f() { call(1, 2); x := (1, 2); (i32, i32) t = x; a := [1, 2]; "
        "(a[0]) = 3; }",
        valid);
    EXPECT_FALSE(valid.HasErrors());
    for (const std::string& source :
         {"void f() { i32 a, = (1, 2); }", "void f() { a, b, := (1, 2); }",
          "void f() { i32 a, b; }", "void f() { a, b = (1, 2) }",
          "void f() { a[0], b = (1, 2); }"}) {
        SCOPED_TRACE(source);
        DiagnosticBag diagnostics;
        ParseSource(source, diagnostics);
        EXPECT_TRUE(diagnostics.HasErrors());
    }
}

TEST(ParserErrorTests, ParsesFamiliesGenericReturnsAndNamedCompoundTypes) {
    const std::string source =
        "error Fault { Bad, Worse, } "
        "error widen(Fault value) { return value; } "
        "(Fault, error) f() { "
        "const Fault first, second = (Fault.Bad, none); "
        "[2]Fault values = [first, second]; "
        "&const Fault ref = &values[0]; "
        "(Fault, error) pair = (first, second); return pair; }";
    DiagnosticBag diagnostics;
    auto unit = ParseSource(source, diagnostics);
    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 3);
    const auto& family =
        static_cast<const ErrorDeclaration&>(*unit->Declarations()[0]);
    EXPECT_EQ(family.Name(), "Fault");
    ASSERT_EQ(family.Variants().size(), 2);
    EXPECT_EQ(family.Variants()[0].range.start.offset, source.find("Bad"));
    EXPECT_EQ(family.Variants()[1].name, "Worse");
    const auto& widen =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[1]);
    EXPECT_EQ(widen.ReturnType().Type(), PrimitiveType::ErrorValue);
    EXPECT_EQ(widen.Parameters()[0]->Type().TypeForm(), TypeKind::Named);
    EXPECT_EQ(widen.Parameters()[0]->Type().Name(), "Fault");
    const auto& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[2]);
    const auto& binding = static_cast<const DestructureStatement&>(
        *function.Body().Statements()[0]);
    EXPECT_EQ(binding.Bindings()[1]->ExplicitType()->Name(), "Fault");
    EXPECT_TRUE(binding.Bindings()[1]->ExplicitType()->IsConst());
    const auto& tuple = static_cast<const TupleExpression&>(binding.Value());
    const auto& variant =
        static_cast<const MemberAccessExpression&>(*tuple.Elements()[0]);
    EXPECT_EQ(variant.Member(), "Bad");
    EXPECT_EQ(variant.MemberRange().start.offset, source.find("Fault.Bad") + 6);
}

TEST(ParserErrorTests, RejectsMalformedErrorDeclarationsAndRecovers) {
    for (const std::string& declaration :
         {"error Fault { Bad Worse }", "error Fault { none = 0, Bad }",
          "error Fault { Bad = 1 }", "error Fault { Bad(i32) }",
          "error Fault { , }", "error Fault { Bad,"}) {
        SCOPED_TRACE(declaration);
        DiagnosticBag diagnostics;
        auto unit = ParseSource(declaration + " void after() {}", diagnostics);
        ASSERT_NE(unit, nullptr);
        EXPECT_TRUE(diagnostics.HasErrors());
        ASSERT_FALSE(unit->Declarations().empty());
        EXPECT_EQ(static_cast<const FunctionDeclaration&>(
                      *unit->Declarations().back())
                      .Name(),
                  "after");
    }
}

TEST(ParserEnumTests, ParsesBackingDefaultsAndDiscriminants) {
    const std::string source =
        "export enum TokenKind: u16 { default None = 0, Identifier, "
        "End = 42, }";
    DiagnosticBag diagnostics;
    auto unit = ParseSource(source, diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 1);
    const auto& enumeration =
        static_cast<const EnumDeclaration&>(*unit->Declarations().front());
    EXPECT_TRUE(enumeration.exported);
    EXPECT_EQ(enumeration.Name(), "TokenKind");
    ASSERT_NE(enumeration.BackingType(), nullptr);
    EXPECT_EQ(enumeration.BackingType()->Type(), PrimitiveType::U16);
    ASSERT_EQ(enumeration.Variants().size(), 3);
    EXPECT_TRUE(enumeration.Variants()[0].isDefault);
    EXPECT_EQ(enumeration.Variants()[0].discriminant, "0");
    EXPECT_FALSE(enumeration.Variants()[1].isDefault);
    EXPECT_TRUE(enumeration.Variants()[1].discriminant.empty());
    EXPECT_EQ(enumeration.Variants()[2].discriminant, "42");
}

TEST(ParserEnumTests, UsesU32WhenTheBackingTypeIsOmitted) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource("enum Direction { North, South, }", diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const auto& enumeration =
        static_cast<const EnumDeclaration&>(*unit->Declarations().front());
    EXPECT_EQ(enumeration.BackingType(), nullptr);
    ASSERT_EQ(enumeration.Variants().size(), 2);
}

TEST(ParserEnumTests, ParsesRustStyleMatchExpressions) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(
        "enum TokenKind { Identifier, Number, End, } "
        "i32 classify(TokenKind kind) { return match kind { "
        "TokenKind.Identifier => 1, TokenKind.Number => 2, _ => 0, }; }",
        diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    const auto& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[1]);
    const auto& returned =
        static_cast<const ReturnStatement&>(*function.Body().Statements()[0]);
    const auto& match = static_cast<const MatchExpression&>(*returned.Value());
    EXPECT_EQ(match.Value().Kind(), AstNodeKind::NameExpression);
    ASSERT_EQ(match.Arms().size(), 3);
    EXPECT_EQ(match.Arms()[0]->EnumName(), "TokenKind");
    EXPECT_EQ(match.Arms()[0]->Variant(), "Identifier");
    EXPECT_TRUE(match.Arms()[2]->IsWildcard());
}

TEST(ParserErrorTests, KeepsQualifiedValuesAndRawAddressesAsExpressions) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(
        "error Fault { Bad } unsafe void f() { Fault value = Fault.Bad; "
        "Fault.Bad; (Fault.Bad); &raw value; *ptr = Fault.Bad; "
        "a, b := (Fault.Bad, none); }",
        diagnostics);
    ASSERT_FALSE(diagnostics.HasErrors());
    const auto& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[1]);
    ASSERT_EQ(function.Body().Statements().size(), 6);
    for (std::size_t i = 1; i < 5; ++i)
        EXPECT_EQ(function.Body().Statements()[i]->Kind(),
                  AstNodeKind::ExpressionStatement);
}

TEST(ParserStructureTests, ParsesPrivateFieldsMethodsReceiversAndConstruction) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(
        "structure Box { i32 x = 1; private { i32 y; bool flag; } }; "
        "Box Box::init() override { return build Box { self.y = 2; }; } "
        "private unsafe i32 Box::read(*const self) { return self->y; } "
        "void Box::set(&self, i32 value) { self->y = value; } "
        "void f() { Box box = Box::init(); box.set(42); r := &box; r->set(2); "
        "}",
        diagnostics);
    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 5);
    const auto& structure =
        static_cast<const StructureDeclaration&>(*unit->Declarations()[0]);
    ASSERT_EQ(structure.Fields().size(), 3);
    EXPECT_FALSE(structure.Fields()[0]->IsPrivate());
    EXPECT_TRUE(structure.Fields()[1]->IsPrivate());
    EXPECT_NE(structure.Fields()[0]->Initializer(), nullptr);
    const auto& init =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[1]);
    EXPECT_EQ(init.Owner(), "Box");
    EXPECT_TRUE(init.HasModifier(FunctionModifier::Override));
    const auto& read =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[2]);
    EXPECT_TRUE(read.IsUnsafe());
    EXPECT_TRUE(read.HasModifier(FunctionModifier::Private));
    EXPECT_EQ(read.Parameters()[0]->Type().TypeForm(), TypeKind::Pointer);
    EXPECT_TRUE(read.Parameters()[0]->Type().Target()->IsConst());
    EXPECT_EQ(read.Parameters()[0]->Type().Target()->Name(), "Box");
    const auto& returned =
        static_cast<const ReturnStatement&>(*read.Body().Statements()[0]);
    const auto& access =
        static_cast<const MemberAccessExpression&>(*returned.Value());
    EXPECT_EQ(access.Access(), TokenType::Arrow);
}

TEST(ParserStructureTests, RejectsMalformedStructureSyntax) {
    for (const std::string& source :
         {"structure Box { i32 x; }", "structure { i32 x; };",
          "structure Box { i32; };",
          "structure Box { private { private { i32 x; } } };",
          "unsafe structure Box {};",
          "Box Box::init() override override { return build Box {}; }"}) {
        SCOPED_TRACE(source);
        DiagnosticBag diagnostics;
        EXPECT_NE(ParseSource(source, diagnostics), nullptr);
        EXPECT_TRUE(diagnostics.HasErrors());
    }
}

TEST(ParserAllocationTests, ParsesCompoundTypeArgumentsAndRejectsMalformedNew) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(
        "unsafe void f() { new<*(i32, bool)>(none); new<[2]u8>([1, 2]); "
        "free(null); }",
        diagnostics);
    ASSERT_FALSE(diagnostics.HasErrors());
    const auto& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    const auto& statement = static_cast<const ExpressionStatement&>(
        *function.Body().Statements()[0]);
    const auto& call = static_cast<const CallExpression&>(statement.Value());
    ASSERT_NE(call.TypeArgument(), nullptr);
    EXPECT_EQ(call.TypeArgument()->TypeForm(), TypeKind::Pointer);
    EXPECT_EQ(call.TypeArgument()->Target()->TypeForm(), TypeKind::Tuple);
    for (const std::string& source :
         {"unsafe void f() { new<>(1); }", "unsafe void f() { new<u8(1); }",
          "unsafe void f() { new<u8>; }"}) {
        DiagnosticBag invalid;
        (void)ParseSource(source, invalid);
        EXPECT_TRUE(invalid.HasErrors()) << source;
    }
}

TEST(ParserTests, PreservesOwnershipContractsWithoutChangingPointerTypes) {
    DiagnosticBag diagnostics;
    const auto unit = ParseSource(
        "unsafe (own *const i32, error) f(own *i32 p, lend *const i32 q) { "
        "return (p, none); }",
        diagnostics);
    ASSERT_FALSE(diagnostics.HasErrors());
    const auto& function =
        static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
    const auto& result = *function.ReturnType().Elements()[0];
    EXPECT_EQ(result.Contract(), OwnershipContract::Own);
    EXPECT_EQ(result.TypeForm(), TypeKind::Pointer);
    EXPECT_TRUE(result.Target()->IsConst());
    EXPECT_EQ(function.Parameters()[0]->Type().Contract(),
              OwnershipContract::Own);
    EXPECT_EQ(function.Parameters()[1]->Type().Contract(),
              OwnershipContract::Lend);
}

TEST(ParserTests, ParsesModuleImportsAndExportsOnOneLineOrMultipleLines) {
    for (const std::string& imports :
         {"@imports :: root::math :: lib::std::containers as collections;",
          "@imports\n    :: root::math\n    :: lib::std::containers as "
          "collections;"}) {
        DiagnosticBag diagnostics;
        auto unit =
            ParseSource("module example; " + imports +
                            " export i32 answer() { return math::answer(); }",
                        diagnostics);
        ASSERT_FALSE(diagnostics.HasErrors());
        EXPECT_EQ(unit->moduleName, "example");
        ASSERT_EQ(unit->imports.size(), 2);
        EXPECT_EQ(unit->imports[0].path,
                  (std::vector<std::string>{"root", "math"}));
        EXPECT_EQ(unit->imports[1].alias, "collections");
        EXPECT_FALSE(unit->imports[0].explicitAlias);
        EXPECT_TRUE(unit->imports[1].explicitAlias);
        ASSERT_EQ(unit->Declarations().size(), 1);
        EXPECT_TRUE(unit->Declarations()[0]->exported);
        const auto& function =
            static_cast<const FunctionDeclaration&>(*unit->Declarations()[0]);
        const auto& returned = static_cast<const ReturnStatement&>(
            *function.Body().Statements()[0]);
        const auto& call =
            static_cast<const CallExpression&>(*returned.Value());
        EXPECT_EQ(static_cast<const NameExpression&>(call.Callee()).Name(),
                  "math::answer");
    }
}

TEST(ParserTests, ParsesQuiverVisibilityOnTopLevelDeclarations) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(
        "quiver i32 value() { return 42; } quiver structure Box { i32 n; }; "
        "quiver enum State { On } quiver error Failure { Failed } quiver "
        "func Callback(i32) => (i32);",
        diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    ASSERT_EQ(unit->Declarations().size(), 5);
    for (const auto& declaration : unit->Declarations()) {
        EXPECT_TRUE(declaration->quiverVisible);
        EXPECT_FALSE(declaration->exported);
    }
}

TEST(ParserTests, PrivateModulesDoNotChangeDeclarationVisibility) {
    DiagnosticBag diagnostics;
    auto unit = ParseSource(
        "private module example; export i32 shared() { return 42; } "
        "quiver structure Box { i32 value; }; i32 local() { return 0; }",
        diagnostics);

    ASSERT_FALSE(diagnostics.HasErrors());
    EXPECT_EQ(unit->moduleName, "example");
    EXPECT_TRUE(unit->privateModule);
    ASSERT_EQ(unit->Declarations().size(), 3);
    EXPECT_FALSE(unit->Declarations()[0]->quiverVisible);
    EXPECT_TRUE(unit->Declarations()[0]->exported);
    EXPECT_TRUE(unit->Declarations()[1]->quiverVisible);
    EXPECT_FALSE(unit->Declarations()[2]->quiverVisible);
    EXPECT_FALSE(unit->Declarations()[2]->exported);
}

TEST(ParserTests, RejectsDuplicateAndConflictingVisibilityModifiers) {
    for (const std::string& source :
         {"export quiver void value() {}", "quiver export void value() {}",
          "quiver quiver void value() {}"}) {
        DiagnosticBag diagnostics;
        (void)ParseSource(source, diagnostics);
        ASSERT_TRUE(diagnostics.HasErrors()) << source;
        ASSERT_FALSE(diagnostics.Diagnostics().empty());
        EXPECT_NE(diagnostics.Diagnostics()[0].message.find(
                      "duplicate or conflicting visibility modifier"),
                  std::string::npos)
            << source;
    }
}
