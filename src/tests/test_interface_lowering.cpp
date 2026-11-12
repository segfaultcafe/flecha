#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "lowering/interface_layout.hpp"
#include "lowering/interface_reference.hpp"
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

AnalyzedFixture AnalyzeFixtureFile(std::string_view name) {
    const std::filesystem::path path =
        std::filesystem::path(FLECHA_INTERFACE_FIXTURE_DIR) / name;
    std::ifstream stream(path);
    if (!stream)
        throw std::runtime_error("could not open interface fixture '" +
                                 path.string() + "'");
    return AnalyzeFixture(
        std::string(std::istreambuf_iterator<char>(stream), {}));
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

std::size_t CountPlans(const BoundProgram& program, std::string_view concrete) {
    std::size_t result = 0;
    for (const InterfaceTablePlan& plan : program.InterfaceTables())
        if (plan.providerMap.concrete.DisplayName() == concrete) ++result;
    return result;
}

const InterfaceType& FindInterface(const SemanticModel& model,
                                   std::string_view name) {
    for (const std::unique_ptr<InterfaceType>& interfaceType :
         model.Interfaces())
        if (interfaceType->displayName == name) return *interfaceType;
    throw std::logic_error("interface not found");
}

const BoundFunction& FindFunction(const BoundProgram& program,
                                  std::string_view name) {
    for (const std::unique_ptr<BoundFunction>& function : program.Functions())
        if (function->Symbol().Name() == name) return *function;
    throw std::logic_error("bound function not found");
}

void CollectSlots(const InterfaceTableLayout& layout,
                  std::vector<const SlotLayout*>& result) {
    for (const ParentLayout& parent : layout.parents)
        CollectSlots(*parent.table, result);
    for (const SlotLayout& slot : layout.slots) result.push_back(&slot);
}

const InterfaceTableLayout& FindSubtable(const InterfaceTableLayout& layout,
                                         std::string_view name) {
    if (layout.interfaceType->displayName == name) return layout;
    for (const ParentLayout& parent : layout.parents) {
        try {
            return FindSubtable(*parent.table, name);
        } catch (const std::logic_error&) {
        }
    }
    throw std::logic_error("interface subtable not found");
}

}  // namespace

TEST(InterfaceCastTests, AcceptsTheStaticCastAndUpcastMatrix) {
    AnalyzedFixture fixture = AnalyzeFixtureFile("valid_casts_upcasts.fl");

    ASSERT_FALSE(fixture.diagnostics.HasErrors());
    std::unique_ptr<BoundProgram> program = LowerFixture(fixture);
    ASSERT_NE(program, nullptr);
    EXPECT_EQ(CountPlans(*program, "EmptyOnly"), 1);
    EXPECT_EQ(CountPlans(*program, "MultiConcrete"), 3);
    EXPECT_EQ(CountPlans(*program, "CastMode"), 1);
    EXPECT_EQ(CountPlans(*program, "Box<i32>"), 1);
    EXPECT_EQ(CountPlans(*program, "Box<string>"), 1);
}

TEST(InterfaceCastTests, RejectsEveryCastThatWouldNeedDynamicKnowledge) {
    AnalyzedFixture fixture = AnalyzeFixtureFile("invalid_casts_upcasts.fl");

    std::vector<std::string> messages;
    for (const Diagnostic& diagnostic : fixture.diagnostics.Diagnostics())
        if (diagnostic.code == DiagnosticCode::InvalidCast)
            messages.push_back(diagnostic.message);
    ASSERT_EQ(messages.size(), 6);
    EXPECT_NE(messages[0].find("Unrelated"), std::string::npos);
    EXPECT_NE(messages[1].find("&Child"), std::string::npos);
    EXPECT_NE(messages[1].find("&Sibling"), std::string::npos);
    EXPECT_NE(messages[2].find("&Root"), std::string::npos);
    EXPECT_NE(messages[2].find("&Child"), std::string::npos);
    EXPECT_NE(messages[4].find("GenericConcrete<i32>"), std::string::npos);
    EXPECT_NE(messages[5].find("requires an addressable value"),
              std::string::npos);
    EXPECT_EQ(LowerFixture(fixture), nullptr);
}

