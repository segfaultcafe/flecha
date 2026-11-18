#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "lowering/lowerer.hpp"
#include "semantic/async_plan.hpp"
#include "semantic/semantic_analyzer.hpp"

namespace {
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
    const FunctionDeclaration& Function(
        const std::string& name = "main") const {
        for (const auto& declaration : unit->Declarations())
            if (declaration->Kind() == AstNodeKind::FunctionDeclaration) {
                const auto& function =
                    static_cast<const FunctionDeclaration&>(*declaration);
                if (function.Name() == name) return function;
            }
        throw std::runtime_error("missing test function");
    }
    const AsyncFramePlan* Plan(const std::string& name = "main") const {
        return model.AsyncPlanFor(Function(name));
    }
    void ExpectValid() const {
        for (const auto& diagnostic : diagnostics.Diagnostics())
            if (diagnostic.severity == DiagnosticSeverity::Error)
                ADD_FAILURE() << DiagnosticCodeName(diagnostic.code) << ": "
                              << diagnostic.message;
        ASSERT_NE(Plan(), nullptr);
        for (const auto& error : VerifyAsyncFramePlan(*Plan(), model))
            ADD_FAILURE() << error;
    }
};
AsyncSlotId Slot(const AsyncFramePlan& plan, const std::string& name) {
    for (AsyncSlotId id = 0; id < plan.slots.size(); ++id)
        if (plan.slots[id].symbol && plan.slots[id].symbol->Name() == name)
            return id;
    throw std::runtime_error("missing slot " + name);
}
const AsyncState& State(const AsyncFramePlan& plan, AsyncStateKind kind) {
    for (const auto& state : plan.states)
        if (state.kind == kind) return state;
    throw std::runtime_error("missing state");
}
bool Contains(const std::vector<AsyncSlotId>& slots, AsyncSlotId slot) {
    return std::ranges::find(slots, slot) != slots.end();
}
const std::string answer = "async i32 answer() { return 42; } ";

