#include <gtest/gtest.h>

#include "diagnostics/diagnostic_bag.hpp"

using namespace flecha::diagnostics;
using namespace flecha::source;

TEST(DiagnosticTests, StoresStructuredDiagnosticData) {
    DiagnosticBag diagnostics;
    const SourceRange range{{4, 2, 1}, {5, 2, 2}};

    diagnostics.Report(DiagnosticCode::UnexpectedCharacter, range,
                       "unexpected character '@'");

    ASSERT_EQ(diagnostics.Diagnostics().size(), 1);
    const Diagnostic& diagnostic = diagnostics.Diagnostics().front();
    EXPECT_EQ(DiagnosticCodeName(diagnostic.code), "FLECHA-L0001");
    EXPECT_EQ(diagnostic.severity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.message, "unexpected character '@'");
    EXPECT_EQ(diagnostic.range, range);
    EXPECT_TRUE(diagnostics.HasErrors());
}

TEST(DiagnosticTests, DistinguishesWarningsFromErrors) {
    DiagnosticBag diagnostics;
    const SourceRange range{{0, 1, 1}, {0, 1, 1}};

    diagnostics.Report(DiagnosticCode::UnexpectedCharacter, range, "warning",
                       DiagnosticSeverity::Warning);

    EXPECT_FALSE(diagnostics.Empty());
    EXPECT_FALSE(diagnostics.HasErrors());
}

TEST(DiagnosticTests, AssignmentTargetHasStableParserCode) {
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::InvalidAssignmentTarget),
              "FLECHA-P0003");
}

TEST(DiagnosticTests, SemanticCodesAreStableAndContiguous) {
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::DuplicateDeclaration),
              "FLECHA-S0001");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::UnknownName), "FLECHA-S0002");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::IncompatibleType),
              "FLECHA-S0003");
    EXPECT_EQ(
        DiagnosticCodeName(DiagnosticCode::SemanticInvalidAssignmentTarget),
        "FLECHA-S0004");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::SymbolNotWritable),
              "FLECHA-S0005");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::NotCallable), "FLECHA-S0006");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::WrongArgumentCount),
              "FLECHA-S0007");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::IncompatibleArgument),
              "FLECHA-S0008");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::InvalidOperand),
              "FLECHA-S0009");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::InvalidCondition),
              "FLECHA-S0010");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::InvalidReturnType),
              "FLECHA-S0011");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::CannotInferType),
              "FLECHA-S0012");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::InvalidVariableType),
              "FLECHA-S0013");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::LoopControlOutsideLoop),
              "FLECHA-S0014");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::DuplicatePosition),
              "FLECHA-S0015");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::UnknownPosition),
              "FLECHA-S0016");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::InvalidCast), "FLECHA-S0017");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::InvalidMember),
              "FLECHA-S0018");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::NotIndexable), "FLECHA-S0019");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::BorrowEscapesOwner),
              "FLECHA-S0020");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::UninitializedVariable),
              "FLECHA-S0021");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::BorrowRequiresInitializer),
              "FLECHA-S0022");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::InvalidExtern),
              "FLECHA-S0055");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::InvalidMatch), "FLECHA-S0057");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::InvalidBorrowContract),
              "FLECHA-S0058");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::MovedValue), "FLECHA-S0059");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::InvalidFormat),
              "FLECHA-S0060");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::InvalidDefaultArgument),
              "FLECHA-S0061");
}

TEST(DiagnosticTests, CompilerCodesAreStableAndContiguous) {
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::NativeTargetUnavailable),
              "FLECHA-C0001");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::ObjectEmissionFailed),
              "FLECHA-C0002");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::RuntimeUnavailable),
              "FLECHA-C0003");
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::LinkerFailed), "FLECHA-C0004");
}
