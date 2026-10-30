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
    std::string CheckGraph(const std::string& graph,
                           const std::string& entry = "app/main.fl",
                           flecha::core::ModuleLoadMode mode =
                               flecha::core::ModuleLoadMode::Runtime) {
        Write("target/package-graph.json", graph);
        std::string error;
        const auto packages = flecha::core::PackageGraph::Load(
            root / "target/package-graph.json", error);
        if (!packages) return "package graph error: " + error;
        flecha::diagnostics::DiagnosticBag diagnostics;
        flecha::core::ModuleLoader loader(diagnostics);
        auto unit = loader.Load(root / entry, *packages, mode);
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

TEST_F(ModuleTests, KeepsImportedLifecycleWarningsLocalAndNamesReadable) {
    Write("dependency.fl", R"(module dependency;
        export structure Imported { i32 value; };
        Imported Imported::zero() override {
            return build Imported { self.value = 1; };
        }
    )");
    Write("main.fl", R"(module main;
        @imports :: root::dependency;
        structure Local { i32 value; };
        Local Local::zero() override {
            return build Local { self.value = 1; };
        }
        void main() {}
    )");

    const std::string messages = Check();
    EXPECT_NE(messages.find("structure 'Local' is not trivially zeroed"),
              std::string::npos);
    EXPECT_EQ(messages.find("structure 'Imported' is not trivially zeroed"),
              std::string::npos);
    EXPECT_EQ(messages.find("$m"), std::string::npos);
}

TEST_F(ModuleTests, NominalTypeDiagnosticsNeverExposeInternalModuleNames) {
    Write("dependency.fl", R"(module dependency;
        export structure Record { i32 value; };
        export enum State { default Ready, }
        export error Failure { Broken }
    )");
    Write("main.fl", R"(module main;
        @imports :: root::dependency;
        void main() {
            dependency::Record record = 1;
            dependency::State state = 1;
            dependency::Failure failure = 1;
        }
    )");

    const std::string messages = Check();
    EXPECT_NE(messages.find("Record"), std::string::npos);
    EXPECT_NE(messages.find("State"), std::string::npos);
    EXPECT_NE(messages.find("Failure"), std::string::npos);
    EXPECT_EQ(messages.find("$m"), std::string::npos);
}
TEST_F(ModuleTests, ComposesLogicalModulesAcrossArbitraryFilenames) {
    Write("first_piece.fl", R"(module shared;
        i32 left() { return right(); }
        export i32 answer() { return left(); }
    )");
    Write("second_piece.fl", R"(module shared;
        i32 right() { return 42; }
    )");
    Write("main.fl", R"(module main; @imports :: root::shared;
        i32 main() { return shared::answer(); }
    )");
    EXPECT_EQ(Check(), "");

    Write("main.fl", R"(module main; @imports :: root::first_piece;
        void main() {}
    )");
    EXPECT_NE(Check().find("cannot open module 'first_piece'"),
              std::string::npos);
}

TEST_F(ModuleTests, ExportsVariadicForwardingFunctionsAcrossModules) {
    Write("output.fl", R"(module output;
        export void print(values...) { stdout(values...); }
    )");
    Write("main.fl", R"(module main; @imports :: root::output;
        i32 main() { output::print("answer: ", 42); return 0; }
    )");

    EXPECT_EQ(Check(), "");
}

TEST_F(ModuleTests, GivesAnonymousFunctionsUniqueNamesAcrossSourceUnits) {
    Write("first_piece.fl", R"(module shared;
        func Reader() => (i32);
        export i32 first() {
            Reader read = []() => (i32) { return 20; };
            return read();
        }
    )");
    Write("second_piece.fl", R"(module shared;
        export i32 second() {
            Reader read = []() => (i32) { return 22; };
            return read();
        }
    )");
    Write("main.fl", R"(module main; @imports :: root::shared;
        i32 main() { return shared::first() + shared::second(); }
    )");
    EXPECT_EQ(Check(), "");
}

TEST_F(ModuleTests, SharesModuleConstantsAcrossFilesAndImportsExports) {
    Write("constant_values.fl", R"(module constants;
        const i32 BASE = 40;
        export const i32 PUBLIC = BASE + 2;
    )");
    Write("constant_api.fl", R"(module constants;
        export i32 answer() { return BASE + 2; }
    )");
    Write("main.fl", R"(module main; @imports :: root::constants;
        i32 main() { return constants::answer() + constants::PUBLIC - 42; }
    )");
    EXPECT_EQ(Check(), "");

    Write("main.fl", R"(module main; @imports :: root::constants;
        i32 main() { return constants::BASE; }
    )");
    EXPECT_NE(Check().find("unknown name 'constants::BASE'"),
              std::string::npos);
}
TEST_F(ModuleTests, ModuleConstantVisibilityCannotLeakPrivateTypes) {
    Write("constants.fl", R"(module constants;
        enum State { Ready }
        export const State DEFAULT_STATE = State.Ready;
    )");
    Write("main.fl", R"(module main; @imports :: root::constants;
        void main() {}
    )");
    EXPECT_NE(Check().find("exported APIs cannot expose non-exported types"),
              std::string::npos);
}
TEST_F(ModuleTests, KeepsImportsLocalToTheirSourceUnit) {
    Write("dependency.fl",
          "module dependency; export i32 value() { return 42; }");
    Write("one.fl", R"(module shared; @imports :: root::dependency;
        export i32 one() { return dependency::value(); }
    )");
    Write("two.fl", R"(module shared;
        export i32 two() { return dependency::value(); }
    )");
    Write("main.fl", R"(module main; @imports :: root::shared;
        i32 main() { return shared::one() + shared::two(); }
    )");
    const std::string missingImport = Check();
    EXPECT_NE(missingImport.find("unknown"), std::string::npos)
        << missingImport;
    EXPECT_NE(missingImport.find((root / "two.fl").string()),
              std::string::npos);

    Write("two.fl", R"(module shared; @imports :: root::dependency;
        export i32 two() { return dependency::value(); }
    )");
    EXPECT_EQ(Check(), "");
}
TEST_F(ModuleTests, OverlapsSameNamedImportsAndDiagnosesOnlyCollisions) {
    Write("first/implementation.fl", R"(module errors;
        export i32 parse() { return 20; }
        export i32 collision() { return 1; }
        export T parse_value<T>(T value) { return value; }
        export T select<T>(T value) { return value; }
    )");
    Write("second/implementation.fl", R"(module errors;
        export i32 network() { return 22; }
        export i32 collision() { return 2; }
        export T network_value<T>(T value) { return value; }
        export T select<T>(T value) { return value; }
    )");
    Write("main.fl", R"(module main;
        @imports :: root::first::errors :: root::second::errors;
        i32 main() { return errors::parse() + errors::network()
            + errors::parse_value<i32>(0) + errors::network_value<i32>(0); }
    )");
    EXPECT_EQ(Check(), "");

    Write("main.fl", R"(module main;
        @imports :: root::first::errors :: root::second::errors;
        i32 main() { return errors::collision(); }
    )");
    const std::string ambiguous = Check();
    EXPECT_NE(ambiguous.find("ambiguous imported name 'errors::collision'"),
              std::string::npos);
    EXPECT_NE(ambiguous.find((root / "first/implementation.fl").string()),
              std::string::npos);
    EXPECT_NE(ambiguous.find((root / "second/implementation.fl").string()),
              std::string::npos);

    Write("main.fl", R"(module main;
        @imports :: root::first::errors :: root::second::errors;
        i32 main() { return errors::select<i32>(42); }
    )");
    EXPECT_NE(Check().find("ambiguous imported generic name 'errors::select'"),
              std::string::npos);

    Write("main.fl", R"(module main;
        @imports :: root::first::errors as first
                 :: root::second::errors as second;
        i32 main() { return first::collision() + second::collision(); }
    )");
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
TEST_F(ModuleTests, PackageGraphResolvesRecursivePackageLocalAliasesAndRoots) {
    Write("app/main.fl", R"(module main;
        @imports :: lib::middle::api :: lib::std::marker;
        i32 main() { return api::answer() + marker::offset(); }
    )");
    Write("middle/api.fl", R"(module api;
        @imports :: root::helper :: lib::leaf::value;
        export i32 answer() { return helper::base() + value::rest(); }
    )");
    Write("middle/helper.fl",
          "module helper; export i32 base() { return 19; }");
    Write("leaf/value.fl", "module value; export i32 rest() { return 22; }");
    Write("std/marker.fl", "module marker; export i32 offset() { return 1; }");
    EXPECT_EQ(CheckGraph(R"({
        "schema": 1,
        "root": "workspace",
        "packages": [
            {"id":"workspace", "identity":"local/app", "source":"../app",
             "dependencies":{"middle":"middle@1", "std":"toolchain-std"}},
            {"id":"middle@1", "identity":"example/middle", "version":"1.0.0",
             "source":"../middle", "checksum":"sha256:middle",
             "dependencies":{"leaf":"leaf@2"}},
            {"id":"leaf@2", "identity":"example/leaf", "version":"2.0.0",
             "source":"../leaf", "dependencies":{}},
            {"id":"toolchain-std", "identity":"flecha/std", "source":"../std",
             "flecha":">=0.3.0, <0.4.0", "dependencies":{}}
        ]
    })"),
              "");
}
TEST_F(ModuleTests,
       PackageGraphIsolatesRepeatedAliasesModulesAndGenericSpecializations) {
    Write("app/main.fl", R"(module main;
        @imports :: lib::first::use as first :: lib::second::use as second;
        i32 main() { return first::answer() + second::answer(); }
    )");
    Write("first/use.fl", R"(module use; @imports :: lib::common::model;
        export i32 answer() {
            value := model::make<i32>(20);
            return model::read<i32>(&value);
        }
    )");
    Write("second/use.fl", R"(module use; @imports :: lib::common::model;
        export i32 answer() {
            value := model::make<i32>(22);
            return model::read<i32>(&value);
        }
    )");
    Write("left/model.fl", R"(module model;
        export structure Box<T> { T value; };
        export Box<T> make<T>(T value) {
            result := Box<T>::init(); result.value = value; return result;
        }
        export T read<T>(&const Box<T> value) { return value->value; }
    )");
    Write("right/model.fl", R"(module model;
        export structure Box<T> { T value; };
        export Box<T> make<T>(T value) {
            result := Box<T>::init(); result.value = value; return result;
        }
        export T read<T>(&const Box<T> value) { return value->value; }
    )");
    EXPECT_EQ(CheckGraph(R"({
        "schema":1, "root":"app",
        "packages":[
            {"id":"app", "identity":"local/app", "source":"../app",
             "dependencies":{"first":"first", "second":"second"}},
            {"id":"first", "identity":"owner/first", "source":"../first",
             "dependencies":{"common":"left"}},
            {"id":"second", "identity":"owner/second", "source":"../second",
             "dependencies":{"common":"right"}},
            {"id":"left", "identity":"one/common", "source":"../left",
             "dependencies":{}},
            {"id":"right", "identity":"two/common", "source":"../right",
             "dependencies":{}}
        ]
    })"),
              "");
}
TEST_F(ModuleTests, PackageGraphDiagnosesAliasesRelativeToImportingPackage) {
    Write("app/main.fl", R"(module main; @imports :: lib::middle::api;
        i32 main() { return api::answer(); }
    )");
    Write("middle/api.fl", R"(module api; @imports :: lib::missing::value;
        export i32 answer() { return value::answer(); }
    )");
    const std::string errors = CheckGraph(R"({
        "schema":1, "root":"app",
        "packages":[
            {"id":"app", "identity":"local/app", "source":"../app",
             "dependencies":{"middle":"middle"}},
            {"id":"middle", "identity":"owner/middle", "source":"../middle",
             "dependencies":{}}
        ]
    })");
    EXPECT_NE(
        errors.find("package 'owner/middle' has no dependency alias 'missing'"),
        std::string::npos)
        << errors;
}
TEST_F(ModuleTests, PackageGraphDiagnosesCyclesAndMalformedGraphs) {
    const std::string cycle = CheckGraph(R"({
        "schema":1, "root":"app",
        "packages":[
            {"id":"app", "identity":"local/app", "source":"../app",
             "dependencies":{"dependency":"dependency"}},
            {"id":"dependency", "identity":"owner/dependency",
             "source":"../dependency", "dependencies":{"app":"app"}}
        ]
    })");
    EXPECT_NE(cycle.find("package graph cycle: app -> dependency -> app"),
              std::string::npos)
        << cycle;

    const std::string missing = CheckGraph(R"({
        "schema":1, "root":"app",
        "packages":[
            {"id":"app", "identity":"local/app", "source":"../app",
             "dependencies":{"missing":"absent"}}
        ]
    })");
    EXPECT_NE(missing.find("references missing node 'absent'"),
              std::string::npos)
        << missing;

    const std::string invalidAlias = CheckGraph(R"({
        "schema":1, "root":"app",
        "packages":[
            {"id":"app", "identity":"local/app", "source":"../app",
             "dependencies":{"if":"app"}}
        ]
    })");
    EXPECT_NE(invalidAlias.find("invalid dependency alias 'if'"),
              std::string::npos)
        << invalidAlias;

    const std::string unsupported = CheckGraph(R"({
        "schema":2, "root":"app", "packages":[]
    })");
    EXPECT_NE(unsupported.find("unsupported package graph schema 2"),
              std::string::npos)
        << unsupported;

    const std::string malformed = CheckGraph("{\"schema\":1,", "app/main.fl");
    EXPECT_NE(malformed.find("package graph error: line 1"), std::string::npos)
        << malformed;
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
TEST_F(ModuleTests,
       ContextuallyInfersConstNominalParametersInsideGenericFunctions) {
    Write("adapter.fl", R"(module adapter;
        structure Marker {};
        export func Action<T>(&const T) => ();
        func PairAction<K, V>(&const K, &const V) => ();

        void consume<K, V>(PairAction<K, V> action) {}

        export void adapt<T>(Action<T> action) {
            consume<T, Marker>([&action](key, marker) => {
                action(key);
            });
        }
    )");
    Write("main.fl", R"(module main; @imports :: root::adapter;
        void main() {
            adapter::adapt<i32>((value) => {});
        }
    )");
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
TEST_F(ModuleTests, TypeSetsFollowModuleAndQuiverVisibility) {
    Write("sets.fl", R"(module sets;
        export structure UserId { i32 value; };
        export typeset identifiers { integer, UserId, };
        export T identity<T: identifiers>(T value) { return value; }
    )");
    Write("main.fl", R"(module main; @imports :: root::sets;
        T local_identity<T: sets::identifiers>(T value) { return value; }
        i32 main() {
            local_identity<i32>(1);
            return sets::identity<i32>(42);
        }
    )");
    EXPECT_EQ(Check(), "");

    Write("sets.fl", R"(module sets;
        typeset hidden { integer, };
        export T identity<T: hidden>(T value) { return value; }
    )");
    EXPECT_NE(
        Check().find("exported generics cannot use non-exported type sets"),
        std::string::npos);

    Write("sets.fl", R"(module sets;
        structure Hidden { i32 value; };
        export typeset leaked { Hidden, };
    )");
    EXPECT_NE(
        Check().find("visible type sets cannot include less-visible types"),
        std::string::npos);

    Write("sets.fl", R"(module sets;
        quiver typeset nearby { integer, };
    )");
    Write("main.fl", R"(module main; @imports :: root::sets;
        T identity<T: sets::nearby>(T value) { return value; }
        i32 main() { return identity<i32>(42); }
    )");
    EXPECT_EQ(Check(), "");

    Write("nested/main.fl", R"(module main; @imports :: root::sets;
        T identity<T: sets::nearby>(T value) { return value; }
        i32 main() { return identity<i32>(42); }
    )");
    EXPECT_NE(
        CheckAt("nested/main.fl").find("'sets::nearby' is quiver-visible"),
        std::string::npos);
}
TEST_F(ModuleTests, ModuleNamesDoNotShadowBuiltinTypeSets) {
    Write("integer.fl", R"(module integer;
        export structure Marker { i32 value; };
        export i32 answer() { return 42; }
    )");
    Write("main.fl", R"(module main; @imports :: root::integer;
        typeset values { integer, integer::Marker, };

        T constrained<T: integer>(T value) { return value; }
        T accepted<T: values>(T value) { return value; }
        bool is_integer<T>() { return T.isof<integer>; }

        i32 main() {
            integer::Marker marker = integer::Marker::init();
            accepted<integer::Marker>(marker);
            if !is_integer<i32>() { return 1; }
            if !3.isof<integer> { return 2; }
            return integer::answer() + constrained<i32>(0);
        }
    )");

    EXPECT_EQ(Check(), "");
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
TEST_F(ModuleTests, RejectsCyclesAndMissingDeclaredModules) {
    Write("main.fl", "module main; @imports :: root::other; void main() {}");
    EXPECT_NE(Check().find("cannot open module"), std::string::npos);
    Write("other.fl", "module wrong; export i32 value() { return 42; }");
    EXPECT_NE(Check().find("cannot open module 'other'"), std::string::npos);
    Write("main.fl", R"(module main; @imports :: root::wrong;
        i32 main() { return wrong::value(); }
    )");
    EXPECT_EQ(Check(), "");
    Write("main.fl", "module main; @imports :: root::other; void main() {}");
    Write("other.fl", "module other; @imports :: root::main;");
    EXPECT_NE(Check().find("import cycle"), std::string::npos);
    Write("other.fl", "export i32 value() { return 42; }");
    EXPECT_NE(Check().find("cannot open module 'other'"), std::string::npos);
}
TEST_F(ModuleTests, RejectsDuplicateAliasesAndLocalNameCollisions) {
    Write("a.fl", "module a; export i32 value() { return 1; }");
    Write("main.fl",
          "module main; @imports :: root::a :: root::a; i32 main() { return "
          "a::value(); }");
    EXPECT_EQ(Check(), "");
    Write("main.fl",
          "module main; @imports :: root::a as same :: root::a as same; "
          "void main() {}");
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
    fs::rename(root / "math_test.fl", root / "arbitrary_cases.fl");

    flecha::diagnostics::DiagnosticBag diagnostics;
    flecha::core::ModuleLoader loader(diagnostics);
    auto unit =
        loader.Load(root / "arbitrary_cases.fl", root, {{"std", root / "std"}},
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
TEST_F(ModuleTests, AggregatesTestsFromMultipleArbitrarilyNamedSources) {
    Write("std/tests/testing.fl",
          "module testing; export structure UnitTest { usize failures; }; "
          "UnitTest UnitTest::init(stringv name, bool verbose) override { "
          "return build UnitTest { self.failures = 0; }; } void "
          "UnitTest::fail(&self) { self->failures++; } bool "
          "UnitTest::failed(&const self) { return self->failures != 0; } "
          "void UnitTest::printOutput(&const self) {}");
    Write("implementation.fl", R"(module math;
        i32 secret() { return 42; }
    )");
    Write("arithmetic_cases.fl", R"(module math_test;
        @imports :: root::math :: lib::std::tests::testing;
        test arithmetic(&testing::UnitTest t) {
            if math::secret() != 42 { t->fail(); }
        }
    )");
    Write("boundary_cases.fl", R"(module math_test;
        @imports :: root::math :: lib::std::tests::testing;
        test boundary(&testing::UnitTest t) {
            if math::secret() < 0 { t->fail(); }
        }
    )");
    EXPECT_EQ(CheckAt("boundary_cases.fl", flecha::core::ModuleLoadMode::Test),
              "");
}
TEST_F(ModuleTests, RejectsDuplicateDeclarationsAndMixedModulePrivacy) {
    Write("one.fl", "module shared; i32 value() { return 1; }");
    Write("two.fl", "module shared; i32 value() { return 2; }");
    Write("main.fl", "module main; @imports :: root::shared; void main() {}");
    const std::string duplicate = Check();
    EXPECT_NE(duplicate.find("duplicate"), std::string::npos) << duplicate;

    Write("two.fl", "private module shared; i32 other() { return 2; }");
    EXPECT_NE(Check().find("mixes ordinary and private module declarations"),
              std::string::npos);
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
          "i32 main() { return division::value(); }");
    EXPECT_EQ(Check(), "");
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
TEST_F(ModuleTests, WildcardsUseDeclaredNamesAndRejectEscapingSymlinks) {
    Write("math/bad-name.fl", "module bad; export i32 value() { return 42; }");
    Write("main.fl", "module main; @imports :: root::math::*; void main() {}");
    EXPECT_EQ(Check(), "");
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

TEST_F(ModuleTests, PrivateModulesAreImportableOnlyWithinTheirQuiver) {
    Write("pack/api.fl", R"(private module api;
        export structure Answer { i32 value; };
        Answer Answer::init() override {
            return build Answer { self.value = 42; };
        }
        export Answer answer() { return Answer::init(); }
        export T identity<T>(T value) { return value; }
    )");
    Write("pack/main.fl", R"(module main; @imports :: root::pack::api;
        i32 main() {
            return api::identity<api::Answer>(api::answer()).value;
        }
    )");
    EXPECT_NE(CheckAt("pack/main.fl").find("'private module' is deprecated"),
              std::string::npos);

    Write("main.fl", R"(module main; @imports :: root::pack::api;
        i32 main() { return api::answer().value; }
    )");
    const std::string errors = Check();
    EXPECT_NE(errors.find("private module 'api'"), std::string::npos);
    EXPECT_NE(errors.find("modules in quiver 'root::pack'"), std::string::npos);
}

TEST_F(ModuleTests, PrivateModuleExportsRemainGloballyClassified) {
    Write("main.fl", R"(private module main;
        structure Hidden { i32 value; };
        export Hidden leak(Hidden value) { return value; }
    )");
    const std::string errors = Check();
    EXPECT_NE(errors.find("exported APIs cannot expose non-exported types"),
              std::string::npos);
    EXPECT_NE(errors.find("'private module' is deprecated"), std::string::npos);
}

TEST_F(ModuleTests, WildcardsAndCachedImportsRespectPrivateModules) {
    Write("pack/api.fl",
          "private module api; export i32 value() { return 42; }");
    Write("pack/facade.fl", R"(module facade; @imports :: root::pack::api;
        export i32 value() { return api::value(); }
    )");
    Write("main.fl", R"(module main;
        @imports :: root::pack::facade :: root::pack::api;
        i32 main() { return facade::value(); }
    )");
    EXPECT_NE(Check().find("private module 'api'"), std::string::npos);

    Write("main.fl", R"(module main; @imports :: root::pack::*;
        i32 main() { return facade::value(); }
    )");
    EXPECT_NE(Check().find("private module 'api'"), std::string::npos);
}

TEST_F(ModuleTests, PrivateModulesDoNotCrossSourceIdentities) {
    Write("library/shared/api.fl",
          "private module api; export i32 value() { return 42; }");
    Write("shared/main.fl", R"(module main;
        @imports :: lib::fixture::shared::api;
        i32 main() { return api::value(); }
    )");
    const std::string errors = CheckAt("shared/main.fl");
    EXPECT_NE(errors.find("private module 'api'"), std::string::npos);
    EXPECT_NE(errors.find("quiver 'lib::fixture::shared'"), std::string::npos);
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
            return generic::identity(mapper(box.value));
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