TEST(AsyncPlanTests,
     SemanticAnalysisPublishesVerifiedPlansForProductionLowering) {
    Analysis analysis(answer +
                      "async i32 main() { v, e := answer().await; if e != none "
                      "{ return 1; } return v; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    EXPECT_NE(analysis.Plan("answer"), nullptr);
    flecha::lowering::Lowerer lowerer;
    auto synchronous =
        lowerer.Lower(*analysis.unit, analysis.model, analysis.diagnostics);
    ASSERT_NE(synchronous, nullptr);
    EXPECT_TRUE(synchronous->Functions().empty());
}

TEST(AsyncPlanTests, BothShootSpellingsHaveSuccessFailureAndNoPendingCreation) {
    for (const std::string spelling : {"shoot", ">>-"}) {
        Analysis analysis(
            answer + "async i32 main() { child, ce := " + spelling +
            " answer(); if ce != none { return 1; } value, ae := child.await; "
            "if ae != none { return 2; } return value; }");
        analysis.ExpectValid();
        ASSERT_NE(analysis.Plan(), nullptr);
        const auto& plan = *analysis.Plan();
        const auto& create = State(plan, AsyncStateKind::CreateChild);
        ASSERT_EQ(create.edges.size(), 2);
        EXPECT_EQ(create.edges[0].kind, AsyncEdgeKind::Success);
        EXPECT_EQ(create.edges[1].kind, AsyncEdgeKind::Failure);
        const auto& join = State(plan, AsyncStateKind::ChildAwait);
        ASSERT_TRUE(join.storage);
        EXPECT_EQ(plan.slots[*join.storage].kind, AsyncSlotKind::ChildJoin);
        EXPECT_EQ(join.edges.size(), 3);
    }
}

TEST(AsyncPlanTests,
     CheckedValueIsAbsentOnFailureAndInitializedAtSuccessfulRead) {
    Analysis analysis(answer +
                      "async i32 main() { value, e := answer().await; if e == "
                      "none { return value; } return 0; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    const auto& plan = *analysis.Plan();
    const auto value = Slot(plan, "value");
    bool absent = false, read = false;
    for (const auto& state : plan.states) {
        if (state.initialization[value] == 1) absent = true;
        if (state.read == value) {
            read = true;
            EXPECT_EQ(state.initialization[value], 2);
        }
    }
    EXPECT_TRUE(absent);
    EXPECT_TRUE(read);
}

TEST(AsyncPlanTests, DirectRecursionUsesAnIndirectComputationSlot) {
    Analysis analysis(
        "async i32 main(i32 depth) { if depth == 0 { return 1; } n := depth - "
        "1; value, e := main(n).await; if e != none { return 0; } return "
        "value; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    const auto& plan = *analysis.Plan();
    const auto& await = State(plan, AsyncStateKind::DirectAwait);
    ASSERT_TRUE(await.storage);
    EXPECT_EQ(plan.slots[*await.storage].kind, AsyncSlotKind::Computation);
    EXPECT_EQ(std::ranges::count_if(plan.slots,
                                    [](const auto& slot) {
                                        return slot.kind ==
                                               AsyncSlotKind::Computation;
                                    }),
              1);
}

TEST(AsyncPlanTests, RetainsBorrowOriginWithNoFutureDirectRead) {
    Analysis analysis(answer +
                      "async i32 main() { i32 owner = 5; reference := &owner; "
                      "value, e := answer().await; if e != none { return 0; } "
                      "return *reference + value; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    const auto& plan = *analysis.Plan();
    const auto owner = Slot(plan, "owner");
    const auto reference = Slot(plan, "reference");
    const auto& await = State(plan, AsyncStateKind::DirectAwait);
    EXPECT_FALSE(Contains(await.live, owner));
    EXPECT_TRUE(Contains(await.retained, owner));
    EXPECT_TRUE(Contains(await.live, reference));
    EXPECT_TRUE(Contains(await.retained, reference));
}

TEST(AsyncPlanTests,
     BorrowedAggregateResultsRetainTheirOwnersAcrossAnotherAwait) {
    Analysis analysis(answer +
                      "async ([]const u8, error) view([]const u8 data) <- data "
                      "{ return (data, none); } "
                      "async u8 main() { [4]u8 buffer = none; pair, e := "
                      "view(buffer[:]).await; if e != none { return 0; } "
                      "bytes, ordinaryError := pair; v, e2 := answer().await; "
                      "if e2 != none { return 0; } return bytes[0]; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    const auto& plan = *analysis.Plan();
    const auto buffer = Slot(plan, "buffer");
    bool checked = false;
    for (const auto& state : plan.states)
        if (state.kind == AsyncStateKind::DirectAwait &&
            state.operation->Bindings()[0]->Name() == "v") {
            checked = true;
            EXPECT_TRUE(Contains(state.retained, buffer));
            EXPECT_FALSE(Contains(state.live, buffer));
        }
    EXPECT_TRUE(checked);
}

TEST(AsyncPlanTests, SavedDeferRetainsScalarArgumentsAcrossSuspension) {
    Analysis analysis(
        answer +
        "void cleanup(i32 value) {} async i32 main() { defer cleanup(7); v, e "
        ":= answer().await; if e != none { return 0; } return v; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    const auto& plan = *analysis.Plan();
    const auto& await = State(plan, AsyncStateKind::DirectAwait);
    bool found = false;
    for (AsyncSlotId id = 0; id < plan.slots.size(); ++id)
        if (plan.slots[id].kind == AsyncSlotKind::DeferredCall) {
            found = true;
            EXPECT_TRUE(Contains(await.retained, id));
            EXPECT_EQ(await.initialization[id], 2);
        }
    EXPECT_TRUE(found);
}

TEST(AsyncPlanTests, ReturnValueSurvivesInnerToOuterCancellationAndDraining) {
    Analysis analysis(answer +
                      "async i32 main() { outer, oe := shoot answer(); if oe "
                      "!= none { return 0; } "
                      "{ inner, ie := shoot answer(); if ie != none { return "
                      "1; } return 42; } }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    const auto& plan = *analysis.Plan();
    ASSERT_TRUE(plan.returnValue);
    bool nested = false;
    for (const auto& state : plan.states) {
        if (!state.flowNode || !plan.flow.nodes[*state.flowNode].memoryReturn ||
            state.scopes.size() != 2 ||
            !std::ranges::all_of(state.scopes, [&](auto scope) {
                return plan.scopes[scope].children.has_value();
            }))
            continue;
        nested = true;
        auto next = state.edges.at(0).target;
        for (auto scope = state.scopes.rbegin(); scope != state.scopes.rend();
             ++scope) {
            EXPECT_EQ(plan.states[next].kind, AsyncStateKind::CancelChildren);
            EXPECT_EQ(plan.states[next].cleanupScope, *scope);
            next = plan.states[next].edges.at(0).target;
            const auto& drain = plan.states[next];
            EXPECT_EQ(drain.kind, AsyncStateKind::DrainChildren);
            EXPECT_TRUE(Contains(drain.retained, *plan.returnValue));
            EXPECT_EQ(drain.initialization[*plan.returnValue], 2);
            next = drain.edges.at(0).target;
            EXPECT_EQ(plan.states[next].kind, AsyncStateKind::CleanupScope);
            next = plan.states[next].edges.at(0).target;
        }
        EXPECT_EQ(plan.states[next].kind, AsyncStateKind::Complete);
        EXPECT_EQ(plan.states[next].read, plan.returnValue);
    }
    EXPECT_TRUE(nested);
}

TEST(AsyncPlanTests, LoopActivationsResetValuesAndDoNotRetainExitedScopes) {
    Analysis analysis(
        answer +
        "async i32 main(bool again) { while again { v, e := answer().await; if "
        "e != none { proceed; } if v == 0 { break; } } x, e2 := "
        "answer().await; if e2 != none { return 0; } return x; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    const auto& plan = *analysis.Plan();
    const auto v = Slot(plan, "v");
    for (const auto& state : plan.states)
        if (state.kind == AsyncStateKind::DirectAwait) {
            if (state.operation->Bindings()[0]->Name() == "v")
                EXPECT_EQ(state.initialization[v], 1);
            else
                EXPECT_FALSE(Contains(state.retained, v));
        }
}

TEST(AsyncPlanTests, CreationFailurePreservesMoveOnlyArgumentInitialization) {
    Analysis analysis(
        "structure Owner { i32 value; }; void Owner::tear(&self) override {} "
        "async Owner consume(Owner value) { return value; } async i32 main() { "
        "Owner owner = none; "
        "task, ce := shoot consume(owner); if ce != none { return owner.value; "
        "} "
        "result, ae := task.await; if ae != none { return 0; } return "
        "result.value; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    const auto& plan = *analysis.Plan();
    const auto owner = Slot(plan, "owner");
    EXPECT_EQ(State(plan, AsyncStateKind::ChildAwait).initialization[owner], 4);
    bool failureRead = false;
    for (const auto& state : plan.states)
        if (state.read == owner && state.flowNode &&
            plan.flow.nodes[*state.flowNode].read &&
            plan.flow.nodes[*state.flowNode].read->Range().start.offset >
                State(plan, AsyncStateKind::CreateChild)
                    .operation->Range()
                    .end.offset) {
            failureRead = true;
            EXPECT_EQ(state.initialization[owner], 2);
        }
    EXPECT_TRUE(failureRead);
}

TEST(AsyncPlanTests, BorrowedOwningSliceCaptureRetainsOwnerUntilDrain) {
    Analysis analysis(
        "async i32 fill(&own []u8 data) { data->clear(); return 1; } "
        "async i32 main() { own []u8 owner = none; reference := &owner; "
        "child, ce := shoot fill(reference); if ce != none { return 0; } "
        "return 7; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    const auto& plan = *analysis.Plan();
    EXPECT_TRUE(Contains(State(plan, AsyncStateKind::DrainChildren).retained,
                         Slot(plan, "owner")));
}

TEST(AsyncPlanTests, InvalidAndUnresolvedProgramsDoNotPublishPlans) {
    for (const std::string body :
         {"v, e := answer().await; return v;", "child, ce := shoot answer();",
          "v, e := answer().await; if e == none { return 0; } return v;"}) {
        Analysis analysis(answer + "async i32 main() { " + body + " }");
        EXPECT_EQ(analysis.Plan(), nullptr);
    }
    Analysis sync("i32 main() { return 0; }");
    EXPECT_EQ(sync.Plan(), nullptr);
    Analysis noVoid(
        "async void f() {} async void main() { v, e := f().await; }");
    EXPECT_EQ(noVoid.Plan(), nullptr);
}

TEST(AsyncPlanTests, DumpsAreStableAcrossIndependentAnalysesAndRebuilds) {
    const auto source =
        answer +
        "async i32 main(bool again) { while again { t, ce := shoot answer(); "
        "if ce != none { break; } v, ae := t.await; } return 0; }";
    Analysis first(source), second(source);
    first.ExpectValid();
    second.ExpectValid();
    ASSERT_NE(first.Plan(), nullptr);
    ASSERT_NE(second.Plan(), nullptr);
    EXPECT_EQ(DumpAsyncFramePlan(*first.Plan()),
              DumpAsyncFramePlan(*second.Plan()));
    EXPECT_EQ(
        DumpAsyncFramePlan(*first.Plan()),
        DumpAsyncFramePlan(BuildAsyncFramePlan(first.Function(), first.model)));
}

TEST(AsyncPlanTests,
     VerifierRejectsCorruptContinuationsRetentionAndInitialization) {
    Analysis analysis(answer +
                      "async i32 main() { owner := 3; ref := &owner; t, ce := "
                      "shoot answer(); if ce != none { return 0; } v, e := "
                      "t.await; if e != none { return 0; } return *ref + v; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    for (int mutation = 0; mutation != 8; ++mutation) {
        auto plan = *analysis.Plan();
        if (mutation == 0)
            plan.states[plan.entry].edges[0].target = plan.states.size();
        if (mutation == 7) plan.flow.nodes[plan.flow.entry].successors.clear();
        if (mutation == 1)
            plan.slots[Slot(plan, "owner")].stableAddress = false;
        for (auto& state : plan.states) {
            if (mutation == 2 && state.kind == AsyncStateKind::ChildAwait)
                state.retained.clear();
            if (mutation == 3 && state.kind == AsyncStateKind::CreateChild)
                std::swap(state.edges[0].target, state.edges[1].target);
            if (mutation == 4 && state.kind == AsyncStateKind::DrainChildren)
                state.edges[0].target = plan.entry;
            if (mutation == 5 && state.read == Slot(plan, "v"))
                state.initialization[*state.read] = 1;
            if (mutation == 6 && state.flowNode &&
                plan.flow.nodes[*state.flowNode].memoryReturn)
                state.writes.clear();
        }
        EXPECT_FALSE(VerifyAsyncFramePlan(plan, analysis.model).empty())
            << mutation;
    }
}

TEST(AsyncPlanTests, DeferStackPrecedesGuardedOwnerDestructionAfterDrain) {
    Analysis analysis(
        answer +
        "structure Owner { i32 value; }; void Owner::tear(&self) override {} "
        "void cleanup(i32 n) {} async i32 main(bool again) { Owner owner = "
        "none; "
        "#repeat defer cleanup(7); if again { jumpto repeat; } "
        "child, ce := shoot answer(); if ce != none { return 0; } return 1; }");
    // A jump from an if body crosses scopes and is intentionally unsupported.
    EXPECT_EQ(analysis.Plan(), nullptr);
    Analysis valid(
        answer +
        "structure Owner { i32 value; }; void Owner::tear(&self) override {} "
        "void cleanup(i32 n) {} async i32 main() { Owner owner = none; "
        "defer cleanup(7); child, ce := shoot answer(); if ce != none { return "
        "0; } return 1; }");
    valid.ExpectValid();
    ASSERT_NE(valid.Plan(), nullptr);
    const auto& plan = *valid.Plan();
    const auto& drain = State(plan, AsyncStateKind::DrainChildren);
    const auto& defers = plan.states[drain.edges.front().target];
    EXPECT_EQ(defers.kind, AsyncStateKind::RunDefers);
    ASSERT_TRUE(defers.storage);
    EXPECT_EQ(plan.slots[*defers.storage].kind, AsyncSlotKind::DeferredStack);
    const auto& cleanup = plan.states[defers.edges.front().target];
    EXPECT_EQ(cleanup.kind, AsyncStateKind::CleanupScope);
    EXPECT_TRUE(Contains(cleanup.destroys, Slot(plan, "owner")));
    EXPECT_TRUE(plan.slots[Slot(plan, "owner")].guarded);
    Analysis repeating(
        "void cleanup(i32 n) {} async void main() { while true { #again defer "
        "cleanup(7); "
        "jumpto again; } }");
    repeating.ExpectValid();
    ASSERT_NE(repeating.Plan(), nullptr);
    EXPECT_EQ(std::ranges::count_if(repeating.Plan()->slots,
                                    [](const auto& slot) {
                                        return slot.kind ==
                                               AsyncSlotKind::DeferredStack;
                                    }),
              1);
}

TEST(AsyncPlanTests, CompletionTypesAndChildConsumptionAreExplicit) {
    Analysis analysis(
        "async (i32, error) f() { return (1, none); } "
        "async i32 main() { child, ce := shoot f(); if ce != none { return 0; "
        "} "
        "pair, ae := child.await; if ae != none { return 0; } v, ordinaryError "
        ":= pair; return v; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    const auto& plan = *analysis.Plan();
    const auto& create = State(plan, AsyncStateKind::CreateChild);
    const auto& await = State(plan, AsyncStateKind::ChildAwait);
    ASSERT_TRUE(create.checked);
    ASSERT_TRUE(await.checked);
    EXPECT_TRUE(create.checked->transactionalCreation);
    EXPECT_EQ(create.checked->callee->Name(), "f");
    EXPECT_EQ(await.checked->consumedChild, Slot(plan, "child"));
    EXPECT_EQ(plan.slots[*await.checked->value].type.Kind(),
              SemanticTypeKind::Tuple);
    EXPECT_EQ(plan.slots[*await.checked->value].type.Elements()[1].Kind(),
              SemanticTypeKind::ErrorValue);
    EXPECT_NE(await.checked->value, await.checked->error);
}

TEST(AsyncPlanTests, ValidBorrowingMethodsKeepResolvedReceiverOperands) {
    Analysis analysis(
        "structure Box { i32 value; }; "
        "async i32 Box::read(&const self) { return self->value; } "
        "async i32 main() { Box box = none; v, e := box.read().await; if e != "
        "none { return 0; } return v; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    const auto& plan = *analysis.Plan();
    const auto& await = State(plan, AsyncStateKind::DirectAwait);
    ASSERT_TRUE(await.checked);
    ASSERT_NE(await.checked->callee, nullptr);
    EXPECT_TRUE(await.checked->callee->HasReceiver());
    EXPECT_EQ(await.checked->arguments.size(), 1);
    EXPECT_TRUE(Contains(await.retained, Slot(plan, "box")));
}

TEST(AsyncPlanTests, VerifierRejectsAValueReadOnTheFailureContinuation) {
    Analysis analysis(answer +
                      "async i32 main() { v, e := answer().await; if e != none "
                      "{ return 0; } return v; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    auto plan = *analysis.Plan();
    for (auto& state : plan.states)
        if (state.kind == AsyncStateKind::CheckedFailure)
            state.read = Slot(plan, "v");
    const auto errors = VerifyAsyncFramePlan(plan, analysis.model);
    EXPECT_NE(std::ranges::find(errors, "read of absent or moved frame value"),
              errors.end());
}

TEST(AsyncPlanTests, LoopTransfersDrainOnlyTheExitedChildScope) {
    for (const std::string transfer : {"break", "proceed"}) {
        Analysis analysis(answer +
                          "async i32 main(bool again) { while again { child, "
                          "ce := shoot answer(); "
                          "if ce != none { return 0; } " +
                          transfer + "; } return 1; }");
        analysis.ExpectValid();
        ASSERT_NE(analysis.Plan(), nullptr);
        const auto& plan = *analysis.Plan();
        bool tested = false;
        for (const auto& state : plan.states) {
            if (!state.flowNode) continue;
            const auto* syntax = plan.flow.nodes[*state.flowNode].transfer;
            if (!syntax || (syntax->Kind() != AstNodeKind::BreakStatement &&
                            syntax->Kind() != AstNodeKind::ProceedStatement))
                continue;
            tested = true;
            const auto& cancel = plan.states[state.edges.front().target];
            EXPECT_EQ(cancel.kind, AsyncStateKind::CancelChildren);
            EXPECT_EQ(cancel.scopes.size(), 2);
            const auto& drain = plan.states[cancel.edges.front().target];
            EXPECT_EQ(drain.kind, AsyncStateKind::DrainChildren);
            const auto& cleanup = plan.states[drain.edges.front().target];
            EXPECT_EQ(cleanup.kind, AsyncStateKind::CleanupScope);
            const auto& resume = plan.states[cleanup.edges.front().target];
            EXPECT_TRUE(resume.flowNode.has_value());
        }
        EXPECT_TRUE(tested);
    }
}

TEST(AsyncPlanTests, OwnedReturnIsTransferredBeforeDrainingAndGuardedCleanup) {
    Analysis analysis(
        answer +
        "structure Owner { i32 value; }; void Owner::tear(&self) override {} "
        "async Owner make() { Owner owner = none; child, ce := shoot answer(); "
        "if ce != none { return owner; } return owner; } "
        "async i32 main() { result, e := make().await; if e != none { return "
        "0; } return result.value; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan("make"), nullptr);
    const auto& plan = *analysis.Plan("make");
    ASSERT_TRUE(plan.returnValue);
    EXPECT_TRUE(plan.slots[*plan.returnValue].type.OwnsStorage());
    const auto owner = Slot(plan, "owner");
    for (const auto& state : plan.states)
        if (state.kind == AsyncStateKind::DrainChildren) {
            EXPECT_EQ(state.initialization[owner], 4);
            EXPECT_EQ(state.initialization[*plan.returnValue], 2);
            EXPECT_TRUE(Contains(state.retained, *plan.returnValue));
        }
}

TEST(AsyncPlanTests, ConflictingBorrowedOwningSliceAccessPreventsPlanning) {
    Analysis analysis(
        "async i32 fill(&own []u8 data) { data->clear(); return 1; } "
        "async i32 main() { own []u8 owner = none; reference := &owner; "
        "child, ce := shoot fill(reference); if ce != none { return 0; } "
        "owner.clear(); return 7; }");
    EXPECT_EQ(analysis.Plan(), nullptr);
    EXPECT_TRUE(std::ranges::any_of(
        analysis.diagnostics.Diagnostics(), [](const auto& diagnostic) {
            return diagnostic.code == DiagnosticCode::BorrowConflict;
        }));
}
TEST(AsyncPlanTests, ForwardedFailureHasAnErrorTerminalAndNoCompletedValue) {
    Analysis analysis(answer + "async i32 main() { return answer().await?; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    const auto& plan = *analysis.Plan();
    ASSERT_TRUE(plan.executionError);
    ASSERT_TRUE(plan.returnValue);
    const auto& await = State(plan, AsyncStateKind::DirectAwait);
    ASSERT_NE(await.awaitOperation, nullptr);
    ASSERT_TRUE(await.checked);
    ASSERT_TRUE(await.checked->value);
    EXPECT_EQ(await.checked->error, *plan.executionError);
    const auto& failure = plan.states[await.edges[1].target];
    EXPECT_TRUE(failure.writes.size() == 1 &&
                failure.writes.front() == *plan.executionError);
    EXPECT_FALSE(Contains(failure.writes, *await.checked->value));
    const auto& failed = State(plan, AsyncStateKind::Failed);
    EXPECT_EQ(failed.read, plan.executionError);
    EXPECT_EQ(failed.initialization[*plan.returnValue], 1);
    EXPECT_EQ(failed.initialization[*plan.executionError], 2);
    const auto dump = DumpAsyncFramePlan(plan);
    EXPECT_NE(dump.find("execution-error"), std::string::npos);
    EXPECT_NE(dump.find("forward=1"), std::string::npos);
    EXPECT_NE(dump.find("failed"), std::string::npos);
    flecha::lowering::Lowerer lowerer;
    auto synchronous =
        lowerer.Lower(*analysis.unit, analysis.model, analysis.diagnostics);
    ASSERT_NE(synchronous, nullptr);
    EXPECT_TRUE(synchronous->Functions().empty());
}

TEST(AsyncPlanTests, VoidAwaitsNeverAllocateCompletedValueStorage) {
    for (const std::string body : {"err := work().await;", "work().await?;"}) {
        Analysis analysis("async void work() {} async void main() { " + body +
                          " }");
        analysis.ExpectValid();
        ASSERT_NE(analysis.Plan(), nullptr);
        const auto& plan = *analysis.Plan();
        const auto& await = State(plan, AsyncStateKind::DirectAwait);
        ASSERT_TRUE(await.checked);
        EXPECT_FALSE(await.checked->value);
        EXPECT_FALSE(plan.returnValue);
        EXPECT_FALSE(std::ranges::any_of(plan.slots, [](const auto& slot) {
            return slot.kind == AsyncSlotKind::AwaitValue;
        }));
        if (body.front() == 'e') {
            EXPECT_FALSE(plan.executionError);
            EXPECT_EQ(plan.slots[await.checked->error].symbol->Name(), "err");
        } else
            EXPECT_TRUE(plan.executionError);
        EXPECT_NE(DumpAsyncFramePlan(plan).find("value=void"),
                  std::string::npos);
    }
}

TEST(AsyncPlanTests, ForwardedFailureCancelsDrainsAndRunsDefersBeforeFailing) {
    Analysis analysis(
        answer +
        "void cleanup(i32 n) {} async i32 main() { "
        "child, ce := shoot answer(); if ce != none { return 0; } "
        "defer cleanup(1); { defer cleanup(2); value := answer().await?; } "
        "return child.await?; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    const auto& plan = *analysis.Plan();
    bool nested = false;
    for (const auto& exit : plan.flow.taskScopeExits) {
        if (!plan.flow.nodes[exit.source].failureExit ||
            exit.scopes.size() != 2)
            continue;
        nested = true;
        EXPECT_EQ(exit.kind, TaskScopeExitKind::CancelAndDrain);
        EXPECT_FALSE(exit.successor);
        auto id = plan.states[exit.source].edges.front().target;
        std::vector<AsyncStateKind> kinds;
        while (plan.states[id].kind != AsyncStateKind::Failed) {
            const auto& state = plan.states[id];
            kinds.push_back(state.kind);
            const auto edge = std::ranges::find_if(state.edges, [](auto edge) {
                return edge.kind != AsyncEdgeKind::Pending;
            });
            ASSERT_NE(edge, state.edges.end());
            id = edge->target;
            ASSERT_LT(kinds.size(), 20);
        }
        EXPECT_EQ(
            kinds,
            (std::vector<AsyncStateKind>{
                AsyncStateKind::RunDefers, AsyncStateKind::CleanupScope,
                AsyncStateKind::CancelChildren, AsyncStateKind::DrainChildren,
                AsyncStateKind::RunDefers, AsyncStateKind::CleanupScope}));
    }
    EXPECT_TRUE(nested);
}

TEST(AsyncPlanTests, ForwardedChildJoinConsumesOnceOnEitherOutcome) {
    for (const std::string shoot : {"shoot", ">>-"}) {
        Analysis analysis(
            answer + "async i32 main() { child, ce := " + shoot +
            " answer(); if ce != none { return 0; } return child.await?; }");
        analysis.ExpectValid();
        ASSERT_NE(analysis.Plan(), nullptr);
        const auto& plan = *analysis.Plan();
        const auto& await = State(plan, AsyncStateKind::ChildAwait);
        ASSERT_TRUE(await.checked);
        EXPECT_EQ(await.checked->consumedChild, Slot(plan, "child"));
        const auto& failed = State(plan, AsyncStateKind::Failed);
        EXPECT_EQ(failed.initialization[*plan.executionError], 2);
        EXPECT_TRUE(std::ranges::any_of(plan.states, [&](const auto& state) {
            return Contains(state.moves, Slot(plan, "child"));
        }));
    }
}

TEST(AsyncPlanTests,
     ForwardingInLoopsKeepsBorrowedOwnersAndErrorStorageStable) {
    Analysis analysis(
        "async []const u8 view([]const u8 bytes) <- bytes { return bytes; } "
        "async u8 main(bool again) { [8]u8 owner = none; while again { "
        "bytes := view(owner[:]).await?; if bytes[0] == 0 { break; } } return "
        "0; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    const auto& plan = *analysis.Plan();
    const auto& await = State(plan, AsyncStateKind::DirectAwait);
    EXPECT_TRUE(Contains(await.retained, Slot(plan, "owner")));
    EXPECT_TRUE(Contains(await.retained, *plan.executionError));
    ASSERT_TRUE(await.checked->value);
    EXPECT_EQ(plan.slots[*await.checked->value].type.Kind(),
              SemanticTypeKind::Slice);
}

TEST(AsyncPlanTests, VerifierRejectsForwardingCleanupAndValueCorruption) {
    Analysis analysis(answer + "async i32 main() { return answer().await?; }");
    analysis.ExpectValid();
    ASSERT_NE(analysis.Plan(), nullptr);
    for (int mutation = 0; mutation < 4; ++mutation) {
        auto plan = *analysis.Plan();
        for (auto& state : plan.states) {
            if (mutation == 0 && state.kind == AsyncStateKind::Failed)
                state.read = plan.returnValue;
            if (mutation == 1 && state.kind == AsyncStateKind::CheckedFailure)
                state.writes.clear();
            if (mutation == 2 && state.kind == AsyncStateKind::DirectAwait)
                state.checked->value.reset();
            if (mutation == 3 && state.kind == AsyncStateKind::CleanupScope)
                state.edges.clear();
        }
        EXPECT_FALSE(VerifyAsyncFramePlan(plan, analysis.model).empty());
    }
}

}  // namespace
