#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "lowering/interface_layout.hpp"
#include "lowering/lowerer.hpp"
#include "semantic/semantic_analyzer.hpp"

using namespace flecha::core;
using namespace flecha::diagnostics;
using namespace flecha::lowering;
using namespace flecha::semantic;

namespace {

struct AnalyzedFixture final {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit;
    SemanticModel model;
};

AnalyzedFixture AnalyzeFixture(const std::string& source) {
    AnalyzedFixture result;
    Tokenizer tokenizer(source, result.diagnostics);
    Parser parser(tokenizer.Tokenize(), result.diagnostics);
    result.unit = parser.Parse();
    SemanticAnalyzer analyzer(result.diagnostics);
    result.model = analyzer.Analyze(*result.unit);
    return result;
}

std::unique_ptr<BoundProgram> LowerFixture(AnalyzedFixture& fixture) {
    Lowerer lowerer;
    return lowerer.Lower(*fixture.unit, fixture.model, fixture.diagnostics);
}

const InterfaceTablePlan& FindPlan(const BoundProgram& program,
                                   std::string_view concrete,
                                   std::string_view root) {
    for (const InterfaceTablePlan& plan : program.InterfaceTables())
        if (plan.providerMap.concrete.DisplayName() == concrete &&
            plan.providerMap.root->displayName == root)
            return plan;
    throw std::logic_error("interface table plan not found");
}

const InterfaceType& FindInterface(const SemanticModel& model,
                                   std::string_view name) {
    for (const std::unique_ptr<InterfaceType>& interfaceType :
         model.Interfaces())
        if (interfaceType->displayName == name) return *interfaceType;
    throw std::logic_error("interface not found");
}

void CollectSlots(const InterfaceTableLayout& layout,
                  std::vector<const SlotLayout*>& result) {
    for (const ParentLayout& parent : layout.parents)
        CollectSlots(*parent.table, result);
    for (const SlotLayout& slot : layout.slots) result.push_back(&slot);
}

}  // namespace

TEST(InterfaceLayoutTests, PlacesParentsBeforeLocalSlotsInDeclarationOrder) {
    AnalyzedFixture fixture = AnalyzeFixture(R"(
        interface FirstParent {
            error first(&self) default;
        };
        error FirstParent::first(&self) { return none; }

        interface SecondParent {
            error second(&self) default;
        };
        error SecondParent::second(&self) { return none; }

        interface Root << FirstParent, SecondParent {
            error localFirst(&self) default;
            error localSecond(&self) final;
        };
        error Root::localFirst(&self) { return none; }
        error Root::localSecond(&self) { return none; }

        structure Concrete << Root {};
    )");

    ASSERT_FALSE(fixture.diagnostics.HasErrors());
    InterfaceLoweringPlanner planner({4, 4});
    const InterfaceTableLayout layout =
        planner.BuildLayout(FindInterface(fixture.model, "Root"));

    ASSERT_EQ(layout.parents.size(), 2);
    EXPECT_EQ(layout.parents[0].interfaceType->displayName, "FirstParent");
    EXPECT_EQ(layout.parents[0].offset, 0);
    EXPECT_EQ(layout.parents[1].interfaceType->displayName, "SecondParent");
    EXPECT_EQ(layout.parents[1].offset, 4);
    ASSERT_EQ(layout.slots.size(), 2);
    EXPECT_EQ(layout.slots[0].slot.origin->key.name, "localFirst");
    EXPECT_EQ(layout.slots[0].offset, 8);
    EXPECT_EQ(layout.slots[1].slot.origin->key.name, "localSecond");
    EXPECT_EQ(layout.slots[1].offset, 12);
    EXPECT_EQ(layout.size, 16);
    EXPECT_EQ(layout.alignment, 4);
}

