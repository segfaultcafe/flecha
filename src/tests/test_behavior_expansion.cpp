#include <gtest/gtest.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include "analyzer/analysis_session.hpp"
#include "codegen/llvm_code_generator.hpp"
#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "lowering/lowerer.hpp"
#include "semantic/semantic_analyzer.hpp"

namespace {
using namespace flecha;
const std::filesystem::path root =
    std::filesystem::path(FLECHA_BEHAVIOR_FIXTURE_DIR) / "expansion";
std::string Read(const std::filesystem::path& file) {
    std::ifstream stream(file);
    return {std::istreambuf_iterator<char>(stream), {}};
}
std::string Messages(const diagnostics::DiagnosticBag& bag) {
    std::string text;
    for (const auto& d : bag.Diagnostics()) text += d.message + "\n";
    return text;
}
struct Analysis {
    diagnostics::DiagnosticBag diagnostics;
    std::unique_ptr<core::CompilationUnit> unit;
    semantic::SemanticModel model;
};
Analysis Analyze(const std::string& text) {
    Analysis result;
    core::Tokenizer tokenizer(text, result.diagnostics);
    core::Parser parser(tokenizer.Tokenize(), result.diagnostics);
    result.unit = parser.Parse();
    semantic::SemanticAnalyzer analyzer(result.diagnostics);
    result.model = analyzer.Analyze(*result.unit);
    return result;
}
std::string IR(Analysis& result) {
    lowering::Lowerer lowerer;
    auto program =
        lowerer.Lower(*result.unit, result.model, result.diagnostics);
    if (!program) return {};
    codegen::LlvmCodeGenerator generator;
    auto module = generator.Generate(*program, "behavior_expansion");
    EXPECT_NE(module, nullptr) << generator.Error();
    std::string text;
    llvm::raw_string_ostream out(text);
    if (module) module->print(out, nullptr);
    return text;
}
TEST(BehaviorExpansion, PositiveFixturesEmitEachSelectedMethodOnce) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() != ".fl") continue;
        SCOPED_TRACE(entry.path().filename().string());
        auto result = Analyze(Read(entry.path()));
        ASSERT_FALSE(result.diagnostics.HasErrors())
            << Messages(result.diagnostics);
        lowering::Lowerer lowerer;
        auto program =
            lowerer.Lower(*result.unit, result.model, result.diagnostics);
        ASSERT_NE(program, nullptr);
        for (const auto& plan : result.model.BehaviorPlans()) {
            EXPECT_TRUE(plan.valid);
            for (const auto& body : plan.bodies) {
                const auto* symbol =
                    result.model.DeclaredSymbol(*body.declaration);
                ASSERT_NE(symbol, nullptr);
                const auto count = std::count_if(
                    program->Functions().begin(), program->Functions().end(),
                    [&](const auto& function) {
                        return &function->Symbol() == symbol;
                    });
                EXPECT_EQ(count, body.declaration->HasVariadicPack() ? 0 : 1);
                EXPECT_EQ(
                    std::count_if(result.model.GeneratedFunctions().begin(),
                                  result.model.GeneratedFunctions().end(),
                                  [&](const auto& function) {
                                      return function.get() == body.declaration;
                                  }),
                    1);
            }
        }
        const auto text = IR(result);
        EXPECT_FALSE(text.empty());
        EXPECT_EQ(text.find("$behavior$"), std::string::npos);
        EXPECT_EQ(text.find("Field<"), std::string::npos);
        EXPECT_EQ(text.find("::fields"), std::string::npos);
        EXPECT_EQ(text.find("::decorator"), std::string::npos);
        auto repeat = Analyze(Read(entry.path()));
        EXPECT_EQ(text, IR(repeat));
    }
}
TEST(BehaviorExpansion, FailuresPublishNoMethodsOrConformances) {
    for (const auto& entry :
         std::filesystem::directory_iterator(root / "invalid")) {
        SCOPED_TRACE(entry.path().filename().string());
        const auto source = Read(entry.path());
        auto result = Analyze(source);
        ASSERT_TRUE(result.diagnostics.HasErrors());
        const auto start = source.find("// expect ") + 10;
        const auto expected = source.substr(start, 12);
        EXPECT_TRUE(std::any_of(result.diagnostics.Diagnostics().begin(),
                                result.diagnostics.Diagnostics().end(),
                                [&](const auto& diagnostic) {
                                    return diagnostics::DiagnosticCodeName(
                                               diagnostic.code) == expected;
                                }))
            << expected << "\n"
            << Messages(result.diagnostics);
        for (const auto& type : result.model.Structures()) {
            EXPECT_EQ(type->conformances.size(),
                      type->syntax->Conformances().size());
            for (const auto& [name, overloads] : type->methods)
                for (const auto* method : overloads)
                    EXPECT_FALSE(method->Declaration()->sourceId.starts_with(
                        "$behavior$"));
        }
        for (const auto& symbol : result.model.Symbols())
            EXPECT_FALSE(symbol->Name().starts_with("$behavior$"));
        for (const auto& function : result.model.GeneratedFunctions())
            EXPECT_FALSE(function->sourceId.starts_with("$behavior$"));
        lowering::Lowerer lowerer;
        EXPECT_EQ(lowerer.Lower(*result.unit, result.model, result.diagnostics),
                  nullptr);
    }
}
TEST(BehaviorExpansion,
     HandwrittenProviderSuppressesInvalidReflectionCompletely) {
    auto result = Analyze(Read(root / "suppression.fl"));
    ASSERT_FALSE(result.diagnostics.HasErrors())
        << Messages(result.diagnostics);
    ASSERT_EQ(result.model.BehaviorPlans().size(), 1u);
    EXPECT_TRUE(result.model.BehaviorPlans()[0].bodies.empty());
    EXPECT_TRUE(result.model.BehaviorPlans()[0].substitutions.empty());
    EXPECT_TRUE(result.model.ReflectionOccurrences().empty());
    const auto* target = result.model.BehaviorPlans()[0].target.Structure();
    ASSERT_EQ(target->methods.at("read").size(), 1u);
    EXPECT_FALSE(
        target->methods.at("read")[0]->Declaration()->sourceId.starts_with(
            "$behavior$"));
}
TEST(BehaviorExpansion, CompositionUsesOnlyDeclaredRootsAndOrdinaryClosure) {
    auto result = Analyze(Read(root / "composition.fl"));
    ASSERT_FALSE(result.diagnostics.HasErrors())
        << Messages(result.diagnostics);
    const auto* target = result.model.BehaviorPlans()[0].target.Structure();
    std::set<std::string> direct, closure;
    for (const auto& reference : target->conformances)
        direct.insert(reference.type->displayName);
    for (const auto* type : target->conformanceClosure)
        closure.insert(type->displayName);
    EXPECT_EQ(direct, (std::set<std::string>{"Explicit", "Read", "Other"}));
    EXPECT_EQ(closure,
              (std::set<std::string>{"Explicit", "Read", "Other", "Base"}));
}
TEST(BehaviorExpansion, GenericBodiesReuseSpecializationsAndReanalyzeSafely) {
    auto result = Analyze(Read(root / "generic.fl"));
    ASSERT_FALSE(result.diagnostics.HasErrors())
        << Messages(result.diagnostics);
    ASSERT_EQ(result.model.BehaviorPlans().size(), 2u);
    EXPECT_NE(result.model.BehaviorPlans()[0].target,
              result.model.BehaviorPlans()[1].target);
    const auto before = result.unit->Declarations().size();
    const auto ir = IR(result);
    semantic::SemanticAnalyzer again(result.diagnostics);
    result.model = again.Analyze(*result.unit);
    ASSERT_FALSE(result.diagnostics.HasErrors())
        << Messages(result.diagnostics);
    EXPECT_EQ(before, result.unit->Declarations().size());
    EXPECT_EQ(ir, IR(result));
}
TEST(BehaviorExpansion, NavigationAndRebuildUseSourceProvenance) {
    const auto file = root / "overloads.fl";
    const auto source = Read(file);
    analyzer::AnalysisSession session(root);
    ASSERT_EQ(session.Open(file, source, 1),
              analyzer::OverlayUpdateResult::Applied);
    auto result = session.Analyze(file);
    ASSERT_FALSE(result.HasErrors()) << Messages(result.diagnostics);
    const auto call = source.find("item.read()") + 5;
    const auto definition = result.Definition(file, call);
    ASSERT_TRUE(definition);
    EXPECT_EQ(definition->start.file, file.string());
    EXPECT_EQ(definition->start.offset, source.find("T::read") + 3);
    ASSERT_TRUE(result.Hover(file, call));
    for (const auto& occurrence : result.Occurrences(file))
        EXPECT_FALSE(occurrence.name.starts_with('$'));
    auto changed = source;
    changed.erase(changed.find("@meta structure"), 6);
    ASSERT_EQ(session.Change(file, changed, 2),
              analyzer::OverlayUpdateResult::Applied);
    auto rebuilt = session.Analyze(file);
    EXPECT_TRUE(rebuilt.HasErrors());
    ASSERT_TRUE(rebuilt.semanticModel);
    EXPECT_TRUE(rebuilt.semanticModel->BehaviorPlans().empty());
    for (const auto& type : rebuilt.semanticModel->Structures())
        EXPECT_TRUE(type->conformances.empty());
}

