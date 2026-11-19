#include <gtest/gtest.h>
#include <llvm/Support/Program.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#include "codegen/native_compiler.hpp"
#include "core/ast_walk.hpp"
#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "lowering/bound_ir_dumper.hpp"
#include "lowering/lowerer.hpp"
#include "semantic/async_plan.hpp"
#include "semantic/control_flow_graph.hpp"
#include "semantic/semantic_analyzer.hpp"

namespace {
using namespace flecha;
using namespace flecha::core;
using namespace flecha::semantic;
using namespace flecha::diagnostics;

struct Analysis {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit;
    SemanticModel model;
    explicit Analysis(const std::string& source) {
        Tokenizer tokenizer(source, diagnostics);
        Parser parser(tokenizer.Tokenize(), diagnostics);
        unit = parser.Parse();
        SemanticAnalyzer analyzer(diagnostics);
        model = analyzer.Analyze(*unit);
    }
    bool Has(DiagnosticCode code) const {
        return std::ranges::any_of(
            diagnostics.Diagnostics(), [&](const auto& d) {
                return d.code == code &&
                       d.severity == DiagnosticSeverity::Error;
            });
    }
    void ExpectValid() const {
        for (const auto& d : diagnostics.Diagnostics())
            EXPECT_NE(d.severity, DiagnosticSeverity::Error) << d.message;
    }
};

void RunTry(const std::string& source, const std::string& name) {
    Analysis analysis(source);
    analysis.ExpectValid();
    ASSERT_FALSE(analysis.diagnostics.HasErrors());
    lowering::Lowerer lowerer;
    auto program =
        lowerer.Lower(*analysis.unit, analysis.model, analysis.diagnostics);
    ASSERT_NE(program, nullptr);
    codegen::NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    const auto path =
        std::filesystem::temp_directory_path() / ("flecha_try_" + name);
    ASSERT_TRUE(compiler.BuildExecutable(*program, name, path.string()))
        << compiler.Error();
    const std::string filename = path.string();
    const llvm::StringRef arguments[] = {filename};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(filename, arguments), 42);
    std::filesystem::remove(path);
}

TEST(TryFrontendTests, PreservesSyntaxAndLowersExplicitPropagation) {
    Analysis analysis(
        "(i32, error) work() { return (42, none); } (i32, error) f() { return "
        "(try work(), none); }");
    analysis.ExpectValid();
    unsigned attempts = 0;
    WalkAst(*analysis.unit->Declarations().back(),
            [&](AstNode& node, bool entering) {
                if (entering && node.Kind() == AstNodeKind::TryExpression)
                    ++attempts;
            });
    EXPECT_EQ(attempts, 1u);
    const auto& f = static_cast<const FunctionDeclaration&>(
        *analysis.unit->Declarations().back());
    const auto graph = BuildControlFlowGraph(f, analysis.model);
    const auto dispatch = std::ranges::find_if(
        graph.nodes, [](const auto& n) { return n.propagationDispatch; });
    ASSERT_NE(dispatch, graph.nodes.end());
    ASSERT_EQ(dispatch->successors.size(), 2u);
    EXPECT_NE(graph.nodes[dispatch->successors[1]].propagationReturn, nullptr);
    lowering::Lowerer lowerer;
    auto program =
        lowerer.Lower(*analysis.unit, analysis.model, analysis.diagnostics);
    ASSERT_NE(program, nullptr);
    EXPECT_NE(lowering::DumpBoundProgram(*program).find("(try "),
              std::string::npos);
}

TEST(TryFrontendTests, RequiresOrdinaryCallWithFinalError) {
    for (const std::string source :
         {"error f() { error e = none; try e; return none; }",
          "i32 g() { return 1; } error f() { try g(); return none; }",
          "(error, i32) g() { return (none, 1); } error f() { try g(); return "
          "none; }"}) {
        Analysis analysis(source);
        EXPECT_TRUE(analysis.Has(DiagnosticCode::InvalidOperand));
    }
}

