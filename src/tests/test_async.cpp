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

TEST(AsyncFrontendTests, AwaitsDirectCallsAndBothChildCreationSpellings) {
    Analysis result = Analyze(
        "async i32 answer!() { return 42; } "
        "async i32 main!() { a := shoot answer(); b := >>- answer(); "
        "x := a.await; y := b.await; return x + y + answer!().await; }");
    EXPECT_FALSE(Has(result, DiagnosticCode::UnexpectedToken));
    EXPECT_FALSE(Has(result, DiagnosticCode::UnawaitedAsyncCall));
    EXPECT_FALSE(Has(result, DiagnosticCode::InvalidSpawn));
    EXPECT_FALSE(Has(result, DiagnosticCode::InvalidAwait));
    EXPECT_FALSE(Has(result, DiagnosticCode::InvalidCrashable));
    int spawns = 0, awaits = 0;
    WalkAst(Function(result, 1), [&](AstNode& node, bool entering) {
        if (!entering) return;
        if (node.Kind() == AstNodeKind::SpawnExpression) {
            ++spawns;
            EXPECT_EQ(result.model.TypeOf(static_cast<const Expression&>(node))
                          .Kind(),
                      SemanticTypeKind::Task);
        }
        if (node.Kind() == AstNodeKind::AwaitExpression) {
            ++awaits;
            EXPECT_EQ(result.model.TypeOf(static_cast<const Expression&>(node))
                          .Kind(),
                      SemanticTypeKind::I32);
        }
    });
    EXPECT_EQ(spawns, 2);
    EXPECT_EQ(awaits, 3);
}