std::map<std::string, std::string> CanonicalIR(Analysis& analysis,
                                               bool jsonOnly = false) {
    lowering::Lowerer lowerer;
    auto program =
        lowerer.Lower(*analysis.unit, analysis.model, analysis.diagnostics);
    EXPECT_NE(program, nullptr);
    if (!program) return {};
    codegen::LlvmCodeGenerator generator;
    auto module = generator.Generate(*program, "parity");
    EXPECT_NE(module, nullptr) << generator.Error();
    if (!module) return {};
    std::map<std::string, std::string> result;
    const auto capture = [&](const auto& value, const std::string& name) {
        std::string text;
        llvm::raw_string_ostream stream(text);
        value.print(stream);
        result[name] = text;
    };
    for (const auto& function : *module)
        if (!jsonOnly || function.getName().contains("Response::"))
            capture(function, function.getName().str());
    if (!jsonOnly) {
        for (const auto& global : module->globals())
            capture(global, global.getName().str());
        for (const auto* type : module->getIdentifiedStructTypes())
            capture(*type, type->getName().str());
    }
    return result;
}
TEST(BehaviorExpansion, GeneratedMethodsMatchHandwrittenIRTablesAndLayout) {
    const std::string common = R"(
        interface Read { i32 read(&const self) virtual; };
        i32 main() { item := Item::init(); &const Read view = item as &const Read; return view->read(); }
    )";
    auto generated = Analyze(common + R"(
        decoration meta { application() target structure; };
        behavior meta<T> => structure << Read {
            i32 T::read(&const self) override { return self->value; }
        };
        @meta structure Item { private i32 value = 42; bool flag; };
    )");
    auto handwritten = Analyze(common + R"(
        structure Item << Read { private i32 value = 42; bool flag; };
        i32 Item::read(&const self) override { return self->value; }
    )");
    ASSERT_FALSE(generated.diagnostics.HasErrors())
        << Messages(generated.diagnostics);
    ASSERT_FALSE(handwritten.diagnostics.HasErrors())
        << Messages(handwritten.diagnostics);
    EXPECT_EQ(CanonicalIR(generated), CanonicalIR(handwritten));
}
TEST(BehaviorExpansion, LegacyCompilerOwnedJsonIRIsUnchanged) {
    const auto source = Read(root / "legacy_json.fl");
    auto before = Analyze(source);
    auto after = Analyze(source + R"(
        decoration meta { application() target structure; };
        interface Read { i32 read(&const self) virtual; };
        behavior meta<T> => structure << Read { i32 T::read(&const self) override { return 42; } };
        @meta structure Extra {};
    )");
    ASSERT_FALSE(before.diagnostics.HasErrors())
        << Messages(before.diagnostics);
    ASSERT_FALSE(after.diagnostics.HasErrors()) << Messages(after.diagnostics);
    const auto original = CanonicalIR(before, true);
    EXPECT_FALSE(original.empty());
    EXPECT_EQ(original, CanonicalIR(after, true));
}
TEST(BehaviorExpansion, FieldValidationEmitsOnlySelectedOrdinaryOperations) {
    const std::string common = R"(
        interface Read { i32 read(&const self) virtual; };
        i32 main() { Item item; item.value = 42; return item.read(); }
    )";
    auto generated = Analyze(common + R"(
        decoration meta { application() target structure; };
        behavior meta<T> => structure << Read {
            i32 T::read(&const self) override {
                i32 sum = 0;
                for<F> Field<T, F> field = T::fields() {
                    if !field::isPrivate() {
                        field::require(F.isof<i32>, "expected i32");
                        T::require(T.isof<Read>, "expected Read");
                        field::requireUnique(field::name());
                        sum = sum + field::get(self);
                    }
                }
                return sum;
            }
        };
        @meta structure Item { i32 value; private bool secret; };
    )");
    auto handwritten = Analyze(common + R"(
        structure Item << Read { i32 value; private bool secret; };
        i32 Item::read(&const self) override {
            i32 sum = 0;
            { { sum = sum + (self)->value; } }
            {}
            return sum;
        }
    )");
    ASSERT_FALSE(generated.diagnostics.HasErrors())
        << Messages(generated.diagnostics);
    ASSERT_FALSE(handwritten.diagnostics.HasErrors())
        << Messages(handwritten.diagnostics);
    EXPECT_EQ(CanonicalIR(generated), CanonicalIR(handwritten));
}
TEST(BehaviorExpansion, NamespaceAttachedGenericHelpersKeepLexicalScope) {
    analyzer::AnalysisSession session(root / "modules");
    auto result = session.Analyze("main.fl");
    ASSERT_FALSE(result.HasErrors()) << Messages(result.diagnostics);
    ASSERT_TRUE(result.semanticModel);
    EXPECT_EQ(result.semanticModel->BehaviorPlans().size(), 2u);
}
TEST(BehaviorExpansion, MissingRequirementsPrecedeBodyExpansion) {
    auto result = Analyze(Read(root / "invalid/missing_before_body.fl"));
    ASSERT_TRUE(result.diagnostics.HasErrors());
    ASSERT_EQ(result.diagnostics.Diagnostics().size(), 1u);
    const auto& diagnostic = result.diagnostics.Diagnostics().front();
    EXPECT_EQ(diagnostic.code, diagnostics::DiagnosticCode::InvalidInterface);
    for (const auto* note :
         {"behavior declared here", "behavior activated here",
          "concrete T = Item", "required interface method"})
        EXPECT_TRUE(std::any_of(
            diagnostic.related.begin(), diagnostic.related.end(),
            [&](const auto& related) { return related.message == note; }))
            << note;
    EXPECT_TRUE(result.model.ReflectionOccurrences().empty());
}
TEST(BehaviorExpansion,
     GenericErrorsIdentifyOnlyTheFailingFieldSpecialization) {
    auto result = Analyze(Read(root / "invalid/generic_field.fl"));
    ASSERT_TRUE(result.diagnostics.HasErrors());
    bool field = false, target = false;
    for (const auto& diagnostic : result.diagnostics.Diagnostics()) {
        EXPECT_FALSE(diagnostic.range.start.file.starts_with("$"));
        for (const auto& related : diagnostic.related) {
            EXPECT_FALSE(related.range.start.file.starts_with("$"));
            EXPECT_EQ(related.message.find("Box<i32>"), std::string::npos);
            field |= related.message == "field 'value', F = bool";
            target |= related.message == "concrete T = Box<bool>";
        }
    }
    EXPECT_TRUE(field);
    EXPECT_TRUE(target);
}
TEST(BehaviorExpansion, AbortedGenericBodiesRestoreDeclarationsAndAliases) {
    const auto source = R"(
        decoration meta { application() target structure; };
        interface Read { i32 read(&const self) virtual; };
        A invalid<A>(A value) { return "wrong"; }
        behavior meta<T> => structure << Read {
            i32 T::read(&const self) override { return invalid<i32>(42); }
        };
        @meta structure Item {};
    )";
    auto result = Analyze(source);
    ASSERT_TRUE(result.diagnostics.HasErrors());
    EXPECT_TRUE(result.unit->genericAliases.empty());
    for (const auto& declaration : result.unit->Declarations())
        EXPECT_EQ(declaration->specializationOrigin, nullptr);
    EXPECT_TRUE(result.model.BehaviorPlans().empty());
    bool activation = false;
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        for (const auto& related : diagnostic.related) {
            EXPECT_FALSE(related.range.start.file.starts_with("$"));
            activation |= related.message == "concrete T = Item";
        }
    EXPECT_TRUE(activation);
}
TEST(BehaviorExpansion, CompositeFieldTypesKeepCrossModuleNominalIdentity) {
    analyzer::AnalysisSession session(root / "composite_modules");
    auto result = session.Analyze("main.fl");
    ASSERT_FALSE(result.HasErrors()) << Messages(result.diagnostics);
    ASSERT_TRUE(result.semanticModel);
    EXPECT_EQ(result.semanticModel->BehaviorPlans().size(), 1u);
}
TEST(BehaviorExpansion, RollbackPreservesUnrelatedSemanticQueries) {
    // Exercise rollback before planning, during body checking, after generic
    // reanalysis, and after a normal body fails despite a valid activation.
    for (const auto* fixture :
         {"invalid/explicit_collision.fl", "invalid/missing.fl",
          "invalid/body_type.fl", "invalid/generic_field.fl", "structure.fl"}) {
        SCOPED_TRACE(fixture);
        const auto file = root / fixture;
        const auto source = Read(file) + R"(
            i32 independent(i32 parameter) { return parameter; }
            i32 unrelated() { i32 local = independent(42); return local; }
            void other_error() { unknown_name; }
        )";
        analyzer::AnalysisSession session(root);
        ASSERT_EQ(session.Open(file, source, 1),
                  analyzer::OverlayUpdateResult::Applied);
        auto result = session.Analyze(file);
        ASSERT_TRUE(result.HasErrors());
        ASSERT_TRUE(result.semanticModel);
        EXPECT_TRUE(result.semanticModel->BehaviorPlans().empty());
        for (const auto& type : result.semanticModel->Structures())
            EXPECT_EQ(type->conformances.size(),
                      type->syntax->Conformances().size());
        for (const auto& function : result.semanticModel->GeneratedFunctions())
            EXPECT_FALSE(function->sourceId.starts_with("$behavior$"));
        for (const auto& [use, declaration] :
             {std::pair{"return parameter", "i32 parameter"},
              std::pair{"return local", "i32 local"},
              std::pair{"= independent", "i32 independent"}}) {
            const auto useOffset =
                source.find(use) + std::string(use).find(' ') + 1;
            const auto declarationOffset = source.find(declaration) + 4;
            const auto definition = result.Definition(file, useOffset);
            ASSERT_TRUE(definition) << use;
            EXPECT_EQ(definition->start.file, file.string());
            EXPECT_EQ(definition->start.offset, declarationOffset);
            EXPECT_TRUE(result.Hover(file, useOffset)) << use;
            const auto occurrence = result.OccurrenceAt(file, useOffset);
            ASSERT_TRUE(occurrence) << use;
            EXPECT_FALSE(occurrence->type.empty());
            const auto references = result.References(file, declarationOffset);
            ASSERT_TRUE(references) << use;
            EXPECT_EQ(references->size(), 2u);
        }
        lowering::Lowerer lowerer;
        EXPECT_EQ(lowerer.Lower(*result.unit, *result.semanticModel,
                                result.diagnostics),
                  nullptr);
    }
}
}  // namespace