TEST(TryFrontendTests, RequiresCompatibleEnclosingError) {
    for (const std::string returned : {"void", "i32", "(error, i32)"}) {
        Analysis analysis("error g() { return none; } " + returned +
                          " f() { try g(); return none; }");
        EXPECT_TRUE(analysis.Has(DiagnosticCode::InvalidReturnType));
    }
    Analysis narrow(
        "error E { Bad } error g() { return none; } E f() { try g(); return "
        "none; }");
    EXPECT_TRUE(narrow.Has(DiagnosticCode::IncompatibleType));
    Analysis different(
        "error E { Bad } error F { Bad } E g() { return none; } F f() { try "
        "g(); return none; }");
    EXPECT_TRUE(different.Has(DiagnosticCode::IncompatibleType));
}

TEST(TryFrontendTests, RejectsInvalidFailureDefaults) {
    Analysis analysis(
        "error g() { return none; } (&i32, error) f(&i32 x) <- x { try g(); "
        "return (x, none); }");
    EXPECT_TRUE(analysis.Has(DiagnosticCode::InvalidReturnType));
}

TEST(TryFrontendTests, RejectsMovingImmovableSuccessValue) {
    Analysis analysis(R"(
        structure Pinned { i32 value; };
        unsafe void Pinned::tear(&self) override {}
        unsafe (Pinned, error) work() { return (build Pinned {}, none); }
        unsafe error f() { value := try work(); return none; }
    )");
    EXPECT_TRUE(analysis.Has(DiagnosticCode::NonCopyableValue));
}

TEST(TryFrontendTests, RejectsDefaultArgumentAndBuildEscape) {
    Analysis defaults(
        "(i32, error) g() { return (1, none); } error f(i32 x = try g()) { "
        "return none; }");
    EXPECT_TRUE(defaults.Has(DiagnosticCode::InvalidOperand));
    Analysis build(
        "error g() { return none; } structure S { i32 x; }; error f() { s := "
        "build S { try g(); self.x = 1; }; return none; }");
    EXPECT_TRUE(build.Has(DiagnosticCode::InvalidOperand));
}

TEST(TryFrontendTests, KeepsQuestionExclusiveToAwait) {
    for (const std::string syntax : {"g()?", "g?!(1)"}) {
        Analysis analysis("error g() { return none; } error f() { " + syntax +
                          "; return none; }");
        EXPECT_TRUE(analysis.Has(DiagnosticCode::ExpectedToken) ||
                    analysis.Has(DiagnosticCode::UnexpectedToken));
    }
}

TEST(TryFrontendTests, ParsesPeekFormsAndRejectsBareHookBlocks) {
    Analysis valid(R"(
        error E { Bad }
        error Other { Mapped }
        E fail() { return E.Bad; }
        error unbound() { try fail() peek {}; return none; }
        error inferred() {
            try fail() peek err { if err != E.Bad { err = E.Bad; } };
            return none;
        }
        error widened() {
            try fail() peek err as error { err = E.Bad; };
            return none;
        }
        Other explicit(bool handled) {
            try fail() peek err {
                if handled { return none; }
                return Other.Mapped;
            };
            return none;
        }
        (&i32, Other) explicitReference(&i32 value, bool handled) <- value {
            try fail() peek {
                if handled { return (value, none); }
                return (value, Other.Mapped);
            };
            return (value, none);
        }
    )");
    valid.ExpectValid();

    Analysis bare(R"(
        error E { Bad }
        E fail() { return E.Bad; }
        error invalid() { try fail() {}; return none; }
    )");
    EXPECT_TRUE(bare.Has(DiagnosticCode::ExpectedToken) ||
                bare.Has(DiagnosticCode::UnexpectedToken));
}