TEST(InterfaceCastTests, KeepsDeclaredRootAndNestedSubtableOffsets) {
    AnalyzedFixture fixture = AnalyzeFixtureFile("valid_casts_upcasts.fl");

    ASSERT_FALSE(fixture.diagnostics.HasErrors());
    std::unique_ptr<BoundProgram> program = LowerFixture(fixture);
    ASSERT_NE(program, nullptr);
    const InterfaceTableLayout& root =
        FindPlan(*program, "MultiConcrete", "Root").layout;
    ASSERT_EQ(root.parents.size(), 3);
    EXPECT_EQ(root.parents[0].interfaceType->displayName, "EmptyBranch");
    EXPECT_EQ(root.parents[0].offset, 0);
    EXPECT_EQ(root.parents[1].interfaceType->displayName, "Prefix");
    EXPECT_EQ(root.parents[1].offset, 0);
    EXPECT_EQ(root.parents[2].interfaceType->displayName, "Parent");
    EXPECT_EQ(root.parents[2].offset, sizeof(void*));

    const InterfaceTableLayout& parent = FindSubtable(root, "Parent");
    ASSERT_EQ(parent.parents.size(), 2);
    EXPECT_EQ(parent.parents[0].interfaceType->displayName, "Pad");
    EXPECT_EQ(parent.parents[0].offset, 0);
    EXPECT_EQ(parent.parents[1].interfaceType->displayName, "Grand");
    EXPECT_EQ(parent.parents[1].offset, sizeof(void*));
    EXPECT_EQ(FindSubtable(root, "Grand").size, sizeof(void*));
}

TEST(InterfaceCastTests, LeavesShiftLeftExpressionSemanticsUnchanged) {
    AnalyzedFixture fixture =
        AnalyzeFixture("i32 shifted(i32 value) { return value << 3; }");
    EXPECT_FALSE(fixture.diagnostics.HasErrors());
}

TEST(InterfaceLayoutTests, PlacesParentsBeforeLocalSlotsInDeclarationOrder) {
    AnalyzedFixture fixture = AnalyzeFixtureFile("valid_lowering_layouts.fl");

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
    AnalyzedFixture fixture = AnalyzeFixtureFile("valid_lowering_layouts.fl");

    ASSERT_FALSE(fixture.diagnostics.HasErrors());
    std::unique_ptr<BoundProgram> program = LowerFixture(fixture);
    ASSERT_NE(program, nullptr);
    const InterfaceTableLayout& layout =
        FindPlan(*program, "EmptyConcrete", "Empty").layout;
    EXPECT_EQ(layout.size, 0);
    EXPECT_EQ(layout.alignment, 1);
    EXPECT_TRUE(layout.parents.empty());
    EXPECT_TRUE(layout.slots.empty());
}

TEST(InterfaceLayoutTests, ReusesOneSlotAcrossVirtualDefaultAndFinal) {
    AnalyzedFixture fixture = AnalyzeFixtureFile("valid_lowering_layouts.fl");

    ASSERT_FALSE(fixture.diagnostics.HasErrors());
    std::unique_ptr<BoundProgram> program = LowerFixture(fixture);
    ASSERT_NE(program, nullptr);
    const InterfaceTablePlan& plan =
        FindPlan(*program, "VerticalConcrete", "Sealed");
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
    AnalyzedFixture fixture = AnalyzeFixtureFile("valid_lowering_layouts.fl");

    ASSERT_FALSE(fixture.diagnostics.HasErrors());
    std::unique_ptr<BoundProgram> program = LowerFixture(fixture);
    ASSERT_NE(program, nullptr);
    const InterfaceTablePlan& plan =
        FindPlan(*program, "AdjustedConcrete", "Provided");
    std::vector<const SlotLayout*> slots;
    CollectSlots(plan.layout, slots);
    ASSERT_EQ(slots.size(), 2);
    const SlotLayout* run =
        slots[0]->slot.origin->key.name == "adjusted" ? slots[0] : slots[1];
    const EffectiveInterfaceProvider* provider =
        plan.providerMap.Find(*run->slot.origin);
    ASSERT_NE(provider, nullptr);
    EXPECT_EQ(provider->kind, InterfaceProviderKind::AdjustmentThunk);
    EXPECT_EQ(provider->tableAdjustment,
              -static_cast<std::ptrdiff_t>(sizeof(void*)));
    EXPECT_EQ(provider->function->InterfaceOwner()->displayName, "Provided");
}