TEST(AsyncFrontendTests, ShootCanBeImmediatelyJoinedThroughParentheses) {
    Analysis result = Analyze(
        "async i32 f!() { return 1; } async i32 g!() { return (shoot "
        "f()).await; }");
    EXPECT_FALSE(Has(result, DiagnosticCode::InvalidSpawn));
    EXPECT_FALSE(Has(result, DiagnosticCode::InvalidAwait));
    EXPECT_FALSE(Has(result, DiagnosticCode::UnawaitedAsyncCall));
    EXPECT_FALSE(Has(result, DiagnosticCode::InvalidCrashable));
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

TEST(AsyncFrontendTests, MutableCaptureStaysLiveUntilJoinEvenWithoutHandleUse) {
    const std::string prefix =
        "async void fill!([]u8 bytes) { bytes[0] = 42; } ";
    for (const std::string shoot :
         {"task := shoot fill(buffer[:]);", "shoot fill(buffer[:]);"}) {
        Analysis result =
            Analyze(prefix + "async void main!() { [16]u8 buffer = none; " +
                    shoot + " buffer[0] = 7; }");
        EXPECT_TRUE(Has(result, DiagnosticCode::BorrowConflict));
        EXPECT_FALSE(Has(result, DiagnosticCode::UnknownName));
        EXPECT_FALSE(Has(result, DiagnosticCode::InvalidSpawn));
    }
}

TEST(AsyncFrontendTests, JoinReleasesCaptureLoansWithoutBorrowedResult) {
    Analysis result = Analyze(
        "async void fill!([]u8 bytes) { bytes[0] = 42; } "
        "async void main!() { [16]u8 buffer = none; task := shoot "
        "fill(buffer[:]); "
        "task.await; buffer[0] = 7; }");
    EXPECT_FALSE(Has(result, DiagnosticCode::BorrowConflict));
    EXPECT_FALSE(Has(result, DiagnosticCode::MovedReference));
    EXPECT_FALSE(Has(result, DiagnosticCode::InvalidSpawn));
}

TEST(AsyncFrontendTests,
     SharedCapturesCoexistButOverlappingMutableCaptureDoesNot) {
    const std::string prefix =
        "async void read!([]const u8 bytes) {} async void fill!([]u8 bytes) "
        "{} ";
    Analysis shared = Analyze(prefix +
                              "async void main!() { [16]u8 buffer = none; "
                              "a := shoot read(buffer[:]); b := shoot "
                              "read(buffer[:]); a.await; b.await; }");
    EXPECT_FALSE(Has(shared, DiagnosticCode::BorrowConflict));
    Analysis mixed = Analyze(prefix +
                             "async void main!() { [16]u8 buffer = none; "
                             "a := shoot read(buffer[:]); b := shoot "
                             "fill(buffer[:]); a.await; b.await; }");
    EXPECT_TRUE(Has(mixed, DiagnosticCode::BorrowConflict));
}

TEST(AsyncFrontendTests, BorrowedResultKeepsItsOriginAfterJoin) {
    Analysis result = Analyze(
        "async []const u8 view!([]const u8 bytes) <- bytes { return bytes; } "
        "async u8 main!() { [16]u8 buffer = none; task := shoot "
        "view(buffer[:]); "
        "slice := task.await; buffer[0] = 7; return slice[0]; }");
    EXPECT_TRUE(Has(result, DiagnosticCode::BorrowConflict));
    EXPECT_FALSE(Has(result, DiagnosticCode::InvalidBorrowContract));
    EXPECT_FALSE(Has(result, DiagnosticCode::InvalidAwait));
}

// Async execution is gated, but borrow analysis must still diagnose unsafe
// captures without introducing unrelated errors into otherwise valid calls.
void ExpectCaptureDiagnostics(const Analysis& result, bool conflict) {
    EXPECT_TRUE(Has(result, DiagnosticCode::AsyncNotImplemented));
    EXPECT_EQ(Has(result, DiagnosticCode::BorrowConflict), conflict);
    for (const Diagnostic& diagnostic : result.diagnostics.Diagnostics()) {
        if (diagnostic.severity != DiagnosticSeverity::Error) continue;
        EXPECT_TRUE(
            diagnostic.code == DiagnosticCode::AsyncNotImplemented ||
            (conflict && diagnostic.code == DiagnosticCode::BorrowConflict))
            << diagnostic.message;
    }
}

TEST(AsyncFrontendTests, SpawnedMethodRetainsMutableReceiverUntilJoin) {
    const std::string prefix =
        "structure Box { i32 value; }; "
        "async void Box::fill(&self) { self->value = 1; } ";
    for (const std::string call : {"box.fill()", "reference->fill()"}) {
        for (const bool join : {false, true}) {
            SCOPED_TRACE(call + (join ? " joined" : " pending"));
            const Analysis result = Analyze(
                prefix + "async void main!() { Box box = none; " +
                (call == "reference->fill()" ? "reference := &box; " : "") +
                "child := shoot " + call + "; " +
                (join ? "child.await; " : "") + "box.value = 2; }");
            ExpectCaptureDiagnostics(result, !join);
        }
    }
    const Analysis discarded = Analyze(prefix +
                                       "async void main!() { Box box = none; "
                                       "shoot box.fill(); box.value = 2; }");
    ExpectCaptureDiagnostics(discarded, true);
}

TEST(AsyncFrontendTests, SpawnedMethodRetainsArgumentsUntilJoin) {
    const std::string prefix =
        "structure Worker {}; "
        "async void Worker::fill(&const self, i32 count, []u8 bytes) { "
        "bytes[0] = count.cast<u8>; } ";
    for (const bool join : {false, true}) {
        SCOPED_TRACE(join ? "joined" : "pending");
        const Analysis result =
            Analyze(prefix +
                    "async void main!() { Worker worker = none; "
                    "[2]u8 bytes = none; "
                    "child := shoot worker.fill(1, bytes[:]); " +
                    (join ? "child.await; " : "") + "bytes[0] = 2; }");
        ExpectCaptureDiagnostics(result, !join);
    }
}

TEST(AsyncFrontendTests, SpawnedMethodsShareConstReceiversButPreventMutation) {
    const std::string prefix =
        "structure Box { i32 value; }; "
        "async i32 Box::read(&const self) { return self->value; } "
        "async void Box::fill(&self) { self->value = 1; } ";
    for (const std::string operation :
         {"value := box.value;", "box.value = 2;", "c := shoot box.fill();"}) {
        SCOPED_TRACE(operation);
        const Analysis result =
            Analyze(prefix +
                    "async void main!() { Box box = none; "
                    "a := shoot box.read(); b := shoot box.read(); " +
                    operation + " a.await; b.await; }");
        ExpectCaptureDiagnostics(result, operation != "value := box.value;");
    }
}

TEST(AsyncFrontendTests,
     SpawnedMethodRetainsSharedArgumentsWithoutExclusivity) {
    const std::string prefix =
        "structure Worker {}; "
        "async void Worker::read(&const self, []const u8 bytes) {} ";
    for (const std::string operation :
         {"value := bytes[0];", "bytes[0] = 2;"}) {
        SCOPED_TRACE(operation);
        const Analysis result =
            Analyze(prefix +
                    "async void main!() { Worker worker = none; "
                    "[2]u8 bytes = none; "
                    "a := shoot worker.read(bytes[:]); "
                    "b := shoot worker.read(bytes[:]); " +
                    operation + " a.await; b.await; }");
        ExpectCaptureDiagnostics(result, operation == "bytes[0] = 2;");
    }
}

TEST(AsyncFrontendTests, SpawnedStaticMethodRetainsArgumentsUntilJoin) {
    const std::string prefix =
        "structure Worker {}; "
        "async void Worker::fill([]u8 bytes) { bytes[0] = 1; } ";
    for (const bool join : {false, true}) {
        SCOPED_TRACE(join ? "joined" : "pending");
        const Analysis result =
            Analyze(prefix +
                    "async void main!() { [2]u8 bytes = none; "
                    "child := shoot Worker::fill(bytes[:]); " +
                    (join ? "child.await; " : "") + "bytes[0] = 2; }");
        ExpectCaptureDiagnostics(result, !join);
    }
}

TEST(AsyncFrontendTests, SpawnDoesNotCaptureNestedSynchronousMethodLoans) {
    const Analysis result = Analyze(
        "structure Box { i32 value; }; "
        "i32 Box::read(&const self) { return self->value; } "
        "async void Box::consume(&const self, i32 value) {} "
        "async void consume!(i32 value) {} "
        "async void main!() { Box worker = none; Box source = none; "
        "a := shoot worker.consume(source.read()); "
        "b := shoot consume(source.read()); source.value = 2; "
        "a.await; b.await; }");
    ExpectCaptureDiagnostics(result, false);
}

TEST(AsyncFrontendTests, JoinedMethodResultRetainsReceiverBorrow) {
    const Analysis result = Analyze(
        "structure Box { i32 value; }; "
        "async &const i32 Box::view(&const self) <- self { "
        "return &self->value; } "
        "async i32 main!() { Box box = none; "
        "child := shoot box.view(); reference := child.await; "
        "box.value = 2; return *reference; }");
    ExpectCaptureDiagnostics(result, true);
}

TEST(AsyncFrontendTests, ChildResultCannotBeJoinedTwiceOrThroughMovedHandle) {
    const std::string prefix = "async i32 answer!() { return 42; } ";
    Analysis joined = Analyze(prefix +
                              "async void main!() { task := shoot answer(); "
                              "task.await; task.await; }");
    EXPECT_TRUE(Has(joined, DiagnosticCode::MovedReference));
    Analysis moved = Analyze(prefix +
                             "async void main!() { task := shoot answer(); "
                             "other := task; task.await; other.await; }");
    EXPECT_TRUE(Has(moved, DiagnosticCode::MovedReference));
}
}  // namespace
