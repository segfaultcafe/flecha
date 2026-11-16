#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

#include "core/ast_walk.hpp"
#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "lowering/lowerer.hpp"
#include "semantic/control_flow_graph.hpp"
#include "semantic/semantic_analyzer.hpp"

namespace {
using namespace flecha::core;
using namespace flecha::semantic;
using namespace flecha::diagnostics;

struct Analysis final {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit;
    SemanticModel model;
};
Analysis Analyze(const std::string& text) {
    Analysis result;
    Tokenizer tokenizer(text, result.diagnostics);
    Parser parser(tokenizer.Tokenize(), result.diagnostics);
    result.unit = parser.Parse();
    SemanticAnalyzer analyzer(result.diagnostics);
    result.model = analyzer.Analyze(*result.unit);
    return result;
}
bool Has(const Analysis& analysis, DiagnosticCode code) {
    return std::ranges::any_of(analysis.diagnostics.Diagnostics(),
                               [code](const Diagnostic& diagnostic) {
                                   return diagnostic.code == code &&
                                          diagnostic.severity ==
                                              DiagnosticSeverity::Error;
                               });
}
const FunctionDeclaration& Function(const Analysis& analysis,
                                    std::size_t index = 0) {
    return static_cast<const FunctionDeclaration&>(
        *analysis.unit->Declarations()[index]);
}

TEST(AsyncFrontendTests,
     AsyncEffectPreservesCompletedResultAndBlocksSyncLowering) {
    Analysis result = Analyze("async i32 answer() { return 42; }");
    ASSERT_FALSE(Has(result, DiagnosticCode::UnexpectedToken));
    const auto& function = Function(result);
    ASSERT_TRUE(function.IsAsync());
    const auto* symbol = static_cast<const FunctionSymbol*>(
        result.model.DeclaredSymbol(function));
    ASSERT_NE(symbol, nullptr);
    EXPECT_TRUE(symbol->IsAsync());
    EXPECT_EQ(symbol->ReturnType().Kind(), SemanticTypeKind::I32);
    EXPECT_EQ(symbol->Type().Name(), "async func() => (i32)");
    EXPECT_TRUE(Has(result, DiagnosticCode::AsyncNotImplemented));
    flecha::lowering::Lowerer lowerer;
    EXPECT_EQ(lowerer.Lower(*result.unit, result.model, result.diagnostics),
              nullptr);
    DiagnosticBag empty;
    EXPECT_EQ(lowerer.Lower(*result.unit, result.model, empty), nullptr);
}

TEST(AsyncFrontendTests, AsyncEffectCannotBeErasedByCallableAssignment) {
    CallableSignature sync;
    sync.result = SemanticType(SemanticTypeKind::I32);
    CallableSignature async = sync;
    async.isAsync = true;
    SemanticType a(std::make_shared<const CallableSignature>(async));
    SemanticType b(std::make_shared<const CallableSignature>(sync));
    EXPECT_FALSE(IsAssignable(a, b));
    EXPECT_FALSE(IsAssignable(b, a));
}

TEST(AsyncFrontendTests, PreservesAdjacentShiftAndMinusExpressions) {
    for (const std::string expression : {"x >> -y", "x>>-y"}) {
        Analysis result =
            Analyze("i32 shift(i32 x, i32 y) { return " + expression + "; }");
        EXPECT_FALSE(Has(result, DiagnosticCode::UnexpectedToken));
        EXPECT_FALSE(Has(result, DiagnosticCode::ExpectedToken));
        const auto& returned = static_cast<const ReturnStatement&>(
            *Function(result).Body().Statements()[0]);
        ASSERT_EQ(returned.Value()->Kind(), AstNodeKind::BinaryExpression);
        const auto& binary =
            static_cast<const BinaryExpression&>(*returned.Value());
        EXPECT_EQ(binary.OperatorType(), TokenType::ShiftRight);
    }
}

TEST(AsyncFrontendTests, PreservesSynchronousSpawnMemberNames) {
    Analysis result = Analyze(
        "structure Worker {}; i32 Worker::spawn(&const self) { return 42; } "
        "i32 main() { Worker worker = none; return worker.spawn(); }");
    EXPECT_FALSE(result.diagnostics.HasErrors());
}

TEST(AsyncFrontendTests, ShootIsKeywordAndSpawnIsAnOrdinaryIdentifier) {
    DiagnosticBag diagnostics;
    Tokenizer tokenizer("shoot spawn", diagnostics);
    const auto tokens = tokenizer.Tokenize();
    ASSERT_GE(tokens.size(), 2U);
    EXPECT_EQ(tokens[0].type, TokenType::Shoot);
    EXPECT_EQ(tokens[1].type, TokenType::Identifier);
    EXPECT_FALSE(diagnostics.HasErrors());

    Analysis result =
        Analyze("i32 spawn() { return 42; } i32 main() { return spawn(); }");
    EXPECT_FALSE(result.diagnostics.HasErrors());
}

TEST(AsyncFrontendTests, AwaitingOuterCallDoesNotAuthorizeBareArgumentCall) {
    Analysis result = Analyze(
        "async i32 f() { return 1; } async i32 g(i32 x) { return x; } async "
        "i32 main() { return g(f()).await; }");
    EXPECT_TRUE(Has(result, DiagnosticCode::UnawaitedAsyncCall));
}

TEST(AsyncFrontendTests, UnsafeSynchronousCodeCannotAwait) {
    Analysis result = Analyze(
        "async i32 f() { return 1; } unsafe i32 main() { return f().await; }");
    EXPECT_TRUE(Has(result, DiagnosticCode::InvalidAsync));
}

TEST(AsyncFrontendTests, DiagnosesSynchronousAwaitAndSpawn) {
    Analysis awaited =
        Analyze("i32 f() { return 1; } async i32 main() { return f().await; }");
    EXPECT_TRUE(Has(awaited, DiagnosticCode::InvalidAwait));
    Analysis spawned = Analyze(
        "i32 f!() { return 1; } async void main!() { t := shoot f(); }");
    EXPECT_TRUE(Has(spawned, DiagnosticCode::InvalidSpawn));
}

TEST(AsyncFrontendTests, DiagnosesIndirectAsyncCalls) {
    Analysis result = Analyze(
        "async i32 f() { return 1; } async i32 main() { call := f; return "
        "call().await; }");
    EXPECT_TRUE(Has(result, DiagnosticCode::InvalidAsync));
}

TEST(AsyncFrontendTests, OrdinarySynchronousDeferRemainsValidInAsyncBody) {
    Analysis result =
        Analyze("void cleanup() {} async void main() { defer cleanup(); }");
    EXPECT_TRUE(Has(result, DiagnosticCode::AsyncNotImplemented));
    EXPECT_FALSE(Has(result, DiagnosticCode::InvalidDefer));
    EXPECT_FALSE(Has(result, DiagnosticCode::InvalidAsync));
}

TEST(AsyncFrontendTests, RejectsCrossScopeJumpsButPreservesSameScopeJumps) {
    Analysis cross = Analyze(
        "async void main() { while true { jumpto outside; } #outside }");
    EXPECT_TRUE(Has(cross, DiagnosticCode::InvalidAsync));
    Analysis same = Analyze("async void main() { #again jumpto again; }");
    EXPECT_FALSE(Has(same, DiagnosticCode::InvalidAsync));
}

TEST(AsyncFrontendTests, ReturnDrainsEveryExitedBlockFromInnerToOuter) {
    Analysis result = Analyze("async void main(bool c) { if c { return; } }");
    const auto& function = Function(result);
    const ControlFlowGraph graph =
        BuildControlFlowGraph(function, result.model);
    int returns = 0, fallthroughs = 0;
    for (const TaskScopeExit& exit : graph.taskScopeExits) {
        if (exit.kind == TaskScopeExitKind::CancelAndDrain) {
            ++returns;
            ASSERT_EQ(exit.scopes.size(), 2);
            EXPECT_EQ(exit.scopes.back(), &function.Body());
            EXPECT_EQ(graph.scopeParents.at(exit.scopes.front()),
                      &function.Body());
            EXPECT_FALSE(exit.successor);
        } else
            ++fallthroughs;
    }
    EXPECT_EQ(returns, 1);
    EXPECT_GE(fallthroughs, 1);
}

TEST(AsyncFrontendTests, LoopTransfersDrainOnlyExitedScopes) {
    for (const std::string transfer : {"break", "proceed"}) {
        Analysis result =
            Analyze("async void main(bool c) { while c { if c { " + transfer +
                    "; } } }");
        const auto& function = Function(result);
        const ControlFlowGraph graph =
            BuildControlFlowGraph(function, result.model);
        int drains = 0;
        for (const TaskScopeExit& exit : graph.taskScopeExits) {
            if (exit.kind != TaskScopeExitKind::CancelAndDrain) continue;
            ++drains;
            ASSERT_EQ(exit.scopes.size(), 2);
            EXPECT_NE(exit.scopes.back(), &function.Body());
            EXPECT_EQ(graph.scopeParents.at(exit.scopes.back()),
                      &function.Body());
            EXPECT_TRUE(exit.successor);
        }
        EXPECT_EQ(drains, 1);
    }
}

// Successful frontend analysis still has one execution-gate error per async
// declaration. Reject every other error so positive cases cannot pass
// vacuously.
void ExpectChecked(const Analysis& result) {
    EXPECT_TRUE(Has(result, DiagnosticCode::AsyncNotImplemented));
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        if (diagnostic.severity == DiagnosticSeverity::Error)
            EXPECT_EQ(diagnostic.code, DiagnosticCode::AsyncNotImplemented)
                << diagnostic.message;
}

TEST(AsyncFrontendTests, CheckedOperationsHaveDistinctSemanticRecords) {
    for (const std::string shoot : {"shoot", ">>-"}) {
        const auto result = Analyze(
            "async i32 f() { return 42; } async i32 main() { "
            "child, ce := " +
            shoot +
            " f(); if ce != none { return 1; } "
            "v, ae := child.await; if ae != none { return 2; } "
            "w, e := f().await; if e == none { return v + w; } return 3; }");
        ExpectChecked(result);
        const auto& body = Function(result, 1).Body();
        const auto& declaration =
            static_cast<const DestructureStatement&>(*body.Statements()[0]);
        const auto* checked = result.model.CheckedResultFor(declaration);
        ASSERT_NE(checked, nullptr);
        EXPECT_TRUE(checked->creation);
        EXPECT_EQ(checked->value->Type().Name(), "task<i32>");
        EXPECT_EQ(checked->error->Type().Kind(), SemanticTypeKind::ErrorValue);
        EXPECT_EQ(result.model.TypeOf(declaration.Value()).Name(),
                  "checked-async<task<i32>>");
        int operations = 0;
        WalkAst(Function(result, 1), [&](AstNode& node, bool entering) {
            if (entering && (node.Kind() == AstNodeKind::AwaitExpression ||
                             node.Kind() == AstNodeKind::SpawnExpression)) {
                ++operations;
                EXPECT_EQ(
                    result.model.TypeOf(static_cast<const Expression&>(node))
                        .Kind(),
                    SemanticTypeKind::CheckedAsyncResult);
            }
        });
        EXPECT_EQ(operations, 3);
    }
}

TEST(AsyncFrontendTests, CheckedAwaitControlFlow) {
    for (const std::string body :
         {"if e == none { return v; } return 0;",
          "if e != none { return 0; } return v;",
          "if none == e { return v; } return 0;",
          "if !(e != none) { return v; } return 0;",
          "if e == none && v == 42 { return v; } return 0;",
          "if e != none || v == 42 { return 0; } return v;",
          "while e == none { return v; } return 0;",
          "if e != none { if true { return 0; } else { return 1; } } return v;",
          "if e == none { x := v; } if e != none { return 0; } return v;"}) {
        SCOPED_TRACE(body);
        ExpectChecked(
            Analyze("async i32 f() { return 42; } async i32 main() { "
                    "v, e := f().await; " +
                    body + " }"));
    }
}

TEST(AsyncFrontendTests, RejectsUncheckedAndInvertedUses) {
    for (const std::string body :
         {"return v;", "if e != none { return v; } return 0;",
          "if e == none { x := v; } return v;",
          "if e == none || v == 42 { return 0; } return 0;",
          "while e != none { return v; } return 0;",
          "while e == none { x := v; break; } return v;",
          "e = none; if e == none { return v; } return 0;",
          "other := e; if other == none { return v; } return 0;"}) {
        SCOPED_TRACE(body);
        const auto result = Analyze(
            "async i32 f() { return 42; } async i32 main() { "
            "v, e := f().await; " +
            body + " }");
        EXPECT_TRUE(Has(result, DiagnosticCode::UninitializedVariable));
    }
}

TEST(AsyncFrontendTests, CreationRequiresCheckBeforeAwaitForBothSpellings) {
    for (const std::string shoot : {"shoot", ">>-"}) {
        for (const std::string condition : {"", "if ce != none"}) {
            const auto result = Analyze(
                "async i32 f() { return 1; } async void main() { "
                "child, ce := " +
                shoot + " f(); " + condition +
                " { v, e := child.await; } return; }");
            EXPECT_TRUE(Has(result, DiagnosticCode::UninitializedVariable));
        }
    }
}

TEST(AsyncFrontendTests, ResultsCannotBeStoredForwardedReturnedOrDiscarded) {
    for (const std::string expression : {"shoot f()", ">>- f()", "f().await"}) {
        for (const std::string body :
             {"x := " + expression + ";", "return " + expression + ";",
              "use(" + expression + ");", expression + ";"}) {
            SCOPED_TRACE(body);
            const auto result = Analyze(
                "async i32 f() { return 1; } void use(i32 x) {} "
                "async void main() { " +
                body + " }");
            EXPECT_TRUE(Has(result, DiagnosticCode::InvalidAsync));
        }
    }
    EXPECT_TRUE(Has(Analyze("async i32 f() { return 1; } async void main() { "
                            "v, e := (shoot f()).await; }"),
                    DiagnosticCode::InvalidAsync));
    EXPECT_TRUE(Has(Analyze("async i32 f() { return 1; } async void main() { "
                            "_, e := f().await; }"),
                    DiagnosticCode::InvalidAsync));
}

TEST(AsyncFrontendTests, VoidCompletionAndImplicitJoinFailureStayUnsupported) {
    EXPECT_TRUE(Has(
        Analyze("async void f() {} async void main() { v, e := f().await; }"),
        DiagnosticCode::InvalidAsync));
    EXPECT_TRUE(Has(Analyze("async i32 f() { return 1; } async void main() { "
                            "child, e := shoot f(); }"),
                    DiagnosticCode::InvalidAsync));
    ExpectChecked(
        Analyze("async i32 f() { return 1; } async void main() { "
                "child, e := shoot f(); return; }"));
}

TEST(AsyncFrontendTests, CreationFailureRetainsMoveOnlyArgument) {
    for (const std::string shoot : {"shoot", ">>-"}) {
        const std::string prefix =
            "structure Owner { i32 value; }; "
            "void Owner::tear(&self) override {} "
            "async Owner f(Owner owner) { return owner; } "
            "async i32 main() { Owner owner = none; child, ce := " +
            shoot + " f(owner); ";
        ExpectChecked(Analyze(prefix + "if ce != none { return owner.value; } "
                                       "v, e := child.await; if e != none { "
                                       "return 0; } return v.value; }"));
        EXPECT_TRUE(
            Has(Analyze(prefix +
                        "if ce == none { return owner.value; } return 0; }"),
                DiagnosticCode::MovedValue));
        EXPECT_TRUE(Has(Analyze(prefix + "return owner.value; }"),
                        DiagnosticCode::MovedValue));
        EXPECT_TRUE(
            Has(Analyze(prefix + "if ce != none { return 0; } "
                                 "v, e := child.await; if e != none { return "
                                 "0; } copy := v; return v.value; }"),
                DiagnosticCode::MovedValue));
    }
}

TEST(AsyncFrontendTests, OrdinaryErrorCompletionIsNotExecutionFailure) {
    for (const std::string resultType : {"error", "(i32, error)"}) {
        const auto result =
            Analyze("async " + resultType + " f() { return " +
                    (resultType == "error" ? "none" : "(1, none)") +
                    "; } async void main() { "
                    "v, e := f().await; if e != none { return; } return; }");
        ExpectChecked(result);
        const auto& declaration = static_cast<const DestructureStatement&>(
            *Function(result, 1).Body().Statements()[0]);
        const auto* checked = result.model.CheckedResultFor(declaration);
        ASSERT_NE(checked, nullptr);
        EXPECT_EQ(checked->value->Type().Kind(),
                  resultType == "error" ? SemanticTypeKind::ErrorValue
                                        : SemanticTypeKind::Tuple);
        EXPECT_EQ(checked->error->Type().Kind(), SemanticTypeKind::ErrorValue);
    }
}

TEST(AsyncFrontendTests, LoopActivationsAndNestedScopesKeepChecksSeparate) {
    ExpectChecked(Analyze(
        "async i32 f() { return 1; } async void main(bool again) { "
        "while again { child, ce := shoot f(); if ce != none { proceed; } "
        "v, e := child.await; if e != none { break; } x := v; } }"));
    EXPECT_TRUE(Has(
        Analyze(
            "async i32 f() { return 1; } async void main(bool again) { "
            "while again { v, e := f().await; if e != none { x := v; } } }"),
        DiagnosticCode::UninitializedVariable));
    ExpectChecked(
        Analyze("async i32 f() { return 1; } async i32 main() { "
                "v, e := f().await; if e != none { return 0; } "
                "{ v, e := f().await; if e != none { return 0; } x := v; } "
                "return v; }"));
}

TEST(AsyncFrontendTests, ChildCannotBeJoinedTwiceOrThroughMovedHandle) {
    const std::string prefix =
        "async i32 f() { return 1; } async void main() { "
        "child, ce := shoot f(); if ce != none { return; } ";
    EXPECT_TRUE(
        Has(Analyze(prefix +
                    "v, e := child.await; w, other := child.await; return; }"),
            DiagnosticCode::MovedReference));
    const auto moved =
        Analyze(prefix + "other := child; v, e := child.await; return; }");
    EXPECT_TRUE(Has(moved, DiagnosticCode::MovedValue) ||
                Has(moved, DiagnosticCode::MovedReference));
    EXPECT_TRUE(Has(Analyze(prefix + "v, e := child.await; if e != none { w, x "
                                     ":= child.await; } return; }"),
                    DiagnosticCode::MovedReference));
}

TEST(AsyncFrontendTests, CaptureLoansFollowCreationOutcomeAndJoin) {
    const std::string prefix =
        "async i32 fill([]u8 bytes) { bytes[0] = 42; return 0; } "
        "async void main() { [16]u8 buffer = none; child, ce := shoot "
        "fill(buffer[:]); ";
    ExpectChecked(Analyze(prefix +
                          "if ce != none { buffer[0] = 7; return; } "
                          "v, e := child.await; buffer[0] = 7; return; }"));
    EXPECT_TRUE(Has(Analyze(prefix + "buffer[0] = 7; return; }"),
                    DiagnosticCode::BorrowConflict));
    EXPECT_TRUE(
        Has(Analyze(prefix + "if ce == none { buffer[0] = 7; } return; }"),
            DiagnosticCode::BorrowConflict));
}

TEST(AsyncFrontendTests, BorrowedResultsRetainOriginsOnlyOnSuccess) {
    for (const bool child : {false, true}) {
        const std::string prefix =
            "async []const u8 view([]const u8 bytes) <- bytes { return bytes; "
            "} "
            "async u8 main() { [16]u8 buffer = none; " +
            std::string(child ? "task, ce := shoot view(buffer[:]); if ce != "
                                "none { return 0; } v, e := task.await; "
                              : "v, e := view(buffer[:]).await; ");
        ExpectChecked(Analyze(
            prefix +
            "if e != none { buffer[0] = 7; return 0; } return v[0]; }"));
        EXPECT_TRUE(Has(
            Analyze(prefix +
                    "if e != none { return 0; } buffer[0] = 7; return v[0]; }"),
            DiagnosticCode::BorrowConflict));
    }
}

TEST(AsyncFrontendTests, MethodsRetainReceiversAndArgumentsUntilJoin) {
    const std::string definitions =
        "structure Box { i32 value; }; "
        "async i32 Box::fill(&self) { self->value = 1; return 0; } "
        "async i32 Box::write(&const self, []u8 bytes) { bytes[0] = 1; return "
        "0; } "
        "async i32 Box::staticWrite([]u8 bytes) { bytes[0] = 1; return 0; } ";
    for (const std::string call :
         {"box.fill()", "reference->fill()", "box.write(bytes[:])",
          "Box::staticWrite(bytes[:])"}) {
        for (const bool join : {false, true}) {
            SCOPED_TRACE(call + (join ? " joined" : " pending"));
            const auto result = Analyze(
                definitions +
                "async void main() { Box box = none; "
                "[2]u8 bytes = none; reference := &box; child, ce := shoot " +
                call + "; if ce != none { return; } " +
                (join ? "v, e := child.await; " : "") +
                (call.find("fill") != std::string::npos ? "box.value = 2; "
                                                        : "bytes[0] = 2; ") +
                "return; }");
            if (join)
                ExpectChecked(result);
            else
                EXPECT_TRUE(Has(result, DiagnosticCode::BorrowConflict));
        }
    }
}

TEST(AsyncFrontendTests, SharedCapturesAndPreparedSynchronousCalls) {
    const std::string prefix =
        "structure Box { i32 value; }; "
        "i32 Box::read(&const self) { return self->value; } "
        "async i32 Box::consume(&const self, i32 value) { return value; } "
        "async i32 read([]const u8 bytes) { return 0; } ";
    ExpectChecked(Analyze(
        prefix +
        "async void main() { Box worker = none; Box source = none; "
        "prepared := source.read(); a, ae := shoot worker.consume(prepared); "
        "if ae != none { return; "
        "} "
        "source.value = 2; [2]u8 bytes = none; "
        "b, be := shoot read(bytes[:]); if be != none { return; } "
        "c, ce := shoot read(bytes[:]); if ce != none { return; } "
        "x, xe := a.await; y, ye := b.await; z, ze := c.await; return; }"));
}

TEST(AsyncFrontendTests, MethodResultRetainsReceiverBorrow) {
    const auto result = Analyze(
        "structure Box { i32 value; }; "
        "async &const i32 Box::view(&const self) <- self { return "
        "&self->value; } "
        "async i32 main() { Box box = none; child, ce := shoot box.view(); "
        "if ce != none { return 0; } reference, e := child.await; "
        "if e != none { return 0; } box.value = 2; return *reference; }");
    EXPECT_TRUE(Has(result, DiagnosticCode::BorrowConflict));
}

TEST(AsyncFrontendTests, AliasedErrorWritesCannotForgeSuccess) {
    for (const std::string mutation : {"p := &e; *p = none;", "clear(&e);"}) {
        const auto result = Analyze(
            "void clear(&error e) { *e = none; } "
            "async i32 f() { return 1; } async i32 main() { v, e := "
            "f().await; " +
            mutation + " if e == none { return v; } return 0; }");
        EXPECT_TRUE(Has(result, DiagnosticCode::UninitializedVariable));
    }
}

TEST(AsyncFrontendTests,
     BorrowedTupleCompletionSurvivesCheckedAndOrdinaryDestructuring) {
    for (const bool child : {false, true}) {
        const std::string prefix =
            "async ([]const u8, error) view([]const u8 bytes) <- bytes { "
            "return (bytes, none); } "
            "async u8 main() { [16]u8 buffer = none; " +
            std::string(child ? "task, ce := shoot view(buffer[:]); if ce != "
                                "none { return 0; } pair, e := task.await; "
                              : "pair, e := view(buffer[:]).await; ") +
            "if e != none { return 0; } view, ordinaryError := pair; ";
        ExpectChecked(Analyze(prefix + "return view[0]; }"));
        EXPECT_TRUE(Has(Analyze(prefix + "buffer[0] = 7; return view[0]; }"),
                        DiagnosticCode::BorrowConflict));
    }
}

TEST(AsyncFrontendTests, ChildErrorCompletionIsNotFlattened) {
    ExpectChecked(
        Analyze("async error f() { return none; } async error main() { "
                "child, ce := shoot f(); if ce != none { return ce; } "
                "ordinaryError, e := child.await; if e != none { return e; } "
                "return ordinaryError; }"));
}

TEST(AsyncFrontendTests,
     ExecutionFailureDoesNotRestoreAlreadyTransferredArguments) {
    const std::string prefix =
        "structure Owner { i32 value; }; void Owner::tear(&self) override {} "
        "async Owner f(Owner owner) { return owner; } async i32 main() { Owner "
        "owner = none; ";
    EXPECT_TRUE(Has(Analyze(prefix + "v, e := f(owner).await; if e != none { "
                                     "return owner.value; } return 0; }"),
                    DiagnosticCode::MovedValue));
}

TEST(AsyncFrontendTests,
     UnresolvedArgumentPreparationRequiresSeparateStatement) {
    for (const std::string operation :
         {"shoot f(make())", ">>- f(make())", "f(make()).await"}) {
        const auto result = Analyze(
            "i32 make() { return 1; } async i32 f(i32 value) { return value; } "
            "async void main() { v, e := " +
            operation + "; return; }");
        EXPECT_TRUE(Has(result, DiagnosticCode::InvalidAsync));
        EXPECT_TRUE(std::ranges::any_of(
            result.diagnostics.Diagnostics(), [](const auto& diagnostic) {
                return diagnostic.message.find(
                           "argument-preparation effects") != std::string::npos;
            }));
    }
    EXPECT_TRUE(Has(
        Analyze(
            "structure Owner {}; void Owner::tear(&self) override {} "
            "async i32 f((Owner, i32) input) { return 1; } async void main() { "
            "Owner owner = none; child, e := shoot f((owner, 1)); return; }"),
        DiagnosticCode::InvalidAsync));
    ExpectChecked(Analyze(
        "structure Owner {}; void Owner::tear(&self) override {} "
        "async i32 f((Owner, i32) input) { return 1; } async void main() { "
        "Owner owner = none; prepared := (owner, 1); child, e := shoot "
        "f(prepared); "
        "if e != none { moved := prepared; return; } v, ae := child.await; "
        "return; }"));
}

TEST(AsyncFrontendTests, ChildAwaitMustBeImmediatelyDestructured) {
    for (const std::string operation :
         {"stored := child.await;", "use(child.await);",
          "return child.await;"}) {
        EXPECT_TRUE(
            Has(Analyze("async i32 f() { return 1; } void use(i32 v) {} async "
                        "void main() { "
                        "child, ce := shoot f(); if ce != none { return; } " +
                        operation + " }"),
                DiagnosticCode::InvalidAsync));
    }
}

TEST(AsyncFrontendTests, JoinOnOneBranchCannotBeRepeatedAfterMerge) {
    EXPECT_TRUE(
        Has(Analyze("async i32 f() { return 1; } async void main(bool join) { "
                    "child, ce := shoot f(); if ce != none { return; } "
                    "if join { v, e := child.await; } w, other := child.await; "
                    "return; }"),
            DiagnosticCode::MovedReference));
}

TEST(AsyncFrontendTests, ExplicitConditionalJoinPermitsNormalScopeExit) {
    ExpectChecked(Analyze(
        "async i32 f() { return 1; } async void main() { "
        "child, e := shoot f(); if e == none { v, ae := child.await; } }"));
}

TEST(AsyncFrontendTests, CrashableBodiesKeepOrdinaryExplicitCallEffects) {
    ExpectChecked(
        Analyze("async i32 f!() { return 1; } async void main!() { "
                "child, e := shoot f!(); if e != none { return; } v, ae := "
                "child.await; }"));
    EXPECT_TRUE(Has(Analyze("async i32 f!() { return 1; } async void main() { "
                            "child, e := shoot f!(); return; }"),
                    DiagnosticCode::InvalidCrashable));
}

}  // namespace