TEST(InterfaceProviderTests, PrefersAnExactConcreteProvider) {
    AnalyzedFixture fixture = AnalyzeFixtureFile("valid_lowering_layouts.fl");

    ASSERT_FALSE(fixture.diagnostics.HasErrors());
    std::unique_ptr<BoundProgram> program = LowerFixture(fixture);
    ASSERT_NE(program, nullptr);
    const InterfaceProviderMap& providers =
        FindPlan(*program, "ConcreteOverride", "Runnable").providerMap;
    ASSERT_EQ(providers.providers.size(), 1);
    EXPECT_EQ(providers.providers[0].kind,
              InterfaceProviderKind::ConcreteThunk);
    EXPECT_EQ(providers.providers[0].function->Owner()->displayName,
              "ConcreteOverride");
}

TEST(InterfaceProviderTests, BuildsIndependentPlansForMultipleRoots) {
    AnalyzedFixture fixture = AnalyzeFixtureFile("valid_lowering_layouts.fl");

    ASSERT_FALSE(fixture.diagnostics.HasErrors());
    std::unique_ptr<BoundProgram> program = LowerFixture(fixture);
    ASSERT_NE(program, nullptr);
    ASSERT_EQ(CountPlans(*program, "MultiRootConcrete"), 3);
    const InterfaceTablePlan& supplied =
        FindPlan(*program, "MultiRootConcrete", "Supplied");
    const InterfaceTablePlan& required =
        FindPlan(*program, "MultiRootConcrete", "Required");
    EXPECT_EQ(supplied.layout.size, sizeof(void*));
    EXPECT_EQ(required.layout.size, sizeof(void*));
    EXPECT_EQ(supplied.providerMap.providers[0].kind,
              InterfaceProviderKind::InterfaceBody);
    EXPECT_EQ(required.providerMap.providers[0].kind,
              InterfaceProviderKind::ConcreteThunk);
}

TEST(InterfaceProviderTests, TreatsUnresolvedVirtualAsASemanticInvariant) {
    AnalyzedFixture fixture =
        AnalyzeFixtureFile("invalid_lowering_provider.fl");

    ASSERT_TRUE(fixture.diagnostics.HasErrors());
    ASSERT_EQ(fixture.model.Structures().size(), 1);
    const InterfaceType& required =
        FindInterface(fixture.model, "RequiredProvider");
    InterfaceLoweringPlanner planner;
    EXPECT_THROW(
        {
            try {
                (void)planner.BuildPlan(*fixture.model.Structures()[0],
                                        required);
            } catch (const std::logic_error& error) {
                EXPECT_NE(std::string(error.what())
                              .find("unresolved virtual method 'provide'"),
                          std::string::npos);
                throw;
            }
        },
        std::logic_error);
}

TEST(InterfaceLayoutTests, RejectsInvalidCompositionAsAnInternalInvariant) {
    AnalyzedFixture fixture = AnalyzeFixtureFile("invalid_hierarchy.fl");

    ASSERT_TRUE(fixture.diagnostics.HasErrors());
    InterfaceLoweringPlanner planner;
    EXPECT_THROW(
        (void)planner.BuildLayout(FindInterface(fixture.model, "Diamond")),
        std::logic_error);
}

