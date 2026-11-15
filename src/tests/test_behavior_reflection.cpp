#include <gtest/gtest.h>
#include <llvm/Support/raw_ostream.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "analyzer/analysis_session.hpp"
#include "codegen/llvm_code_generator.hpp"
#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "lowering/lowerer.hpp"
#include "semantic/semantic_analyzer.hpp"
namespace {
using namespace flecha;
struct Result {
    diagnostics::DiagnosticBag diagnostics;
    std::unique_ptr<core::CompilationUnit> unit;
    semantic::SemanticModel model;
};
std::string Messages(const diagnostics::DiagnosticBag& bag) {
    std::string out;
    for (auto& d : bag.Diagnostics()) out += d.message + "\n";
    return out;
}
std::string Fixture(const std::string& name) {
    std::ifstream file(std::filesystem::path(FLECHA_BEHAVIOR_FIXTURE_DIR) /
                       "reflection" / name);
    return {std::istreambuf_iterator<char>(file), {}};
}
Result Analyze(std::string source) {
    Result r;
    core::Tokenizer tokenizer(source, r.diagnostics);
    core::Parser parser(tokenizer.Tokenize(), r.diagnostics);
    r.unit = parser.Parse();
    semantic::SemanticAnalyzer analyzer(r.diagnostics);
    r.model = analyzer.Analyze(*r.unit);
    return r;
}
TEST(BehaviorReflection, FieldIdentityOrderMetadataAndDetachedBodies) {
    auto r = Analyze(Fixture("fields.fl"));
    ASSERT_FALSE(r.diagnostics.HasErrors()) << Messages(r.diagnostics);
    ASSERT_EQ(r.model.BehaviorPlans().size(), 3u);
    auto& plan = r.model.BehaviorPlans()[0];
    ASSERT_TRUE(plan.valid);
    ASSERT_EQ(plan.pack.fields.size(), 3u);
    EXPECT_EQ(plan.pack.fields[0].field->name, "first");
    EXPECT_EQ(plan.pack.fields[1].field->name, "second");
    EXPECT_EQ(plan.pack.fields[2].field->name, "third");
    EXPECT_NE(plan.pack.fields[0].field, plan.pack.fields[1].field);
    EXPECT_EQ(plan.pack.fields[0].type, plan.pack.fields[1].type);
    ASSERT_TRUE(plan.pack.fields[0].metadata);
    EXPECT_EQ(std::get<std::uint64_t>(
                  plan.pack.fields[0].metadata->members[1].value.value),
              3u);
    EXPECT_FALSE(plan.pack.fields[1].metadata);
    ASSERT_EQ(plan.bodies.size(), 1u);
    EXPECT_EQ(r.model.DeclaredSymbol(*plan.bodies[0].declaration), nullptr);
    EXPECT_TRUE(plan.target.Structure()->conformances.empty());
    EXPECT_FALSE(plan.target.Structure()->methods.contains("inspect"));
    EXPECT_FALSE(r.model.BehaviorPlans()[1].metadata);
    EXPECT_TRUE(r.model.BehaviorPlans()[2].pack.fields.empty());
    EXPECT_FALSE(plan.substitutions.empty());
    for (const auto& symbol : plan.bodies[0].semantics->Symbols())
        if (symbol->Kind() == semantic::SymbolKind::ModuleConstant)
            EXPECT_NE(plan.bodies[0].semantics->ModuleConstantFor(*symbol),
                      nullptr);
}
TEST(BehaviorReflection, PositiveFixtures) {
    const auto root =
        std::filesystem::path(FLECHA_BEHAVIOR_FIXTURE_DIR) / "reflection";
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() != ".fl") continue;
        SCOPED_TRACE(entry.path().filename().string());
        auto result = Analyze(Fixture(entry.path().filename().string()));
        EXPECT_FALSE(result.diagnostics.HasErrors())
            << Messages(result.diagnostics);
    }
}
TEST(BehaviorReflection, InvalidFixturesHaveStableDiagnostics) {
    const auto root = std::filesystem::path(FLECHA_BEHAVIOR_FIXTURE_DIR) /
                      "reflection" / "invalid";
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        SCOPED_TRACE(entry.path().filename().string());
        const auto source =
            Fixture("invalid/" + entry.path().filename().string());
        const auto start = source.find("// expect ") + 10;
        const auto code = source.substr(start, 12);
        auto result = Analyze(source);
        bool found = false;
        for (auto& d : result.diagnostics.Diagnostics())
            if (diagnostics::DiagnosticCodeName(d.code) == code) found = true;
        EXPECT_TRUE(found) << code << "\n" << Messages(result.diagnostics);
    }
}
TEST(BehaviorReflection, GenericFieldsBindDistinctClosedTypes) {
    auto r = Analyze(Fixture("generic.fl"));
    ASSERT_FALSE(r.diagnostics.HasErrors()) << Messages(r.diagnostics);
    ASSERT_EQ(r.model.BehaviorPlans().size(), 2u);
    auto& a = r.model.BehaviorPlans()[0];
    auto& b = r.model.BehaviorPlans()[1];
    ASSERT_EQ(a.pack.fields.size(), 1u);
    ASSERT_EQ(b.pack.fields.size(), 1u);
    EXPECT_NE(a.target, b.target);
    EXPECT_NE(a.pack.fields[0].field, b.pack.fields[0].field);
    EXPECT_EQ(a.pack.fields[0].type.Kind(), semantic::SemanticTypeKind::I32);
    EXPECT_EQ(b.pack.fields[0].type.Kind(), semantic::SemanticTypeKind::Bool);
}
TEST(BehaviorReflection, TypedMetadataDomain) {
    auto r = Analyze(Fixture("metadata.fl"));
    ASSERT_FALSE(r.diagnostics.HasErrors()) << Messages(r.diagnostics);
    ASSERT_EQ(r.model.BehaviorPlans().size(), 1u);
    const auto& members = r.model.BehaviorPlans()[0].metadata->members;
    ASSERT_EQ(members.size(), 7u);
    EXPECT_EQ(std::get<std::string>(members[0].value.value), "label");
    EXPECT_EQ(std::get<std::uint64_t>(members[1].value.value), 7u);
    EXPECT_TRUE(std::get<bool>(members[2].value.value));
    EXPECT_EQ(std::get<char>(members[3].value.value), 'x');
    EXPECT_DOUBLE_EQ(std::get<double>(members[4].value.value), 3.5);
    EXPECT_EQ(
        std::get<semantic::EnumMetadataValue>(members[5].value.value).value,
        1u);
    EXPECT_EQ(
        std::get<semantic::ErrorMetadataValue>(members[6].value.value).code,
        1u);
}
TEST(BehaviorReflection, NamespaceMultifileAndSourceLocalImports) {
    const auto root = std::filesystem::path(FLECHA_BEHAVIOR_FIXTURE_DIR) /
                      "reflection" / "modules";
    analyzer::AnalysisSession session(root);
    auto result = session.Analyze(root / "main.fl");
    ASSERT_FALSE(result.HasErrors()) << Messages(result.diagnostics);
    ASSERT_TRUE(result.semanticModel);
    EXPECT_EQ(result.semanticModel->BehaviorPlans().size(), 2u);
}
TEST(BehaviorReflection, AnalyzerNavigationAndIncompletePrefixes) {
    const auto root =
        std::filesystem::path(FLECHA_BEHAVIOR_FIXTURE_DIR) / "reflection";
    analyzer::AnalysisSession session(root);
    auto result = session.Analyze(root / "metadata.fl");
    ASSERT_FALSE(result.HasErrors()) << Messages(result.diagnostics);
    const auto source = Fixture("metadata.fl");
    auto offset = source.find("decorator().version") + 12;
    auto hover = result.Hover(root / "metadata.fl", offset);
    ASSERT_TRUE(hover);
    EXPECT_NE(hover->type.find("i32"), std::string::npos);
    auto definition = result.Definition(root / "metadata.fl", offset);
    ASSERT_TRUE(definition);
    EXPECT_EQ(definition->start.offset, source.find("i32 version") + 4);
    auto projection = session.Analyze(root / "owning_take.fl");
    ASSERT_FALSE(projection.HasErrors()) << Messages(projection.diagnostics);
    const auto projectedSource = Fixture("owning_take.fl");
    auto fieldDefinition = projection.Definition(
        root / "owning_take.fl", projectedSource.find("field::get") + 7);
    ASSERT_TRUE(fieldDefinition);
    EXPECT_EQ(fieldDefinition->start.offset, projectedSource.find("pointer;"));
    const std::string prefix =
        "decoration meta { application() target structure; }; interface Root { "
        "void run(&self) virtual; }; behavior meta<T> => structure << Root { "
        "void T::run(&self) override { ";
    for (const std::string incomplete :
         {"for<", "for<F>", "for<F> Field<T,",
          "for<F> Field<T,F> field =", "T::fields(", "field::decorator()."}) {
        SCOPED_TRACE(incomplete);
        ASSERT_EQ(
            session.Open("editing.fl",
                         prefix + incomplete + "\n} }; structure Following {};",
                         1),
            analyzer::OverlayUpdateResult::Applied);
        auto editing = session.Analyze("editing.fl");
        EXPECT_TRUE(editing.HasErrors());
        ASSERT_TRUE(editing.unit);
        bool found = false;
        for (auto& decl : editing.unit->Declarations())
            if (decl->Kind() == core::AstNodeKind::StructureDeclaration &&
                static_cast<const core::StructureDeclaration&>(*decl).Name() ==
                    "Following")
                found = true;
        EXPECT_TRUE(found) << Messages(editing.diagnostics);
        EXPECT_TRUE(session.Close("editing.fl"));
    }
}