TEST(InterfaceLayoutTests, KeepsAReallyEmptyInterfaceAtZeroSize) {
    AnalyzedFixture fixture = AnalyzeFixture(R"(
        interface Empty {};
        structure Concrete << Empty {};
    )");

    ASSERT_FALSE(fixture.diagnostics.HasErrors());
    std::unique_ptr<BoundProgram> program = LowerFixture(fixture);
    ASSERT_NE(program, nullptr);
    const InterfaceTableLayout& layout =
        FindPlan(*program, "Concrete", "Empty").layout;
    EXPECT_EQ(layout.size, 0);
    EXPECT_EQ(layout.alignment, 1);
    EXPECT_TRUE(layout.parents.empty());
    EXPECT_TRUE(layout.slots.empty());
}

TEST(InterfaceLayoutTests, ReusesOneSlotAcrossVirtualDefaultAndFinal) {
    AnalyzedFixture fixture = AnalyzeFixture(R"(
        interface Requirement {
            error run(&self) virtual;
        };
        interface Defaulted << Requirement {
            error run(&self) default;
        };
        error Defaulted::run(&self) override { return none; }
        interface Sealed << Defaulted {
            error run(&self) final;
        };
        error Sealed::run(&self) override { return none; }
        structure Concrete << Sealed {};
    )");

    ASSERT_FALSE(fixture.diagnostics.HasErrors());
    std::unique_ptr<BoundProgram> program = LowerFixture(fixture);
    ASSERT_NE(program, nullptr);
    const InterfaceTablePlan& plan = FindPlan(*program, "Concrete", "Sealed");
    std::vector<const SlotLayout*> slots;
    CollectSlots(plan.layout, slots);
    ASSERT_EQ(slots.size(), 1);
    EXPECT_EQ(slots[0]->slot.origin->declaredBy->displayName, "Requirement");
    EXPECT_EQ(slots[0]->slot.effective->declaredBy->displayName, "Sealed");
    EXPECT_EQ(slots[0]->slot.effective->category, MethodCategory::Final);
    ASSERT_EQ(plan.providerMap.providers.size(), 1);
    EXPECT_EQ(plan.providerMap.providers[0].kind,
              InterfaceProviderKind::InterfaceBody);
}

TEST(InterfaceProviderTests, RecordsAdjustmentForADeeperDefaultProvider) {
    AnalyzedFixture fixture = AnalyzeFixture(R"(
        interface Prefix {
            error prefix(&self) default;
        };
        error Prefix::prefix(&self) { return none; }
        interface Requirement {
            error run(&self) virtual;
        };
        interface Provided << Prefix, Requirement {
            error run(&self) default;
        };
        error Provided::run(&self) override { return none; }
        structure Concrete << Provided {};
    )");

    ASSERT_FALSE(fixture.diagnostics.HasErrors());
    std::unique_ptr<BoundProgram> program = LowerFixture(fixture);
    ASSERT_NE(program, nullptr);
    const InterfaceTablePlan& plan = FindPlan(*program, "Concrete", "Provided");
    std::vector<const SlotLayout*> slots;
    CollectSlots(plan.layout, slots);
    ASSERT_EQ(slots.size(), 2);
    const SlotLayout* run =
        slots[0]->slot.origin->key.name == "run" ? slots[0] : slots[1];
    const EffectiveInterfaceProvider* provider =
        plan.providerMap.Find(*run->slot.origin);
    ASSERT_NE(provider, nullptr);
    EXPECT_EQ(provider->kind, InterfaceProviderKind::AdjustmentThunk);
    EXPECT_EQ(provider->tableAdjustment,
              -static_cast<std::ptrdiff_t>(sizeof(void*)));
    EXPECT_EQ(provider->function->InterfaceOwner()->displayName, "Provided");
}