TEST(InterfaceProviderTests, PlansEachClosedGenericSpecialization) {
    AnalyzedFixture fixture = AnalyzeFixtureFile("valid_lowering_layouts.fl");

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

TEST(InterfaceProviderTests, PlansEnumProviders) {
    AnalyzedFixture fixture = AnalyzeFixtureFile("valid_lowering_layouts.fl");

    ASSERT_FALSE(fixture.diagnostics.HasErrors());
    std::unique_ptr<BoundProgram> program = LowerFixture(fixture);
    ASSERT_NE(program, nullptr);
    const InterfaceTablePlan& plan = FindPlan(*program, "Mode", "Resettable");
    EXPECT_EQ(plan.providerMap.concrete.kind, ConcreteTypeKind::Enum);
    ASSERT_EQ(plan.providerMap.providers.size(), 1);
    EXPECT_EQ(plan.providerMap.providers[0].kind,
              InterfaceProviderKind::ConcreteThunk);
    ASSERT_NE(plan.providerMap.providers[0].function->EnumOwner(), nullptr);
    EXPECT_EQ(plan.providerMap.providers[0].function->EnumOwner()->displayName,
              "Mode");
}

TEST(InterfaceReferenceTests, PreservesBorrowTypesInBoundFunctionsAndFields) {
    AnalyzedFixture fixture =
        AnalyzeFixtureFile("valid_interface_references.fl");

    ASSERT_FALSE(fixture.diagnostics.HasErrors());
    std::unique_ptr<BoundProgram> program = LowerFixture(fixture);
    ASSERT_NE(program, nullptr);

    const BoundFunction* mutableRelay = nullptr;
    const BoundFunction* constRelay = nullptr;
    for (const std::unique_ptr<BoundFunction>& function :
         program->Functions()) {
        if (function->Symbol().Name() == "relayMutableReference")
            mutableRelay = function.get();
        if (function->Symbol().Name() == "relayConstReference")
            constRelay = function.get();
    }
    ASSERT_NE(mutableRelay, nullptr);
    ASSERT_NE(constRelay, nullptr);
    ASSERT_EQ(mutableRelay->Symbol().ParameterTypes().size(), 1);
    ASSERT_EQ(constRelay->Symbol().ParameterTypes().size(), 1);

    const SemanticType mutableType = mutableRelay->Symbol().ReturnType();
    const SemanticType constType = constRelay->Symbol().ReturnType();
    EXPECT_TRUE(IsInterfaceReference(mutableType));
    EXPECT_TRUE(IsInterfaceReference(constType));
    EXPECT_FALSE(mutableType.Target()->IsConst());
    EXPECT_TRUE(constType.Target()->IsConst());
    EXPECT_EQ(mutableRelay->Symbol().ParameterTypes()[0], mutableType);
    EXPECT_EQ(constRelay->Symbol().ParameterTypes()[0], constType);
    EXPECT_TRUE(mutableType.HasBorrowedLifetime());
    EXPECT_TRUE(constType.HasBorrowedLifetime());
    EXPECT_FALSE(mutableType.OwnsStorage());
    EXPECT_FALSE(constType.OwnsStorage());

    const StructureType* fields = nullptr;
    for (const std::unique_ptr<StructureType>& structure :
         fixture.model.Structures())
        if (structure->displayName == "InterfaceReferenceFields")
            fields = structure.get();
    ASSERT_NE(fields, nullptr);
    ASSERT_EQ(fields->fields.size(), 2);
    EXPECT_TRUE(IsInterfaceReference(fields->fields[0].type));
    EXPECT_TRUE(IsInterfaceReference(fields->fields[1].type));

    EXPECT_EQ(CountPlans(*program, "EmptyReferenceStructure"), 1);
    EXPECT_EQ(CountPlans(*program, "EmptyReferenceEnum"), 1);
    EXPECT_EQ(CountPlans(*program, "ReadableStructure"), 1);
    EXPECT_EQ(CountPlans(*program, "ReadableEnum"), 1);
}

TEST(InterfaceDispatchTests, PreservesResolvedContractsInDedicatedBoundCalls) {
    AnalyzedFixture fixture = AnalyzeFixtureFile("valid_dynamic_dispatch.fl");

    ASSERT_FALSE(fixture.diagnostics.HasErrors());
    std::unique_ptr<BoundProgram> program = LowerFixture(fixture);
    ASSERT_NE(program, nullptr);

    const BoundFunction& function = FindFunction(*program, "callRootStep");
    ASSERT_EQ(function.Body().Statements().size(), 1);
    const auto& returned = static_cast<const BoundReturnStatement&>(
        *function.Body().Statements().front());
    ASSERT_NE(returned.Expression(), nullptr);
    ASSERT_EQ(returned.Expression()->Kind(),
              BoundNodeKind::InterfaceCallExpression);
    const auto& call = static_cast<const BoundInterfaceCallExpression&>(
        *returned.Expression());
    EXPECT_EQ(call.Contract().key.name, "step");
    ASSERT_NE(call.Contract().declaredBy, nullptr);
    EXPECT_EQ(call.Contract().declaredBy->displayName, "Counter");
    ASSERT_EQ(call.Arguments().size(), 1);
    EXPECT_TRUE(IsInterfaceReference(call.Receiver().Type()));
    EXPECT_EQ(call.Receiver().Type().Target()->Interface()->displayName,
              "Root");
}