TEST(BehaviorReflection, ExpandedPlansDoNotChangeIR) {
    auto source = Fixture("fields.fl");
    auto with = Analyze(source);
    ASSERT_FALSE(with.diagnostics.HasErrors()) << Messages(with.diagnostics);
    for (auto it = with.model.Behaviors().rbegin();
         it != with.model.Behaviors().rend(); ++it)
        source.erase(it->declaration->Range().start.offset,
                     it->declaration->Range().Length());
    auto without = Analyze(source);
    ASSERT_FALSE(without.diagnostics.HasErrors());
    auto generate = [](Result& result) {
        lowering::Lowerer lowerer;
        auto program =
            lowerer.Lower(*result.unit, result.model, result.diagnostics);
        EXPECT_NE(program, nullptr);
        codegen::LlvmCodeGenerator generator;
        auto module = generator.Generate(*program, "reflection_runtime_parity");
        EXPECT_NE(module, nullptr) << generator.Error();
        std::string text;
        llvm::raw_string_ostream stream(text);
        if (module) module->print(stream, nullptr);
        return text;
    };
    EXPECT_EQ(generate(with), generate(without));
    EXPECT_TRUE(with.model.BehaviorPlans()[0]
                    .target.Structure()
                    ->conformanceClosure.empty());
}
TEST(BehaviorReflection, ProjectionErrorsHaveExactFieldProvenance) {
    auto source = Fixture("invalid/field_type.fl");
    auto result = Analyze(source);
    ASSERT_TRUE(result.diagnostics.HasErrors());
    for (auto& d : result.diagnostics.Diagnostics())
        if (d.code == diagnostics::DiagnosticCode::IncompatibleType) {
            EXPECT_LT(d.range.end.offset, source.size());
            bool field = false, activation = false;
            for (auto& r : d.related) {
                field = field || r.message == "field 'first', F = bool";
                activation =
                    activation || r.message == "behavior activated here";
            }
            EXPECT_TRUE(field);
            EXPECT_TRUE(activation);
        }
}

TEST(BehaviorReflection, ActivationsStillDoNotMakeMethodsCallable) {
    auto result = Analyze(Fixture("fields.fl") +
                          "void call() { Record item; item.inspect(); }");
    EXPECT_TRUE(result.diagnostics.HasErrors());
    EXPECT_NE(Messages(result.diagnostics).find("inspect"), std::string::npos);
}
TEST(BehaviorReflection, CyclicConstantsFailWithoutRecursingInPlans) {
    auto source = Fixture("metadata.fl");
    auto pos = source.find("const i32 NEXT = 3;");
    source.replace(pos, 19, "const i32 NEXT = BASE;");
    auto result = Analyze(source);
    EXPECT_TRUE(result.diagnostics.HasErrors());
    EXPECT_NE(Messages(result.diagnostics).find("cyclic module constant"),
              std::string::npos);
}

}  // namespace
