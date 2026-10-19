#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/module_loader.hpp"
#include "semantic/semantic_analyzer.hpp"

namespace {
namespace fs = std::filesystem;
class ModuleTests : public ::testing::Test {
   protected:
    fs::path root;
    void SetUp() override {
        const auto* test =
            ::testing::UnitTest::GetInstance()->current_test_info();
        root = fs::temp_directory_path() /
               (std::string("flecha_modules_") + test->test_suite_name() + "_" +
                test->name());
        fs::remove_all(root);
        fs::create_directories(root);
    }
    void TearDown() override { fs::remove_all(root); }
    void Write(const std::string& name, const std::string& text) {
        fs::create_directories((root / name).parent_path());
        std::ofstream(root / name) << text;
    }
    std::string Check(flecha::core::ModuleLoadMode mode =
                          flecha::core::ModuleLoadMode::Runtime) {
        return CheckAt("main.fl", mode);
    }
    std::string CheckAt(const std::string& entry,
                        flecha::core::ModuleLoadMode mode =
                            flecha::core::ModuleLoadMode::Runtime) {
        flecha::diagnostics::DiagnosticBag diagnostics;
        flecha::core::ModuleLoader loader(diagnostics);
        auto unit = loader.Load(root / entry, root,
                                {{"fixture", root / "library"},
                                 {"mirror", root / "library"},
                                 {"std", root / "std"}},
                                mode);
        if (!diagnostics.HasErrors()) {
            flecha::semantic::SemanticAnalyzer analyzer(diagnostics);
            auto model = analyzer.Analyze(*unit);
        }
        std::string messages;
        for (const auto& diagnostic : diagnostics.Diagnostics())
            messages +=
                diagnostic.range.start.file + ":" + diagnostic.message + "\n";
        return messages;
    }
};
TEST_F(ModuleTests, ResolvesNestedPathsAliasesAndIdenticallyNamedModules) {
    Write("a/calc.fl", "module calc; export i32 value() { return 20; }");
    Write("b/calc.fl", "module calc; export i32 value() { return 22; }");
    Write("main.fl",
          "module main; @imports :: root::a::calc :: root::b::calc as other; "
          "i32 main() { return calc::value() + other::value(); }");
    EXPECT_EQ(Check(), "");
}
TEST_F(ModuleTests, LibrariesUseTheirOwnRootAndDiamondImportsShareIdentity) {
    Write("library/helper.fl",
          "module helper; export structure Box { i32 n; };");
    Write("library/one.fl",
          "module one; @imports :: root::helper; export helper::Box make() { "
          "return helper::Box::init(); }");
    Write("library/two.fl",
          "module two; @imports :: root::helper; export i32 read(&const "
          "helper::Box b) { return b->n; }");
    Write(
        "main.fl",
        "module main; @imports :: lib::fixture::one :: lib::fixture::two; i32 "
        "main() { b := one::make(); return two::read(&b); }");
    EXPECT_EQ(Check(), "");
}
TEST_F(ModuleTests, ExportedStructuresBundleMethodsButRespectPrivacy) {
    Write("box.fl",
          "module box; export structure Box { private i32 n; }; i32 "
          "Box::get(&const self) { return self->n; } private void "
          "Box::reset(&self) {} ");
    Write("main.fl",
          "module main; @imports :: root::box; i32 main() { box::Box b = "
          "box::Box::init(); return b.get(); }");
    EXPECT_EQ(Check(), "");
    Write("main.fl",
          "module main; @imports :: root::box; void main() { b := "
          "box::Box::init(); b.reset(); }");
    EXPECT_NE(Check().find("private"), std::string::npos);
    Write("main.fl",
          "module main; @imports :: root::box; i32 main() { b := "
          "box::Box::init(); return b.n; }");
    EXPECT_NE(Check().find("private"), std::string::npos);
}
TEST_F(ModuleTests, GenericDiagnosticsUseSourceTypeNames) {
    Write("array.fl",
          "module array; export structure Array<T> { T value; }; "
          "void Array<T>::set(&self, usize index, T value) {}");
    Write("main.fl",
          "module main; @imports :: root::array; void main() { "
          "values := array::Array<i32>::init(); values.set(values); }");
    const std::string errors = Check();
    EXPECT_NE(errors.find("expected 'usize', found 'Array<i32>'"),
              std::string::npos);
    EXPECT_EQ(errors.find("$specialization"), std::string::npos);
}
TEST_F(ModuleTests, ExportedGenericCallableSignaturesCrossModuleBoundaries) {
    Write("callbacks.fl",
          "module callbacks; export func Mapper<T>(T) => (T); export T "
          "apply<T>(Mapper<T> callback, T value) { return callback(value); }");
    Write("main.fl",
          "module main; @imports :: root::callbacks; i32 main() { "
          "callbacks::Mapper<i32> increment = (value) => { return value + 1; "
          "}; return callbacks::apply<i32>(increment, 41); }");
    EXPECT_EQ(Check(), "");
}
TEST_F(ModuleTests, GenericCallableExportsRespectTypeVisibility) {
    Write("callbacks.fl",
          "module callbacks; structure Hidden { i32 value; }; export func "
          "Transform<T>(Hidden, T) => (T);");
    Write("main.fl",
          "module main; @imports :: root::callbacks; void main() {}");
    EXPECT_NE(Check().find("cannot expose non-exported types"),
              std::string::npos);
}
TEST_F(ModuleTests, PrivateDeclarationsAndTransitiveImportsDoNotLeak) {
    Write("hidden.fl",
          "module hidden; i32 secret() { return 42; } structure Secret { i32 "
          "n; }; export i32 visible() { return secret(); }");
    Write("middle.fl",
          "module middle; @imports :: root::hidden; export i32 value() { "
          "return hidden::visible(); }");
    Write("main.fl",
          "module main; @imports :: root::hidden; i32 main() { return "
          "hidden::secret(); }");
    EXPECT_NE(Check().find("unknown name"), std::string::npos);
    Write("main.fl",
          "module main; @imports :: root::hidden; void main() { hidden::Secret "
          "s = hidden::Secret::init(); }");
    EXPECT_NE(Check().find("unknown type"), std::string::npos);
    Write("main.fl",
          "module main; @imports :: root::middle; i32 main() { return "
          "hidden::visible(); }");
    EXPECT_FALSE(Check().empty());
}
TEST_F(ModuleTests, RejectsCyclesMissingFilesAndModuleNameMismatches) {
    Write("main.fl", "module main; @imports :: root::other; void main() {}");
    EXPECT_NE(Check().find("cannot open module"), std::string::npos);
    Write("other.fl", "module wrong; export i32 value() { return 42; }");
    EXPECT_NE(Check().find("must match filename"), std::string::npos);
    Write("other.fl", "module other; @imports :: root::main;");
    EXPECT_NE(Check().find("import cycle"), std::string::npos);
    Write("other.fl", "export i32 value() { return 42; }");
    EXPECT_NE(Check().find("require a module declaration"), std::string::npos);
}
TEST_F(ModuleTests, RejectsDuplicateAliasesAndLocalNameCollisions) {
    Write("a.fl", "module a; export i32 value() { return 1; }");
    Write("main.fl",
          "module main; @imports :: root::a :: root::a; void main() {}");
    EXPECT_NE(Check().find("duplicate import alias"), std::string::npos);
    Write("main.fl",
          "module main; @imports :: root::a; void a() {} void main() {}");
    EXPECT_NE(Check().find("conflicts with declaration"), std::string::npos);
}
TEST_F(ModuleTests, ImportedOwnershipContractsRejectReuseAndReportSourceFiles) {
    Write("memory.fl",
          "module memory; export unsafe void consume(own *i32 p) { free(p); }");
    Write("main.fl",
          "module main; @imports :: root::memory; unsafe void main() { p,e := "
          "new<i32>(42); memory::consume(p); free(p); }");
    const auto errors = Check();
    EXPECT_NE(errors.find("ownership transfer"), std::string::npos);
    EXPECT_NE(errors.find((root / "main.fl").string()), std::string::npos);
    Write("memory.fl",
          "module memory; export unsafe void consume(own *i32 p) {}");
    EXPECT_NE(Check().find((root / "memory.fl").string()), std::string::npos);
}
TEST_F(ModuleTests, BorrowedResultContractsCrossModuleBoundaries) {
    Write("access.fl",
          "module access; export &const i32 view(&const i32 value) <- value "
          "{ return value; } export stringv text(stringv value) <- value { "
          "return value; }");
    Write("main.fl",
          "module main; @imports :: root::access; i32 main() { i32 value = "
          "42; result := access::view(&value); return *result; }");
    EXPECT_EQ(Check(), "");

    Write("main.fl",
          "module main; @imports :: root::access; i32 main() { i32 value = "
          "42; result := access::view(&value); value = 0; return *result; }");
    EXPECT_NE(Check().find("conflicts with live reference"), std::string::npos);

    Write("main.fl",
          "module main; @imports :: root::access; void main() { string value "
          "= \"flecha\"; result := access::text(value.view()); value = "
          "\"arrow\"; stdout(result); }");
    EXPECT_NE(Check().find("conflicts with live reference"), std::string::npos);
}
TEST_F(ModuleTests, ValidatesAndPrivilegesPairedTestModules) {
    Write("std/tests/testing.fl",
          "module testing; export structure UnitTest { usize failures; }; "
          "UnitTest UnitTest::init(stringv name, bool verbose) override { "
          "return build UnitTest { self.failures = 0; }; } void "
          "UnitTest::fail(&self) { self->failures++; } usize "
          "UnitTest::failureCount(&const self) { return self->failures; } "
          "bool UnitTest::failed(&const self) { return self->failures != 0; } "
          "void UnitTest::printOutput(&const self) {}");
    Write("math.fl",
          "module math; i32 secret() { return 42; } export structure Box { "
          "private i32 value; }; Box Box::init() override { return build Box "
          "{ self.value = 42; }; } private i32 Box::hidden(&const self) { "
          "return self->value; }");
    Write("main.fl",
          "module main; @imports :: root::math :: "
          "lib::std::tests::testing; void main() {}");
    EXPECT_EQ(Check(), "");

    Write("math_test.fl",
          "module math_test; @imports :: root::math :: "
          "lib::std::tests::testing; test private_access("
          "&testing::UnitTest t) { box := math::Box::init(); if "
          "math::secret() != box.hidden() { t->fail(); } }");
    fs::rename(root / "main.fl", root / "saved_main.fl");
    fs::rename(root / "math_test.fl", root / "main.fl");
    EXPECT_NE(Check().find("must match filename"), std::string::npos);
    fs::rename(root / "main.fl", root / "math_test.fl");
    fs::rename(root / "saved_main.fl", root / "main.fl");

    flecha::diagnostics::DiagnosticBag diagnostics;
    flecha::core::ModuleLoader loader(diagnostics);
    auto unit =
        loader.Load(root / "math_test.fl", root, {{"std", root / "std"}},
                    flecha::core::ModuleLoadMode::Test);
    for (const auto& diagnostic : diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(diagnostics.HasErrors());
    flecha::semantic::SemanticAnalyzer analyzer(diagnostics);
    auto model = analyzer.Analyze(*unit);
    ASSERT_TRUE(diagnostics.HasErrors());
    std::string messages;
    for (const auto& diagnostic : diagnostics.Diagnostics())
        messages += diagnostic.message + "\n";
    EXPECT_NE(messages.find("method '$m1$Box::hidden' is private"),
              std::string::npos);
}

TEST_F(ModuleTests, RejectsInvalidTestModulesAndSignatures) {
    Write("std/tests/testing.fl",
          "module testing; export structure UnitTest { usize failures; };");
    Write("math.fl", "module math; export i32 value() { return 42; }");
    Write("main.fl", "module main; test invalid(i32 value) {}");
    EXPECT_NE(Check().find("test must be declared"), std::string::npos);

    const auto analyzeTestModule = [&]() {
        flecha::diagnostics::DiagnosticBag diagnostics;
        flecha::core::ModuleLoader loader(diagnostics);
        auto unit =
            loader.Load(root / "math_test.fl", root, {{"std", root / "std"}},
                        flecha::core::ModuleLoadMode::Test);
        flecha::semantic::SemanticAnalyzer analyzer(diagnostics);
        if (!diagnostics.HasErrors()) (void)analyzer.Analyze(*unit);
        std::string messages;
        for (const auto& diagnostic : diagnostics.Diagnostics())
            messages += diagnostic.message + "\n";
        return messages;
    };

    Write("math_test.fl",
          "module math_test; @imports :: root::math :: "
          "lib::std::tests::testing; test invalid(testing::UnitTest t) {}");
    EXPECT_NE(analyzeTestModule().find("mutable '&testing::UnitTest'"),
              std::string::npos);

    Write("math_test.fl",
          "module math_test; @imports :: root::math :: "
          "lib::std::tests::testing; test extra(&testing::UnitTest t, i32 "
          "value) {}");
    EXPECT_NE(analyzeTestModule().find("exactly one"), std::string::npos);
}
TEST_F(ModuleTests, RequiresImportsBeforeDeclarationsAndRejectsWrongDirective) {
    Write("main.fl", "module main; void main() {} @imports :: root::a;");
    EXPECT_FALSE(Check().empty());
    Write("main.fl", "module main; @wrong :: root::a; void main() {}");
    EXPECT_FALSE(Check().empty());
}
}  // namespace

namespace {
TEST_F(ModuleTests, RejectsPrivateTypesInExportedApis) {
    Write("main.fl",
          "module main; structure Secret { i32 n; }; export Secret leak() { "
          "return Secret::init(); }");
    EXPECT_NE(Check().find("cannot expose non-exported types"),
              std::string::npos);
    Write("main.fl",
          "module main; error Secret { Failed } export Secret leak() { return "
          "Secret.Failed; }");
    EXPECT_NE(Check().find("cannot expose non-exported types"),
              std::string::npos);
    Write("main.fl",
          "module main; structure Secret { i32 n; }; export structure Public { "
          "Secret value; };");
    EXPECT_NE(Check().find("cannot expose non-exported types"),
              std::string::npos);
    Write("main.fl",
          "module main; structure Secret { i32 n; }; export structure Public { "
          "private Secret value; };");
    EXPECT_EQ(Check(), "");
}
TEST_F(ModuleTests, DistinguishesErrorFamiliesWithIdenticalNames) {
    Write("a.fl",
          "module a; export error Failure { Failed } export Failure value() { "
          "return Failure.Failed; }");
    Write("b.fl",
          "module b; export error Failure { Failed } export Failure value() { "
          "return Failure.Failed; }");
    Write("main.fl",
          "module main; @imports :: root::a :: root::b; void main() { "
          "a::Failure value = b::value(); }");
    EXPECT_FALSE(Check().empty());
}
TEST_F(ModuleTests, RejectsSourceRootEscapesAndConflictingRoots) {
    Write("outside.fl", "module outside; export i32 value() { return 1; }");
    fs::create_directories(root / "library");
    std::error_code error;
    fs::create_symlink(root / "outside.fl", root / "library/outside.fl", error);
    if (error) GTEST_SKIP() << "symbolic links are unavailable: " << error;
    Write("main.fl",
          "module main; @imports :: lib::fixture::outside; void main() {}");
    EXPECT_NE(Check().find("escapes its source root"), std::string::npos);
    Write("library/shared.fl",
          "module shared; export i32 value() { return 1; }");
    Write("main.fl",
          "module main; @imports :: root::library::shared :: "
          "lib::fixture::shared "
          "as other; void main() {}");
    EXPECT_NE(Check().find("conflicting source roots"), std::string::npos);
}
}  // namespace

namespace {
TEST_F(ModuleTests, ReportsEqualOffsetsInDifferentFilesIndependently) {
    Write("a.fl", "module a; export unsafe void leak(own *i32 p) {}");
    Write("b.fl", "module b; export unsafe void leak(own *i32 p) {}");
    Write("main.fl",
          "module main; @imports :: root::a :: root::b; void main() {}");
    const std::string errors = Check();
    EXPECT_NE(errors.find((root / "a.fl").string()), std::string::npos);
    EXPECT_NE(errors.find((root / "b.fl").string()), std::string::npos);
}
}  // namespace

namespace {
TEST_F(ModuleTests, SelectedModulesKeepNamespacesAndSkipOtherFiles) {
    Write("math/factorial.fl",
          "module factorial; export i32 value() { return 20; }");
    Write("math/division.fl",
          "module division; export i32 value() { return 22; }");
    Write("math/broken.fl", "invalid source");
    Write("main.fl",
          "module main; @imports :: root::math::{factorial, division}; "
          "i32 main() { return factorial::value() + division::value(); }");
    EXPECT_EQ(Check(), "");
    Write("main.fl",
          "module main; @imports :: root::math::{missing}; void main() {}");
    EXPECT_NE(Check().find("cannot open module"), std::string::npos);
}
TEST_F(ModuleTests, WildcardsDiscoverTypesAndOnlyImmediateModuleFiles) {
    Write("math/factorial.fl",
          "module factorial; export structure Box { i32 n; };");
    Write("math/division.fl",
          "module division; export i32 value() { return 42; }");
    Write("math/nested/broken.fl", "invalid source");
    Write("math/README.md", "not Flecha source");
    Write("main.fl",
          "module main; @imports :: root::math::*; "
          "i32 main() { factorial::Box b = factorial::Box::init(); return "
          "division::value(); }");
    EXPECT_EQ(Check(), "");
    Write("main.fl",
          "module main; @imports :: root::math::* :: root::math::division; "
          "void main() {}");
    EXPECT_NE(Check().find("duplicate import alias"), std::string::npos);
}
TEST_F(ModuleTests, LibraryWildcardsAndEmptyDirectories) {
    Write("library/a.fl", "module a; export i32 value() { return 42; }");
    Write("main.fl",
          "module main; @imports :: lib::fixture::*; i32 main() { return "
          "a::value(); }");
    EXPECT_EQ(Check(), "");
    fs::create_directories(root / "empty");
    Write("main.fl", "module main; @imports :: root::empty::*; void main() {}");
    EXPECT_EQ(Check(), "");
    Write("main.fl",
          "module main; @imports :: root::missing::*; void main() {}");
    EXPECT_NE(Check().find("cannot open module directory"), std::string::npos);
}
TEST_F(ModuleTests, WildcardsRejectInvalidNamesAndEscapingSymlinks) {
    Write("math/bad-name.fl", "module bad; void main() {}");
    Write("main.fl", "module main; @imports :: root::math::*; void main() {}");
    EXPECT_NE(Check().find("invalid module filename"), std::string::npos);
    fs::remove(root / "math/bad-name.fl");
    Write("outside.fl", "module outside; export i32 value() { return 42; }");
    fs::create_directories(root / "library");
    std::error_code error;
    fs::create_symlink(root / "outside.fl", root / "library/outside.fl", error);
    if (error) GTEST_SKIP() << "symbolic links are unavailable: " << error;
    Write("main.fl",
          "module main; @imports :: lib::fixture::*; void main() {}");
    EXPECT_NE(Check().find("escapes its source root"), std::string::npos);
}
TEST_F(ModuleTests, RejectsMalformedModuleSelections) {
    for (const std::string path :
         {"root::math::{}", "root::math::{a,}", "root::math::{root}", "lib::*",
          "root::* as everything"}) {
        Write("main.fl",
              "module main; @imports :: " + path + "; void main() {}");
        EXPECT_FALSE(Check().empty()) << path;
    }
}
}  // namespace

TEST_F(ModuleTests, GenericDefinitionsKeepPrivateHelpersAndCallerTypeIdentity) {
    Write("helper.fl", R"(module helper;
        i32 secret() { return 42; }
        export T identity<T>(T x) { secret(); return x; }
        export structure Box<T> { T value; };
        T Box<T>::get(&const self) { return self->value; }
    )");
    Write("main.fl", R"(module main; @imports :: root::helper;
        structure Private { i32 value; };
        i32 main() {
            Private p = helper::identity<Private>(Private::init());
            helper::Box<Private> b = helper::Box<Private>::init();
            Private q = b.get();
            return p.value + q.value;
        }
    )");
    EXPECT_EQ(Check(), "");
}

TEST_F(ModuleTests, GenericDiamondImportsShareSpecializationIdentity) {
    Write("base.fl", "module base; export structure Box<T> { T value; };");
    Write("one.fl",
          "module one; @imports :: root::base; export base::Box<T> make<T>() { "
          "return base::Box<T>::init(); }");
    Write("two.fl",
          "module two; @imports :: root::base; export T read<T>(&const "
          "base::Box<T> b) { return b->value; }");
    Write("main.fl",
          "module main; @imports :: root::one :: root::two; i32 main() { b := "
          "one::make<i32>(); return two::read<i32>(&b); }");
    EXPECT_EQ(Check(), "");
}

TEST_F(ModuleTests, GenericVisibilityIsCheckedAtDefinitionAndUse) {
    Write("helper.fl", "module helper; T secret<T>(T value) { return value; }");
    Write("main.fl",
          "module main; @imports :: root::helper; i32 main() { return "
          "helper::secret<i32>(42); }");
    EXPECT_NE(Check().find("module-private"), std::string::npos);
    Write("helper.fl",
          "module helper; structure Secret { i32 value; }; export Secret "
          "reveal<T>(T value) { return Secret::init(); }");
    Write("main.fl", "module main; @imports :: root::helper; void main() {}");
    EXPECT_NE(Check().find("non-exported"), std::string::npos);
}

TEST_F(ModuleTests, OrdinaryExportsCanExposePublicSpecializations) {
    Write("box.fl", R"(module box;
        export structure Box<T> { T value; };
        export Box<i32> make() { return Box<i32>::init(); }
    )");
    Write("main.fl",
          "module main; @imports :: root::box; i32 main() { box::Box<i32> b = "
          "box::make(); return b.value; }");
    EXPECT_EQ(Check(), "");
    Write("box.fl", R"(module box;
        structure Secret { i32 value; };
        export structure Box<T> { T value; };
        export Box<Secret> make() { return Box<Secret>::init(); }
    )");
    EXPECT_NE(Check().find("private"), std::string::npos);
}

TEST_F(ModuleTests, PublicGenericMethodsAcceptCallerPrivateArguments) {
    Write("box.fl", R"(module box;
        export structure Box { i32 value; };
        U Box::identity<U>(&const self, U value) { return value; }
    )");
    Write("main.fl", R"(module main; @imports :: root::box;
        structure Private { i32 value; };
        i32 main() {
            b := box::Box::init();
            p := b.identity<Private>(Private::init());
            return p.value;
        }
    )");
    EXPECT_EQ(Check(), "");
}

TEST_F(ModuleTests, MoveOnlyValuesTransferAcrossModuleBoundaries) {
    Write("box.fl", R"(module box;
        export structure Box { i32 value; };
        void Box::tear(&self) override {}
        export Box pass(Box value) { return value; }
    )");
    Write("main.fl", R"(module main; @imports :: root::box;
        i32 main() {
            first := box::Box::init();
            second := box::pass(first);
            first = box::Box::init();
            return first.value + second.value;
        }
    )");
    EXPECT_EQ(Check(), "");
}

TEST_F(ModuleTests, QuiverDeclarationsReachSiblingModulesAndTheirMembers) {
    Write("api.fl", R"(module api;
        quiver i32 value() { return 40; }
        quiver structure Box { i32 number; };
        Box Box::init() override { return build Box { self.number = 1; }; }
        i32 Box::read(&const self) { return self->number; }
        quiver enum State { On, Off }
        quiver error Failure { Failed }
        quiver func Mapper(i32) => (i32);
    )");
    Write("main.fl", R"(module main; @imports :: root::api;
        i32 main() {
            box := api::Box::init();
            api::State state = api::State.On;
            error failure = api::Failure.Failed;
            api::Mapper mapper = (value) => { return value + 1; };
            return api::value() + box.number + box.read() + mapper(0);
        }
    )");
    EXPECT_EQ(Check(), "");
}

TEST_F(ModuleTests, QuiverBoundariesRejectParentNestedAndLibraryModules) {
    Write("pack/api.fl", "module api; quiver i32 value() { return 42; }");
    Write("main.fl", R"(module main; @imports :: root::pack::api;
        i32 main() { return api::value(); }
    )");
    std::string errors = Check();
    EXPECT_NE(errors.find("'api::value' is quiver-visible"), std::string::npos);
    EXPECT_NE(errors.find("modules in 'root::pack'"), std::string::npos);
    EXPECT_EQ(errors.find("unknown name"), std::string::npos);

    Write("pack/nested/main.fl", R"(module main;
        @imports :: root::pack::api;
        i32 main() { return api::value(); }
    )");
    errors = CheckAt("pack/nested/main.fl");
    EXPECT_NE(errors.find("'api::value' is quiver-visible"), std::string::npos);

    Write("library/api.fl", "module api; quiver i32 value() { return 42; }");
    Write("main.fl", R"(module main; @imports :: lib::fixture::api;
        i32 main() { return api::value(); }
    )");
    errors = Check();
    EXPECT_NE(errors.find("'api::value' is quiver-visible"), std::string::npos);
    EXPECT_NE(errors.find("modules in 'lib::fixture'"), std::string::npos);

    Write("library/shared/api.fl",
          "module api; quiver i32 value() { return 42; }");
    Write("shared/main.fl", R"(module main;
        @imports :: lib::fixture::shared::api;
        i32 main() { return api::value(); }
    )");
    errors = CheckAt("shared/main.fl");
    EXPECT_NE(errors.find("'api::value' is quiver-visible"), std::string::npos);
    EXPECT_NE(errors.find("modules in 'lib::fixture::shared'"),
              std::string::npos);
}

TEST_F(ModuleTests, FullQuiverWildcardDoesNotBypassVisibility) {
    Write("pack/hidden.fl", "module hidden; quiver i32 value() { return 42; }");
    Write("pack/public.fl", "module public; export i32 value() { return 1; }");
    Write("main.fl", R"(module main; @imports :: root::pack::*;
        i32 main() { return hidden::value() + public::value(); }
    )");
    const std::string errors = Check();
    EXPECT_NE(errors.find("'hidden::value' is quiver-visible"),
              std::string::npos);
    EXPECT_EQ(errors.find("public::value"), std::string::npos);
}

TEST_F(ModuleTests, LibraryModulesShareOnlyTheirLibraryQuiver) {
    Write("library/secret.fl",
          "module secret; quiver i32 value() { return 42; }");
    Write("library/facade.fl", R"(module facade;
        @imports :: root::secret;
        export i32 value() { return secret::value(); }
    )");
    Write("main.fl", R"(module main; @imports :: lib::fixture::facade;
        i32 main() { return facade::value(); }
    )");
    EXPECT_EQ(Check(), "");

    Write("main.fl", R"(module main;
        @imports :: lib::fixture::facade :: lib::mirror::secret;
        i32 main() { return facade::value() + secret::value(); }
    )");
    const std::string errors = Check();
    EXPECT_NE(errors.find("modules in 'lib::mirror'"), std::string::npos);
}

TEST_F(ModuleTests, QuiverApisEnforceNominalTypeExposure) {
    Write("main.fl", R"(module main;
        quiver structure Shared { i32 value; };
        structure Hidden { i32 value; };
        quiver Shared accepted(Shared value) { return value; }
    )");
    EXPECT_EQ(Check(), "");

    Write("main.fl", R"(module main;
        quiver structure Shared { i32 value; };
        export Shared leaked(Shared value) { return value; }
    )");
    EXPECT_NE(Check().find("exported APIs cannot expose non-exported types"),
              std::string::npos);

    Write("main.fl", R"(module main;
        structure Hidden { i32 value; };
        quiver Hidden leaked(Hidden value) { return value; }
    )");
    EXPECT_NE(Check().find("quiver APIs cannot expose module-private types"),
              std::string::npos);

    Write("main.fl", R"(module main;
        structure Hidden { i32 value; };
        quiver structure Shared { Hidden value; };
    )");
    EXPECT_NE(Check().find("quiver APIs cannot expose module-private types"),
              std::string::npos);
}

TEST_F(ModuleTests, OwnerVisibilityControlsMethodApiExposure) {
    Write("main.fl", R"(module main;
        structure Hidden { i32 value; };
        quiver structure Shared { i32 value; };
        Hidden Shared::leak(&const self) { return Hidden::init(); }
    )");
    EXPECT_NE(Check().find("quiver APIs cannot expose module-private types"),
              std::string::npos);

    Write("main.fl", R"(module main;
        structure Hidden { i32 value; };
        quiver structure Shared { private Hidden value; };
        private Hidden Shared::hidden(&const self) { return self->value; }
    )");
    EXPECT_EQ(Check(), "");
}

TEST_F(ModuleTests, QuiverGenericDeclarationsRespectInstantiationBoundary) {
    Write("generic.fl", R"(module generic;
        quiver T identity<T>(T value) { return value; }
        quiver structure Box<T> { T value; };
        quiver func Mapper<T>(T) => (T);
    )");
    Write("main.fl", R"(module main; @imports :: root::generic;
        i32 main() {
            box := generic::Box<i32>::init();
            generic::Mapper<i32> mapper = (value) => { return value; };
            return generic::identity<i32>(mapper(box.value));
        }
    )");
    EXPECT_EQ(Check(), "");

    Write("nested/main.fl", R"(module main; @imports :: root::generic;
        i32 main() { return generic::identity<i32>(42); }
    )");
    const std::string errors = CheckAt("nested/main.fl");
    EXPECT_NE(errors.find("'generic::identity' is quiver-visible"),
              std::string::npos);

    Write("main.fl", R"(module main;
        quiver structure Shared { i32 value; };
        export structure Box<T> { T value; };
        export Box<Shared> leak() { return Box<Shared>::init(); }
    )");
    EXPECT_NE(Check().find("exported APIs cannot expose non-exported types"),
              std::string::npos);
}

TEST_F(ModuleTests, RejectsVisibilityModifiersOnMemberFunctions) {
    Write("main.fl", R"(module main;
        quiver structure Box { i32 value; };
        quiver i32 Box::value(&const self) { return self->value; }
    )");
    const std::string errors = Check();
    EXPECT_NE(errors.find("member functions cannot be marked quiver"),
              std::string::npos);
    EXPECT_NE(errors.find("visibility comes from the owning type"),
              std::string::npos);
}

TEST_F(ModuleTests, PrivateMembersRequireTheExactOwningType) {
    Write("main.fl", R"(module main;
        structure First { private i32 value; };
        private i32 First::hidden(&const self) { return self->value; }
        structure Second { i32 value; };
        i32 Second::read(&const self, &const First other) {
            return other->value + other->hidden();
        }
    )");
    const std::string errors = Check();
    EXPECT_NE(errors.find("field 'value' is private"), std::string::npos);
    EXPECT_NE(errors.find("hidden' is private"), std::string::npos);
}