TEST(TryFrontendTests, ValidatesPeekBindingAndControlFlow) {
    Analysis invalidType(R"(
        error E { Bad }
        E fail() { return E.Bad; }
        error f() { try fail() peek err as i32 {}; return none; }
    )");
    EXPECT_TRUE(invalidType.Has(DiagnosticCode::InvalidVariableType));

    Analysis incompatibleFallthrough(R"(
        error E { Bad }
        error Other { Mapped }
        E fail() { return E.Bad; }
        Other f() { try fail() peek {}; return none; }
    )");
    EXPECT_TRUE(incompatibleFallthrough.Has(DiagnosticCode::IncompatibleType));

    Analysis propagating(R"(
        error E { Bad }
        E fail() { return E.Bad; }
        error f() { try fail() peek { try fail(); }; return none; }
    )");
    EXPECT_TRUE(propagating.Has(DiagnosticCode::InvalidStructure));

    Analysis outerBreak(R"(
        error E { Bad }
        E fail() { return E.Bad; }
        error f() {
            while true { try fail() peek { break; }; break; }
            return none;
        }
    )");
    EXPECT_TRUE(outerBreak.Has(DiagnosticCode::LoopControlOutsideLoop));

    Analysis innerLoop(R"(
        error E { Bad }
        E fail() { return E.Bad; }
        error f() {
            try fail() peek { while true { break; } };
            return none;
        }
    )");
    innerLoop.ExpectValid();
}

TEST(TryFrontendTests, PreservesBorrowOrigin) {
    Analysis valid(R"(
        ([]const u8, error) view([]const u8 bytes) <- bytes { return (bytes, none); }
        error f([]const u8 input) {
            result := try view(input);
            length := result.len();
            return none;
        }
    )");
    valid.ExpectValid();
    Analysis invalid(R"(
        ([]u8, error) view([]u8 bytes) <- bytes { return (bytes, none); }
        error f() {
            [2]u8 data = none;
            result := try view(data[:]);
            data[0] = 1;
            length := result.len();
            return none;
        }
    )");
    EXPECT_TRUE(invalid.Has(DiagnosticCode::BorrowConflict));
}

TEST(TryFrontendTests, TracksMovedResultsAndArguments) {
    Analysis analysis(R"(
        (own []i32, error) take(own []i32 value) { return (value, none); }
        error f() {
            input, allocationError := new<own []i32>(2);
            if allocationError != none { return allocationError; }
            output := try take(input);
            n := input.len();
            return none;
        }
    )");
    EXPECT_TRUE(analysis.Has(DiagnosticCode::MovedValue));
}

TEST(TryFrontendTests, KeepsAsyncReturnAndExecutionFailureDistinct) {
    Analysis analysis(R"(
        error check() { return none; }
        async i32 work() { return 42; }
        async (i32, error) f() { try check(); value := work().await?; return (value, none); }
    )");
    for (const auto& d : analysis.diagnostics.Diagnostics())
        if (d.severity == DiagnosticSeverity::Error)
            ADD_FAILURE() << DiagnosticCodeName(d.code) << ": " << d.message;
    const auto& function = static_cast<const FunctionDeclaration&>(
        *analysis.unit->Declarations().back());
    const auto* plan = analysis.model.AsyncPlanFor(function);
    ASSERT_NE(plan, nullptr);
    EXPECT_TRUE(VerifyAsyncFramePlan(*plan, analysis.model).empty());
    EXPECT_TRUE(std::ranges::any_of(
        plan->flow.nodes, [](const auto& n) { return n.propagationReturn; }));
    EXPECT_TRUE(std::ranges::any_of(
        plan->flow.nodes, [](const auto& n) { return n.failureExit; }));
    lowering::Lowerer lowerer;
    auto synchronous =
        lowerer.Lower(*analysis.unit, analysis.model, analysis.diagnostics);
    ASSERT_NE(synchronous, nullptr);
    EXPECT_FALSE(synchronous->Functions().empty());
}

