#include <gtest/gtest.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <string>

#include "analyzer/analysis_session.hpp"
#include "codegen/llvm_code_generator.hpp"
#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "lowering/lowerer.hpp"
#include "semantic/semantic_analyzer.hpp"

namespace {
using namespace flecha;

struct BehaviorAnalysis final {
    diagnostics::DiagnosticBag diagnostics;
    std::unique_ptr<core::CompilationUnit> unit;
    semantic::SemanticModel model;
};

std::string Fixture(std::string_view name) {
    std::ifstream stream(std::filesystem::path(FLECHA_BEHAVIOR_FIXTURE_DIR) /
                         name);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}

BehaviorAnalysis AnalyzeBehavior(std::string source) {
    BehaviorAnalysis result;
    core::Tokenizer tokenizer(source, result.diagnostics);
    core::Parser parser(tokenizer.Tokenize(), result.diagnostics);
    result.unit = parser.Parse();
    semantic::SemanticAnalyzer analyzer(result.diagnostics);
    result.model = analyzer.Analyze(*result.unit);
    return result;
}

std::string Messages(const diagnostics::DiagnosticBag& diagnostics) {
    std::string result;
    for (const auto& diagnostic : diagnostics.Diagnostics())
        result += diagnostic.message + "\n";
    return result;
}

TEST(BehaviorTests, ParsesAndRegistersBothTargetKinds) {
    auto result = AnalyzeBehavior(Fixture("records.fl"));
    ASSERT_FALSE(result.diagnostics.HasErrors())
        << Messages(result.diagnostics);
    ASSERT_EQ(result.model.Behaviors().size(), 2u);
    const auto& behavior = result.model.Behaviors()[0];
    EXPECT_EQ(behavior.declaration->Kind(),
              core::AstNodeKind::BehaviorDeclaration);
    EXPECT_EQ(behavior.declaration->targetParameter, "T");
    EXPECT_EQ(behavior.declaration->target, core::DecorationTarget::Structure);
    EXPECT_EQ(behavior.decoration->Name(), "meta");
    ASSERT_EQ(behavior.roots.size(), 1u);
    EXPECT_EQ(behavior.roots[0].type->displayName, "Writable");
    ASSERT_EQ(behavior.methods.size(), 1u);
    EXPECT_EQ(behavior.methods[0].signature.key.name, "write");
    EXPECT_EQ(behavior.methods[0].signature.key.receiver,
              semantic::MethodReceiverKind::ConstReference);
    EXPECT_NE(behavior.methods[0].requirement, nullptr);
    EXPECT_EQ(result.model.DeclaredSymbol(*behavior.methods[0].declaration),
              nullptr);
    EXPECT_FALSE(behavior.declaration->methods[0]->Body().Statements().empty());
    EXPECT_EQ(result.model.Behaviors()[1].declaration->target,
              core::DecorationTarget::Enum);
}

TEST(BehaviorTests, AggregatesDirectAndFieldMetadataAndExcludesInterfaces) {
    auto result = AnalyzeBehavior(Fixture("records.fl"));
    ASSERT_FALSE(result.diagnostics.HasErrors())
        << Messages(result.diagnostics);
    const auto& candidates = result.model.BehaviorActivations();
    ASSERT_EQ(candidates.size(), 3u);
    EXPECT_EQ(candidates[0].applications.size(), 3u);
    EXPECT_EQ(candidates[1].applications.size(), 1u);
    EXPECT_EQ(candidates[2].target->Kind(), core::AstNodeKind::EnumDeclaration);
    for (const auto& candidate : candidates) EXPECT_TRUE(candidate.valid);
    for (const auto& structure : result.model.Structures()) {
        if (structure->displayName == "Ordinary") {
            ASSERT_EQ(structure->conformances.size(), 1u);
            EXPECT_EQ(structure->conformances.front().type->displayName,
                      "Child");
        } else {
            EXPECT_EQ(structure->conformances.size(), 1u);
            EXPECT_EQ(structure->conformanceClosure.size(), 1u);
            EXPECT_TRUE(structure->methods.contains("write"));
        }
    }
    EXPECT_EQ(result.model.Enums().front()->conformances.size(), 1u);
}

TEST(BehaviorTests, DiscoversDistinctGenericTargetSpecializations) {
    auto result = AnalyzeBehavior(Fixture("generic_targets.fl"));
    ASSERT_FALSE(result.diagnostics.HasErrors())
        << Messages(result.diagnostics);
    ASSERT_EQ(result.model.BehaviorActivations().size(), 2u);
    const auto& first = result.model.BehaviorActivations()[0];
    const auto& second = result.model.BehaviorActivations()[1];
    EXPECT_NE(first.target, second.target);
    EXPECT_EQ(first.behavior, second.behavior);
    EXPECT_EQ(first.applications.size(), 2u);
    EXPECT_EQ(second.applications.size(), 2u);
    for (const auto& structure : result.model.Structures())
        EXPECT_EQ(structure->conformances.size(), 1u);
}

TEST(BehaviorTests, EmitsBehaviorMethodsAndInterfaceTables) {
    auto result = AnalyzeBehavior(Fixture("records.fl"));
    ASSERT_FALSE(result.diagnostics.HasErrors())
        << Messages(result.diagnostics);
    lowering::Lowerer lowerer;
    auto program =
        lowerer.Lower(*result.unit, result.model, result.diagnostics);
    ASSERT_NE(program, nullptr);
    ASSERT_FALSE(result.diagnostics.HasErrors())
        << Messages(result.diagnostics);
    std::size_t methods = 0, tables = 0;
    for (const auto& function : program->Functions())
        methods += function->Symbol().Name().contains("write");
    for (const auto& table : program->InterfaceTables())
        tables += table.providerMap.root->displayName == "Writable";
    EXPECT_EQ(methods, 3u);
    EXPECT_EQ(tables, 3u);
}

TEST(BehaviorTests, UnactivatedBehaviorDeclarationsDoNotChangeGeneratedIR) {
    std::string source = std::regex_replace(
        Fixture("records.fl"), std::regex(R"(@meta(\([^)]*\))?\s*)"), "");
    auto withBehaviors = AnalyzeBehavior(source);
    ASSERT_FALSE(withBehaviors.diagnostics.HasErrors());
    for (auto it = withBehaviors.model.Behaviors().rbegin();
         it != withBehaviors.model.Behaviors().rend(); ++it) {
        const auto& range = it->declaration->Range();
        source.erase(range.start.offset, range.Length());
    }
    auto withoutBehaviors = AnalyzeBehavior(source);
    ASSERT_FALSE(withoutBehaviors.diagnostics.HasErrors());
    const auto generate = [](BehaviorAnalysis& analysis) {
        lowering::Lowerer lowerer;
        auto program =
            lowerer.Lower(*analysis.unit, analysis.model, analysis.diagnostics);
        codegen::LlvmCodeGenerator generator;
        auto module = generator.Generate(*program, "behavior_runtime_parity");
        EXPECT_NE(module, nullptr) << generator.Error();
        std::string text;
        llvm::raw_string_ostream stream(text);
        if (module) module->print(stream, nullptr);
        return text;
    };
    EXPECT_EQ(generate(withBehaviors), generate(withoutBehaviors));
}

TEST(BehaviorTests,
     DuplicateBehaviorsInvalidateBothContractsAndAllActivations) {
    auto result = AnalyzeBehavior(Fixture("invalid/duplicate_behavior.fl") +
                                  "@meta structure Item {};\n");
    ASSERT_EQ(result.model.Behaviors().size(), 2u);
    EXPECT_FALSE(result.model.Behaviors()[0].valid);
    EXPECT_FALSE(result.model.Behaviors()[1].valid);
    EXPECT_TRUE(result.model.BehaviorActivations().empty());
    const auto found = std::find_if(
        result.diagnostics.Diagnostics().begin(),
        result.diagnostics.Diagnostics().end(), [](const auto& diagnostic) {
            return diagnostic.code ==
                   diagnostics::DiagnosticCode::DuplicateBehavior;
        });
    ASSERT_NE(found, result.diagnostics.Diagnostics().end());
    ASSERT_EQ(found->related.size(), 1u);
    EXPECT_LT(found->related[0].range.start.offset, found->range.start.offset);
}

TEST(BehaviorTests, ConflictsAreDeterministicAndLeaveConcreteGraphsUntouched) {
    const std::string source = Fixture("invalid/derived_conflict.fl");
    auto first = AnalyzeBehavior(source);
    auto second = AnalyzeBehavior(source);
    EXPECT_EQ(Messages(first.diagnostics), Messages(second.diagnostics));
    ASSERT_EQ(first.model.BehaviorActivations().size(), 2u);
    for (const auto& candidate : first.model.BehaviorActivations())
        EXPECT_FALSE(candidate.valid);
    ASSERT_EQ(first.model.Structures().size(), 1u);
    EXPECT_TRUE(first.model.Structures().front()->conformances.empty());
    EXPECT_TRUE(first.model.Structures().front()->conformanceClosure.empty());
}

TEST(BehaviorTests, NavigationHoverAndOccurrencesExposeSemanticRecords) {
    const std::filesystem::path root = FLECHA_BEHAVIOR_FIXTURE_DIR;
    analyzer::AnalysisSession session(root);
    std::string source = Fixture("records.fl");
    source.replace(0, std::string("module records;").size(), "module tooling;");
    ASSERT_EQ(session.Open("tooling.fl", source, 1),
              analyzer::OverlayUpdateResult::Applied);
    auto snapshot = session.Analyze("tooling.fl");
    ASSERT_FALSE(snapshot.HasErrors());
    const std::size_t behaviorName = source.find("behavior meta") + 9;
    const auto hover = snapshot.Hover("tooling.fl", behaviorName);
    ASSERT_TRUE(hover);
    EXPECT_EQ(hover->kind, analyzer::SemanticItemKind::Behavior);
    EXPECT_TRUE(hover->type.contains("=> structure << Writable"));
    const auto definition = snapshot.Definition("tooling.fl", behaviorName);
    ASSERT_TRUE(definition);
    EXPECT_EQ(definition->start.offset, source.find("decoration meta") + 11);
    const auto rootDefinition =
        snapshot.Definition("tooling.fl", source.find("<< Writable") + 3);
    ASSERT_TRUE(rootDefinition);
    EXPECT_EQ(rootDefinition->start.offset,
              source.find("interface Writable") + 10);
    const auto methodDefinition =
        snapshot.Definition("tooling.fl", source.find("T::write") + 3);
    ASSERT_TRUE(methodDefinition);
    EXPECT_EQ(methodDefinition->start.offset, source.find("write(&const"));
    const auto references =
        snapshot.References("tooling.fl", source.find("decoration meta") + 11);
    ASSERT_TRUE(references);
    EXPECT_EQ(references->size(),
              9u);  // declaration, two behaviors, six applications
    const auto tokens = snapshot.Occurrences("tooling.fl");
    for (const auto& keyword : {"behavior meta", "structure <<", "enum <<"}) {
        const std::size_t position = source.find(keyword);
        EXPECT_TRUE(
            std::any_of(tokens.begin(), tokens.end(), [&](const auto& token) {
                return token.range.start.offset == position &&
                       token.kind == analyzer::SemanticItemKind::Keyword;
            }));
    }
    const auto symbols = snapshot.Symbols("meta");
    EXPECT_EQ(std::count_if(symbols.begin(), symbols.end(),
                            [](const auto& symbol) {
                                return symbol.kind ==
                                       analyzer::SemanticItemKind::Behavior;
                            }),
              2);
}

TEST(BehaviorTests, MultifileAndNamespaceAttachedModulesOwnBehaviors) {
    const std::filesystem::path root =
        std::filesystem::path(FLECHA_BEHAVIOR_FIXTURE_DIR) / "modules/valid";
    analyzer::AnalysisSession session(root);
    auto snapshot = session.Analyze("main.fl");
    ASSERT_FALSE(snapshot.HasErrors());
    ASSERT_TRUE(snapshot.semanticModel);
    ASSERT_EQ(snapshot.semanticModel->Behaviors().size(), 2u);
    ASSERT_EQ(snapshot.semanticModel->BehaviorActivations().size(), 2u);
    for (const auto& behavior : snapshot.semanticModel->Behaviors()) {
        EXPECT_EQ(behavior.declaration->moduleId,
                  behavior.decoration->moduleId);
        EXPECT_NE(behavior.declaration->sourceId,
                  behavior.decoration->sourceId);
    }
}

TEST(BehaviorTests, RootsUseTheDeclaringSourceImportScope) {
    const std::filesystem::path fixtures = FLECHA_BEHAVIOR_FIXTURE_DIR;
    analyzer::AnalysisSession valid(fixtures / "modules/imports");
    EXPECT_FALSE(valid.Analyze("main.fl").HasErrors());
    analyzer::AnalysisSession invalid(fixtures / "modules/source_scope");
    EXPECT_TRUE(invalid.Analyze("main.fl").HasErrors());
}

TEST(BehaviorTests, RejectsForeignOwnershipAndMultifileDuplicates) {
    for (const auto& fixture : {"foreign", "namespace_mismatch", "duplicate"}) {
        SCOPED_TRACE(fixture);
        const std::filesystem::path root =
            std::filesystem::path(FLECHA_BEHAVIOR_FIXTURE_DIR) / "modules" /
            fixture;
        analyzer::AnalysisSession session(root);
        auto snapshot = session.Analyze("main.fl");
        EXPECT_TRUE(snapshot.HasErrors());
        ASSERT_TRUE(snapshot.semanticModel);
        EXPECT_TRUE(snapshot.semanticModel->BehaviorActivations().empty());
    }
}

TEST(BehaviorTests, RecoversFromIncompleteBehaviorWhileEditing) {
    analyzer::AnalysisSession session(FLECHA_BEHAVIOR_FIXTURE_DIR);
    const std::string prefix =
        "module editing; decoration meta { application() target structure; }; "
        "interface Root {}; ";
    for (const auto& suffix :
         {"behavior", "behavior meta<", "behavior meta<T> =>",
          "behavior meta<T> => structure <<",
          "behavior meta<T> => structure << Root {",
          "behavior meta<T> => structure << Root { i32 T::method(&self) "
          "override {"}) {
        SCOPED_TRACE(suffix);
        ASSERT_EQ(session.Open("editing.fl", prefix + suffix, 1),
                  analyzer::OverlayUpdateResult::Applied);
        EXPECT_TRUE(session.Analyze("editing.fl").HasErrors());
        EXPECT_TRUE(session.Close("editing.fl"));
    }
    ASSERT_EQ(
        session.Open("editing.fl",
                     prefix + "behavior meta<T> => structure << Root {};", 2),
        analyzer::OverlayUpdateResult::Applied);
    EXPECT_FALSE(session.Analyze("editing.fl").HasErrors());
}

}  // namespace