TEST(InterfaceProviderTests, PrefersAnExactConcreteProvider) {
    AnalyzedFixture fixture = AnalyzeFixture(R"(
        interface Runnable {
            error run(&self) default;
        };
        error Runnable::run(&self) { return none; }
        structure Concrete << Runnable {};
        error Concrete::run(&self) override { return none; }
    )");

    ASSERT_FALSE(fixture.diagnostics.HasErrors());
    std::unique_ptr<BoundProgram> program = LowerFixture(fixture);
    ASSERT_NE(program, nullptr);
    const InterfaceProviderMap& providers =
        FindPlan(*program, "Concrete", "Runnable").providerMap;
    ASSERT_EQ(providers.providers.size(), 1);
    EXPECT_EQ(providers.providers[0].kind,
              InterfaceProviderKind::ConcreteThunk);
    EXPECT_EQ(providers.providers[0].function->Owner()->displayName,
              "Concrete");
}

TEST(InterfaceProviderTests, BuildsIndependentPlansForMultipleRoots) {
    AnalyzedFixture fixture = AnalyzeFixture(R"(
        interface Supplied {
            error supplied(&self) default;
        };
        error Supplied::supplied(&self) { return none; }
        interface Required {
            error required(&self) virtual;
        };
        structure Concrete << Supplied, Required {};
        error Concrete::required(&self) override { return none; }
    )");

    ASSERT_FALSE(fixture.diagnostics.HasErrors());
    std::unique_ptr<BoundProgram> program = LowerFixture(fixture);
    ASSERT_NE(program, nullptr);
    ASSERT_EQ(program->InterfaceTables().size(), 2);
    const InterfaceTablePlan& supplied =
        FindPlan(*program, "Concrete", "Supplied");
    const InterfaceTablePlan& required =
        FindPlan(*program, "Concrete", "Required");
    EXPECT_EQ(supplied.layout.size, sizeof(void*));
    EXPECT_EQ(required.layout.size, sizeof(void*));
    EXPECT_EQ(supplied.providerMap.providers[0].kind,
              InterfaceProviderKind::InterfaceBody);
    EXPECT_EQ(required.providerMap.providers[0].kind,
              InterfaceProviderKind::ConcreteThunk);
}

TEST(InterfaceProviderTests, TreatsUnresolvedVirtualAsASemanticInvariant) {
    AnalyzedFixture fixture = AnalyzeFixture(R"(
        interface Required {
            error run(&self) virtual;
        };
        structure Missing << Required {};
    )");

    ASSERT_TRUE(fixture.diagnostics.HasErrors());
    ASSERT_EQ(fixture.model.Structures().size(), 1);
    const InterfaceType& required = FindInterface(fixture.model, "Required");
    InterfaceLoweringPlanner planner;
    EXPECT_THROW(
        {
            try {
                (void)planner.BuildPlan(*fixture.model.Structures()[0],
                                        required);
            } catch (const std::logic_error& error) {
                EXPECT_NE(std::string(error.what())
                              .find("unresolved virtual method 'run'"),
                          std::string::npos);
                throw;
            }
        },
        std::logic_error);
}

TEST(InterfaceProviderTests, PlansEachClosedGenericSpecialization) {
    AnalyzedFixture fixture = AnalyzeFixture(R"(
        interface Inspectable {
            error inspect(&const self) virtual;
        };
        structure Box<T> << Inspectable {
            T value;
        };
        error Box<T>::inspect(&const self) override { return none; }
        void instantiate(Box<i32> value) {}
    )");

    ASSERT_FALSE(fixture.diagnostics.HasErrors());
    std::unique_ptr<BoundProgram> program = LowerFixture(fixture);
    ASSERT_NE(program, nullptr);
    const InterfaceTablePlan& plan =
        FindPlan(*program, "Box<i32>", "Inspectable");
    ASSERT_EQ(plan.providerMap.providers.size(), 1);
    EXPECT_EQ(plan.providerMap.providers[0].kind,
              InterfaceProviderKind::ConcreteThunk);
    EXPECT_EQ(plan.providerMap.providers[0].function->Owner()->displayName,
              "Box<i32>");
}