TEST(TryFrontendTests, OrdinaryFailureDrainsChildrenAsCompletedErrorReturn) {
    Analysis analysis(R"(
        error check() { return none; }
        async i32 work() { return 42; }
        async (i32, error) f() {
            child, ce := shoot work();
            if ce != none { return (none, ce); }
            try check();
            value, ae := child.await;
            if ae != none { return (none, ae); }
            return (value, none);
        }
    )");
    for (const auto& d : analysis.diagnostics.Diagnostics())
        if (d.severity == DiagnosticSeverity::Error)
            ADD_FAILURE() << DiagnosticCodeName(d.code) << ": " << d.message;
    const auto& function = static_cast<const FunctionDeclaration&>(
        *analysis.unit->Declarations().back());
    const auto* plan = analysis.model.AsyncPlanFor(function);
    ASSERT_NE(plan, nullptr);
    EXPECT_TRUE(VerifyAsyncFramePlan(*plan, analysis.model).empty());
    bool drains = false;
    for (const auto& exit : plan->flow.taskScopeExits)
        if (plan->flow.nodes[exit.source].propagationReturn) {
            EXPECT_EQ(exit.kind, TaskScopeExitKind::CancelAndDrain);
            EXPECT_FALSE(exit.scopes.empty());
            drains = true;
        }
    EXPECT_TRUE(drains);
    EXPECT_FALSE(std::ranges::any_of(plan->states, [](const auto& state) {
        return state.kind == AsyncStateKind::Failed;
    }));
}

TEST(TryRuntimeTests, ErrorOnlyMultipleResultsAndNestedErrorPayload) {
    RunTry(R"(
        error E { Bad }
        E check(bool fail) { if fail { return E.Bad; } return none; }
        error forward(bool fail) { try check(fail); return none; }
        (i32, i32, E) pair(bool fail) { try check(fail); return (20, 22, none); }
        (i32, error) sum(bool fail) { a, b := try pair(fail); return (a + b, none); }
        ((i32, error), E) nested() { return ((42, E.Bad), none); }
        (error, E) error_payload() { return (E.Bad, none); }
        error exercise() {
            if forward(false) != none or forward(true) != E.Bad { return E.Bad; }
            value := try sum(false);
            if value != 42 { return E.Bad; }
            missing, failure := sum(true);
            if missing != 0 or failure != E.Bad { return E.Bad; }
            number, payload := try nested();
            if number != 42 or payload != E.Bad { return E.Bad; }
            preserved := try error_payload();
            if preserved != E.Bad { return E.Bad; }
            return none;
        }
        i32 main() { if exercise() != none { return 1; } return 42; }
    )",
           "shapes");
}

TEST(TryRuntimeTests, PeekObservesEnclosingScopeAndRewritesError) {
    RunTry(R"(
        error E { Original, Rewritten }
        error Other { Mapped }
        E fail() { return E.Original; }
        E succeed() { return none; }

        unsafe error observe(*i32 seen) {
            i32 x = 3;
            try fail() peek { *seen = x; };
            *seen = 99;
            return none;
        }

        E rewrite() {
            try fail() peek err { err = E.Rewritten; };
            return none;
        }

        error map() {
            try fail() peek err as error { err = Other.Mapped; };
            return none;
        }

        error route(bool handled) {
            try fail() peek err as error {
                if handled { return none; }
                err = Other.Mapped;
            };
            return none;
        }

        unsafe error skip_on_success(*i32 seen) {
            try succeed() peek { *seen = 99; };
            *seen += 1;
            return none;
        }

        i32 main() { unsafe {
            seen := 0;
            if observe(&raw seen) != E.Original or seen != 3 { return 1; }
            if rewrite() != E.Rewritten { return 2; }
            if map() != Other.Mapped { return 3; }
            seen = 0;
            if skip_on_success(&raw seen) != none or seen != 1 { return 4; }
            if route(true) != none { return 5; }
            if route(false) != Other.Mapped { return 6; }
            return 42;
        } }
    )",
           "peek");
}

TEST(TryRuntimeTests, EvaluatesOnceSkipsLaterOperandsAndRunsDefers) {
    RunTry(R"(
        error E { Bad }
        unsafe void add(*i32 count, i32 value) { *count += value; }
        unsafe (i32, E) step(*i32 count, bool fail) {
            *count += 1;
            if fail { return (99, E.Bad); }
            return (20, none);
        }
        i32 combine(i32 a, i32 b) { return a + b; }
        unsafe (i32, error) run(*i32 count, bool fail) {
            defer add(count, 10);
            value := combine(try step(count, fail), try step(count, false));
            return (value + 2, none);
        }
        i32 main() {
            unsafe {
                count := 0;
                value, e := run(&raw count, true);
                if e != E.Bad or value != 0 or count != 11 { return 1; }
                count = 0;
                value, e = run(&raw count, false);
                if e != none or value != 42 or count != 12 { return 2; }
                return 42;
            }
        }
    )",
           "order");
}

TEST(TryRuntimeTests,
     LoopsShortCircuitGenericsMethodsClosuresAndCrashableCalls) {
    RunTry(R"(
        error E { Bad }
        (T, error) identity<T>(T value) { return (value, none); }
        (i32, error) work!() { return (42, none); }
        structure S { i32 value; };
        (i32, error) S::read(&const self) { return (self->value, none); }
        (bool, error) fail() { return (false, E.Bad); }
        func Reader() => (i32, error);
        error exercise() {
            if false and try fail() { return E.Bad; }
            if true or try fail() {} else { return E.Bad; }
            n := 0;
            while n < 3 { n = try identity<i32>(n + 1); }
            Reader reader = []() => (i32, error) { return (42, none); };
            if try reader() != 42 { return E.Bad; }
            s := build S { self.value = 42; };
            if try s.read() != 42 { return E.Bad; }
            return none;
        }
        (i32, error) combined!() { return (try work!(), none); }
        i32 main!() {
            if exercise() != none { return 1; }
            v, e := combined!();
            if e != none { return 2; }
            return v;
        }
    )",
           "contexts");
}

const std::string owner = R"(
    error E { Bad }
    structure Token { *i32 tears; };
    unsafe Token Token::init(*i32 tears) override {
        return build Token { self.tears = tears; };
    }
    void Token::tear(&self) override { unsafe { if self->tears != null { (*self->tears)++; } } }
    (i32, E) number(bool fail) { if fail { return (100, E.Bad); } return (42, none); }
    unsafe (Token, E) token(*i32 tears, bool fail) {
        value := Token::init(tears);
        if fail { return (value, E.Bad); }
        return (value, none);
    }
    void consume(Token token, i32 value) {}
)";

TEST(TryRuntimeTests, CleansOwnedPayloadOnBothPathsAndPendingArguments) {
    RunTry(owner + R"(
        unsafe error payload(*i32 tears, bool fail) {
            value := try token(tears, fail);
            return none;
        }
        unsafe error pending(*i32 tears, bool fail) {
            value := Token::init(tears);
            consume(value, try number(fail));
            return none;
        }
        unsafe error deferred(*i32 tears) {
            value := Token::init(tears);
            defer consume(value, try number(true));
            return none;
        }
        i32 main() { unsafe {
            tears := 0;
            if payload(&raw tears, true) != E.Bad or tears != 1 { return 1; }
            if payload(&raw tears, false) != none or tears != 2 { return 2; }
            if pending(&raw tears, true) != E.Bad or tears != 3 { return 3; }
            if pending(&raw tears, false) != none or tears != 4 { return 4; }
            if deferred(&raw tears) != E.Bad or tears != 5 { return 5; }
            return 42;
        } }
    )",
           "owners");
}

TEST(TryRuntimeTests, CleansPendingOnceCallableAndDeferredStringSnapshot) {
    RunTry(owner + R"(
        unsafe once func Action(Token token, i32 n) => ();
        void text(stringv value, i32 n) {}
        unsafe error run(*i32 tears) {
            capture := Token::init(tears);
            Action action = unsafe once [own capture](Token token, i32 n) => () {};
            argument := Token::init(tears);
            action(argument, try number(true));
            return none;
        }
        error snapshot() {
            string value = "captured before failure";
            defer text(value.view(), try number(true));
            return none;
        }
        i32 main() { unsafe {
            tears := 0;
            if run(&raw tears) != E.Bad or tears != 2 { return 1; }
            if snapshot() != E.Bad { return 2; }
            return 42;
        } }
    )",
           "pending_callable");
}

TEST(TryRuntimeTests, CleansPartialReturnTupleAndArray) {
    RunTry(owner + R"(
        unsafe (Token, i32, error) partial(*i32 tears) {
            value := Token::init(tears);
            return (value, try number(true), none);
        }
        unsafe error array(*i32 tears) {
            first := Token::init(tears);
            values := [first, try token(tears, true)];
            return none;
        }
        i32 main() { unsafe {
            tears := 0;
            {
                value, n, e := partial(&raw tears);
                if n != 0 or e != E.Bad or tears != 1 { return 1; }
            }
            if tears != 1 { return 2; }
            if array(&raw tears) != E.Bad or tears != 3 { return 3; }
            return 42;
        } }
    )",
           "partial");
}

TEST(TryRuntimeTests, PropagatesRecoverableManagedAndRawAllocations) {
    RunTry(R"(
        error managed() {
            values := try new<own []i32>(4);
            values[0] = 42;
            return none;
        }
        unsafe error raw() {
            value := try new<i32>(42);
            defer free(value);
            return none;
        }
        unsafe error initializer() {
            value := try new<i32>(try broken());
            free(value);
            return none;
        }
        error E { Bad }
        (i32, error) broken() { return (5, E.Bad); }
        error exhausted() {
            values := try new<own []i32>(usize.max_value);
            return none;
        }
        i32 main() { unsafe {
            if managed() != none or raw() != none { return 1; }
            if initializer() != E.Bad { return 2; }
            if exhausted() == none { return 3; }
            return 42;
        } }
    )",
           "allocations");
}
TEST(TryRuntimeTests, MultipleOwnedResultsAndReturnedPayload) {
    RunTry(owner + R"(
        unsafe (Token, Token, error) pair(*i32 tears, bool fail) {
            first := Token::init(tears);
            second := Token::init(tears);
            if fail { return (first, second, E.Bad); }
            return (first, second, none);
        }
        unsafe error use(*i32 tears, bool fail) {
            first, second := try pair(tears, fail);
            return none;
        }
        ((i32, error), error) outer(bool fail) {
            if fail { return ((99, none), E.Bad); }
            return ((42, E.Bad), none);
        }
        (i32, error) forward(bool fail) { return try outer(fail); }
        i32 main() { unsafe {
            tears := 0;
            if use(&raw tears, true) != E.Bad or tears != 2 { return 1; }
            if use(&raw tears, false) != none or tears != 4 { return 2; }
            value, e := forward(true);
            if value != 0 or e != E.Bad { return 3; }
            value, e = forward(false);
            if value != 42 or e != E.Bad { return 4; }
            return 42;
        } }
    )",
           "multiple_owned");
}

TEST(TryRuntimeTests, InterfaceDispatchAndNestedClosurePropagation) {
    RunTry(R"(
        error E { Bad }
        interface Reader { (i32, error) read(&const self) virtual; };
        structure Source << Reader { i32 value; };
        (i32, error) Source::read(&const self) override { return (self->value, none); }
        (i32, error) read(&const Reader reader) { return (try reader->read(), none); }
        error fail() { return E.Bad; }
        func Check() => (error);
        i32 main() {
            source := build Source { self.value = 42; };
            value, e := read(&source);
            if value != 42 or e != none { return 1; }
            Check callback = []() => (error) { try fail(); return none; };
            if callback() != E.Bad { return 2; }
            return 42;
        }
    )",
           "dispatch");
}

TEST(TryRuntimeTests, FailureDefaultsUseSemanticEnumAndStructureDefaults) {
    RunTry(R"(
        error E { Bad }
        enum Choice: i32 { default Selected = 42, Other = 7 }
        structure Result { i32 number; };
        Result Result::zero() override { return build Result { self.number = 42; }; }
        error fail() { return E.Bad; }
        (Choice, Result, error) run() { try fail(); return (none, none, none); }
        i32 main() {
            choice, result, e := run();
            if choice != Choice.Selected or result.number != 42 or e != E.Bad { return 1; }
            return 42;
        }
    )",
           "defaults");
}
}  // namespace
