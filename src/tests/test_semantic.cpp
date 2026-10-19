#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "semantic/scope.hpp"
#include "semantic/semantic_analyzer.hpp"

using namespace flecha::core;
using namespace flecha::diagnostics;
using namespace flecha::semantic;
using namespace flecha::source;

namespace {

struct Analysis final {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit;
    SemanticModel model;
};

Analysis Analyze(const std::string& source) {
    Analysis result;
    Tokenizer tokenizer(source, result.diagnostics);
    std::vector<Token> tokens = tokenizer.Tokenize();
    Parser parser(std::move(tokens), result.diagnostics);
    result.unit = parser.Parse();
    SemanticAnalyzer analyzer(result.diagnostics);
    result.model = analyzer.Analyze(*result.unit);
    return result;
}

const Diagnostic* FindDiagnostic(const Analysis& analysis,
                                 DiagnosticCode code) {
    for (const Diagnostic& diagnostic : analysis.diagnostics.Diagnostics()) {
        if (diagnostic.code == code) return &diagnostic;
    }
    return nullptr;
}

const Diagnostic* FindErrorDiagnostic(const Analysis& analysis,
                                      DiagnosticCode code) {
    for (const Diagnostic& diagnostic : analysis.diagnostics.Diagnostics()) {
        if (diagnostic.code == code &&
            diagnostic.severity == DiagnosticSeverity::Error)
            return &diagnostic;
    }
    return nullptr;
}

const FunctionDeclaration& FunctionAt(const Analysis& analysis,
                                      std::size_t index) {
    return static_cast<const FunctionDeclaration&>(
        *analysis.unit->Declarations()[index]);
}

const VariableDeclarationStatement& VariableAt(
    const FunctionDeclaration& function, std::size_t index) {
    return static_cast<const VariableDeclarationStatement&>(
        *function.Body().Statements()[index]);
}

}  // namespace

TEST(SemanticTypeTests, RepresentsEverySyntaxPrimitive) {
    EXPECT_EQ(SemanticType::FromSyntax(PrimitiveType::I8).Name(), "i8");
    EXPECT_EQ(SemanticType::FromSyntax(PrimitiveType::I16).Name(), "i16");
    EXPECT_EQ(SemanticType::FromSyntax(PrimitiveType::I32).Name(), "i32");
    EXPECT_EQ(SemanticType::FromSyntax(PrimitiveType::I64).Name(), "i64");
    EXPECT_EQ(SemanticType::FromSyntax(PrimitiveType::ISize).Name(), "isize");
    EXPECT_EQ(SemanticType::FromSyntax(PrimitiveType::U8).Name(), "u8");
    EXPECT_EQ(SemanticType::FromSyntax(PrimitiveType::U16).Name(), "u16");
    EXPECT_EQ(SemanticType::FromSyntax(PrimitiveType::U32).Name(), "u32");
    EXPECT_EQ(SemanticType::FromSyntax(PrimitiveType::U64).Name(), "u64");
    EXPECT_EQ(SemanticType::FromSyntax(PrimitiveType::USize).Name(), "usize");
    EXPECT_EQ(SemanticType::FromSyntax(PrimitiveType::F32).Name(), "f32");
    EXPECT_EQ(SemanticType::FromSyntax(PrimitiveType::F64).Name(), "f64");
    EXPECT_EQ(SemanticType::FromSyntax(PrimitiveType::Bool).Name(), "bool");
    EXPECT_EQ(SemanticType::FromSyntax(PrimitiveType::Char).Name(), "char");
    EXPECT_EQ(SemanticType::FromSyntax(PrimitiveType::Void).Name(), "void");
    EXPECT_EQ(SemanticType::FromSyntax(PrimitiveType::String).Name(), "string");
    EXPECT_EQ(SemanticType::FromSyntax(PrimitiveType::StringView).Name(),
              "stringv");
    EXPECT_TRUE(IsAssignable(SemanticType(SemanticTypeKind::I32),
                             SemanticType(SemanticTypeKind::None)));
    EXPECT_FALSE(IsAssignable(SemanticType(SemanticTypeKind::I32),
                              SemanticType(SemanticTypeKind::I64)));
}

TEST(SemanticCallableTests, ResolvesStructuralSignaturesAndFunctionValues) {
    Analysis result = Analyze(R"(
        func Mapper(i32) => (i32);
        func Equivalent(i32 value) => (i32);
        i32 twice(i32 value) { return value * 2; }
        i32 apply(Mapper callback, i32 value) { return callback(value); }
        i32 main() {
            Equivalent callback = twice;
            Mapper moved = callback;
            return apply(moved, 21);
        }
    )");

    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    const auto& declaration = static_cast<const CallableTypeDeclaration&>(
        *result.unit->Declarations()[0]);
    const auto* symbol = result.model.DeclaredSymbol(declaration);
    ASSERT_NE(symbol, nullptr);
    EXPECT_EQ(symbol->Kind(), SymbolKind::CallableType);
    EXPECT_EQ(symbol->Type().Name(), "func(i32) => (i32)");
    EXPECT_FALSE(symbol->Type().IsCopyable());
    EXPECT_TRUE(symbol->Type().OwnsStorage());
}

TEST(SemanticCallableTests, DiagnosesSignatureAndSafetyMismatches) {
    Analysis result = Analyze(R"(
        func Mapper(i32) => (i32);
        func Wrong(i64) => (i32);
        func InvalidPointer(*u8) => ();
        structure RawBox { *u8 pointer; };
        func InvalidTransitive(RawBox) => ();
        i32 twice(i32 value) { return value * 2; }
        i32 main() { Wrong callback = twice; return 0; }
    )");

    EXPECT_NE(FindErrorDiagnostic(result, DiagnosticCode::IncompatibleType),
              nullptr);
    EXPECT_NE(FindErrorDiagnostic(result, DiagnosticCode::UnsafeOperation),
              nullptr);
}

TEST(SemanticCallableTests, RequiresUnsafeContextForUnsafeIndirectCalls) {
    Analysis result = Analyze(R"(
        unsafe func Reader(lend *const i32 source) => (i32);
        unsafe i32 read(lend *const i32 source) { return *source; }
        i32 invoke(Reader reader, *const i32 source) {
            return reader(source);
        }
    )");

    EXPECT_NE(FindErrorDiagnostic(result, DiagnosticCode::UnsafeOperation),
              nullptr);
}

TEST(SemanticCallableTests, TreatsCallableVariablesAsMoveOnly) {
    Analysis result = Analyze(R"(
        func Mapper(i32) => (i32);
        i32 twice(i32 value) { return value * 2; }
        i32 main() {
            Mapper first = twice;
            Mapper second = first;
            return first(21);
        }
    )");

    EXPECT_NE(FindErrorDiagnostic(result, DiagnosticCode::MovedValue), nullptr);
}

TEST(SemanticCallableTests, TreatsCallableAggregatesAsMoveOnly) {
    const std::vector<std::string> sources = {
        R"(
            func Mapper(i32) => (i32);
            i32 twice(i32 value) { return value * 2; }
            i32 main() {
                Mapper callback = twice;
                (Mapper, i32) first = (callback, 21);
                (Mapper, i32) second = first;
                (Mapper, i32) invalid = first;
                return 0;
            }
        )",
        R"(
            func Mapper(i32) => (i32);
            i32 twice(i32 value) { return value * 2; }
            i32 main() {
                Mapper callback = twice;
                [1]Mapper first = [callback];
                [1]Mapper second = first;
                [1]Mapper invalid = first;
                return 0;
            }
        )",
        R"(
            func Mapper(i32) => (i32);
            structure Holder { Mapper callback; };
            Holder Holder::init(Mapper callback) <- callback override {
                return build Holder { self.callback = callback; };
            }
            i32 twice(i32 value) { return value * 2; }
            i32 main() {
                Mapper callback = twice;
                Holder first = Holder::init(callback);
                Holder second = first;
                Holder invalid = first;
                return 0;
            }
        )",
    };

    for (const std::string& source : sources) {
        SCOPED_TRACE(source);
        EXPECT_NE(
            FindErrorDiagnostic(Analyze(source), DiagnosticCode::MovedValue),
            nullptr);
    }
}

TEST(SemanticCallableTests,
     InfersCapturelessAnonymousFunctionsFromAssignmentsArgumentsAndReturns) {
    Analysis result = Analyze(R"(
        func Mapper(i32) => (i32);
        i32 apply(Mapper callback, i32 value) { return callback(value); }
        Mapper make() { return (value) => { return value + 1; }; }
        i32 main() {
            explicit := [](i32 value) => (i32) { return value * 2; };
            Mapper inferred = (value) => { return value + 2; };
            Mapper assigned = [](i32 value) => (i32) { return value; };
            assigned = (value) => { return value + 3; };
            return apply(explicit, 10) + apply(inferred, 10) +
                   apply([](value) => { return value; }, 10) + make()(0);
        }
    )");

    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    EXPECT_EQ(result.model.AnonymousFunctions().size(), 6);
    for (const auto* function : result.model.AnonymousFunctions()) {
        const auto* symbol = result.model.DeclaredSymbol(*function);
        ASSERT_NE(symbol, nullptr);
        EXPECT_EQ(symbol->Kind(), SymbolKind::Function);
        EXPECT_EQ(symbol->Type().Name(), "func(i32) => (i32)");
    }
}

TEST(SemanticCallableTests, DiagnosesMissingContextAndCaptures) {
    Analysis result = Analyze(R"(
        func Mapper(i32) => (i32);
        i32 main() {
            outer := 41;
            inferred := (value) => { return value; };
            Mapper capturing = (value) => { return value + outer; };
            return 0;
        }
    )");

    EXPECT_NE(FindErrorDiagnostic(result, DiagnosticCode::CannotInferType),
              nullptr);
    EXPECT_NE(FindErrorDiagnostic(result, DiagnosticCode::UnknownName),
              nullptr);
}

TEST(SemanticCallableTests, ResolvesExplicitCopyCaptures) {
    Analysis result = Analyze(R"(
        func Mapper(i32) => (i32);
        i32 main() {
            i32 factor = 2;
            string label = "scale";
            Mapper callback = [copy factor, copy label](value) => {
                label = "used";
                return value * factor;
            };
            factor = 10;
            return callback(21);
        }
    )");

    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_EQ(result.model.AnonymousFunctions().size(), 1);
    const auto& captures =
        result.model.Captures(*result.model.AnonymousFunctions().front());
    ASSERT_EQ(captures.size(), 2);
    EXPECT_EQ(captures[0].source->Name(), "factor");
    EXPECT_EQ(captures[0].variable->Name(), "factor");
    EXPECT_EQ(captures[0].variable->Type().Name(), "i32");
    EXPECT_EQ(captures[1].variable->Type().Name(), "string");
}

TEST(SemanticCallableTests, RequiresEveryOuterLocalToBeCaptured) {
    Analysis result = Analyze(R"(
        func Mapper(i32) => (i32);
        i32 main() {
            i32 captured = 1;
            i32 omitted = 2;
            Mapper callback = [copy captured](value) => {
                return value + captured + omitted;
            };
            return callback(1);
        }
    )");

    EXPECT_NE(FindErrorDiagnostic(result, DiagnosticCode::UnknownName),
              nullptr);
}

TEST(SemanticCallableTests, RejectsInvalidCopyCaptures) {
    EXPECT_NE(FindErrorDiagnostic(
                  Analyze("func Mapper(i32) => (i32); i32 main() { i32 x = 1; "
                          "Mapper f = [copy x, copy x](v) => { return v; }; "
                          "return 0; }"),
                  DiagnosticCode::DuplicateDeclaration),
              nullptr);
    EXPECT_NE(FindErrorDiagnostic(
                  Analyze("func Mapper(i32) => (i32); i32 main() { Mapper f = "
                          "[copy missing](v) => { return v; }; return 0; }"),
                  DiagnosticCode::UnknownName),
              nullptr);
    EXPECT_NE(FindErrorDiagnostic(
                  Analyze("func Mapper(i32) => (i32); i32 main() { Mapper "
                          "original = (v) => { return v; }; Mapper f = [copy "
                          "original](v) => { return original(v); }; return 0; "
                          "}"),
                  DiagnosticCode::NonCopyableValue),
              nullptr);
    EXPECT_NE(FindErrorDiagnostic(
                  Analyze("func Mapper(i32) => (i32); i32 main() { i32 x = 1; "
                          "&const i32 r = &x; Mapper f = [copy r](v) => { "
                          "return v; }; return 0; }"),
                  DiagnosticCode::InvalidBorrowContract),
              nullptr);
    EXPECT_NE(FindErrorDiagnostic(
                  Analyze("func Mapper(i32) => (i32); unsafe i32 main() { i32 "
                          "x = 1; *i32 p = &raw x; Mapper f = [copy p](v) => { "
                          "return v; }; return 0; }"),
                  DiagnosticCode::UnsafeOperation),
              nullptr);
}

TEST(SemanticCallableTests, AllowsRawCopyCapturesOnlyOnUnsafeFunctions) {
    Analysis valid = Analyze(R"(
        unsafe func Reader() => (i32);
        unsafe i32 main() {
            i32 value = 42;
            *i32 pointer = &raw value;
            Reader read = unsafe [copy pointer]() => (i32) {
                return *pointer;
            };
            return read();
        }
    )");
    for (const auto& diagnostic : valid.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    EXPECT_FALSE(valid.diagnostics.HasErrors());

    Analysis invalid = Analyze(R"(
        func Reader() => (i32);
        unsafe i32 main() {
            i32 value = 42;
            *i32 pointer = &raw value;
            Reader read = [copy pointer]() => (i32) { return 0; };
            return read();
        }
    )");
    EXPECT_NE(FindErrorDiagnostic(invalid, DiagnosticCode::UnsafeOperation),
              nullptr);
}

TEST(SemanticCallableTests, ResolvesUnsafeRawResourceCaptures) {
    Analysis result = Analyze(R"(
        unsafe func Reader() => (i32);
        unsafe i32 main() {
            pointer, failure := new<i32>(42);
            if failure != none { return 1; }
            i32 borrowed = 0;
            {
                Reader reader = unsafe [lend pointer]() => {
                    return *pointer;
                };
                borrowed = reader();
            }
            free(pointer);

            owned, secondFailure := new<i32>(7);
            if secondFailure != none { return 2; }
            Reader owner = unsafe [own owned]() => { return *owned; };
            return borrowed + owner();
        }
    )");

    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_EQ(result.model.AnonymousFunctions().size(), 2);
    const auto& lent =
        result.model.Captures(*result.model.AnonymousFunctions()[0]);
    const auto& owned =
        result.model.Captures(*result.model.AnonymousFunctions()[1]);
    ASSERT_EQ(lent.size(), 1);
    ASSERT_EQ(owned.size(), 1);
    EXPECT_FALSE(lent[0].variable->Writable());
    EXPECT_FALSE(owned[0].variable->Writable());
}

TEST(SemanticCallableTests, RequiresUnsafeRawResourceCaptureLiterals) {
    Analysis result = Analyze(R"(
        func Reader() => (i32);
        unsafe i32 main() {
            pointer, failure := new<i32>(42);
            if failure != none { return 1; }
            Reader reader = [lend pointer]() => { return *pointer; };
            free(pointer);
            return 0;
        }
    )");
    EXPECT_NE(FindErrorDiagnostic(result, DiagnosticCode::UnsafeOperation),
              nullptr);
}

TEST(SemanticCallableTests, RequiresRawResourcesForLendAndOwnCaptures) {
    Analysis result = Analyze(R"(
        unsafe func Reader() => (i32);
        unsafe i32 main() {
            i32 value = 42;
            Reader lent = unsafe [lend value]() => { return value; };
            Reader owned = unsafe [own value]() => { return value; };
            return 0;
        }
    )");
    EXPECT_NE(FindErrorDiagnostic(result, DiagnosticCode::NonCopyableValue),
              nullptr);
}

TEST(SemanticCallableTests, RejectsInvalidRawResourceCaptureTransfers) {
    Analysis copied = Analyze(R"(
        unsafe func Reader() => (i32);
        unsafe i32 main() {
            pointer, failure := new<i32>(42);
            if failure != none { return 1; }
            Reader reader = unsafe [copy pointer]() => { return *pointer; };
            free(pointer);
            return reader();
        }
    )");
    EXPECT_NE(FindErrorDiagnostic(copied, DiagnosticCode::UnverifiedOwnership),
              nullptr);

    Analysis stackOwner = Analyze(R"(
        unsafe func Reader() => (i32);
        unsafe i32 main() {
            i32 value = 42;
            *i32 pointer = &raw value;
            Reader reader = unsafe [own pointer]() => { return *pointer; };
            return reader();
        }
    )");
    EXPECT_NE(
        FindErrorDiagnostic(stackOwner, DiagnosticCode::UnverifiedOwnership),
        nullptr);
}

TEST(SemanticCallableTests, RejectsLendCaptureEscapesAndOwnerConflicts) {
    Analysis returned = Analyze(R"(
        unsafe func Reader() => (i32);
        unsafe Reader bind(lend *const i32 pointer) {
            return unsafe [lend pointer]() => { return *pointer; };
        }
    )");
    EXPECT_NE(
        FindErrorDiagnostic(returned, DiagnosticCode::UnverifiedOwnership),
        nullptr);

    Analysis stored = Analyze(R"(
        unsafe func Reader() => (i32);
        unsafe i32 main() {
            pointer, failure := new<i32>(42);
            if failure != none { return 1; }
            Reader escaped = unsafe []() => (i32) { return 0; };
            {
                Reader local = unsafe [lend pointer]() => { return *pointer; };
                escaped = local;
            }
            free(pointer);
            return 0;
        }
    )");
    EXPECT_NE(FindErrorDiagnostic(stored, DiagnosticCode::UnverifiedOwnership),
              nullptr);

    Analysis freed = Analyze(R"(
        unsafe func Reader() => (i32);
        unsafe i32 main() {
            pointer, failure := new<i32>(42);
            if failure != none { return 1; }
            Reader reader = unsafe [lend pointer]() => { return *pointer; };
            free(pointer);
            return reader();
        }
    )");
    EXPECT_NE(FindErrorDiagnostic(freed, DiagnosticCode::UnverifiedOwnership),
              nullptr);
}

TEST(SemanticCallableTests, RejectsConsumptionFromReusableOwnCapture) {
    Analysis result = Analyze(R"(
        unsafe func Consumer() => ();
        unsafe i32 main() {
            pointer, failure := new<i32>(42);
            if failure != none { return 1; }
            Consumer consumer = unsafe [own pointer]() => {
                free(pointer);
            };
            consumer();
            return 0;
        }
    )");
    EXPECT_NE(FindErrorDiagnostic(result, DiagnosticCode::UnverifiedOwnership),
              nullptr);
}

TEST(SemanticCallableTests, OwnsMovableCallablesAndSafeAggregates) {
    Analysis result = Analyze(R"(
        func Reader() => (i32);
        structure State { i32 value; };
        State State::init(i32 value) override {
            return build State { self.value = value; };
        }
        void State::tear(&self) override {}

        Reader wrap(i32 value) {
            State state = State::init(value);
            Reader inner = [own state]() => { return state.value; };
            return [own inner]() => { return inner(); };
        }
        i32 main() {
            Reader callback = wrap(42);
            return callback();
        }
    )");

    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    EXPECT_FALSE(result.diagnostics.HasErrors());
    ASSERT_EQ(result.model.AnonymousFunctions().size(), 2);
    for (const auto* function : result.model.AnonymousFunctions()) {
        ASSERT_EQ(result.model.Captures(*function).size(), 1);
        EXPECT_EQ(result.model.Captures(*function)[0].syntax->Mode(),
                  AnonymousFunctionExpression::CaptureMode::Own);
        EXPECT_FALSE(function->IsUnsafe());
    }
}

TEST(SemanticCallableTests, MovesGeneralizedOwnCaptureSources) {
    Analysis result = Analyze(R"(
        func Reader() => (i32);
        i32 main() {
            Reader inner = []() => (i32) { return 21; };
            Reader outer = [own inner]() => { return inner() * 2; };
            return inner() + outer();
        }
    )");

    EXPECT_NE(FindErrorDiagnostic(result, DiagnosticCode::MovedValue), nullptr);
}

TEST(SemanticCallableTests, RejectsInvalidGeneralizedOwnCaptures) {
    Analysis copyable = Analyze(R"(
        func Reader() => (i32);
        i32 main() {
            i32 value = 42;
            Reader callback = [own value]() => { return value; };
            return callback();
        }
    )");
    EXPECT_NE(FindErrorDiagnostic(copyable, DiagnosticCode::NonCopyableValue),
              nullptr);

    Analysis immovable = Analyze(R"(
        func Reader() => (i32);
        structure State { i32 value; };
        State State::init() override { return build State {}; }
        unsafe void State::tear(&self) override {}
        unsafe i32 main() {
            State state = State::init();
            Reader callback = [own state]() => { return state.value; };
            return callback();
        }
    )");
    EXPECT_NE(FindErrorDiagnostic(immovable, DiagnosticCode::NonCopyableValue),
              nullptr);

    Analysis transparentRaw = Analyze(R"(
        unsafe func Reader() => (i32);
        unsafe i32 main() {
            i32 value = 42;
            *i32 pointer = &raw value;
            Reader inner = unsafe []() => (i32) { return 0; };
            (Reader, *i32) state = (inner, pointer);
            Reader callback = unsafe [own state]() => { return 42; };
            return callback();
        }
    )");
    EXPECT_NE(
        FindErrorDiagnostic(transparentRaw, DiagnosticCode::NonCopyableValue),
        nullptr);
}

TEST(SemanticCallableTests,
     RejectsConsumptionFromReusableGeneralizedOwnCapture) {
    Analysis result = Analyze(R"(
        func Reader() => (i32);
        void consume(Reader callback) {}
        i32 main() {
            Reader inner = []() => (i32) { return 42; };
            Reader outer = [own inner]() => {
                consume(inner);
                return 42;
            };
            return outer();
        }
    )");

    EXPECT_NE(FindErrorDiagnostic(result, DiagnosticCode::NonCopyableValue),
              nullptr);
}

TEST(SemanticCallableTests, PropagatesBorrowOriginsThroughOwnCaptures) {
    Analysis valid = Analyze(R"(
        func Reader() => (i32);
        Reader wrap(&const i32 source) <- source {
            Reader inner = [&const source]() => { return *source; };
            return [own inner]() => { return inner(); };
        }
        i32 main() {
            i32 value = 42;
            Reader callback = wrap(&value);
            return callback();
        }
    )");
    for (const auto& diagnostic : valid.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    EXPECT_FALSE(valid.diagnostics.HasErrors());

    Analysis escaped = Analyze(R"(
        func Reader() => (i32);
        Reader invalid() {
            i32 local = 42;
            Reader inner = [&const local]() => { return local; };
            return [own inner]() => { return inner(); };
        }
    )");
    EXPECT_NE(
        FindErrorDiagnostic(escaped, DiagnosticCode::InvalidBorrowContract),
        nullptr);
}

TEST(SemanticCallableTests, ChecksCopyCaptureSourceInitialization) {
    EXPECT_NE(FindErrorDiagnostic(
                  Analyze("func Mapper(i32) => (i32); i32 main() { i32 x; "
                          "Mapper f = [copy x](v) => { return v + x; }; return "
                          "0; }"),
                  DiagnosticCode::UninitializedVariable),
              nullptr);
}

TEST(SemanticCallableTests, ResolvesCheckedReferenceCaptures) {
    Analysis result = Analyze(R"(
        func Reader() => (i32);
        i32 main() {
            i32 value = 40;
            const i32 limit = 2;
            Reader callback = [&value, &const limit]() => {
                value = value + limit;
                return value;
            };
            return callback();
        }
    )");

    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    const auto& captures =
        result.model.Captures(*result.model.AnonymousFunctions().front());
    ASSERT_EQ(captures.size(), 2);
    EXPECT_TRUE(captures[0].variable->Writable());
    EXPECT_FALSE(captures[1].variable->Writable());
}

TEST(SemanticCallableTests, RejectsWritesThroughSharedCaptures) {
    Analysis result = Analyze(R"(
        func Writer() => ();
        void main() {
            i32 value = 1;
            Writer callback = [&const value]() => { value = 2; };
        }
    )");

    EXPECT_NE(FindErrorDiagnostic(result, DiagnosticCode::SymbolNotWritable),
              nullptr);

    Analysis reborrow = Analyze(R"(
        func Writer() => ();
        Writer bind(&i32 source) <- source {
            return [&const source]() => { *source = 2; };
        }
    )");
    EXPECT_NE(FindErrorDiagnostic(reborrow, DiagnosticCode::SymbolNotWritable),
              nullptr);
}

TEST(SemanticCallableTests, RejectsMutableCaptureOfReadOnlyStorage) {
    Analysis result = Analyze(R"(
        func Reader() => (i32);
        i32 main() {
            const i32 value = 1;
            Reader callback = [&value]() => { return value; };
            return callback();
        }
    )");

    EXPECT_NE(FindErrorDiagnostic(result, DiagnosticCode::SymbolNotWritable),
              nullptr);
}

TEST(SemanticCallableTests, EnforcesCheckedCaptureLoansUntilLastUse) {
    Analysis conflict = Analyze(R"(
        func Reader() => (i32);
        i32 main() {
            i32 value = 1;
            Reader callback = [&const value]() => { return value; };
            value = 2;
            return callback();
        }
    )");
    EXPECT_NE(FindErrorDiagnostic(conflict, DiagnosticCode::BorrowConflict),
              nullptr);

    Analysis released = Analyze(R"(
        func Reader() => (i32);
        i32 main() {
            i32 value = 1;
            Reader callback = [&const value]() => { return value; };
            i32 result = callback();
            value = 2;
            return result + value;
        }
    )");
    for (const auto& diagnostic : released.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    EXPECT_FALSE(released.diagnostics.HasErrors());
}

TEST(SemanticCallableTests, DiagnosesCheckedCaptureEscapes) {
    Analysis storage = Analyze(R"(
        func Reader() => (i32);
        i32 main() {
            Reader callback = () => { return 0; };
            {
                i32 local = 1;
                callback = [&const local]() => { return local; };
            }
            return callback();
        }
    )");
    EXPECT_NE(FindErrorDiagnostic(storage, DiagnosticCode::ReferenceEscape),
              nullptr);

    Analysis returned = Analyze(R"(
        func Reader() => (i32);
        Reader invalid() {
            i32 local = 1;
            return [&const local]() => { return local; };
        }
    )");
    EXPECT_NE(
        FindErrorDiagnostic(returned, DiagnosticCode::InvalidBorrowContract),
        nullptr);

    Analysis container = Analyze(R"(
        func Reader() => (i32);
        [1]Reader invalid() {
            i32 local = 1;
            return [[&const local]() => { return local; }];
        }
    )");
    EXPECT_NE(
        FindErrorDiagnostic(container, DiagnosticCode::InvalidBorrowContract),
        nullptr);

    Analysis containerStorage = Analyze(R"(
        func Reader() => (i32);
        i32 zero() { return 0; }
        i32 main() {
            [1]Reader callbacks = [zero];
            {
                i32 local = 42;
                callbacks[0] = [&const local]() => { return local; };
            }
            return 0;
        }
    )");
    EXPECT_NE(
        FindErrorDiagnostic(containerStorage, DiagnosticCode::ReferenceEscape),
        nullptr);
}

TEST(SemanticCallableTests, PropagatesCaptureOriginsThroughCallableReturns) {
    Analysis valid = Analyze(R"(
        func Reader() => (i32);
        Reader bind(&const i32 source) <- source {
            return [&const source]() => { return *source; };
        }
        i32 main() {
            i32 value = 41;
            Reader callback = bind(&value);
            return callback();
        }
    )");
    for (const auto& diagnostic : valid.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    EXPECT_FALSE(valid.diagnostics.HasErrors());

    Analysis conflict = Analyze(R"(
        func Reader() => (i32);
        Reader bind(&const i32 source) <- source {
            return [&const source]() => { return *source; };
        }
        i32 main() {
            i32 value = 41;
            Reader callback = bind(&value);
            value = 42;
            return callback();
        }
    )");
    EXPECT_NE(FindErrorDiagnostic(conflict, DiagnosticCode::BorrowConflict),
              nullptr);
}

TEST(SemanticCallableTests, PropagatesCaptureOriginsThroughStructures) {
    Analysis missingContract = Analyze(R"(
        func Reader() => (i32);
        structure Holder { Reader callback; };
        Holder Holder::init(Reader callback) override {
            return build Holder { self.callback = callback; };
        }
    )");
    EXPECT_NE(FindErrorDiagnostic(missingContract,
                                  DiagnosticCode::InvalidBorrowContract),
              nullptr);

    Analysis valid = Analyze(R"(
        func Reader() => (i32);
        structure Holder { Reader callback; };
        Holder Holder::init(Reader callback) <- callback override {
            return build Holder { self.callback = callback; };
        }
        i32 main() {
            i32 value = 42;
            Reader callback = [&const value]() => { return value; };
            Holder holder = Holder::init(callback);
            return value;
        }
    )");
    for (const auto& diagnostic : valid.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    EXPECT_FALSE(valid.diagnostics.HasErrors());
}

TEST(SemanticCallableTests, EnforcesAnonymousFunctionPointerSafety) {
    Analysis valid = Analyze(R"(
        unsafe func Reader(*const i32) => (i32);
        i32 main() {
            Reader inferred = (source) => { return *source; };
            explicit := unsafe [](*const i32 source) => (i32) {
                return *source;
            };
            return 0;
        }
    )");
    for (const auto& diagnostic : valid.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    EXPECT_FALSE(valid.diagnostics.HasErrors());
    EXPECT_EQ(valid.model.AnonymousFunctions().size(), 2);

    Analysis invalid = Analyze(R"(
        i32 main() {
            callback := [](*const i32 source) => (i32) { return 0; };
            return 0;
        }
    )");
    EXPECT_NE(FindErrorDiagnostic(invalid, DiagnosticCode::UnsafeOperation),
              nullptr);
}

TEST(SemanticMatchTests, AcceptsExhaustiveAndWildcardEnumMatches) {
    Analysis result = Analyze(R"(
        enum TokenKind: u8 { Identifier = 1, Number = 4, End = 9, }
        i32 classify(TokenKind kind) {
            return match kind {
                TokenKind.Identifier => 1,
                TokenKind.Number => 2,
                TokenKind.End => 3,
            };
        }
        bool is_terminal(TokenKind kind) {
            return match kind {
                TokenKind.End => true,
                _ => false,
            };
        }
    )");

    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
}

TEST(SemanticMatchTests, DiagnosesPatternsExhaustivenessAndArmTypes) {
    const std::string enums =
        "enum State { Idle, Running, Done, } "
        "enum Other { Idle, } ";
    for (const std::string& function : {
             "i32 f(State s) { return match s { State.Idle => 0, }; }",
             "i32 f(State s) { return match s { State.Idle => 0, State.Idle => "
             "1, _ => 2, }; }",
             "i32 f(State s) { return match s { Other.Idle => 0, _ => 1, }; }",
             "i32 f(State s) { return match s { State.Missing => 0, _ => 1, }; "
             "}",
             "i32 f(State s) { return match s { _ => 0, State.Idle => 1, }; }",
             "i32 f(State s) { return match s { State.Idle => 0, _ => true, }; "
             "}",
             "i32 f(i32 n) { return match n { State.Idle => 0, _ => 1, }; }",
         }) {
        SCOPED_TRACE(function);
        Analysis result = Analyze(enums + function);
        EXPECT_NE(FindDiagnostic(result, DiagnosticCode::InvalidMatch),
                  nullptr);
    }
}

TEST(SemanticMatchTests, MergesDefiniteAssignmentAcrossEveryArm) {
    Analysis valid = Analyze(R"(
        enum State { Idle, Running, }
        i32 f(State state) {
            i32 result;
            match state {
                State.Idle => result = 1,
                State.Running => result = 2,
            };
            return result;
        }
    )");
    EXPECT_FALSE(valid.diagnostics.HasErrors());

    Analysis invalid = Analyze(R"(
        enum State { Idle, Running, }
        i32 f(State state) {
            i32 result;
            match state {
                State.Idle => result = 1,
                State.Running => 2,
            };
            return result;
        }
    )");
    EXPECT_NE(FindDiagnostic(invalid, DiagnosticCode::UninitializedVariable),
              nullptr);
}

TEST(SemanticMatchTests, ExpandsGenericFunctionsContainingMatches) {
    Analysis result = Analyze(R"(
        enum Choice { First, Second, }
        T choose<T>(Choice choice, T first, T second) {
            return match choice {
                Choice.First => first,
                Choice.Second => second,
            };
        }
        i32 main() { return choose<i32>(Choice.Second, 20, 22); }
    )");

    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
}

TEST(ScopeTests, InsertsLooksUpParentsShadowsAndRejectsDuplicates) {
    const SourceRange range{{0, 1, 1}, {1, 1, 2}};
    VariableSymbol outer(SymbolKind::LocalVariable, "value",
                         SemanticType(SemanticTypeKind::I32), range, true);
    VariableSymbol inner(SymbolKind::LocalVariable, "value",
                         SemanticType(SemanticTypeKind::Bool), range, true);
    Scope global;
    Scope child(&global);

    EXPECT_TRUE(global.Declare(outer));
    EXPECT_FALSE(global.Declare(outer));
    EXPECT_EQ(child.Lookup("value"), &outer);
    EXPECT_TRUE(child.Declare(inner));
    EXPECT_EQ(child.LookupLocal("value"), &inner);
    EXPECT_EQ(child.Lookup("value"), &inner);
    EXPECT_EQ(global.Lookup("missing"), nullptr);
}

TEST(SemanticExternTests, AcceptsScalarAndUnsafePointerAbiDeclarations) {
    Analysis result = Analyze(
        "extern i32 native_add(i32 left, i32 right); "
        "extern unsafe i32 native_read(*const char path, usize length, "
        "*u8 output, usize capacity); "
        "i32 add(i32 left, i32 right) { return native_add(left, right); }");

    EXPECT_FALSE(result.diagnostics.HasErrors());
    const auto* scalar = static_cast<const FunctionSymbol*>(
        result.model.DeclaredSymbol(FunctionAt(result, 0)));
    ASSERT_NE(scalar, nullptr);
    EXPECT_TRUE(scalar->IsExtern());
    EXPECT_EQ(scalar->LinkName(), "native_add");
}

TEST(SemanticExternTests, RejectsUnsupportedAndUntrackedAbiSignatures) {
    for (const std::string& declaration : {
             "extern string bad(stringv value);",
             "extern i32 bad(*u8 value);",
             "extern unsafe error bad(i32 value);",
             "extern unsafe own *u8 bad();",
             "extern unsafe *string bad();",
             "extern unsafe void arbitrary(*string output);",
             "extern unsafe i32 bad(&i32 value);",
             "extern i32 bad<T>(i32 value);",
         }) {
        Analysis result = Analyze(declaration);
        EXPECT_NE(FindDiagnostic(result, DiagnosticCode::InvalidExtern),
                  nullptr)
            << declaration;
    }
}

TEST(SemanticExternTests, RawStringDataRequiresUnsafePermission) {
    EXPECT_NE(FindDiagnostic(Analyze("*const char bad(stringv value) { return "
                                     "value.data(); }"),
                             DiagnosticCode::UnsafeOperation),
              nullptr);
    EXPECT_FALSE(Analyze("unsafe *const char good(stringv value) { return "
                         "value.data(); }")
                     .diagnostics.HasErrors());
}

TEST(SemanticExternTests, DoesNotBypassTrackedAllocationContracts) {
    Analysis result = Analyze(
        "extern unsafe void native_use(*i32 value); "
        "unsafe void use() { value, err := new<i32>(42); if err != none { "
        "return; } native_use(value); free(value); }");

    EXPECT_NE(FindDiagnostic(result, DiagnosticCode::UnverifiedOwnership),
              nullptr);
}

TEST(SemanticDeclarationTests, RegistersFunctionsParametersAndLocals) {
    Analysis analysis = Analyze(
        "i32 add(i32 left, i32 right) { i32 sum = left + right; "
        "return sum; }");

    ASSERT_FALSE(analysis.diagnostics.HasErrors());
    const FunctionDeclaration& function = FunctionAt(analysis, 0);
    const VariableDeclarationStatement& variable = VariableAt(function, 0);
    ASSERT_NE(analysis.model.DeclaredSymbol(function), nullptr);
    EXPECT_EQ(analysis.model.DeclaredSymbol(function)->Kind(),
              SymbolKind::Function);
    ASSERT_NE(analysis.model.DeclaredSymbol(*function.Parameters()[0]),
              nullptr);
    EXPECT_EQ(analysis.model.DeclaredSymbol(*function.Parameters()[0])->Kind(),
              SymbolKind::Parameter);
    ASSERT_NE(analysis.model.DeclaredSymbol(variable), nullptr);
    EXPECT_EQ(analysis.model.DeclaredSymbol(variable)->Type().Kind(),
              SemanticTypeKind::I32);
}

TEST(SemanticBuiltInTests, ProvidesTypedStandardOutput) {
    Analysis analysis = Analyze(
        "i32 main() { stdout(\"Flecha\"); "
        "stdoutln(\"Language\"); return 42; }");

    ASSERT_FALSE(analysis.diagnostics.HasErrors());
    const FunctionDeclaration& function = FunctionAt(analysis, 0);
    const ExpressionStatement& statement =
        static_cast<const ExpressionStatement&>(
            *function.Body().Statements()[0]);
    const CallExpression& call =
        static_cast<const CallExpression&>(statement.Value());
    const NameExpression& name =
        static_cast<const NameExpression&>(call.Callee());
    const FunctionSymbol* symbol =
        static_cast<const FunctionSymbol*>(analysis.model.SymbolFor(name));
    ASSERT_NE(symbol, nullptr);
    EXPECT_EQ(symbol->BuiltIn(), BuiltInFunction::Stdout);
    EXPECT_EQ(symbol->ReturnType().Kind(), SemanticTypeKind::Void);
    ASSERT_EQ(symbol->ParameterTypes().size(), 1);
    EXPECT_EQ(symbol->ParameterTypes()[0].Kind(), SemanticTypeKind::StringView);

    const ExpressionStatement& lineStatement =
        static_cast<const ExpressionStatement&>(
            *function.Body().Statements()[1]);
    const CallExpression& lineCall =
        static_cast<const CallExpression&>(lineStatement.Value());
    const NameExpression& lineName =
        static_cast<const NameExpression&>(lineCall.Callee());
    const FunctionSymbol* lineSymbol =
        static_cast<const FunctionSymbol*>(analysis.model.SymbolFor(lineName));
    ASSERT_NE(lineSymbol, nullptr);
    EXPECT_EQ(lineSymbol->BuiltIn(), BuiltInFunction::StdoutLine);
}

TEST(SemanticBuiltInTests, ValidatesStandardOutputArguments) {
    Analysis valid = Analyze(
        "void Main() { stdout(); stdout(42, false, 'x', 1.5); "
        "stdoutln(); stdoutln(\"answer: \", 42); }");
    EXPECT_FALSE(valid.diagnostics.HasErrors());

    Analysis wrongType = Analyze("void Main() { stdout(null); }");
    EXPECT_NE(FindDiagnostic(wrongType, DiagnosticCode::IncompatibleArgument),
              nullptr);

    Analysis lineWrongType = Analyze("void Main() { stdoutln(none); }");
    EXPECT_NE(
        FindDiagnostic(lineWrongType, DiagnosticCode::IncompatibleArgument),
        nullptr);
}

TEST(SemanticBuiltInTests, ValidatesFormattedStandardOutput) {
    Analysis unsupported = Analyze(
        "void Main() { stdoutf(\"expected: %d, text: %s%c\", 42, "
        "\"flecha\", '!', true); }");
    EXPECT_NE(FindDiagnostic(unsupported, DiagnosticCode::InvalidFormat),
              nullptr);
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::InvalidFormat),
              "FLECHA-S0060");

    Analysis supported = Analyze(
        "void Main() { stdoutf(\"expected: %d, text: %s%s, ok: %b/%b, "
        "percent: %%\", 42, \"flecha\", '!', true, 1 as u8); }");
    EXPECT_FALSE(supported.diagnostics.HasErrors());

    Analysis missingFormat = Analyze("void Main() { stdoutf(); }");
    EXPECT_NE(FindDiagnostic(missingFormat, DiagnosticCode::WrongArgumentCount),
              nullptr);

    Analysis nonLiteral = Analyze(
        "void Main() { stringv format = \"%d\"; stdoutf(format, 42); }");
    EXPECT_NE(FindDiagnostic(nonLiteral, DiagnosticCode::InvalidFormat),
              nullptr);

    Analysis incomplete = Analyze("void Main() { stdoutf(\"value: %\", 1); }");
    EXPECT_NE(FindDiagnostic(incomplete, DiagnosticCode::InvalidFormat),
              nullptr);

    Analysis wrongCount = Analyze("void Main() { stdoutf(\"%d %s\", 1); }");
    EXPECT_NE(FindDiagnostic(wrongCount, DiagnosticCode::WrongArgumentCount),
              nullptr);

    Analysis wrongDecimal = Analyze("void Main() { stdoutf(\"%d\", false); }");
    EXPECT_NE(
        FindDiagnostic(wrongDecimal, DiagnosticCode::IncompatibleArgument),
        nullptr);

    Analysis wrongString = Analyze("void Main() { stdoutf(\"%s\", 42); }");
    EXPECT_NE(FindDiagnostic(wrongString, DiagnosticCode::IncompatibleArgument),
              nullptr);

    Analysis wrongBoolean =
        Analyze("void Main() { stdoutf(\"%b\", 1 as u16); }");
    EXPECT_NE(
        FindDiagnostic(wrongBoolean, DiagnosticCode::IncompatibleArgument),
        nullptr);
}

TEST(SemanticStringViewTests, TypesEveryStringViewOperation) {
    Analysis analysis = Analyze(
        "void Main() { text := \"flecha\"; length := text.len(); "
        "first := text.at(0); last := text[5 as u32]; "
        "middle := text.substr(1, 3); }");

    ASSERT_FALSE(analysis.diagnostics.HasErrors());
    const FunctionDeclaration& function = FunctionAt(analysis, 0);
    EXPECT_EQ(
        analysis.model.TypeOf(*VariableAt(function, 1).Initializer()).Kind(),
        SemanticTypeKind::U32);
    EXPECT_EQ(
        analysis.model.TypeOf(*VariableAt(function, 2).Initializer()).Kind(),
        SemanticTypeKind::Char);
    EXPECT_EQ(
        analysis.model.TypeOf(*VariableAt(function, 3).Initializer()).Kind(),
        SemanticTypeKind::Char);
    EXPECT_EQ(
        analysis.model.TypeOf(*VariableAt(function, 4).Initializer()).Kind(),
        SemanticTypeKind::StringView);
}

TEST(SemanticStringViewTests, DiagnosesInvalidMembersIndicesAndArguments) {
    Analysis invalidMember =
        Analyze("void Main() { text := \"x\"; text.missing(); }");
    EXPECT_NE(FindDiagnostic(invalidMember, DiagnosticCode::InvalidMember),
              nullptr);

    Analysis nonStringIndex = Analyze("void Main() { value := 1; value[0]; }");
    EXPECT_NE(FindDiagnostic(nonStringIndex, DiagnosticCode::NotIndexable),
              nullptr);

    Analysis wrongIndex =
        Analyze("void Main() { text := \"x\"; text.at(true); }");
    EXPECT_NE(FindDiagnostic(wrongIndex, DiagnosticCode::IncompatibleArgument),
              nullptr);

    Analysis wrongCount =
        Analyze("void Main() { text := \"x\"; text.substr(0); }");
    EXPECT_NE(FindDiagnostic(wrongCount, DiagnosticCode::WrongArgumentCount),
              nullptr);
}

TEST(SemanticStringTests, SupportsOwningConversionsAndLiteralMembers) {
    Analysis analysis = Analyze(
        "string FromLiteral() { string first = \"hi mom\".string(); "
        "string second = \"hi mom\".cast<string>; "
        "stringv borrowed = first.view(); "
        "stringv symmetric = second.cast<stringv>; "
        "string copied = first.copy(); return copied; }");

    ASSERT_FALSE(analysis.diagnostics.HasErrors());
    const FunctionDeclaration& function = FunctionAt(analysis, 0);
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(VariableAt(function, 0))->Type().Kind(),
        SemanticTypeKind::String);
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(VariableAt(function, 2))->Type().Kind(),
        SemanticTypeKind::StringView);
}

TEST(SemanticStringTests, FormatsScalarsOnlyAsOwningStrings) {
    Analysis valid = Analyze(
        "void Main() { integer := 42.string(); decimal := 1.5.string(); "
        "boolean := true.string(); character := 'x'.string(); "
        "castValue := 42.cast<string>; }");
    ASSERT_FALSE(valid.diagnostics.HasErrors());
    const FunctionDeclaration& function = FunctionAt(valid, 0);
    for (std::size_t index = 0; index < 5; ++index) {
        EXPECT_EQ(valid.model.DeclaredSymbol(VariableAt(function, index))
                      ->Type()
                      .Kind(),
                  SemanticTypeKind::String);
    }

    Analysis invalid = Analyze("void Main() { value := 42.cast<stringv>; }");
    EXPECT_NE(FindDiagnostic(invalid, DiagnosticCode::InvalidCast), nullptr);
}

TEST(SemanticStringTests, ConcatenatesStringsAndViewsIntoOwningStrings) {
    Analysis analysis = Analyze(
        "void Main() { string owned = \"fle\"; stringv view = \"cha\"; "
        "first := owned + view; second := view + owned; "
        "third := \"f\" + \"le\" + \"cha\"; }");
    ASSERT_FALSE(analysis.diagnostics.HasErrors());
    const FunctionDeclaration& function = FunctionAt(analysis, 0);
    for (std::size_t index = 2; index < 5; ++index) {
        EXPECT_EQ(analysis.model.DeclaredSymbol(VariableAt(function, index))
                      ->Type()
                      .Kind(),
                  SemanticTypeKind::String);
    }
}

TEST(SemanticStringTests, RejectsViewsThatEscapeLocalOwners) {
    Analysis direct = Analyze(
        "stringv Bad() { string owned = \"temporary\"; "
        "return owned.view(); }");
    EXPECT_NE(FindDiagnostic(direct, DiagnosticCode::BorrowEscapesOwner),
              nullptr);

    Analysis indirect = Analyze(
        "stringv Bad() { string owned = \"temporary\"; "
        "stringv view = owned; return view; }");
    EXPECT_NE(FindDiagnostic(indirect, DiagnosticCode::BorrowEscapesOwner),
              nullptr);

    Analysis parameter =
        Analyze("stringv Bad(string owned) { return owned.view(); }");
    EXPECT_NE(FindDiagnostic(parameter, DiagnosticCode::BorrowEscapesOwner),
              nullptr);

    Analysis temporary =
        Analyze("stringv Bad() { return (\"a\" + \"b\").view(); }");
    EXPECT_NE(FindDiagnostic(temporary, DiagnosticCode::BorrowEscapesOwner),
              nullptr);

    Analysis safe = Analyze(
        "stringv Identity(stringv value) <- value { return value; } "
        "stringv Literal() { return \"static\"; }");
    EXPECT_FALSE(safe.diagnostics.HasErrors());
}

TEST(SemanticStringTests, TracksBorrowedStringViewResultContracts) {
    const std::string declaration =
        "structure Label { string text; }; "
        "Label Label::init(stringv text) override { return build Label { "
        "self.text = text.string(); }; } "
        "stringv Label::view(&const self) <- self { return "
        "self->text.view(); } ";

    Analysis valid =
        Analyze(declaration +
                "u32 read() { Label label = Label::init(\"flecha\"); "
                "view := label.view(); return view.len(); }");
    EXPECT_FALSE(valid.diagnostics.HasErrors());

    Analysis ownerMutation = Analyze(
        declaration +
        "void mutate() { Label label = Label::init(\"flecha\"); "
        "view := label.view(); label.text = \"arrow\"; stdout(view); }");
    EXPECT_NE(FindDiagnostic(ownerMutation, DiagnosticCode::BorrowConflict),
              nullptr);

    Analysis escape =
        Analyze(declaration +
                "stringv escape() { Label label = Label::init(\"flecha\"); "
                "return label.view(); }");
    EXPECT_NE(FindDiagnostic(escape, DiagnosticCode::InvalidBorrowContract),
              nullptr);

    Analysis wrongSource = Analyze(
        "structure Label { string text; }; "
        "stringv Label::wrong(&const self) <- self { "
        "string local = \"temporary\"; return local.view(); }");
    EXPECT_NE(
        FindDiagnostic(wrongSource, DiagnosticCode::InvalidBorrowContract),
        nullptr);

    Analysis missingContract =
        Analyze("stringv identity(stringv value) { return value; }");
    EXPECT_NE(
        FindDiagnostic(missingContract, DiagnosticCode::InvalidBorrowContract),
        nullptr);

    Analysis directOwnerMutation = Analyze(
        "void mutate() { string owner = \"flecha\"; "
        "view := owner.view(); owner = \"arrow\"; stdout(view); }");
    EXPECT_NE(
        FindDiagnostic(directOwnerMutation, DiagnosticCode::BorrowConflict),
        nullptr);
}

TEST(SemanticDeclarationTests, BindsEveryNameToItsResolvedSymbol) {
    Analysis analysis =
        Analyze("i32 identity(i32 value) { copy := value; return copy; }");

    ASSERT_FALSE(analysis.diagnostics.HasErrors());
    const FunctionDeclaration& function = FunctionAt(analysis, 0);
    const VariableDeclarationStatement& variable = VariableAt(function, 0);
    const NameExpression& parameterUse =
        static_cast<const NameExpression&>(*variable.Initializer());
    const ReturnStatement& returnStatement =
        static_cast<const ReturnStatement&>(*function.Body().Statements()[1]);
    const NameExpression& localUse =
        static_cast<const NameExpression&>(*returnStatement.Value());

    EXPECT_EQ(analysis.model.SymbolFor(parameterUse),
              analysis.model.DeclaredSymbol(*function.Parameters()[0]));
    EXPECT_EQ(analysis.model.SymbolFor(localUse),
              analysis.model.DeclaredSymbol(variable));
}

TEST(SemanticDeclarationTests, DiagnosesDuplicateParameters) {
    Analysis analysis =
        Analyze("i32 choose(i32 value, i32 value) { return value; }");

    const Diagnostic* duplicate =
        FindDiagnostic(analysis, DiagnosticCode::DuplicateDeclaration);
    ASSERT_NE(duplicate, nullptr);
    EXPECT_EQ(duplicate->range.Length(), std::string("value").size());
}

TEST(SemanticDeclarationTests, SupportsExplicitInferenceAndLaterAssignments) {
    Analysis analysis = Analyze(
        "i32 main() { i32 x = 32; bool y = false; value := 32; "
        "x = 64; value = x = 7; return value; }");

    ASSERT_FALSE(analysis.diagnostics.HasErrors());
    const FunctionDeclaration& function = FunctionAt(analysis, 0);
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(VariableAt(function, 0))->Type().Kind(),
        SemanticTypeKind::I32);
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(VariableAt(function, 1))->Type().Kind(),
        SemanticTypeKind::Bool);
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(VariableAt(function, 2))->Type().Kind(),
        SemanticTypeKind::I32);
}

TEST(SemanticDeclarationTests, SupportsDefiniteAssignmentForValueTypes) {
    Analysis valid = Analyze(
        "i32 Main(bool choose) { i32 value; string text; "
        "if choose { value = 40; text = \"left\"; } "
        "else { value = 42; text = \"right\"; } "
        "stdoutln(text); return value; }");
    EXPECT_FALSE(valid.diagnostics.HasErrors());

    Analysis directRead = Analyze("i32 Main() { i32 value; return value; }");
    EXPECT_NE(FindDiagnostic(directRead, DiagnosticCode::UninitializedVariable),
              nullptr);

    Analysis incompleteBranch = Analyze(
        "i32 Main(bool choose) { i32 value; if choose { value = 42; } "
        "return value; }");
    EXPECT_NE(
        FindDiagnostic(incompleteBranch, DiagnosticCode::UninitializedVariable),
        nullptr);

    Analysis loopOnly = Analyze(
        "i32 Main(bool run) { i32 value; while run { value = 42; break; } "
        "return value; }");
    EXPECT_NE(FindDiagnostic(loopOnly, DiagnosticCode::UninitializedVariable),
              nullptr);

    Analysis everyType = Analyze(
        "void Main() { i8 a; i16 b; i32 c; i64 d; isize e; "
        "u8 f; u16 g; u32 h; u64 i; usize j; f32 k; f64 l; "
        "bool m; char n; string o; "
        "a = 1 as i8; b = 1 as i16; c = 1; d = 1 as i64; "
        "e = 1 as isize; f = 1 as u8; g = 1 as u16; h = 1 as u32; "
        "i = 1 as u64; j = 1 as usize; k = 1.0 as f32; l = 1.0; "
        "m = true; n = 'x'; o = \"ready\"; stdoutln(o); }");
    EXPECT_FALSE(everyType.diagnostics.HasErrors());
}

TEST(SemanticDeclarationTests, BorrowedViewsRequireImmediateInitialization) {
    Analysis analysis = Analyze("void Main() { stringv view; }");
    EXPECT_NE(
        FindDiagnostic(analysis, DiagnosticCode::BorrowRequiresInitializer),
        nullptr);
}

TEST(SemanticInferenceTests, InfersEverySupportedLiteralCategory) {
    Analysis analysis = Analyze(
        "i32 main() { integer := 1; floating := 1.5; boolean := true; "
        "character := 'x'; text := \"hi\"; nil := null; zero := none; "
        "return integer; }");

    ASSERT_FALSE(analysis.diagnostics.HasErrors());
    const FunctionDeclaration& function = FunctionAt(analysis, 0);
    const SemanticTypeKind expected[] = {
        SemanticTypeKind::I32,        SemanticTypeKind::F64,
        SemanticTypeKind::Bool,       SemanticTypeKind::Char,
        SemanticTypeKind::StringView, SemanticTypeKind::Null,
        SemanticTypeKind::None};
    for (std::size_t index = 0; index < 7; ++index) {
        EXPECT_EQ(analysis.model.DeclaredSymbol(VariableAt(function, index))
                      ->Type()
                      .Kind(),
                  expected[index]);
    }
}

TEST(SemanticInferenceTests, PropagatesTypesThroughNamesAndExpressions) {
    Analysis analysis = Analyze(
        "i32 main() { base := 32; sum := (base + 10) * 2; "
        "comparison := sum > base; return sum; }");

    ASSERT_FALSE(analysis.diagnostics.HasErrors());
    const FunctionDeclaration& function = FunctionAt(analysis, 0);
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(VariableAt(function, 1))->Type().Kind(),
        SemanticTypeKind::I32);
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(VariableAt(function, 2))->Type().Kind(),
        SemanticTypeKind::Bool);
}

TEST(SemanticScopeTests, AllowsNestedShadowingAndRestoresOuterBinding) {
    Analysis analysis = Analyze(
        "i32 main() { value := 1; if true { value := false; value = true; } "
        "value = 2; return value; }");

    EXPECT_FALSE(analysis.diagnostics.HasErrors());
    EXPECT_GE(analysis.model.Scopes().size(), 3);
}

TEST(SemanticScopeTests, AnalyzesWhileAndAlsoScopes) {
    Analysis analysis = Analyze(
        "i32 main() { value := 0; while value < 2 { inner := value; "
        "value = inner + 1; } also value == 1 { extra := value; "
        "value = extra + 1; } return value; }");

    EXPECT_FALSE(analysis.diagnostics.HasErrors());
}

TEST(SemanticDiagnosticTests, RequiresBooleanWhileAndAlsoConditions) {
    Analysis analysis = Analyze("void Test() { while 1 {} also 2 {} }");

    ASSERT_EQ(analysis.diagnostics.Diagnostics().size(), 2);
    EXPECT_EQ(analysis.diagnostics.Diagnostics()[0].code,
              DiagnosticCode::InvalidCondition);
    EXPECT_EQ(analysis.diagnostics.Diagnostics()[1].code,
              DiagnosticCode::InvalidCondition);
}

TEST(SemanticLoopControlTests, ResolvesPositionsAcrossNestedLoops) {
    Analysis analysis = Analyze(
        "void Test() { while true { while true { jumpto outer; break; } "
        "#outer proceed; } }");

    EXPECT_FALSE(analysis.diagnostics.HasErrors());
}

TEST(SemanticLoopControlTests, DiagnosesInvalidLoopControlAndPositions) {
    Analysis analysis = Analyze(
        "void Test() { proceed; break; jumpto missing; #outside "
        "while true { #duplicate #duplicate jumpto unknown; } }");

    EXPECT_NE(FindDiagnostic(analysis, DiagnosticCode::LoopControlOutsideLoop),
              nullptr);
    EXPECT_NE(FindDiagnostic(analysis, DiagnosticCode::DuplicatePosition),
              nullptr);
    EXPECT_NE(FindDiagnostic(analysis, DiagnosticCode::UnknownPosition),
              nullptr);
}

TEST(SemanticCastTests, TypesChainedNumericCasts) {
    Analysis analysis = Analyze(
        "i32 main() { value := 32.cast<i64>.cast<u8> as i32; "
        "return value; }");

    ASSERT_FALSE(analysis.diagnostics.HasErrors());
    const FunctionDeclaration& function = FunctionAt(analysis, 0);
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(VariableAt(function, 0))->Type().Kind(),
        SemanticTypeKind::I32);
}

TEST(SemanticCastTests, AllowsSameTypeCastsInBothForms) {
    Analysis analysis = Analyze(
        "i32 main() { first := 32 as i32; second := 32.cast<i32>; "
        "return first + second; }");

    EXPECT_FALSE(analysis.diagnostics.HasErrors());
}

TEST(SemanticCastTests, RejectsNonNumericCasts) {
    Analysis analysis =
        Analyze("i32 main() { value := true.cast<i32>; return 0; }");

    EXPECT_NE(FindDiagnostic(analysis, DiagnosticCode::InvalidCast), nullptr);
}

TEST(SemanticCastTests, CentralizesExtensibleExplicitCastPolicy) {
    EXPECT_TRUE(CanExplicitlyCast(SemanticType(SemanticTypeKind::I32),
                                  SemanticType(SemanticTypeKind::F64)));
    EXPECT_FALSE(CanExplicitlyCast(SemanticType(SemanticTypeKind::StringView),
                                   SemanticType(SemanticTypeKind::I32)));
}

TEST(SemanticStringViewTests, SupportsExplicitAndInferredViews) {
    Analysis analysis = Analyze(
        "stringv Identity(stringv value) <- value { return value; } "
        "i32 main() { stringv explicitValue = \"Flecha\"; "
        "inferred := Identity(explicitValue); "
        "if inferred == \"Flecha\" { return 42; } return 0; }");

    ASSERT_FALSE(analysis.diagnostics.HasErrors());
    const FunctionDeclaration& mainFunction = FunctionAt(analysis, 1);
    EXPECT_EQ(analysis.model.DeclaredSymbol(VariableAt(mainFunction, 0))
                  ->Type()
                  .Kind(),
              SemanticTypeKind::StringView);
    EXPECT_EQ(analysis.model.DeclaredSymbol(VariableAt(mainFunction, 1))
                  ->Type()
                  .Kind(),
              SemanticTypeKind::StringView);
}

TEST(SemanticDiagnosticTests, ReportsDuplicateAtExactIdentifierRange) {
    const std::string source = "i32 main() { x := 1; x := 2; return x; }";
    Analysis analysis = Analyze(source);
    const Diagnostic* diagnostic =
        FindDiagnostic(analysis, DiagnosticCode::DuplicateDeclaration);

    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(DiagnosticCodeName(diagnostic->code), "FLECHA-S0001");
    EXPECT_EQ(diagnostic->range.start.offset, source.find("x := 2"));
    EXPECT_EQ(diagnostic->range.Length(), 1);
}

TEST(SemanticDiagnosticTests,
     ReportsUnknownNameAndAssignmentBeforeDeclaration) {
    Analysis analysis =
        Analyze("i32 main() { missing; later = 1; later := 2; return later; }");

    ASSERT_EQ(analysis.diagnostics.Diagnostics().size(), 2);
    EXPECT_EQ(analysis.diagnostics.Diagnostics()[0].code,
              DiagnosticCode::UnknownName);
    EXPECT_EQ(analysis.diagnostics.Diagnostics()[1].code,
              DiagnosticCode::UnknownName);
    EXPECT_EQ(DiagnosticCodeName(DiagnosticCode::UnknownName), "FLECHA-S0002");
}

TEST(SemanticDiagnosticTests, ReportsInvalidTargetAndIncompatibleAssignments) {
    Analysis analysis =
        Analyze("i32 main() { x := 1; (x + 1) = 2; x = false; return x; }");

    EXPECT_NE(FindDiagnostic(analysis,
                             DiagnosticCode::SemanticInvalidAssignmentTarget),
              nullptr);
    const Diagnostic* incompatible =
        FindDiagnostic(analysis, DiagnosticCode::IncompatibleType);
    ASSERT_NE(incompatible, nullptr);
    EXPECT_EQ(DiagnosticCodeName(incompatible->code), "FLECHA-S0003");
}

TEST(SemanticDiagnosticTests, ChecksOperatorsConditionsReturnsAndVoidLocals) {
    Analysis analysis = Analyze(
        "void noop() { return; } i32 main() { void bad = none; "
        "inferred := noop(); invalid := !1; if 1 { return false; } "
        "return false; }");

    EXPECT_NE(FindDiagnostic(analysis, DiagnosticCode::InvalidVariableType),
              nullptr);
    EXPECT_NE(FindDiagnostic(analysis, DiagnosticCode::CannotInferType),
              nullptr);
    EXPECT_NE(FindDiagnostic(analysis, DiagnosticCode::InvalidOperand),
              nullptr);
    EXPECT_NE(FindDiagnostic(analysis, DiagnosticCode::InvalidCondition),
              nullptr);
    EXPECT_NE(FindDiagnostic(analysis, DiagnosticCode::InvalidReturnType),
              nullptr);
}

TEST(SemanticDiagnosticTests, RequiresReturnsOnEveryReachableValuePath) {
    Analysis missing = Analyze(
        "i32 empty() {} "
        "i32 partial(bool condition) { if condition { return 1; } }");
    ASSERT_EQ(missing.diagnostics.Diagnostics().size(), 2);
    for (const Diagnostic& diagnostic : missing.diagnostics.Diagnostics()) {
        EXPECT_EQ(diagnostic.code, DiagnosticCode::InvalidReturnType);
        EXPECT_NE(diagnostic.message.find("not all control paths"),
                  std::string::npos);
    }

    Analysis complete = Analyze(
        "i32 direct() { return 1; } "
        "i32 branches(bool condition) { if condition { return 1; } "
        "else { return 2; } } "
        "i32 diverges() { loop ;; {} } "
        "i32 loop_return() { loop ;; { return 3; } } "
        "void no_value_required() {}");
    EXPECT_FALSE(complete.diagnostics.HasErrors());

    Analysis brokenLoop = Analyze("i32 broken() { loop ;; { break; } }");
    EXPECT_NE(FindDiagnostic(brokenLoop, DiagnosticCode::InvalidReturnType),
              nullptr);
}

TEST(SemanticDiagnosticTests, RejectsAssignmentToFunctionSymbol) {
    Analysis analysis = Analyze(
        "i32 value() { return 1; } i32 main() { value = 2; return 0; }");

    const Diagnostic* diagnostic =
        FindDiagnostic(analysis, DiagnosticCode::SymbolNotWritable);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->message, "'value' is not writable");
}

TEST(SemanticCallTests, ValidatesCalleeArityAndArgumentTypes) {
    Analysis valid = Analyze(
        "i32 add(i32 a, i32 b) { return a + b; } "
        "i32 main() { result := add(1, 2); return result; }");
    EXPECT_FALSE(valid.diagnostics.HasErrors());

    Analysis invalid = Analyze(
        "i32 add(i32 a, bool b) { return a; } "
        "i32 main() { value := 1; value(); add(1); add(false, 2); "
        "return value; }");
    EXPECT_NE(FindDiagnostic(invalid, DiagnosticCode::NotCallable), nullptr);
    EXPECT_NE(FindDiagnostic(invalid, DiagnosticCode::WrongArgumentCount),
              nullptr);
    EXPECT_NE(FindDiagnostic(invalid, DiagnosticCode::IncompatibleArgument),
              nullptr);
}

TEST(SemanticCallTests, ResolvesForwardAndRecursiveCalls) {
    Analysis analysis = Analyze(
        "i32 main() { return Add(Factorial(3), 36); } "
        "i32 Add(i32 left, i32 right) { return left + right; } "
        "i32 Factorial(i32 value) { if value <= 1 { return 1; } "
        "return value * Factorial(value - 1); }");

    EXPECT_FALSE(analysis.diagnostics.HasErrors());
}

TEST(SemanticCallTests, RejectsDuplicateFunctionDeclarations) {
    Analysis analysis = Analyze(
        "i32 Calculate(i32 value) { return value; } "
        "bool Calculate(bool value) { return value; }");

    EXPECT_NE(FindDiagnostic(analysis, DiagnosticCode::DuplicateDeclaration),
              nullptr);
}

TEST(SemanticIntegrationTests, AnalyzesCompleteValidProgram) {
    Analysis analysis = Analyze(
        "i32 add(i32 left, i32 right) { return left + right; } "
        "i32 main() { i32 seed = 32; doubled := seed + seed; "
        "if true { doubled := 4; doubled = doubled + 1; } "
        "doubled = add(doubled, seed); return doubled; }");

    EXPECT_FALSE(analysis.diagnostics.HasErrors());
    EXPECT_TRUE(analysis.diagnostics.Empty());
}

TEST(SemanticIntegrationTests, RecoversMultipleStableExactDiagnostics) {
    const std::string source =
        "i32 id(i32 value) { return value; } "
        "i32 main() { x := 1; x := false; missing = 2; x = false; "
        "x(); id(); return false; }";
    Analysis analysis = Analyze(source);

    EXPECT_NE(FindDiagnostic(analysis, DiagnosticCode::DuplicateDeclaration),
              nullptr);
    EXPECT_NE(FindDiagnostic(analysis, DiagnosticCode::UnknownName), nullptr);
    EXPECT_NE(FindDiagnostic(analysis, DiagnosticCode::IncompatibleType),
              nullptr);
    EXPECT_NE(FindDiagnostic(analysis, DiagnosticCode::NotCallable), nullptr);
    EXPECT_NE(FindDiagnostic(analysis, DiagnosticCode::WrongArgumentCount),
              nullptr);
    EXPECT_NE(FindDiagnostic(analysis, DiagnosticCode::InvalidReturnType),
              nullptr);
    const Diagnostic* unknown =
        FindDiagnostic(analysis, DiagnosticCode::UnknownName);
    ASSERT_NE(unknown, nullptr);
    EXPECT_EQ(unknown->range.start.offset, source.find("missing"));
    EXPECT_EQ(unknown->range.Length(), std::string("missing").size());
}

TEST(SemanticControlFlowAssignmentTests, IntersectsOnlyFallthroughBranches) {
    const std::vector<std::string> programs = {
        "i32 main(bool c) { i32 x; if c { return 0; } else { x = 42; } return "
        "x; }",
        "i32 main(bool c) { i32 x; if c { x = 42; } else { return 0; } return "
        "x; }",
        "i32 main(bool a, bool b) { i32 x; if a { return 0; } "
        "else if b { x = 42; } else { x = 40; } return x; }",
        "i32 main(bool a, bool b) { i32 x; if a { if b { x = 42; } "
        "else { return 0; } } else { x = 40; } return x; }",
        "i32 main(bool c) { string x; if c { x = \"yes\"; } "
        "else { x = \"no\"; } stdoutln(x); return 42; }",
        "i32 main(bool c) { i32 x; if c { return 1; } else { return 2; } "
        "return x; }",
    };
    for (const std::string& program : programs) {
        SCOPED_TRACE(program);
        EXPECT_FALSE(Analyze(program).diagnostics.HasErrors());
    }
}

TEST(SemanticControlFlowAssignmentTests, RejectsIncompleteBranchJoins) {
    const std::vector<std::string> programs = {
        "i32 main(bool c) { i32 x; if c { x = 42; } else {} return x; }",
        "i32 main(bool c) { i32 x; if c { x = 42; } return x; }",
        "i32 main(bool a, bool b) { i32 x; if a { x = 42; } "
        "else if b { return 0; } return x; }",
        "i32 main(bool a, bool b) { i32 x; if a { if b { x = 42; } } "
        "else { x = 40; } return x; }",
        "i32 main(bool c) { string x; if c { x = \"yes\"; } stdoutln(x); "
        "return 0; }",
        "i32 main(bool c) { i32 x; if c {} also c { x = 42; } return x; }",
    };
    for (const std::string& program : programs) {
        SCOPED_TRACE(program);
        Analysis analysis = Analyze(program);
        ASSERT_EQ(analysis.diagnostics.Diagnostics().size(), 1);
        EXPECT_EQ(analysis.diagnostics.Diagnostics()[0].code,
                  DiagnosticCode::UninitializedVariable);
    }
}

TEST(SemanticControlFlowAssignmentTests, ModelsBreakProceedAndAlsoEdges) {
    const std::vector<std::string> programs = {
        "void main(bool c) { while c { i32 x; if c { break; } "
        "else { x = 42; } stdoutln(x); } }",
        "void main(bool c) { while c { i32 x; if c { proceed; } "
        "else { x = 42; } stdoutln(x); } }",
        "void main(bool c) { i32 x; while c { if c { proceed; } "
        "else { x = 42; } } also c { stdoutln(x); } }",
        "void main(bool c) { i32 x; while c { if c { break; } "
        "else { x = 42; } } also c { stdoutln(x); break; } }",
        "void main(bool c) { i32 x; while c { x = 42; } "
        "also c { stdoutln(x); proceed; } }",
        "void main(bool c) { while c { i32 x; while c { break; } "
        "x = 42; stdoutln(x); } }",
        "void main(bool c) { while c { i32 x; proceed; stdoutln(x); } }",
    };
    for (const std::string& program : programs) {
        SCOPED_TRACE(program);
        EXPECT_FALSE(Analyze(program).diagnostics.HasErrors());
    }
}

TEST(SemanticControlFlowAssignmentTests,
     RejectsLoopOnlyAndUnreachableAssignments) {
    const std::vector<std::string> programs = {
        "i32 main(bool c) { i32 x; while c { x = 42; break; } return x; }",
        "i32 main(bool c) { i32 x; while c { x = 42; proceed; } return x; }",
        "i32 main(bool c) { i32 x; while c { proceed; x = 42; } return x; }",
        "void main(bool c) { i32 x; while c { stdoutln(x); x = 42; proceed; } "
        "}",
        "void main(bool c) { while c { i32 x; stdoutln(x); x = 42; } }",
        "void main(bool c) { i32 x; while c { while c { x = 42; break; } "
        "stdoutln(x); } }",
        "void main(bool c) { i32 x; while c {} also c { x = 42; } stdoutln(x); "
        "}",
        "void main(bool c) { i32 x; while c {} also c { stdoutln(x); } }",
    };
    for (const std::string& program : programs) {
        SCOPED_TRACE(program);
        Analysis analysis = Analyze(program);
        ASSERT_EQ(analysis.diagnostics.Diagnostics().size(), 1);
        EXPECT_EQ(analysis.diagnostics.Diagnostics()[0].code,
                  DiagnosticCode::UninitializedVariable);
    }
}

TEST(SemanticControlFlowAssignmentTests, FollowsForwardBackwardAndNestedJumps) {
    const std::vector<std::string> programs = {
        "void main(bool c) { i32 x; while c { x = 42; jumpto end; "
        "#end stdoutln(x); break; } }",
        "void main(bool c) { i32 x; while c { jumpto init; "
        "#read stdoutln(x); break; #init x = 42; jumpto read; } }",
        "void main(bool c) { i32 x; while c { if c { x = 1; jumpto end; } "
        "else { x = 2; jumpto end; } #end stdoutln(x); break; } }",
        "void main(bool c) { i32 x; while c { if c { x = 1; jumpto end; } "
        "x = 2; #end stdoutln(x); break; } }",
        "void main(bool c) { i32 x; while c { jumpto inner; "
        "#outer stdoutln(x); break; while c { #inner x = 42; jumpto outer; } } "
        "}",
        "void main(bool c) { while c { i32 x; if c { jumpto end; } "
        "else { x = 42; } stdoutln(x); #end break; } }",
        "void main(bool c) { while c { jumpto position; string x; "
        "#position x = \"ready\"; stdoutln(x); break; } }",
    };
    for (const std::string& program : programs) {
        SCOPED_TRACE(program);
        EXPECT_FALSE(Analyze(program).diagnostics.HasErrors());
    }
}

TEST(SemanticControlFlowAssignmentTests,
     RejectsBypassedDeclarationsAndAssignments) {
    const std::vector<std::string> programs = {
        "void main(bool c) { i32 x; while c { jumpto end; x = 42; "
        "#end stdoutln(x); break; } }",
        "void main(bool c) { while c { jumpto end; x := 42; "
        "#end stdoutln(x); break; } }",
        "void main(bool c) { while c { jumpto end; string x = \"ready\"; "
        "#end stdoutln(x); break; } }",
        "void main(bool c) { while c { jumpto end; stringv x = \"ready\"; "
        "#end stdoutln(x); break; } }",
        "void main(bool c) { i32 x; while c { if c { jumpto end; } "
        "x = 42; #end stdoutln(x); break; } }",
        "void main(bool c) { i32 x; while c { if c { x = 42; jumpto end; } "
        "#end stdoutln(x); break; } }",
        "void main(bool c) { i32 x; while c { #read stdoutln(x); "
        "x = 42; jumpto read; } }",
        "void main(bool c) { i32 x; while c { jumpto inner; "
        "#outer stdoutln(x); break; while c { #inner jumpto outer; x = 42; } } "
        "}",
        "void main(bool c) { while c { jumpto inner; while c { x := 42; "
        "#inner stdoutln(x); break; } } }",
        "void main(bool c) { while c { #again i32 x; if c { x = 42; jumpto "
        "again; } "
        "stdoutln(x); break; } }",
    };
    for (const std::string& program : programs) {
        SCOPED_TRACE(program);
        Analysis analysis = Analyze(program);
        ASSERT_EQ(analysis.diagnostics.Diagnostics().size(), 1);
        EXPECT_EQ(analysis.diagnostics.Diagnostics()[0].code,
                  DiagnosticCode::UninitializedVariable);
    }
}

TEST(SemanticControlFlowAssignmentTests, DropsLocalsWhenJumpsLeaveTheirScope) {
    Analysis analysis = Analyze(
        "void main(bool c) { while c { if c { string x = \"owned\"; "
        "jumpto outside; #inside stdoutln(x); break; } "
        "#outside jumpto inside; } }");
    ASSERT_EQ(analysis.diagnostics.Diagnostics().size(), 1);
    EXPECT_EQ(analysis.diagnostics.Diagnostics()[0].code,
              DiagnosticCode::UninitializedVariable);
}

TEST(SemanticControlFlowAssignmentTests, DistinguishesShadowedSymbols) {
    Analysis invalid = Analyze(
        "i32 main(bool c) { i32 x; if c { x := 42; stdoutln(x); } "
        "else { x = 40; } return x; }");
    ASSERT_EQ(invalid.diagnostics.Diagnostics().size(), 1);
    EXPECT_EQ(invalid.diagnostics.Diagnostics()[0].code,
              DiagnosticCode::UninitializedVariable);
    Analysis valid = Analyze(
        "i32 main(bool c) { x := 42; if c { i32 x; x = 1; stdoutln(x); } "
        "return x; }");
    EXPECT_FALSE(valid.diagnostics.HasErrors());
    Analysis branchLocal =
        Analyze("i32 main(bool c) { if c { x := 42; } return x; }");
    EXPECT_NE(FindDiagnostic(branchLocal, DiagnosticCode::UnknownName),
              nullptr);
}

TEST(SemanticControlFlowAssignmentTests,
     HonorsExpressionEvaluationAndShortCircuit) {
    const std::vector<std::string> valid = {
        "i32 main(bool c) { i32 x; if (x = 42) == 42 { } return x; }",
        "void main(bool c) { bool x; if c and (x = c) { stdoutln(x); } }",
        "void main(bool c) { bool x; if c or (x = c) {} else { stdoutln(x); } "
        "}",
        "void main(bool c) { bool x; if !(c and (x = c)) {} else { "
        "stdoutln(x); } }",
        "i32 main(bool c) { i32 x; while (x = 42) == 0 {} return x; }",
        "i32 main() { i32 x; return (x = 21) + x; }",
    };
    for (const std::string& program : valid) {
        SCOPED_TRACE(program);
        EXPECT_FALSE(Analyze(program).diagnostics.HasErrors());
    }
    const std::vector<std::string> invalid = {
        "bool main(bool c) { bool x; c and (x = true); return x; }",
        "bool main(bool c) { bool x; c or (x = true); return x; }",
        "void main(bool c) { bool x; if c and (x = c) {} else { stdoutln(x); } "
        "}",
        "void main(bool c) { bool x; if c or (x = c) { stdoutln(x); } }",
        "i32 main() { i32 x; return x + (x = 21); }",
        "i32 main() { i32 x; x = x + 1; return x; }",
    };
    for (const std::string& program : invalid) {
        SCOPED_TRACE(program);
        Analysis analysis = Analyze(program);
        ASSERT_EQ(analysis.diagnostics.Diagnostics().size(), 1);
        EXPECT_EQ(analysis.diagnostics.Diagnostics()[0].code,
                  DiagnosticCode::UninitializedVariable);
    }
}

TEST(SemanticControlFlowAssignmentTests, ReportsExactReachableReadRangeOnce) {
    const std::string source =
        "void main(bool c) {\n"
        "    i32 value;\n"
        "    while c {\n"
        "        jumpto read;\n"
        "        value = 42;\n"
        "        #read stdoutln(value);\n"
        "        jumpto read;\n"
        "    }\n"
        "}";
    Analysis analysis = Analyze(source);
    ASSERT_EQ(analysis.diagnostics.Diagnostics().size(), 1);
    const Diagnostic& diagnostic = analysis.diagnostics.Diagnostics()[0];
    EXPECT_EQ(DiagnosticCodeName(diagnostic.code), "FLECHA-S0021");
    EXPECT_EQ(diagnostic.range.start.offset, source.find("value);"));
    EXPECT_EQ(diagnostic.range.start.line, 6);
    EXPECT_EQ(diagnostic.range.start.column, 24);
    EXPECT_EQ(diagnostic.range.end.line, 6);
    EXPECT_EQ(diagnostic.range.end.column, 29);
    EXPECT_EQ(diagnostic.range.Length(), 5);
}

TEST(SemanticTypeTests, NormalizesAliasesAndRetainsEachQualifierLayer) {
    const std::vector<std::pair<std::string, std::string>> aliases{
        {"constptr u8", "const *u8"},
        {"constref u8", "const &u8"},
        {"constref &u8", "const & &u8"},
        {"&constptr *constref const u8", "&const **const &const u8"}};
    for (const auto& [alias, expanded] : aliases) {
        SCOPED_TRACE(alias);
        Analysis analysis =
            Analyze("void f(" + alias + " a, " + expanded + " b) {}");
        const FunctionDeclaration& function = FunctionAt(analysis, 0);
        const SemanticType first =
            SemanticType::FromSyntax(function.Parameters()[0]->Type());
        const SemanticType second =
            SemanticType::FromSyntax(function.Parameters()[1]->Type());
        EXPECT_EQ(first, second);
        EXPECT_EQ(first.Name(), expanded);
        const FunctionSymbol& symbol = static_cast<const FunctionSymbol&>(
            *analysis.model.DeclaredSymbol(function));
        EXPECT_EQ(symbol.ParameterTypes()[0], first);
        EXPECT_EQ(symbol.ParameterTypes()[1], second);
    }
    const SemanticType byte(SemanticTypeKind::U8);
    const SemanticType readOnlyByte(SemanticTypeKind::U8, true);
    const SemanticType fixedPointer(SemanticTypeKind::Pointer, byte, true);
    const SemanticType readOnlyPointer(SemanticTypeKind::Pointer, readOnlyByte);
    EXPECT_NE(fixedPointer, readOnlyPointer);
    EXPECT_FALSE(IsAssignable(fixedPointer, readOnlyPointer));
    EXPECT_TRUE(IsAssignable(readOnlyPointer, fixedPointer));
    EXPECT_EQ(fixedPointer.WithoutConst().Target()->Name(), "u8");
    EXPECT_EQ(readOnlyPointer.WithoutConst().Target()->Name(), "const u8");
    const SemanticType reference(SemanticTypeKind::Reference, byte);
    EXPECT_FALSE(IsAssignable(reference, SemanticType(SemanticTypeKind::None)));
    EXPECT_FALSE(CanExplicitlyCast(reference, reference));
    EXPECT_FALSE(CanExplicitlyCast(fixedPointer, readOnlyPointer));
    EXPECT_TRUE(IsAssignable(readOnlyByte, byte));
}

TEST(SemanticConstTests, AllowsRuntimeInitializationAndMutableValueCopies) {
    Analysis analysis = Analyze(
        "const i32 get(const i32 value) { return value; } "
        "i32 main() { const i32 answer = get(42); copy := answer; "
        "copy = copy + 1; const i32 zero = none; "
        "const bool yes = answer == 42; const i64 wide = answer as const i64; "
        "const string text = \"hi\"; const stringv view = text.view(); "
        "return copy - 1; }");
    ASSERT_FALSE(analysis.diagnostics.HasErrors());
    const FunctionDeclaration& function = FunctionAt(analysis, 1);
    const Symbol* answer =
        analysis.model.DeclaredSymbol(VariableAt(function, 0));
    const Symbol* copy = analysis.model.DeclaredSymbol(VariableAt(function, 1));
    ASSERT_NE(answer, nullptr);
    ASSERT_NE(copy, nullptr);
    EXPECT_FALSE(answer->Writable());
    EXPECT_TRUE(answer->Type().IsConst());
    EXPECT_TRUE(copy->Writable());
    EXPECT_FALSE(copy->Type().IsConst());
    EXPECT_FALSE(analysis.model.TypeOf(*VariableAt(function, 1).Initializer())
                     .IsConst());
}

TEST(SemanticConstTests, RejectsWritesToLocalsAndParametersAtExactNameRange) {
    const std::vector<std::string> sources{
        "void f() {\n    const i32 value = 1;\n    value = 2;\n}",
        "void f(const i32 value) {\n    value = 2;\n}"};
    for (const std::string& source : sources) {
        SCOPED_TRACE(source);
        Analysis analysis = Analyze(source);
        ASSERT_EQ(analysis.diagnostics.Diagnostics().size(), 1);
        const Diagnostic& diagnostic = analysis.diagnostics.Diagnostics()[0];
        EXPECT_EQ(DiagnosticCodeName(diagnostic.code), "FLECHA-S0005");
        EXPECT_EQ(diagnostic.range.start.offset, source.rfind("value"));
        EXPECT_EQ(diagnostic.range.Length(), 5);
        EXPECT_EQ(diagnostic.range.start.column, 5);
        EXPECT_EQ(diagnostic.range.end.column, 10);
    }
}

TEST(SemanticConstTests, RequiresInitializerAndPreservesBorrowRestrictions) {
    Analysis analysis = Analyze("void f() { const i32 value; }");
    ASSERT_EQ(analysis.diagnostics.Diagnostics().size(), 1);
    const Diagnostic& diagnostic = analysis.diagnostics.Diagnostics()[0];
    EXPECT_EQ(DiagnosticCodeName(diagnostic.code), "FLECHA-S0023");
    EXPECT_EQ(diagnostic.range.start.offset, 21);
    EXPECT_EQ(diagnostic.range.Length(), 5);
    Analysis borrowed = Analyze("void f() { const stringv value; }");
    EXPECT_NE(
        FindDiagnostic(borrowed, DiagnosticCode::BorrowRequiresInitializer),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(borrowed, DiagnosticCode::ConstRequiresInitializer),
        nullptr);
    Analysis escaped = Analyze(
        "stringv f() { const string value = \"hi\"; return value.view(); }");
    EXPECT_NE(FindDiagnostic(escaped, DiagnosticCode::BorrowEscapesOwner),
              nullptr);
    Analysis invalid = Analyze("void f() { const void value = none; }");
    EXPECT_NE(FindDiagnostic(invalid, DiagnosticCode::InvalidVariableType),
              nullptr);
}

TEST(SemanticConstTests,
     UsesResolvedSymbolsForShadowingAndConstvalIsAnIdentifier) {
    Analysis analysis = Analyze(
        "i32 f() { const i32 value = 42; if true { i32 value = 0; "
        "value = 1; } constval := value; constval = constval + 1; "
        "return constval - 1; }");
    EXPECT_FALSE(analysis.diagnostics.HasErrors());
    Analysis invalid = Analyze(
        "i32 f() { i32 value = 42; if true { const i32 value = 0; "
        "value = 1; } value = 43; return value - 1; }");
    ASSERT_EQ(invalid.diagnostics.Diagnostics().size(), 1);
    EXPECT_EQ(invalid.diagnostics.Diagnostics()[0].code,
              DiagnosticCode::SymbolNotWritable);
}

TEST(SemanticConstTests, CannotBypassInitializationWithJump) {
    Analysis analysis = Analyze(
        "i32 f() { while true { jumpto end; const i32 value = 42; #end return "
        "value; } return 0; }");
    ASSERT_EQ(analysis.diagnostics.Diagnostics().size(), 1);
    EXPECT_EQ(analysis.diagnostics.Diagnostics()[0].code,
              DiagnosticCode::UninitializedVariable);
}

TEST(SemanticCompoundTypeTests, RejectsUnsupportedNestedReferenceStorage) {
    for (const std::string& type :
         {"*&i32", "& &i32", "&string", "&stringv", "&void"}) {
        const std::string source = "void f(" + type + " value) {}";
        SCOPED_TRACE(source);
        Analysis analysis = Analyze(source);
        const Diagnostic* diagnostic =
            FindDiagnostic(analysis, DiagnosticCode::UnsupportedCompoundType);
        ASSERT_NE(diagnostic, nullptr);
        EXPECT_EQ(diagnostic->range.start.offset, source.find(type));
        EXPECT_EQ(diagnostic->range.Length(), type.size());
    }
}

TEST(SemanticCompoundTypeTests, RejectsDereferencingScalar) {
    EXPECT_NE(
        FindDiagnostic(Analyze("i32 f() { i32 value = 42; return *value; }"),
                       DiagnosticCode::InvalidOperand),
        nullptr);
}

TEST(SemanticUnsafeTests, EnforcesLexicalPermissionAndExactRanges) {
    const std::string source =
        "unsafe void touch(*i32 p) { *p = 42; }\n"
        "void f(*i32 p) {\n"
        "    touch(p);\n"
        "    *p;\n"
        "    *p = 1;\n"
        "    &raw p;\n"
        "    unsafe { touch(p); { *p = 2; } }\n"
        "    touch(p);\n"
        "}";
    Analysis analysis = Analyze(source);
    ASSERT_EQ(analysis.diagnostics.Diagnostics().size(), 5);
    const std::vector<std::size_t> lines{3, 4, 5, 6, 8};
    const std::vector<std::size_t> lengths{5, 2, 2, 6, 5};
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const Diagnostic& diagnostic =
            analysis.diagnostics.Diagnostics()[index];
        EXPECT_EQ(DiagnosticCodeName(diagnostic.code), "FLECHA-S0025");
        EXPECT_EQ(diagnostic.range.start.line, lines[index]);
        EXPECT_EQ(diagnostic.range.start.column, 5);
        EXPECT_EQ(diagnostic.range.Length(), lengths[index]);
    }
    Analysis callee = Analyze(
        "void safe(*i32 p) { *p = 1; } unsafe void caller(*i32 p) { safe(p); "
        "}");
    ASSERT_EQ(callee.diagnostics.Diagnostics().size(), 1);
    EXPECT_EQ(callee.diagnostics.Diagnostics()[0].code,
              DiagnosticCode::UnsafeOperation);
}

TEST(SemanticUnsafeTests, AllowsUnsafeCallsAndPointerTransportInSafeCode) {
    Analysis analysis = Analyze(
        "unsafe i32 read(*const i32 p) { return *p; } "
        "unsafe i32 forward(*i32 p) { return read(p); } "
        "*i32 transport(*i32 p) { *i32 copy = p; return copy; } "
        "i32 main() { i32 value = 42; *i32 p = none; "
        "unsafe { p = &raw value; } p = transport(p); "
        "if p != null and none != p { unsafe { return forward(p); } } return "
        "0; }");
    EXPECT_FALSE(analysis.diagnostics.HasErrors());
}

TEST(SemanticUnsafeTests, SeparatesBindingConstnessFromTargetConstness) {
    Analysis valid = Analyze(
        "unsafe void f() { i32 x = 1; const i32 y = 2; "
        "constptr i32 fixed = &raw x; *fixed = 3; "
        "*const i32 readOnly = &raw y; readOnly = &raw x; "
        "constptr const i32 both = &raw y; i32 copy = *both; "
        "**i32 nested = &raw readOnly; }");
    // A pointer to a read-only pointer target cannot become **i32.
    ASSERT_EQ(valid.diagnostics.Diagnostics().size(), 1);
    EXPECT_EQ(valid.diagnostics.Diagnostics()[0].code,
              DiagnosticCode::IncompatibleType);
    const std::vector<std::string> invalid{
        "constptr i32 fixed = none; fixed = none;",
        "*const i32 p = none; *p = 1;", "constptr const i32 p = none; *p = 1;",
        "const i32 x = 1; *(&raw x) = 2;", "*const *i32 p = none; *p = none;"};
    for (const std::string& body : invalid) {
        SCOPED_TRACE(body);
        Analysis analysis = Analyze("unsafe void f() { " + body + " }");
        ASSERT_EQ(analysis.diagnostics.Diagnostics().size(), 1);
        EXPECT_EQ(analysis.diagnostics.Diagnostics()[0].code,
                  DiagnosticCode::SymbolNotWritable);
    }
    Analysis deep = Analyze("unsafe void f(**i32 p) { **const i32 q = p; }");
    EXPECT_NE(FindDiagnostic(deep, DiagnosticCode::IncompatibleType), nullptr);
    Analysis missing = Analyze("void f() { constptr i32 p; }");
    EXPECT_NE(FindDiagnostic(missing, DiagnosticCode::ConstRequiresInitializer),
              nullptr);
}

TEST(SemanticUnsafeTests, ChecksInitializedAddressesAndIndirectStoreTargets) {
    const std::vector<std::string> invalid{
        "unsafe void f() { i32 x; *i32 p = &raw x; }",
        "unsafe void f() { *i32 p; *p = 42; }",
        "unsafe void f() { *i32 p; *p = 42; *p; }",
        "i32 init(*i32 p) { return 42; } unsafe void f() { *i32 p; *p = init(p "
        "= none); }",
        "void f() { i32 x; unsafe { i32 y = x; } }",
        "void f(bool c) { i32 x; if c { unsafe { x = 1; } } stdoutln(x); }",
        "void f() { while true { jumpto use; unsafe { *i32 p = none; #use *p = "
        "1; } } }"};
    for (const std::string& source : invalid) {
        SCOPED_TRACE(source);
        Analysis analysis = Analyze(source);
        EXPECT_NE(
            FindDiagnostic(analysis, DiagnosticCode::UninitializedVariable),
            nullptr);
    }
    Analysis valid = Analyze(
        "i32 f() { i32 x; unsafe { (x) = 42; } { i32 x = 0; x = 1; } return x; "
        "}");
    EXPECT_FALSE(valid.diagnostics.HasErrors());
    Analysis scope =
        Analyze("void f() { unsafe { i32 x = 42; } stdoutln(x); }");
    EXPECT_NE(FindDiagnostic(scope, DiagnosticCode::UnknownName), nullptr);
}

TEST(SemanticUnsafeTests,
     RejectsUnsupportedAddressTargetsAndPointerOperations) {
    const std::vector<std::string> invalid{
        "unsafe void f() { &raw 42; }", "unsafe void f() { &raw (1 + 2); }",
        "unsafe void f() { &raw f; }", "unsafe void f(*void p) { *p; }"};
    for (const std::string& source : invalid) {
        SCOPED_TRACE(source);
        EXPECT_NE(FindDiagnostic(Analyze(source),
                                 DiagnosticCode::InvalidPointerAccess),
                  nullptr);
    }
    for (const std::string& body : {"p + 1;", "p - p;", "p < p;", "p as i64;",
                                    "1 as *i32;", "p as *const i32;"}) {
        SCOPED_TRACE(body);
        EXPECT_TRUE(Analyze("unsafe void f(*i32 p) { " + body + " }")
                        .diagnostics.HasErrors());
    }
    EXPECT_NE(FindDiagnostic(Analyze("void f(*&i32 p) {}"),
                             DiagnosticCode::UnsupportedCompoundType),
              nullptr);
    EXPECT_FALSE(
        Analyze("unsafe void f() { i32 x = 1; &x; }").diagnostics.HasErrors());
}

TEST(SemanticUnsafeTests, RejectsConstTargetWritesAtTheDereferenceRange) {
    const std::string source = "unsafe void f(*const i32 p) {\n    *p = 42;\n}";
    Analysis analysis = Analyze(source);
    ASSERT_EQ(analysis.diagnostics.Diagnostics().size(), 1);
    const Diagnostic& diagnostic = analysis.diagnostics.Diagnostics()[0];
    EXPECT_EQ(diagnostic.code, DiagnosticCode::SymbolNotWritable);
    EXPECT_EQ(diagnostic.range.start.line, 2);
    EXPECT_EQ(diagnostic.range.start.column, 5);
    EXPECT_EQ(diagnostic.range.end.column, 7);
}

TEST(SemanticUnsafeTests,
     AddressCreationPreservesNestedConstAndRawIsContextual) {
    Analysis analysis = Analyze(
        "unsafe void f() { const i32 value = 42; p := &raw value; "
        "constptr i32 fixed = none; slot := &raw fixed; raw := 1; raw = 2; }");
    ASSERT_FALSE(analysis.diagnostics.HasErrors());
    const auto& function = FunctionAt(analysis, 0);
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(VariableAt(function, 1))->Type().Name(),
        "*const i32");
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(VariableAt(function, 3))->Type().Name(),
        "*const *i32");
    Analysis deep = Analyze("unsafe void f(*const *i32 slot) { **slot = 42; }");
    EXPECT_FALSE(deep.diagnostics.HasErrors());
}

TEST(SemanticUnsafeTests,
     RejectsIncompatibleNullComparisonsBeforeCodeGeneration) {
    for (const std::string& expression :
         {"null == 1", "null === 1", "1 !== null", "p == 1", "p === none"}) {
        SCOPED_TRACE(expression);
        Analysis analysis =
            Analyze("bool f(*i32 p) { return " + expression + "; }");
        ASSERT_EQ(analysis.diagnostics.Diagnostics().size(), 1);
        EXPECT_EQ(analysis.diagnostics.Diagnostics()[0].code,
                  DiagnosticCode::InvalidOperand);
    }
}

TEST(SemanticReferenceTests, InfersReferenceAndContextualByteLiterals) {
    Analysis analysis = Analyze(
        "u8 f() { u8 value = 3; ref := &value; *ref = *ref * 2; return value; "
        "}");
    ASSERT_FALSE(analysis.diagnostics.HasErrors());
    const auto& function = FunctionAt(analysis, 0);
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(VariableAt(function, 1))->Type().Name(),
        "&u8");
    EXPECT_EQ(
        analysis.model.TypeOf(*VariableAt(function, 0).Initializer()).Name(),
        "u8");
}

TEST(SemanticReferenceTests, CopiesSharedHandlesAndInfersConstStorage) {
    Analysis analysis = Analyze(
        "i32 f() { const i32 value = 3; r := &value; copy := r; return *r + "
        "*copy; }"
        "i32 g() { i32 value = 3; &const i32 r = &value; copy := r; return *r "
        "+ *copy; }");
    EXPECT_FALSE(analysis.diagnostics.HasErrors());
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(VariableAt(FunctionAt(analysis, 0), 1))
            ->Type()
            .Name(),
        "&const i32");
}

TEST(SemanticReferenceTests,
     TransfersMutableHandlesAndReinitializesMovedBindings) {
    for (const std::string& body :
         {"r := &x; moved := r; *moved = 4; return x;",
          "r := &x; moved := r; *moved = 4; r = &y; *r = 5; return x + y;",
          "r := &x; r = &y; *r = 5; return x + y;",
          "r := &x; r = r; *r = 5; return x;"}) {
        SCOPED_TRACE(body);
        EXPECT_FALSE(Analyze("i32 f() { i32 x = 1; i32 y = 2; " + body + " }")
                         .diagnostics.HasErrors());
    }
    const std::string source =
        "i32 f() { i32 x = 1; r := &x; moved := r; return *r; }";
    Analysis analysis = Analyze(source);
    const Diagnostic* diagnostic =
        FindDiagnostic(analysis, DiagnosticCode::MovedReference);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->range.start.offset, source.rfind("r;"));
    EXPECT_EQ(diagnostic->range.Length(), 1);
}

TEST(SemanticReferenceTests,
     FixedBindingsAllowMutationAndReborrowingButNotMoves) {
    EXPECT_FALSE(Analyze("void use(&i32 p) { *p = 6; }"
                         "i32 f() { i32 x = 1; constref i32 r = &x; *r = 2; "
                         "use(r); return x; }")
                     .diagnostics.HasErrors());
    EXPECT_NE(
        FindDiagnostic(
            Analyze("void f() { i32 x = 1; constref i32 r = &x; moved := r; }"),
            DiagnosticCode::MovedReference),
        nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("void f() { i32 x = 1; i32 y = 2; "
                                     "constref i32 r = &x; r = &y; }"),
                             DiagnosticCode::SymbolNotWritable),
              nullptr);
    EXPECT_FALSE(Analyze("i32 f() { i32 x = 1; constref const i32 r = &x; copy "
                         ":= r; return *copy + *r; }")
                     .diagnostics.HasErrors());
}

TEST(SemanticReferenceTests,
     RequiresInitializedNonNullStorageAndCompatibleTypes) {
    const std::vector<std::pair<std::string, DiagnosticCode>> cases{
        {"&i32 r;", DiagnosticCode::ReferenceRequiresInitializer},
        {"&i32 r = none;", DiagnosticCode::IncompatibleType},
        {"&i32 r = null;", DiagnosticCode::IncompatibleType},
        {"i32 x; r := &x;", DiagnosticCode::UninitializedVariable},
        {"r := &42;", DiagnosticCode::InvalidReference},
        {"i32 x = 1; u32 y = 2; r := &x; r = &y;",
         DiagnosticCode::IncompatibleType},
        {"i32 x = 1; r := &x; r = 2;", DiagnosticCode::IncompatibleType},
        {"const i32 x = 1; &i32 r = &x;", DiagnosticCode::IncompatibleType},
        {"i32 x = 1; &const i32 r = &x; *r = 2;",
         DiagnosticCode::SymbolNotWritable},
        {"i32 x = 1; r := &x; r as &i32;", DiagnosticCode::InvalidCast},
        {"i32 x = 1; r := &x; r == r;", DiagnosticCode::InvalidOperand},
    };
    for (const auto& [body, code] : cases) {
        SCOPED_TRACE(body);
        EXPECT_NE(FindDiagnostic(Analyze("void f() { " + body + " }"), code),
                  nullptr);
    }
}

TEST(SemanticReferenceTests, ChecksConflictsUntilLastUse) {
    for (const std::string& body :
         {"r := &x; x = 2; return *r;",
          "r := &x; i32 copy = x; return *r + copy;",
          "r := &x; other := &x; return *r + *other;",
          "&const i32 r = &x; other := &x; return *r + *other;",
          "&const i32 r = &x; x = 2; return *r;"}) {
        SCOPED_TRACE(body);
        EXPECT_NE(FindDiagnostic(Analyze("i32 f() { i32 x = 1; " + body + " }"),
                                 DiagnosticCode::BorrowConflict),
                  nullptr);
    }
    EXPECT_FALSE(
        Analyze("i32 f() { i32 x = 1; r := &x; *r = 4; x = x + 2; return x; }")
            .diagnostics.HasErrors());
    EXPECT_FALSE(
        Analyze("i32 f() { i32 x = 1; &const i32 r = &x; return x + *r; }")
            .diagnostics.HasErrors());
    const std::string source =
        "i32 f() { i32 x = 1; r := &x; x = 2; return *r; }";
    Analysis analysis = Analyze(source);
    const Diagnostic* diagnostic =
        FindDiagnostic(analysis, DiagnosticCode::BorrowConflict);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->range.start.offset, source.find("x = 2"));
    EXPECT_EQ(diagnostic->range.Length(), 1);
}

TEST(SemanticReferenceTests, ReborrowsForCallsWithoutMovingTheCallerHandle) {
    EXPECT_FALSE(Analyze("void bump(&i32 r) { *r = *r + 1; }"
                         "i32 read(&const i32 r) { return *r; }"
                         "i32 f() { i32 x = 1; r := &x; bump(r); bump(r); "
                         "return read(r) + *r; }")
                     .diagnostics.HasErrors());
    EXPECT_FALSE(
        Analyze("i32 sum(&const i32 a, &const i32 b) { return *a + *b; }"
                "i32 f() { i32 x = 3; r := &x; return sum(r, r); }")
            .diagnostics.HasErrors());
    for (const std::string& arguments :
         {"r, r", "&*r, r", "r, &*r", "&x, &x"}) {
        SCOPED_TRACE(arguments);
        EXPECT_NE(FindDiagnostic(Analyze("void use(&i32 a, &i32 b) {} void f() "
                                         "{ i32 x = 1; r := &x; use(" +
                                         arguments + "); }"),
                                 DiagnosticCode::BorrowConflict),
                  nullptr);
    }
    EXPECT_NE(FindDiagnostic(Analyze("void use(&i32 a, i32 b) {} void f() { "
                                     "i32 x = 1; use(&x, x); }"),
                             DiagnosticCode::BorrowConflict),
              nullptr);
    EXPECT_FALSE(
        Analyze(
            "void use(i32 a, &i32 b) {} void f() { i32 x = 1; use(x, &x); }")
            .diagnostics.HasErrors());
}

TEST(SemanticReferenceTests, ChildReborrowsFreezeParentsUntilLastUse) {
    EXPECT_FALSE(Analyze("i32 f() { i32 x = 1; r := &x; child := &*r; *child = "
                         "3; *r = 6; return x; }")
                     .diagnostics.HasErrors());
    for (const std::string& middle :
         {"*r = 3;", "i32 copy = *r;", "r = &y;", "moved := r;"}) {
        SCOPED_TRACE(middle);
        EXPECT_NE(
            FindDiagnostic(
                Analyze(
                    "i32 f() { i32 x = 1; i32 y = 2; r := &x; child := &*r; " +
                    middle + " return *child; }"),
                DiagnosticCode::BorrowConflict),
            nullptr);
    }
    EXPECT_FALSE(Analyze("i32 f() { i32 x = 1; &const i32 r = &x; child := "
                         "&*r; return *r + *child; }")
                     .diagnostics.HasErrors());
}

TEST(SemanticReferenceTests, RejectsEscapesAndUnsupportedConversions) {
    for (const std::string& source :
         {"void f() { i32 x = 1; r := &x; { i32 y = 2; r = &y; } }",
          "void f() { i32 x = 1; r := &x; { child := &x; r = &*child; } }",
          "unsafe void f() { i32 x = 1; r := &x; &raw r; }",
          "unsafe void f() { i32 x = 1; r := &x; &raw *r; }"}) {
        SCOPED_TRACE(source);
        EXPECT_NE(
            FindDiagnostic(Analyze(source), DiagnosticCode::ReferenceEscape),
            nullptr);
    }
    EXPECT_NE(FindDiagnostic(Analyze("unsafe void f(*i32 p) { r := &*p; }"),
                             DiagnosticCode::InvalidReference),
              nullptr);
}

TEST(SemanticReferenceTests, ValidatesBorrowedResultContracts) {
    EXPECT_FALSE(Analyze("&i32 identity(&i32 value) <- value { return value; }")
                     .diagnostics.HasErrors());
    EXPECT_FALSE(
        Analyze("&const i32 shared(&i32 value) <- value { return value; }")
            .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("[]const i32 view([]i32 values) <- values { return "
                         "values; }")
                     .diagnostics.HasErrors());

    for (const std::string& source : {
             "&i32 missing(&i32 value) { return value; }",
             "&i32 unknown(&i32 value) <- other { return value; }",
             "&i32 owned(i32 value) <- value { return &value; }",
             "i32 unnecessary(&i32 value) <- value { return *value; }",
             "&i32 wrong(&i32 left, &i32 right) <- left { return right; }",
             "&i32 local(&i32 value) <- value { i32 result = 1; return "
             "&result; }",
             "((&i32, &i32), i32) nested(&i32 value) <- value { return "
             "((value, value), 0); }",
         }) {
        SCOPED_TRACE(source);
        EXPECT_NE(FindDiagnostic(Analyze(source),
                                 DiagnosticCode::InvalidBorrowContract),
                  nullptr);
    }
    EXPECT_NE(FindDiagnostic(
                  Analyze("&i32 mutable(&const i32 value) <- value { return "
                          "value; }"),
                  DiagnosticCode::InvalidReturnType),
              nullptr);

    const std::string unknown =
        "&i32 invalid(&i32 value) <- missing { return value; }";
    Analysis analysis = Analyze(unknown);
    const Diagnostic* diagnostic =
        FindDiagnostic(analysis, DiagnosticCode::InvalidBorrowContract);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->range.start.offset, unknown.find("missing"));
    EXPECT_EQ(diagnostic->range.Length(), std::string("missing").size());
}

TEST(SemanticReferenceTests, PropagatesBorrowedResultsIntoCallerState) {
    const std::string identity =
        "&i32 identity(&i32 value) <- value { return value; } ";
    EXPECT_NE(FindDiagnostic(
                  Analyze(identity + "i32 f() { i32 x = 1; r := identity(&x); "
                                     "x = 2; return *r; }"),
                  DiagnosticCode::BorrowConflict),
              nullptr);
    EXPECT_FALSE(Analyze(identity + "i32 f() { i32 x = 1; r := identity(&x); "
                                    "*r = 2; x = 3; return x; }")
                     .diagnostics.HasErrors());
    EXPECT_NE(
        FindDiagnostic(
            Analyze(identity + "void f() { i32 x = 1; i32 y = 2; r := &x; "
                               "child := identity(r); r = &y; *child = 3; }"),
            DiagnosticCode::BorrowConflict),
        nullptr);

    const std::string view =
        "[]const i32 view([]const i32 values) <- values { return values; } ";
    EXPECT_NE(
        FindDiagnostic(Analyze(view + "i32 f() { a := [1, 2]; s := view(a[:]); "
                                      "a[0] = 3; return s[0]; }"),
                       DiagnosticCode::BorrowConflict),
        nullptr);
}

TEST(SemanticReferenceTests,
     SupportsImmediatelyDestructuredMultipleBorrowedResults) {
    const std::string pair =
        "(&const i32, &const i32) ends(&const [2]i32 values) <- values { "
        "return (&(*values)[0], &(*values)[1]); } ";
    EXPECT_FALSE(Analyze(pair +
                         "i32 f() { a := [2, 3]; left, right := ends(&a); "
                         "return *left + *right; }")
                     .diagnostics.HasErrors());
    EXPECT_NE(
        FindDiagnostic(Analyze(pair + "i32 f() { a := [2, 3]; left, right := "
                                      "ends(&a); a[0] = 4; return *right; }"),
                       DiagnosticCode::BorrowConflict),
        nullptr);
    EXPECT_NE(FindDiagnostic(
                  Analyze(pair + "void f() { a := [2, 3]; pair := ends(&a); "
                                 "}"),
                  DiagnosticCode::InvalidBorrowContract),
              nullptr);
    EXPECT_NE(FindDiagnostic(Analyze(pair + "void f() { a := [2, 3]; "
                                            "ends(&a); }"),
                             DiagnosticCode::InvalidBorrowContract),
              nullptr);

    const std::string fields =
        "structure Pair { i32 left; i32 right; }; "
        "(&i32, &i32) fields(&Pair value) <- value { return "
        "(&value->left, &value->right); } "
        "(&i32, &i32) forward_fields(&Pair value) <- value { return "
        "fields(value); } ";
    EXPECT_FALSE(Analyze(fields +
                         "i32 f() { Pair pair = none; left, right := "
                         "forward_fields(&pair); *left = 2; *right = 3; return "
                         "pair.left + pair.right; }")
                     .diagnostics.HasErrors());
    EXPECT_NE(FindDiagnostic(
                  Analyze("structure Pair { i32 value; }; "
                          "(&i32, &i32) duplicate(&Pair pair) <- pair { return "
                          "(&pair->value, &pair->value); }"),
                  DiagnosticCode::InvalidBorrowContract),
              nullptr);
}

TEST(SemanticReferenceTests, SupportsReceiverAndTransitiveContracts) {
    const std::string source =
        "structure Box { i32 value; }; "
        "&i32 identity(&i32 value) <- value { return value; } "
        "&i32 forward(&i32 value) <- value { return identity(value); } "
        "&i32 Box::value_ref(&self) <- self { return "
        "forward(&self->value); } ";
    EXPECT_FALSE(Analyze(source +
                         "i32 good() { Box box = none; r := "
                         "box.value_ref(); *r = 4; return box.value; }")
                     .diagnostics.HasErrors());
    EXPECT_NE(FindDiagnostic(
                  Analyze(source + "i32 bad() { Box box = none; r := "
                                   "box.value_ref(); box.value = 4; return "
                                   "*r; }"),
                  DiagnosticCode::BorrowConflict),
              nullptr);
}

TEST(SemanticReferenceTests,
     ReferencesToPointerSlotsStillRequireUnsafePointeeAccess) {
    EXPECT_FALSE(Analyze("void f(*i32 p, *i32 q) { slot := &p; *slot = q; "
                         "unsafe { **slot = 6; } }")
                     .diagnostics.HasErrors());
    EXPECT_NE(
        FindDiagnostic(Analyze("void f(*i32 p) { slot := &p; **slot = 6; }"),
                       DiagnosticCode::UnsafeOperation),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(
            Analyze("void f(constptr i32 p) { slot := &p; *slot = null; }"),
            DiagnosticCode::SymbolNotWritable),
        nullptr);
}

TEST(SemanticReferenceTests, TracksMovesAcrossBranchesLoopsAndJumps) {
    for (const std::string& body :
         {"if flag { moved := r; } return *r;",
          "while flag { moved := r; } return *r;",
          "while flag { #again moved := r; jumpto again; } return 0;",
          "while flag { if flag { moved := r; jumpto done; } #done return *r; "
          "} return 0;",
          "while flag { moved := r; proceed; } return 0;"}) {
        SCOPED_TRACE(body);
        EXPECT_NE(
            FindDiagnostic(Analyze("i32 f(bool flag) { i32 x = 1; r := &x; " +
                                   body + " }"),
                           DiagnosticCode::MovedReference),
            nullptr);
    }
    EXPECT_FALSE(Analyze("i32 f(bool flag) { i32 x = 1; r := &x; if flag { "
                         "moved := r; r = &x; } return *r; }")
                     .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("i32 f(bool flag) { i32 x = 1; while flag { r := &x; "
                         "moved := r; *moved = 2; } return x; }")
                     .diagnostics.HasErrors());
}

TEST(SemanticReferenceTests, UnionsOriginsAtJoinsAndRetainsLoopBorrows) {
    EXPECT_NE(
        FindDiagnostic(Analyze("i32 f(bool flag) { i32 x = 1; i32 y = 2; r := "
                               "&x; if flag { r = &y; } x = 3; return *r; }"),
                       DiagnosticCode::BorrowConflict),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze("i32 f(bool flag) { i32 x = 1; r := &x; while "
                               "flag { *r = 2; x = 3; } return 0; }"),
                       DiagnosticCode::BorrowConflict),
        nullptr);
    EXPECT_FALSE(Analyze("i32 f(bool flag) { i32 x = 1; r := &x; while flag { "
                         "*r = 2; break; } x = 3; return x; }")
                     .diagnostics.HasErrors());
}

TEST(SemanticReferenceTests,
     RejectsRebindingHandleDuringPendingIndirectAssignment) {
    EXPECT_NE(FindDiagnostic(Analyze("i32 read(&const i32 r) { return *r; }"
                                     "void f() { i32 x = 1; i32 y = 2; r := "
                                     "&x; *r = read(r = &y); }"),
                             DiagnosticCode::BorrowConflict),
              nullptr);
}

TEST(SemanticReferenceTests,
     ContextualIntegersCheckRangeWithoutConvertingTypedValues) {
    EXPECT_FALSE(
        Analyze("u8 twice(u8 x) { return x * 2; }"
                "i8 low() { return -((128)); }"
                "u64 high() { return 18446744073709551615; }"
                "i64 lowest() { return -9223372036854775808; }"
                "bool f() { u8 x = 255; x = 3; return twice(x) == 6; }")
            .diagnostics.HasErrors());
    for (const std::string& declaration :
         {"u8 x = 256;", "u8 x = -1;", "i8 x = 128;", "i8 x = -129;",
          "i64 x = 9223372036854775808;", "u64 x = 18446744073709551616;"}) {
        SCOPED_TRACE(declaration);
        EXPECT_NE(FindDiagnostic(Analyze("void f() { " + declaration + " }"),
                                 DiagnosticCode::IntegerLiteralOutOfRange),
                  nullptr);
    }
    EXPECT_NE(
        FindDiagnostic(Analyze("void f() { i32 typed = 3; u8 x = typed; }"),
                       DiagnosticCode::IncompatibleType),
        nullptr);
}

TEST(SemanticReferenceTests, HandlesShortCircuitMovesAndUnreachableCode) {
    EXPECT_NE(FindDiagnostic(Analyze("bool take(&i32 a) { return true; }"
                                     "i32 f(bool flag) { i32 x = 1; i32 y = 2; "
                                     "r := &x; other := &y; "
                                     "flag and take(other = r); return *r; }"),
                             DiagnosticCode::MovedReference),
              nullptr);
    EXPECT_FALSE(Analyze("i32 f() { i32 x = 1; r := &x; moved := r; return "
                         "*moved; *r = 2; }")
                     .diagnostics.HasErrors());
}

TEST(SemanticReferenceTests, PreservesParentsAcrossMovedChildHandles) {
    EXPECT_NE(FindDiagnostic(Analyze("i32 f() { i32 x = 1; i32 y = 2; r := &x; "
                                     "child := &*r; moved := child; "
                                     "r = &y; return *moved; }"),
                             DiagnosticCode::BorrowConflict),
              nullptr);
    EXPECT_FALSE(Analyze("i32 f() { i32 x = 1; i32 y = 2; r := &x; child := "
                         "&*r; moved := child; "
                         "*moved = 3; r = &y; *r = 4; return x + y; }")
                     .diagnostics.HasErrors());
}

TEST(SemanticReferenceTests, CallLoansProtectAgainstMovesInLaterArguments) {
    EXPECT_NE(
        FindDiagnostic(Analyze("void take(&const i32 first, &i32 second) {}"
                               "void f() { i32 x = 1; i32 y = 2; r := &x; "
                               "other := &y; take(r, other = r); }"),
                       DiagnosticCode::BorrowConflict),
        nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("i32 f(bool flag) { while flag { i32 seed "
                                     "= 0; r := &seed; #again "
                                     "i32 x = 1; if flag { r = &x; jumpto "
                                     "again; } return *r; } return 0; }"),
                             DiagnosticCode::BorrowConflict),
              nullptr);
}

TEST(SemanticReferenceTests, AnonymousReborrowsAndLexicalScopeEnds) {
    EXPECT_FALSE(Analyze("void take(&i32 r) { *r = 6; }"
                         "i32 f() { i32 x = 1; { r := &x; *r = 2; } "
                         "take(&*(&x)); return x; }")
                     .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("i32 f() { i32 x = 1; &const i32 r = &x; i32 y = 2; "
                         "copy := r; r = &y; return *copy + *r; }")
                     .diagnostics.HasErrors());
}

TEST(SemanticReferenceTests,
     ReportsBorrowErrorsAfterIndependentFunctionErrors) {
    Analysis analysis = Analyze(
        "void bad() { &i32 missing; }"
        "i32 moved() { i32 x = 1; r := &x; next := r; return *r; }");
    EXPECT_NE(
        FindDiagnostic(analysis, DiagnosticCode::ReferenceRequiresInitializer),
        nullptr);
    EXPECT_NE(FindDiagnostic(analysis, DiagnosticCode::MovedReference),
              nullptr);
}

TEST(SemanticArrayTests, InfersElementsAndPreservesLengthInTypeIdentity) {
    Analysis analysis = Analyze(
        "void f() { inferred := [1, 2, 3]; [3]u8 bytes = [1, 2, 3]; "
        "repeated := [true; 4]; [0]u8 empty = []; [2][2]u8 grid = [[1, 2], [3, "
        "4]]; }");
    ASSERT_FALSE(analysis.diagnostics.HasErrors());
    const auto& function = FunctionAt(analysis, 0);
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(VariableAt(function, 0))->Type().Name(),
        "[3]i32");
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(VariableAt(function, 1))->Type().Name(),
        "[3]u8");
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(VariableAt(function, 2))->Type().Name(),
        "[4]bool");
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(VariableAt(function, 3))->Type().Length(),
        0);
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(VariableAt(function, 4))->Type().Name(),
        "[2][2]u8");
    const SemanticType byte(SemanticTypeKind::U8);
    const SemanticType first(SemanticTypeKind::Array, byte, false, 3);
    const SemanticType second(SemanticTypeKind::Array, byte, false, 4);
    EXPECT_FALSE(IsAssignable(first, second));
    EXPECT_TRUE(IsAssignable(first, first.WithConst()));
    EXPECT_FALSE(CanExplicitlyCast(first, first));
}

TEST(SemanticArrayTests,
     SupportsWholeInitializationCopyReturnsAndElementWrites) {
    EXPECT_FALSE(
        Analyze("[2]u8 make() { return [1, 2]; }"
                "[2]u8 change([2]u8 copy) { copy[0] = 9; return copy; }"
                "usize f(bool flag) { [2]u8 a; if flag { a = make(); } else { "
                "a = none; } "
                "copy := change(a); a[0] = copy[1]; return a.len(); }")
            .diagnostics.HasErrors());
    EXPECT_FALSE(
        Analyze("void f() { [1]*u8 a = [null]; [1]*u8 b = [none; 1]; b = a; }")
            .diagnostics.HasErrors());
    for (const std::string& body :
         {"[2]u8 a; a[0] = 1;", "[2]u8 a; a.len();", "[2]u8 a; r := &a[0];",
          "[2]u8 a; copy := a;", "[2]u8 a; while true { a[0] = 1; break; }"}) {
        SCOPED_TRACE(body);
        EXPECT_NE(FindDiagnostic(Analyze("void f() { " + body + " }"),
                                 DiagnosticCode::UninitializedVariable),
                  nullptr);
    }
}

TEST(SemanticArrayTests,
     RejectsMismatchedLengthsElementsAndUnsupportedStorage) {
    const std::vector<std::pair<std::string, DiagnosticCode>> cases{
        {"[2]u8 a = [1];", DiagnosticCode::ArrayInitializerCount},
        {"[2]u8 a = [1; 3];", DiagnosticCode::ArrayInitializerCount},
        {"[2]u8 a = [];", DiagnosticCode::ArrayInitializerCount},
        {"[1]u8 a = [256];", DiagnosticCode::IntegerLiteralOutOfRange},
        {"i32 x = 1; [1]u8 a = [x];", DiagnosticCode::IncompatibleType},
        {"a := [];", DiagnosticCode::CannotInferType},
        {"a := [none];", DiagnosticCode::InvalidArrayElement},
        {"a := [null];", DiagnosticCode::InvalidArrayElement},
        {"[1]string a = none;", DiagnosticCode::InvalidArrayElement},
        {"a := [\"view\"];", DiagnosticCode::InvalidArrayElement},
        {"[0]void a = none;", DiagnosticCode::InvalidArrayElement},
        {"i32 x = 1; a := [&x];", DiagnosticCode::InvalidArrayElement},
        {"[1]const u8 a = [1];", DiagnosticCode::InvalidArrayElement},
        {"[1]constptr u8 a = [null];", DiagnosticCode::InvalidArrayElement},
        {"[2][1]string a = none;", DiagnosticCode::InvalidArrayElement},
        {"[18446744073709551615]u8 a = none;",
         DiagnosticCode::InvalidArrayLength},
        {"a := [1; 18446744073709551615];", DiagnosticCode::InvalidArrayLength},
        {"[1000000000][1000000000]u64 a = none;",
         DiagnosticCode::InvalidArrayLength},
        {"[0][18446744073709551615]u8 a = none;",
         DiagnosticCode::InvalidArrayLength},
        {"[1]u8 a = [1]; a == a;", DiagnosticCode::InvalidOperand},
        {"[1]u8 a = [1]; a as [1]u8;", DiagnosticCode::InvalidCast},
        {"[1]u8 a = [1]; a.len(1);", DiagnosticCode::WrongArgumentCount},
    };
    for (const auto& [body, code] : cases) {
        SCOPED_TRACE(body);
        EXPECT_NE(FindDiagnostic(Analyze("void f() { " + body + " }"), code),
                  nullptr);
    }
}

TEST(SemanticArrayTests, RequiresUsizeIndicesAndRejectsKnownOutOfBounds) {
    EXPECT_FALSE(Analyze("u8 f(usize i) { [2]u8 a = [1, 2]; return a[i]; }")
                     .diagnostics.HasErrors());
    EXPECT_NE(FindDiagnostic(
                  Analyze("u8 f(i32 i) { [2]u8 a = [1, 2]; return a[i]; }"),
                  DiagnosticCode::IncompatibleArgument),
              nullptr);
    for (const std::string& index :
         {"2", "(3)", "1 + 1", "2 * 3", "4 / 2", "5 - 3"}) {
        const std::string source =
            "u8 f() { [2]u8 a = [1, 2]; return a[" + index + "]; }";
        SCOPED_TRACE(source);
        Analysis analysis = Analyze(source);
        const Diagnostic* diagnostic =
            FindDiagnostic(analysis, DiagnosticCode::ArrayIndexOutOfBounds);
        ASSERT_NE(diagnostic, nullptr);
        EXPECT_EQ(source.substr(diagnostic->range.start.offset,
                                diagnostic->range.Length()),
                  index);
    }
    EXPECT_NE(
        FindDiagnostic(Analyze("u8 f(usize i) { [0]u8 a = []; return a[i]; }"),
                       DiagnosticCode::ArrayIndexOutOfBounds),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze("u8 f() { [2]u8 a = none; return a[-1]; }"),
                       DiagnosticCode::IntegerLiteralOutOfRange),
        nullptr);
}

TEST(SemanticArrayTests,
     EnforcesConstThroughNestedElementsAndSharedReferences) {
    for (const std::string& body :
         {"const [2]u8 a = [1, 2]; a[0] = 3;",
          "const [2]u8 a = [1, 2]; a = [3, 4];",
          "const [2][1]u8 a = [[1], [2]]; a[0][0] = 3;",
          "const [2]u8 a = [1, 2]; r := &a[0]; *r = 3;",
          "[2]u8 a = [1, 2]; &const [2]u8 r = &a; (*r)[0] = 3;"}) {
        SCOPED_TRACE(body);
        EXPECT_NE(FindDiagnostic(Analyze("void f() { " + body + " }"),
                                 DiagnosticCode::SymbolNotWritable),
                  nullptr);
    }
    EXPECT_FALSE(
        Analyze("void f() { const [2]u8 a = [1, 2]; copy := a; copy[0] = 3; }")
            .diagnostics.HasErrors());
    EXPECT_FALSE(
        Analyze("unsafe void f(*u8 p) { const [1]*u8 a = [p]; *a[0] = 3; }")
            .diagnostics.HasErrors());
}

TEST(SemanticArrayTests,
     BorrowsWholeArraysAndElementsWithoutEscapingTemporaries) {
    EXPECT_FALSE(Analyze("void set(&u8 p) { *p = 6; } void clear(&[2]u8 a) { "
                         "(*a)[0] = 0; (*a)[1] = 0; }"
                         "u8 f() { [2]u8 a = [1, 2]; set(&a[0]); r := &a[1]; "
                         "*r = 4; clear(&a); return a[0]; }")
                     .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("u8 f() { [2]u8 a = [1, 2]; r := &a; child := "
                         "&(*r)[0]; *child = 4; (*r)[1] = 5; return a[0]; }")
                     .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("u8 f() { [2]u8 a = [1, 2]; &const u8 first = &a[0]; "
                         "&const u8 second = &a[1]; return *first + *second; }")
                     .diagnostics.HasErrors());
    for (const std::string& body :
         {"r := &a[0]; a[1] = 3; return *r;",
          "r := &a[0]; copy := a; return *r;",
          "r := &a[0]; other := &a[1]; return *r + *other;",
          "r := &a[0]; a = [3, 4]; return *r;",
          "r := &a; child := &(*r)[0]; (*r)[1] = 3; return *child;"}) {
        SCOPED_TRACE(body);
        EXPECT_NE(
            FindDiagnostic(Analyze("u8 f() { [2]u8 a = [1, 2]; " + body + " }"),
                           DiagnosticCode::BorrowConflict),
            nullptr);
    }
    EXPECT_NE(FindDiagnostic(Analyze("void f() { r := &[1, 2][0]; }"),
                             DiagnosticCode::InvalidReference),
              nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("void f() { [1, 2][0] = 3; }"),
                             DiagnosticCode::SemanticInvalidAssignmentTarget),
              nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("void f() { u8 x = 1; r := &x; { [1]u8 a "
                                     "= [2]; r = &a[0]; } }"),
                             DiagnosticCode::ReferenceEscape),
              nullptr);
}

TEST(SemanticArrayTests,
     TracksArrayBorrowsAcrossJoinsLoopsAndLiteralEvaluation) {
    EXPECT_NE(
        FindDiagnostic(Analyze("u8 f(bool flag) { [2]u8 a = [1, 2]; [2]u8 b = "
                               "[3, 4]; r := &a[0]; "
                               "if flag { r = &b[0]; } a[1] = 5; return *r; }"),
                       DiagnosticCode::BorrowConflict),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze("void f(bool flag) { [2]u8 a = [1, 2]; r := "
                               "&a[0]; while flag { *r = 3; a[1] = 4; } }"),
                       DiagnosticCode::BorrowConflict),
        nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("void f() { i32 x; a := [x; 0]; }"),
                             DiagnosticCode::UninitializedVariable),
              nullptr);
    EXPECT_NE(
        FindDiagnostic(
            Analyze("void f() { i32 x = 1; r := &x; moved := r; a := [*r]; }"),
            DiagnosticCode::MovedReference),
        nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("u8 read(&u8 p) { return *p; } u8 f() { "
                                     "[2]u8 a = [1, 2]; r := &a[0]; "
                                     "values := [read(&a[1])]; return *r; }"),
                             DiagnosticCode::BorrowConflict),
              nullptr);
}

TEST(SemanticArrayTests, PreservesRawPointerBoundaryForArrayElements) {
    EXPECT_FALSE(
        Analyze("unsafe void f() { [2]u8 a = [1, 2]; p := &raw a[0]; *p = 6; }")
            .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("unsafe void f(*[2]u8 p) { (*p)[0] = 6; }")
                     .diagnostics.HasErrors());
    EXPECT_NE(FindDiagnostic(Analyze("void f(*[2]u8 p) { (*p)[0] = 6; }"),
                             DiagnosticCode::UnsafeOperation),
              nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze("unsafe void f(*[2]u8 p) { r := &(*p)[0]; }"),
                       DiagnosticCode::InvalidReference),
        nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("unsafe void f() { [2]u8 a = none; r := "
                                     "&a; p := &raw (*r)[0]; }"),
                             DiagnosticCode::ReferenceEscape),
              nullptr);
}

TEST(SemanticArrayTests,
     FreezesAddressingReferenceWhileEvaluatingIndexAndStoreValue) {
    const std::string prefix =
        "usize index(&[2]u8 a) { return 0; } u8 read(&[2]u8 a) { return "
        "(*a)[0]; } "
        "void f() { [2]u8 a = [1, 2]; [2]u8 b = [3, 4]; ref := &a; ";
    for (const std::string& access :
         {"(*ref)[index(ref = &b)] = 7;", "(*ref)[0] = read(ref = &b);",
          "u8 value = (*ref)[index(ref = &b)];"}) {
        SCOPED_TRACE(access);
        EXPECT_NE(FindDiagnostic(Analyze(prefix + access + " }"),
                                 DiagnosticCode::BorrowConflict),
                  nullptr);
    }
}

TEST(SemanticArrayTests, PreservesDeepConstAndArrayShapeDuringReborrowing) {
    EXPECT_NE(FindDiagnostic(
                  Analyze("unsafe void f(*const [2]u8 p) { (*p)[0] = 3; }"),
                  DiagnosticCode::SymbolNotWritable),
              nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze("void f(&[2]u8 p) { [3]u8 a = none; p = &a; }"),
                       DiagnosticCode::IncompatibleType),
        nullptr);
    EXPECT_FALSE(Analyze("u8 read(&const [2]u8 p) { return (*p)[0]; }"
                         "u8 f() { [2]u8 a = [1, 2]; constref [2]u8 r = &a; "
                         "(*r)[0] = 3; return read(r); }")
                     .diagnostics.HasErrors());
}

TEST(SemanticAnalyzerTests, AcceptsRangeBindingsEmptyArraysAndCountedLoops) {
    Analysis result = Analyze(R"(
        void f() {
            a := [1, 2];
            for i := range a { usize copy = i; }
            for (i, v) := range a { usize index = i; i32 value = v; }
            for (u32 i, i32 v) = range a { i = 4; v = 5; }
            for (_, v) := range a { v++; }
            for (_, _) := range a {}
            [0]u8 empty = [];
            for (u8 i, u8 v) = range empty { v++; }
            for (i, row) := range [[1, 2], [3, 4]] { row[0]++; }
            loop u32 x = 1; x <= 10; x++ { if x == 3 { proceed; } if x == 8 { break; } }
            loop i := 0; i < 2; i = i + 1 {}
            loop ;; { break; }
        }
    )");
    EXPECT_FALSE(result.diagnostics.HasErrors());
}

TEST(SemanticAnalyzerTests, ChecksRangeSourceBindingTypesAndIndexCapacity) {
    for (const std::string& source :
         {"void f() { for i := range 4 {} }",
          "void f() { for i := range \"abc\" {} }",
          "void f() { a := [1]; for (u32 i, u8 v) = range a {} }",
          "void f() { a := [1]; for (f64 i, i32 v) = range a {} }",
          "void f() { a := [1]; for (f64 _, i32 v) = range a {} }",
          "void f() { a := [1]; for (u32 i, u8 _) = range a {} }",
          "void f() { a := [0; 257]; for (u8 i, i32 v) = range a {} }",
          "void f() { a := [0; 129]; for (i8 i, i32 v) = range a {} }",
          "void f() { a := [1]; for (i, i) := range a {} }",
          "void f() { a := [1]; for (const u32 i, i32 v) = range a { i++; } }",
          "void f() { a := [1]; for (u32 i, const i32 v) = range a { v++; } "
          "}"}) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
    EXPECT_FALSE(
        Analyze("void f() { a := [0; 256]; for (u8 i, i32 v) = range a {} }")
            .diagnostics.HasErrors());
}

TEST(SemanticAnalyzerTests, ChecksLoopScopeInitializationAndUpdateEdges) {
    for (const std::string& source :
         {"void f() { loop i := 0; i < 2; i++ {} i++; }",
          "void f() { a := [1]; for (i, v) := range a {} v++; }",
          "void f() { i32 x; loop i := 0; i < 2; i++ { x = 2; } x++; }",
          "void f() { i32 x; loop i := 0; i < 2; x++ { proceed; } }",
          "void f() { [2]u8 a; for i := range a {} }",
          "void f() { loop i := 0; 1; i++ {} }",
          "void f() { loop i := 0; i < 2; local++ { local := 1; } }"}) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
    EXPECT_FALSE(
        Analyze("void f() { i32 x; loop i := 0; i < 2; x++ { x = 1; i++; } }")
            .diagnostics.HasErrors());
}

TEST(SemanticAnalyzerTests, IncrementHonorsPlacesConstBorrowingAndUnsafe) {
    for (const std::string& source :
         {"void f() { i32 x; x++; }", "void f() { const i32 x = 1; x++; }",
          "void f() { (1 + 2)++; }", "void f() { [1, 2][0]++; }",
          "void f() { f64 x = 1.0; x++; }", "void f() { bool x = false; x++; }",
          "void f() { *u8 p = none; p++; }", "void f(*u8 p) { (*p)++; }",
          "void f() { x := 1; r := &x; x++; *r = 2; }",
          "void f() { x := 1; &const i32 r = &x; (*r)++; }",
          "void f() { const [2]i32 a = [1, 2]; a[0]++; }"}) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
    EXPECT_FALSE(Analyze("void f() { x := 1; r := &x; (*r)++; unsafe { p := "
                         "&raw x; (*p)++; } }")
                     .diagnostics.HasErrors());
}

TEST(SemanticAnalyzerTests, RangeCopiesRespectLoansAndIterationLifetimes) {
    EXPECT_TRUE(
        Analyze(
            "void f() { a := [1, 2]; r := &a[0]; for i := range a {} *r = 3; }")
            .diagnostics.HasErrors());
    EXPECT_TRUE(Analyze("void f() { x := 1; r := &x; for (_, v) := range [1, "
                        "2] { r = &v; } *r = 3; }")
                    .diagnostics.HasErrors());
    EXPECT_FALSE(
        Analyze(
            "void f() { a := [1, 2]; for (i, v) := range a { a[i] = v + 1; } }")
            .diagnostics.HasErrors());
    EXPECT_TRUE(
        Analyze(
            "void f() { x := 1; r := &x; loop i := 0; i < 2; x++ { *r = i; } }")
            .diagnostics.HasErrors());
}

TEST(SemanticSliceTests, AcceptsCheckedSlicesCallsConstAndElementReferences) {
    const Analysis result = Analyze(R"(
        void bump([]i32 s) { s.front()++; s.back() = 4; }
        i32 read([]const i32 s) { return s.front(); }
        void f() {
            a := [1, 2, 3];
            s := a[:];
            bump(s); read(s);
            child := s[1:]; child[0] = 10;
            r := &s.back(); *r = 5;
            moved := s; moved[0] = 6;
            []const i32 shared = moved[:];
            copy := shared; read(copy); read(shared);
            const []i32 fixed = a[:]; fixed.front() = 1; bump(fixed);
            const [2]i32 frozen = [1, 2];
            inferred := frozen[:]; read(inferred);
            [0]i32 empty = []; empty_slice := empty[:]; empty_slice.len();
            matrix := [[1, 2], [3, 4]]; rows := matrix[:]; rows[0][1] = 7;
        }
    )");
    EXPECT_FALSE(result.diagnostics.HasErrors());
}

TEST(SemanticSliceTests, RejectsBadBoundsSourcesAndUnsupportedStorage) {
    for (const std::string& source :
         {"void f() { a := [1, 2]; s := a[2:1]; }",
          "void f() { a := [1, 2]; s := a[:3]; }",
          "void f() { a := [1, 2]; s := a[3:]; }",
          "void f() { a := [1, 2]; s := a[-1:]; }",
          "void f() { a := [1, 2]; u32 i = 0; s := a[i:]; }",
          "void f() { s := [1, 2][:]; }",
          "void f() { a := 2; s := a[:]; }",
          "void f() { s := \"abc\"[:]; }",
          "void f() { []u8 s; }",
          "void f() { []u8 s = none; }",
          "void f() { []u8 s = null; }",
          "void f([]string s) {}",
          "void f([]&u8 s) {}",
          "void f([][]u8 s) {}",
          "void f([2][]u8 a) {}",
          "void f(&[]u8 s) {}",
          "[]u8 f() { [1]u8 a = [1]; return a[:]; }",
          "unsafe void f(*[2]u8 p) { s := (*p)[:]; }",
          "void f() { a := [1]; s := a[:]; unsafe { p := &raw s; } }",
          "void f() { a := [1]; s := a[:]; unsafe { p := &raw s.front(); } }",
          "void f() { [2]u8 a; s := a[:]; }",
          "void f() { a := [1]; s := a[:]; s.len() = 1; }",
          "void f() { a := [1]; s := a[:]; s.front(0); }",
          "void f() { a := [1]; s := a[:]; s == s; }",
          "void f() { a := [1]; s := a[:] as []i32; }"}) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
}

TEST(SemanticMemoryTests, RuntimeArrayOwnersPreserveLengthAndMoveOwnership) {
    auto valid = Analyze(
        "unsafe (own *[]i32, error) make(usize count) { return "
        "new<[]i32>(count); } "
        "unsafe usize f() { p, e := make(3); if e != none { return 0; } "
        "q := p; defer free(q); q[0] = 42; length := q.len(); values := q[:]; "
        "return length + values.len(); }");
    for (const auto& diagnostic : valid.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;

    EXPECT_TRUE(Analyze("unsafe void f() { p, e := new<[]i32>(2); q := p; "
                        "free(p); free(q); }")
                    .diagnostics.HasErrors());
}

TEST(SemanticSliceTests, EnforcesReadOnlyElementsAndFixedHandles) {
    for (const std::string& source :
         {"void f() { const [1]i32 a = [1]; []i32 s = a[:]; }",
          "void f() { a := [1]; []const i32 s = a[:]; s[0] = 2; }",
          "void f() { a := [1]; []const i32 s = a[:]; s.front()++; }",
          "void f() { a := [1]; []const i32 s = a[:]; []i32 mutable = s[:]; }",
          "void f() { a := [1]; const []i32 s = a[:]; s = a[:]; }",
          "void f() { a := [1]; const []i32 s = a[:]; moved := s; }",
          "void f() { a := [[1]]; []const [1]i32 s = a[:]; s[0][0]++; }",
          "void bump([]i32 s) {} void f() { a := [1]; []const i32 s = a[:]; "
          "bump(s); }"}) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
}

TEST(SemanticSliceTests, TracksMovesLoansAndEscapesAcrossControlFlow) {
    for (const std::string& source :
         {"void f() { a := [1, 2]; s := a[:]; a[0] = 3; s[0] = 2; }",
          "void f() { a := [1, 2]; s := a[:]; a.len(); s.len(); }",
          "void f() { a := [1, 2]; s := a[:]; moved := s; s.len(); }",
          "void f(bool b) { a := [1, 2]; s := a[:]; if b { moved := s; } "
          "s.len(); }",
          "void f() { a := [1, 2]; s := a[:]; child := s[:1]; s[1] = 3; "
          "child[0] = 2; }",
          "void f() { a := [1, 2]; s := a[:]; child := s[:1]; moved := s; "
          "child.len(); }",
          "void f() { a := [1, 2]; s := a[:]; { inner := [3, 4]; s = inner[:]; "
          "} s.len(); }",
          "void f() { a := [1, 2]; s := a[:]; { parent := s[:]; s = parent[:]; "
          "} }",
          "void f() { a := [1, 2]; s := a[:]; r := &s[0]; s[1] = 2; *r = 3; }",
          "void f() { a := [1, 2]; s := a[:]; loop i := 0; i < 2; i++ { moved "
          ":= s; moved.len(); } }",
          "void use([]i32 x, []i32 y) {} void f() { a := [1, 2]; s := a[:]; "
          "use(s, s); }",
          "void use([]const i32 x, []i32 y) {} void f() { a := [1, 2]; s := "
          "a[:]; use(s, s); }"}) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
    EXPECT_FALSE(Analyze("void use([]const i32 x, []const i32 y) {} void f() { "
                         "a := [1, 2]; s := a[:]; use(s, s); s[0]++; }")
                     .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("void f() { a := [1, 2]; s := a[:]; child := s[:1]; "
                         "child[0]++; s[1]++; a[0]++; }")
                     .diagnostics.HasErrors());
}

TEST(SemanticSliceTests, PinsParentsDuringBoundsAndPendingStores) {
    for (const std::string& source :
         {"void f() { a := [1, 2]; b := [3, 4]; s := a[:]; t := b[:]; child := "
          "s[(s = t).len():]; }",
          "void f() { a := [1, 2]; b := [3, 4]; s := a[:]; t := b[:]; s[(s = "
          "t).len()] = 1; }",
          "void f() { a := [1, 2]; b := [3, 4]; s := a[:]; t := b[:]; s[0] = "
          "(s = t).front(); }"}) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
}

TEST(SemanticSliceTests,
     RangesReborrowReadOnlyAndPreserveHandleAfterIteration) {
    EXPECT_FALSE(Analyze("void f() { a := [1, 2]; s := a[:]; for (i, v) := "
                         "range s { usize n = i; i32 x = v; } s[0]++; }")
                     .diagnostics.HasErrors());
    EXPECT_TRUE(Analyze("void f() { a := [1, 2]; s := a[:]; for (i, v) := "
                        "range s { s[i]++; } }")
                    .diagnostics.HasErrors());
    EXPECT_TRUE(Analyze("void f() { a := [1, 2]; s := a[:]; for (u8 i, i32 v) "
                        "= range s {} }")
                    .diagnostics.HasErrors());
}

TEST(SemanticDeferTests, CapturesAtRegistrationAndChecksCallsNormally) {
    Analysis result = Analyze(R"(
        void use(i32 x) {}
        i32 result() { return 1; }
        unsafe void raw(*i32 p) {}
        void f() {
            i32 x;
            defer use(x = 1);
            x++;
            defer result();
            defer (use(x));
            unsafe { defer raw(&raw x); }
            string word = "first";
            defer stdoutln(word.view());
            word = "second";
        }
    )");
    EXPECT_FALSE(result.diagnostics.HasErrors());
    for (const std::string& source :
         {"void f() { defer 42; }", "void f() { defer f; }",
          "void f() { a := [1]; defer a.len(); }",
          "void f() { defer missing(); }",
          "void g(i32 x) {} void f() { defer g(); }",
          "void g(i32 x) {} void f() { defer g(true); }",
          "void g(i32 x) {} void f() { i32 x; defer g(x); x = 1; }",
          "unsafe void g() {} void f() { defer g(); }"}) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
    const Analysis invalid = Analyze("void f() { defer 42; }");
    const auto* diagnostic =
        FindDiagnostic(invalid, DiagnosticCode::InvalidDefer);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->range.start.offset, 17);
    EXPECT_EQ(diagnostic->range.end.offset, 19);
}

TEST(SemanticDeferTests, HoldsBorrowedArgumentsUntilTheirScopeEnds) {
    for (const std::string& source :
         {"void g(&i32 r) {} void f() { x := 1; defer g(&x); x++; }",
          "void g(&i32 r) {} i32 f() { x := 1; defer g(&x); return x; }",
          "void g(&const i32 r) {} void f() { x := 1; defer g(&x); x++; }",
          "void g(&i32 r) {} void f() { x := 1; r := &x; defer g(r); moved := "
          "r; }",
          "void g(&i32 r) {} void f() { x := 1; y := 2; r := &x; defer g(r); r "
          "= &y; }",
          "void g([]i32 s) {} void f() { a := [1, 2]; defer g(a[:]); a[0]++; }",
          "void g([]const i32 s) {} void f() { a := [1]; s := a[:]; defer "
          "g(s); s[0]++; }",
          "void g(&i32 r) {} void f() { x := 1; defer g(&x); defer g(&x); }",
          "void g(&i32 r, i32 x) {} void f() { x := 1; defer g(&x, x++); }",
          "void g(&i32 r) {} void f(bool skip) { x := 1; while true { #again "
          "defer g(&x); if skip { jumpto again; } break; } }",
          "void g(&const i32 r) {} void f() { while true { #again x := 1; "
          "defer g(&x); jumpto again; } }"}) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
    EXPECT_FALSE(Analyze("void g(&i32 r) {} void f() { x := 1; r := &x; { "
                         "defer g(r); } *r = 2; x++; }")
                     .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("void g(&const i32 r) {} i32 f() { x := 1; defer "
                         "g(&x); defer g(&x); return x; }")
                     .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("void g([]i32 s) {} void f() { a := [1]; s := a[:]; { "
                         "defer g(s); } s[0]++; }")
                     .diagnostics.HasErrors());
}

TEST(SemanticDeferTests, TracksRegistrationAndLoanReleaseThroughJumps) {
    EXPECT_FALSE(Analyze(R"(
        void g(&i32 r) {}
        void f() {
            x := 1;
            while true {
                { defer g(&x); jumpto done; }
                #done
                x++;
                break;
            }
        }
    )")
                     .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("void g(&i32 r) {} void f() { x := 1; loop i := 0; i "
                         "< 2; i++ { defer g(&x); proceed; } x++; }")
                     .diagnostics.HasErrors());
    EXPECT_TRUE(
        Analyze("void g(&i32 r) {} void f(bool b) { x := 1; while true { if b "
                "{ jumpto done; } defer g(&x); #done x++; break; } }")
            .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("void g(&i32 r) {} void f() { x := 1; while true { "
                         "jumpto done; defer g(&x); #done x++; break; } }")
                     .diagnostics.HasErrors());
}

TEST(SemanticTupleTests, SupportsValuesReturnsParametersAndContextualElements) {
    const auto result = Analyze(R"(
        (u8, (bool, string)) f(u8 n) { return (n, (true, "text")); }
        (u8, (bool, string)) pass((u8, (bool, string)) p) { return p; }
        i32 main() {
            (u8, (bool, string)) t;
            t = pass(f(42));
            (n, nested) := t;
            (bool ok, string text) = nested;
            (n, (bool, string) copy) = (3, nested);
            (const u8 fixed, _) = t;
            const (u8, bool) frozen = (1, true);
            (u8, bool) zero = none;
            (u8 small, *i32 pointer) = (none, null);
            return n as i32;
        }
    )");
    EXPECT_FALSE(result.diagnostics.HasErrors());
    const SemanticType type =
        result.model.TypeOf(*static_cast<const ReturnStatement&>(
                                 *FunctionAt(result, 0).Body().Statements()[0])
                                 .Value());
    EXPECT_EQ(type.Name(), "(u8, (bool, string))");
    EXPECT_TRUE(type.OwnsStorage());
    EXPECT_TRUE(type.IsCompound());
}

TEST(SemanticTupleTests, ChecksArityTypesMutabilityAndUnsupportedElements) {
    for (const std::string& source :
         {"void f() { (a, b) := 1; }",
          "void f() { (a, b) := (1, 2, 3); }",
          "void f() { (a, a) := (1, 2); }",
          "void f() { a := 0; (a, a) = (1, 2); }",
          "void f() { (a, b) = (1, 2); }",
          "void f() { (i32 a, b) := (1, 2); }",
          "void f() { (bool a, i32 b) = (1, 2); }",
          "void f() { (u8 a, i32 b) = (256, 2); }",
          "void f() { const i32 a = 1; (a, _) = (2, 3); }",
          "void f() { const (i32, bool) a = (1, true); a = (2, false); }",
          "(i32, bool) f() { return (1, 2); }",
          "(i32, bool) f() { return 1; }",
          "void f() { t := (1, 2); t == t; }",
          "void f() { t := (1, 2); t as (i32, i32); }",
          "void f() { (void, i32) t; }",
          "void f() { x := 1; t := (&x, 2); }",
          "void f() { a := [1]; t := (a[:], 2); }",
          "void f() { t := (\"view\", 2); }",
          "void f() { (*&i32, i32) t; }",
          "void f() { [2](i32, bool) t; }",
          "void f() { (string, i32) t = (\"s\", 1); r := &t; }"}) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
    const std::string source = "void f() { (a, a) := (1, 2); }";
    const auto invalid = Analyze(source);
    const auto* diagnostic =
        FindDiagnostic(invalid, DiagnosticCode::InvalidDestructure);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->range.start.offset, source.find("a)"));
}

TEST(SemanticTupleTests, PreservesInitializationAndSimultaneousBindingRules) {
    for (const std::string& source :
         {"void f() { i32 a; i32 b; (a, b) = (1, 2); stdoutln(a + b); }",
          "void f() { i32 a; (i32 b, i32 c) = (a = 1, a); stdoutln(a); }",
          "void f() { a := 1; b := 2; (a, b) = (b, a); }",
          "void f() { a := 1; { (a, b) := (a, 2); stdoutln(a); } }",
          "void f() { (_, _) = (1, 2); }",
          "(i32, i32) pair() { return (1, 2); } void f() { first, err := "
          "pair(); second, err := pair(); stdoutln(first + second + err); }",
          "(i32, i32) pair() { return (1, 2); } void f(i32 err) { value, err "
          ":= pair(); stdoutln(value + err); }",
          "(i32, i32) pair() { return (1, 2); } void f() { i32 err; value, "
          "err := pair(); stdoutln(value + err); }",
          "void f() { i32 err = 7; { value, err := (1, true); if err { "
          "stdoutln(value); } } stdoutln(err); }"}) {
        SCOPED_TRACE(source);
        EXPECT_FALSE(Analyze(source).diagnostics.HasErrors());
    }
    for (const std::string& source :
         {"void f() { i32 a; i32 b; (a, b) = (1, a); }",
          "void f() { (a, b) := (1, a); }",
          "void f(bool c) { i32 a; i32 b; if c { (a, b) = (1, 2); } "
          "stdoutln(a); }",
          "void f() { i32 a; while true { jumpto use; (a, i32 b) = (1, 2); "
          "#use stdoutln(a); break; } }",
          "void f() { (i32, bool) t; (a, b) := t; }",
          "void f() { a, b := (1, 2); a, b := (3, 4); }",
          "void f() { err := 0; _, err := (1, 2); }",
          "void f() { const i32 err = 0; value, err := (1, 2); }",
          "void f() { err := true; value, err := (1, 2); }"}) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }

    const Analysis noNew =
        Analyze("void f() { a, b := (1, 2); a, b := (3, 4); }");
    const Diagnostic* diagnostic =
        FindDiagnostic(noNew, DiagnosticCode::InvalidDestructure);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->message,
              "short destructuring declaration requires at least one new "
              "variable");
}

TEST(SemanticTupleTests, DestructuringCannotWriteThroughActiveLoans) {
    for (const std::string& source :
         {"void use(&i32 r) {} void f() { x := 1; defer use(&x); (x, _) = (2, "
          "3); }",
          "void f() { x := 1; r := &x; (x, _) = (2, 3); stdoutln(*r); }",
          "void f() { a := [1]; r := &a[0]; (a, _) = ([2], 3); stdoutln(*r); }",
          "void f() { t := (1, 2); r := &t; (t, _) = ((2, 3), 4); p := *r; "
          "}"}) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
    EXPECT_FALSE(
        Analyze("void f() { t := (1, 2); r := &t; (a, b) := *r; t = (b, a); }")
            .diagnostics.HasErrors());
}

TEST(SemanticTupleTests, BareBindingGroupsInheritTypesAndQualifiers) {
    EXPECT_FALSE(Analyze(R"(
        (i32, i32) divide() { return (3, 2); }
        void f() {
            i32 q, r = divide();
            i32 first, i32 second = divide();
            a, b := divide();
            a, b = (b, a);
            bool ok, changed, i32 code = (true, false, 42);
            const u8 low, high = (0, 255);
            (i32, bool) left, right = ((42, true), (7, false));
            q, i32 fresh, another = (1, 2, 3);
            _, b = divide();
            (i32 explicit_new, q) = divide();
        }
    )")
                     .diagnostics.HasErrors());
    for (const std::string& source :
         {"void f() { i32 q, r = (1, true); }",
          "void f() { u8 q, r = (1, 256); }",
          "void f() { const i32 q, r = (1, 2); r = 3; }",
          "void f() { i32 q, r := (1, 2); }",
          "void f() { i32 q, r = (1, 2, 3); }", "void f() { q, r = (1, 2); }",
          "void f() { i32 q, q = (1, 2); }",
          "void f() { r := 0; i32 q, r = (1, 2); }",
          "void f() { i32 q, r = (1, q); }",
          "void f() { string q, r = (\"one\", 2); }"}) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
}

TEST(SemanticErrorTests, ResolvesForwardFamiliesAndPreservesNominalIdentity) {
    Analysis analysis = Analyze(
        "Fault f(Fault x) { return x; } "
        "error Fault { Bad, Worse } error Other { Bad } "
        "void use() { first := Fault.Bad; second := Other.Bad; "
        "error any = first; error success = none; Fault ok = none; "
        "Fault a, b = (first, ok); }");
    ASSERT_FALSE(analysis.diagnostics.HasErrors());
    const auto& function = FunctionAt(analysis, 3);
    const auto first =
        analysis.model.DeclaredSymbol(VariableAt(function, 0))->Type();
    const auto second =
        analysis.model.DeclaredSymbol(VariableAt(function, 1))->Type();
    const auto generic =
        analysis.model.DeclaredSymbol(VariableAt(function, 2))->Type();
    EXPECT_EQ(first.Name(), "Fault");
    EXPECT_EQ(generic.Name(), "error");
    EXPECT_TRUE(first.IsErrorValue());
    EXPECT_FALSE(first.IsError());
    EXPECT_NE(first, second);
    EXPECT_TRUE(IsAssignable(generic, first));
    EXPECT_FALSE(IsAssignable(first, generic));
    EXPECT_FALSE(IsAssignable(first, second));
    EXPECT_EQ(
        analysis.model.DeclaredSymbol(*FunctionAt(analysis, 0).Parameters()[0])
            ->Type(),
        first);
    EXPECT_FALSE(first.OwnsStorage());
    EXPECT_FALSE(generic.OwnsStorage());
}

TEST(SemanticErrorTests, ChecksErrorOperatorsAndConversions) {
    const std::string prefix = "error Fault { Bad } error Other { Bad } ";
    for (const std::string& expression :
         {"Fault.Bad == none", "none != Fault.Bad", "Fault.Bad == Fault.Bad",
          "a == Fault.Bad", "Fault.Bad != a", "a == b", "a === b",
          "Fault.Bad === Other.Bad", "Fault.Bad !== none", "a === Fault.Bad"}) {
        SCOPED_TRACE(expression);
        EXPECT_FALSE(Analyze(prefix + "bool f(error a, error b) { return " +
                             expression + "; }")
                         .diagnostics.HasErrors());
    }
    for (const std::string& expression :
         {"Fault.Bad == Other.Bad", "Fault.Bad != Other.Bad", "a + b", "a < b",
          "a == 0", "a == false", "a == null", "!a", "a and b"}) {
        SCOPED_TRACE(expression);
        EXPECT_NE(
            FindDiagnostic(Analyze(prefix + "void f(error a, error b) { " +
                                   expression + "; }"),
                           DiagnosticCode::InvalidOperand),
            nullptr);
    }
    for (const std::string& body :
         {"Fault value = a;", "Fault value = Other.Bad;", "i32 value = a;",
          "error value = 1;", "error value = null;"}) {
        SCOPED_TRACE(body);
        EXPECT_NE(
            FindDiagnostic(Analyze(prefix + "void f(error a) { " + body + " }"),
                           DiagnosticCode::IncompatibleType),
            nullptr);
    }
    for (const std::string& expression :
         {"a as Fault", "Fault.Bad as i32", "1 as error", "a as string"}) {
        SCOPED_TRACE(expression);
        EXPECT_NE(FindDiagnostic(Analyze(prefix + "void f(error a) { " +
                                         expression + "; }"),
                                 DiagnosticCode::InvalidCast),
                  nullptr);
    }
    EXPECT_NE(FindDiagnostic(Analyze(prefix + "void f(error a) { if a {} }"),
                             DiagnosticCode::InvalidCondition),
              nullptr);
}

TEST(SemanticErrorTests, ReportsInvalidFamiliesNamesAndVariantsAtTheirRanges) {
    for (const std::string& source :
         {"error Fault { Bad, Bad }",
          "error Fault { Bad } error Fault { Worse }",
          "error Fault { Bad } void Fault() {}", "error stdout { Bad }"}) {
        SCOPED_TRACE(source);
        EXPECT_NE(FindDiagnostic(Analyze(source),
                                 DiagnosticCode::DuplicateDeclaration),
                  nullptr);
    }
    EXPECT_NE(FindDiagnostic(Analyze("error Empty {}"),
                             DiagnosticCode::InvalidErrorDeclaration),
              nullptr);
    const std::string source =
        "error Fault { Bad } void f() { Fault.Missing; }";
    Analysis missing = Analyze(source);
    const Diagnostic* diagnostic =
        FindDiagnostic(missing, DiagnosticCode::InvalidMember);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->range.start.offset, source.find("Missing"));
    EXPECT_EQ(diagnostic->range.Length(), 7);
    for (const std::string& body :
         {"Unknown value = none;", "*Unknown value = none;",
          "(error, Unknown) value = none;"}) {
        SCOPED_TRACE(body);
        EXPECT_NE(FindDiagnostic(Analyze("void f() { " + body + " }"),
                                 DiagnosticCode::UnknownType),
                  nullptr);
    }
    for (const std::string& body : {"Unknown.Bad;", "i32 Fault = 1; Fault.Bad;",
                                    "Fault value = none; value.Bad;"}) {
        SCOPED_TRACE(body);
        EXPECT_NE(FindDiagnostic(
                      Analyze("error Fault { Bad } void f() { " + body + " }"),
                      DiagnosticCode::InvalidMember),
                  nullptr);
    }
    EXPECT_NE(FindDiagnostic(
                  Analyze("error Fault { Bad } void f() { value := Fault; }"),
                  DiagnosticCode::InvalidOperand),
              nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze("error Fault { Bad } void f() { Fault.Bad(); }"),
                       DiagnosticCode::InvalidOperand),
        nullptr);
    EXPECT_FALSE(Analyze("error Fault { Bad } Fault f(Fault Fault, Fault next) "
                         "{ return next; }")
                     .diagnostics.HasErrors());
}

TEST(SemanticEnumTests, ResolvesNominalValuesDefaultsAndMemberFunctions) {
    Analysis result = Analyze(
        "enum Status: u8 { default Idle = 3, Running = 7, Done, } "
        "bool Status::is_running(&const self) { "
        "return *self == Status.Running; } "
        "Status Status::done() { return Status.Done; } "
        "void use() { Status status = none; status = Status.Running; "
        "[2]Status values = none; (Status, Status) pair = none; "
        "if status.is_running() and Status::done() == Status.Done and "
        "values[0] == Status.Idle {} } ");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_EQ(result.model.Enums().size(), 1);
    const auto& enumeration = *result.model.Enums().front();
    EXPECT_EQ(enumeration.name, "Status");
    EXPECT_EQ(enumeration.backing.Kind(), SemanticTypeKind::U8);
    ASSERT_EQ(enumeration.variants.size(), 3);
    EXPECT_EQ(enumeration.variants[0].value, 3);
    EXPECT_EQ(enumeration.variants[1].value, 7);
    EXPECT_EQ(enumeration.variants[2].value, 8);
    ASSERT_TRUE(enumeration.defaultValue.has_value());
    EXPECT_EQ(*enumeration.defaultValue, 3);
    EXPECT_TRUE(enumeration.methods.contains("is_running"));
    EXPECT_TRUE(enumeration.methods.at("is_running")->HasReceiver());
    EXPECT_TRUE(enumeration.methods.contains("done"));
    EXPECT_FALSE(enumeration.methods.at("done")->HasReceiver());
}

TEST(SemanticEnumTests, RequiresDefaultsForNoneAndKeepsEnumsNominal) {
    Analysis missingDefault = Analyze(
        "enum First { A, } enum Second { A, } "
        "void f() { First first = none; First other = Second.A; }");
    EXPECT_NE(FindDiagnostic(missingDefault, DiagnosticCode::IncompatibleType),
              nullptr);

    Analysis defaulted = Analyze(
        "enum First { default A, } enum Second { default A, } "
        "void f() { First first = none; First other = Second.A; }");
    EXPECT_NE(FindDiagnostic(defaulted, DiagnosticCode::IncompatibleType),
              nullptr);
}

TEST(SemanticEnumTests, RejectsInvalidDeclarationsAndReservedLifecycleNames) {
    for (const std::string& source : {
             "enum Empty {}",
             "enum Bad: bool { Value, }",
             "enum Bad: u8 { Value = 256, }",
             "enum Bad: i8 { Value = -129, }",
             "enum Bad { First = 1, Second = 1, }",
             "enum Bad { default First, default Second, }",
         }) {
        SCOPED_TRACE(source);
        EXPECT_NE(FindDiagnostic(Analyze(source),
                                 DiagnosticCode::InvalidEnumDeclaration),
                  nullptr);
    }
    EXPECT_NE(FindDiagnostic(Analyze("enum Bad { First, First, }"),
                             DiagnosticCode::DuplicateDeclaration),
              nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("enum Bad { Value, } "
                                     "Bad Bad::zero() { return Bad.Value; }"),
                             DiagnosticCode::InvalidLifecycle),
              nullptr);
}

TEST(SemanticEnumTests, EnforcesPrivateMemberAccess) {
    Analysis result = Analyze(
        "enum State { On, Off, } "
        "private bool State::is_on(&const self) { return *self == State.On; } "
        "bool State::visible(&const self) { return self->is_on(); } "
        "void f() { State state = State.On; state.is_on(); }");

    EXPECT_NE(FindDiagnostic(result, DiagnosticCode::PrivateAccess), nullptr);
}

TEST(SemanticEnumTests, SupportsGenericMemberFunctions) {
    Analysis result = Analyze(
        "enum State { Off, default Unknown, On, } "
        "T State::choose<T>(&const self, T on, T off) { "
        "if *self == State.On { return on; } return off; } "
        "i32 f() { State state = State.On; "
        "return state.choose<i32>(42, 0); }");

    EXPECT_FALSE(result.diagnostics.HasErrors());
}

TEST(SemanticErrorTests, IntegratesDefiniteAssignmentQualifiersAndBorrowing) {
    const std::string prefix = "error Fault { Bad } ";
    EXPECT_NE(
        FindDiagnostic(Analyze(prefix + "error f() { error e; return e; }"),
                       DiagnosticCode::UninitializedVariable),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze(prefix + "Fault f(bool flag) { Fault e; if flag "
                                        "{ e = Fault.Bad; } return e; }"),
                       DiagnosticCode::UninitializedVariable),
        nullptr);
    EXPECT_FALSE(Analyze(prefix + "error f(bool flag) { error e; if flag { e = "
                                  "Fault.Bad; } else { e = none; } return e; }")
                     .diagnostics.HasErrors());
    EXPECT_NE(FindDiagnostic(
                  Analyze(prefix +
                          "void f() { const error e = none; e = Fault.Bad; }"),
                  DiagnosticCode::SymbolNotWritable),
              nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze(prefix + "void f() { error e = none; &const "
                                        "error r = &e; *r = Fault.Bad; }"),
                       DiagnosticCode::SymbolNotWritable),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze(prefix + "void f() { error e = none; r := &e; e "
                                        "= Fault.Bad; copy := *r; }"),
                       DiagnosticCode::BorrowConflict),
        nullptr);
    EXPECT_FALSE(Analyze(prefix + "void f() { [2]error a = [Fault.Bad, none]; "
                                  "{ []error s = a[:]; s[0] = none; } "
                                  "(Fault, error) pair = (Fault.Bad, a[0]); "
                                  "Fault x, error y = pair; }")
                     .diagnostics.HasErrors());
}

TEST(SemanticStructureTests,
     ResolvesForwardTypesDefaultInitializationAndMethods) {
    Analysis result = Analyze(
        "Box factory() { return Box::init(); } "
        "structure Box { i32 x; private { i32 y; } }; "
        "i32 Box::value(&const self) { return self->x + self->y; } "
        "void Box::set(&self, i32 value) { self->y = value; } "
        "i32 main() { Box box = factory(); box.set(42); "
        "{ r := &box; r->set(41); } unsafe { p := &raw box; p->set(42); } "
        "return box.value(); }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_EQ(result.model.Structures().size(), 1);
    EXPECT_NE(result.model.Structures()[0]->init, nullptr);
    EXPECT_NE(result.model.Structures()[0]->zero, nullptr);
    EXPECT_EQ(result.model.Structures()[0]->fields.size(), 2);
    EXPECT_EQ(result.model.GeneratedFunctions().size(), 2);
}

TEST(SemanticStructureTests, ValidatesZeroLifecycleAndContextualNone) {
    const std::string valid =
        "structure Position { i32 line; i32 column; }; "
        "Position Position::zero() override { return build Position { "
        "self.line = 1; self.column = 1; }; } "
        "Position make() { return none; } void take(Position value) {} "
        "void f() { Position value = none; value = none; take(none); "
        "if value == none or none != value or value === none or "
        "none !== value {} explicit := Position::zero(); }";
    Analysis result = Analyze(valid);
    EXPECT_FALSE(result.diagnostics.HasErrors());
    std::size_t warnings = 0;
    for (const Diagnostic& diagnostic : result.diagnostics.Diagnostics()) {
        if (diagnostic.severity == DiagnosticSeverity::Warning) {
            ++warnings;
            EXPECT_NE(diagnostic.message.find("Position::zero()"),
                      std::string::npos);
        }
    }
    EXPECT_EQ(warnings, 1);

    for (const std::string& method : {
             "Position Position::zero() { return build Position {}; }",
             "unsafe Position Position::zero() override { return build "
             "Position {}; }",
             "Position Position::zero(i32 n) override { return build Position "
             "{}; }",
             "i32 Position::zero() override { return 0; }",
             "Position Position::zero(&self) override { return build Position "
             "{}; }",
         }) {
        SCOPED_TRACE(method);
        EXPECT_NE(FindDiagnostic(
                      Analyze("structure Position { i32 value; }; " + method),
                      DiagnosticCode::InvalidLifecycle),
                  nullptr);
    }
}

TEST(SemanticStructureTests,
     ClassifiesCanonicalZeroOverridesIndependentlyFromTheirOrigin) {
    Analysis trivial = Analyze(
        "structure Storage { *u8 data; usize length; usize capacity; }; "
        "Storage Storage::zero() override { return build Storage { "
        "self.data = null; self.length = 0; self.capacity = 0; }; } "
        "void Storage::tear(&self) override {} "
        "structure Buffer { Storage storage; }; "
        "Buffer Buffer::zero() override { return build Buffer { "
        "self.storage = none; }; }");
    ASSERT_FALSE(trivial.diagnostics.HasErrors());
    ASSERT_EQ(trivial.model.Structures().size(), 2);
    EXPECT_TRUE(SemanticType(trivial.model.Structures()[0].get())
                    .IsTriviallyZeroable());
    EXPECT_TRUE(SemanticType(trivial.model.Structures()[1].get())
                    .IsTriviallyZeroable());
    for (const Diagnostic& diagnostic : trivial.diagnostics.Diagnostics())
        EXPECT_NE(diagnostic.severity, DiagnosticSeverity::Warning);

    Analysis generic = Analyze(
        "structure Storage<T> { *T data; usize length; }; "
        "Storage<T> Storage<T>::zero() override { return build Storage<T> { "
        "self.data = null; self.length = 0; }; } "
        "void Storage<T>::tear(&self) override {} "
        "void f() { Storage<i32> storage = none; }");
    ASSERT_FALSE(generic.diagnostics.HasErrors());
    for (const Diagnostic& diagnostic : generic.diagnostics.Diagnostics())
        EXPECT_NE(diagnostic.severity, DiagnosticSeverity::Warning);

    Analysis nontrivial = Analyze(
        "structure Offset { i32 value; }; "
        "Offset Offset::zero() override { return build Offset { "
        "self.value = 1; }; } "
        "structure View { Offset offset; }; "
        "View View::zero() override { return build View {}; }");
    ASSERT_FALSE(nontrivial.diagnostics.HasErrors());
    ASSERT_EQ(nontrivial.model.Structures().size(), 2);
    EXPECT_FALSE(SemanticType(nontrivial.model.Structures()[0].get())
                     .IsTriviallyZeroable());
    EXPECT_FALSE(SemanticType(nontrivial.model.Structures()[1].get())
                     .IsTriviallyZeroable());
    std::size_t warnings = 0;
    for (const Diagnostic& diagnostic : nontrivial.diagnostics.Diagnostics())
        if (diagnostic.severity == DiagnosticSeverity::Warning) ++warnings;
    EXPECT_EQ(warnings, 2);
}

TEST(SemanticStructureTests, NoneDirectlyConstructsNonCopyableStructures) {
    Analysis result = Analyze(
        "structure Box { i32 value; }; "
        "Box Box::zero() override { return build Box {}; } "
        "void Box::tear(&self) override {} "
        "structure Temporary { i32 value; }; "
        "void Temporary::tear(&self) override {} "
        "structure Factory { i32 value; }; "
        "Factory Factory::zero() override { Temporary::init(); return build "
        "Factory {}; } "
        "Box make() { return none; } "
        "void f() { Box value = none; value = none; }");
    EXPECT_FALSE(result.diagnostics.HasErrors());
}

TEST(SemanticStructureTests, RejectsInvalidAndRecursiveZeroConstruction) {
    EXPECT_TRUE(
        Analyze("structure Box { i32 value; }; Box Box::init(i32 value) "
                "override { return build Box { self.value = value; }; } "
                "void f() { Box box = none; }")
            .diagnostics.HasErrors());
    EXPECT_NE(FindErrorDiagnostic(
                  Analyze("structure Box { i32 value; }; i32 noisy() { "
                          "stdoutln(1); return 0; } Box Box::zero() override { "
                          "return build Box { self.value = noisy(); }; }"),
                  DiagnosticCode::InvalidLifecycle),
              nullptr);
    EXPECT_NE(
        FindErrorDiagnostic(
            Analyze("structure Text { string value; }; string allocate() { "
                    "return \"value\".string(); } Text Text::zero() override { "
                    "return build Text { self.value = allocate(); }; }"),
            DiagnosticCode::InvalidLifecycle),
        nullptr);
    EXPECT_NE(
        FindErrorDiagnostic(
            Analyze("structure A { i32 value; }; structure B { i32 value; }; "
                    "A A::zero() override { B b = B::zero(); return build A "
                    "{}; } B B::zero() override { A a = A::zero(); return "
                    "build B {}; }"),
            DiagnosticCode::InvalidLifecycle),
        nullptr);
    EXPECT_NE(
        FindErrorDiagnostic(
            Analyze("structure A { i32 value; }; structure B { i32 value; }; "
                    "B default_b() { return none; } A default_a() { return "
                    "none; } A A::zero() override { B b = default_b(); return "
                    "build A {}; } B B::zero() override { A a = default_a(); "
                    "return build B {}; }"),
            DiagnosticCode::InvalidLifecycle),
        nullptr);
    EXPECT_NE(
        FindErrorDiagnostic(
            Analyze("structure Payload { string value = \"value\".string(); }; "
                    "Payload make_payload() { return build Payload {}; } "
                    "structure Box { i32 value; }; Box Box::zero() override { "
                    "Payload payload = make_payload(); return build Box {}; "
                    "}"),
            DiagnosticCode::InvalidLifecycle),
        nullptr);
    EXPECT_NE(
        FindErrorDiagnostic(
            Analyze("structure Loud { i32 value; }; "
                    "Loud Loud::zero() override { return build Loud {}; } "
                    "void Loud::tear(&self) override { stdoutln(123); } "
                    "structure Box { i32 value; }; "
                    "Box Box::zero() override { Loud local = Loud::zero(); "
                    "return build Box {}; }"),
            DiagnosticCode::InvalidLifecycle),
        nullptr);
    EXPECT_NE(
        FindErrorDiagnostic(
            Analyze("structure Noisy { i32 value; }; "
                    "void Noisy::tear(&self) override { stdoutln(123); } "
                    "structure Box { i32 value; }; "
                    "Box Box::zero() override { Noisy::init(); return build "
                    "Box {}; }"),
            DiagnosticCode::InvalidLifecycle),
        nullptr);
    EXPECT_NE(FindErrorDiagnostic(
                  Analyze("structure Noisy { i32 value; }; "
                          "void Noisy::tear(&self) override { stdoutln(123); } "
                          "structure Box { i32 value; }; "
                          "Box Box::zero() override { if Noisy::init() == "
                          "Noisy::init() {} return build Box {}; }"),
                  DiagnosticCode::InvalidLifecycle),
              nullptr);
    EXPECT_NE(
        FindErrorDiagnostic(
            Analyze("structure Loud { i32 value; }; "
                    "Loud Loud::zero() override { return build Loud {}; } "
                    "void Loud::tear(&self) override { stdoutln(123); } "
                    "structure Holder { Loud value; }; "
                    "Holder Holder::zero() override { return build Holder { "
                    "self.value = none; }; }"),
            DiagnosticCode::InvalidLifecycle),
        nullptr);
}

TEST(SemanticStructureTests,
     EnforcesMemberOperatorsPrivacyAndReceiverConstness) {
    const std::string prefix =
        "structure Box { i32 x; private { i32 y; } }; "
        "private i32 Box::secret(&const self) { return self->y; } "
        "private i32 Box::hidden() { return 42; } "
        "i32 Box::read(&const self) { return self->secret() + Box::hidden(); } "
        "void Box::set(&self) { self->y = 42; } ";
    for (const std::string& body :
         {"box.y = 2;", "box.secret();", "Box::hidden();"}) {
        SCOPED_TRACE(body);
        EXPECT_NE(FindDiagnostic(
                      Analyze(prefix + "void f() { Box box = Box::init(); " +
                              body + " }"),
                      DiagnosticCode::PrivateAccess),
                  nullptr);
    }
    for (const std::string& body :
         {"box->read();", "r.read();", "r.x;", "box->x;", "Box::read();",
          "box.hidden();"}) {
        SCOPED_TRACE(body);
        EXPECT_NE(FindDiagnostic(
                      Analyze(prefix +
                              "void f() { Box box = Box::init(); r := &box; " +
                              body + " }"),
                      DiagnosticCode::InvalidMember),
                  nullptr);
    }
    for (const std::string& body : {"box.x = 2;", "box.set();", "r->set();"}) {
        SCOPED_TRACE(body);
        EXPECT_NE(
            FindDiagnostic(
                Analyze(prefix +
                        "void f() { const Box box = Box::init(); r := &box; " +
                        body + " }"),
                DiagnosticCode::SymbolNotWritable),
            nullptr);
    }
    EXPECT_FALSE(
        Analyze(prefix +
                "i32 f() { Box box = Box::init(); return box.read(); }")
            .diagnostics.HasErrors());
}

TEST(SemanticStructureTests, ChecksLifecycleOverridesAndForbidsManualTear) {
    for (const std::string& method :
         {"Box Box::init() { return build Box {}; }",
          "void Box::tear(&self) {}", "void Box::init() override {}",
          "Box Box::init(&self) override { return build Box {}; }",
          "void Box::tear(*self) override {}",
          "void Box::tear(&const self) override {}",
          "void Box::f(&self) override {}"}) {
        SCOPED_TRACE(method);
        EXPECT_NE(FindDiagnostic(Analyze("structure Box {}; " + method),
                                 DiagnosticCode::InvalidLifecycle),
                  nullptr);
    }
    for (const std::string& call : {"box.tear();", "Box::tear();"}) {
        SCOPED_TRACE(call);
        EXPECT_NE(
            FindDiagnostic(
                Analyze("structure Box {}; void Box::tear(&self) override {} "
                        "void f() { Box box = Box::init(); " +
                        call + " }"),
                DiagnosticCode::InvalidLifecycle),
            nullptr);
    }
    EXPECT_NE(FindDiagnostic(Analyze("structure Box {}; void Box::tear(&self) "
                                     "override { self->tear(); }"),
                             DiagnosticCode::InvalidLifecycle),
              nullptr);
}

TEST(SemanticStructureTests,
     EnforcesUnsafeConstructionImmovabilityAndBorrowOnlyCalls) {
    const std::string prefix =
        "structure Box { i32 value; }; unsafe void Box::tear(&self) override "
        "{} ";
    EXPECT_NE(
        FindDiagnostic(Analyze(prefix + "void f() { Box box = Box::init(); }"),
                       DiagnosticCode::UnsafeOperation),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze(prefix + "void f() { Box box = build Box {}; }"),
                       DiagnosticCode::UnsafeOperation),
        nullptr);
    for (const std::string& body : {"copy := box;", "box = Box::init();",
                                    "pair := (box, 1);", "array := [box];"}) {
        SCOPED_TRACE(body);
        EXPECT_NE(
            FindDiagnostic(
                Analyze(prefix + "unsafe void f() { Box box = Box::init(); " +
                        body + " }"),
                DiagnosticCode::NonCopyableValue),
            nullptr);
    }
    EXPECT_NE(FindDiagnostic(Analyze(prefix + "void f(Box value) {}"),
                             DiagnosticCode::NonCopyableValue),
              nullptr);
    EXPECT_NE(
        FindDiagnostic(
            Analyze(
                prefix +
                "unsafe Box f() { Box value = Box::init(); return value; }"),
            DiagnosticCode::NonCopyableValue),
        nullptr);
    EXPECT_FALSE(Analyze(prefix +
                         "unsafe Box f() { return Box::init(); } void "
                         "use(&const Box box) {} "
                         "unsafe void g() { Box box = f(); use(&box); }")
                     .diagnostics.HasErrors());
    EXPECT_NE(
        FindDiagnostic(Analyze(prefix + "structure Parent { Box child; }; void "
                                        "f() { Parent p = Parent::init(); }"),
                       DiagnosticCode::UnsafeOperation),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(
            Analyze(prefix + "structure Parent { Box child; }; unsafe void "
                             "f() { p := Parent::init(); moved := p; }"),
            DiagnosticCode::NonCopyableValue),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze(prefix + "unsafe void f() { left, right := "
                                        "(Box::init(), Box::init()); }"),
                       DiagnosticCode::NonCopyableValue),
        nullptr);
}

TEST(SemanticStructureTests, MovesSafeTearValuesAndRestoresReassignedSources) {
    auto result = Analyze(R"(
        structure Box { i32 value; };
        void Box::tear(&self) override {}
        Box pass(Box value) { return value; }
        Box choose(bool condition, Box left, Box right) {
            if condition { return left; }
            return right;
        }
        i32 check(bool condition) {
            first := Box::init();
            second := first;
            first = Box::init();
            third := pass(second);
            selected := choose(condition, first, third);
            first = Box::init();
            return selected.value + first.value;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
}

TEST(SemanticStructureTests, DiscardingAWholeNonCopyableValueConsumesIt) {
    const std::string prefix = R"(
        structure Box { i32 value; };
        void Box::tear(&self) override {}
    )";
    auto result = Analyze(prefix + R"(
        void check() {
            box := Box::init();
            box;
            stdout(box.value);
        }
    )");
    EXPECT_NE(FindDiagnostic(result, DiagnosticCode::MovedValue), nullptr);
    EXPECT_FALSE(Analyze(prefix + R"(
        enum Pick { Left, Right, }
        void check(Pick pick) {
            left := Box::init();
            right := Box::init();
            match pick { Pick.Left => left, Pick.Right => right, };
        }
    )")
                     .diagnostics.HasErrors());
}

TEST(SemanticStructureTests, DeferredByValueCallsMoveAtRegistration) {
    const std::string prefix = R"(
        structure Box { i32 value; };
        void Box::tear(&self) override {}
        void consume(Box value) {}
    )";
    const std::string source = prefix + R"(
        void check() {
            box := Box::init();
            defer consume(box);
            stdout(box.value);
        }
    )";
    auto result = Analyze(source);
    const Diagnostic* diagnostic =
        FindDiagnostic(result, DiagnosticCode::MovedValue);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->range.start.offset, source.rfind("box"));
}

TEST(SemanticStructureTests, DiagnosesMovedPossibleAndSelfMovedValues) {
    const std::string prefix = R"(
        structure Box { i32 value; };
        void Box::tear(&self) override {}
    )";
    for (const std::string& body : {
             "first := Box::init(); second := first; stdout(first.value);",
             "first := Box::init(); if condition { second := first; } "
             "stdout(first.value);",
             "first := Box::init(); while condition { second := first; break; "
             "} stdout(first.value);",
         }) {
        SCOPED_TRACE(body);
        auto result =
            Analyze(prefix + "void check(bool condition) { " + body + " }");
        const Diagnostic* diagnostic =
            FindDiagnostic(result, DiagnosticCode::MovedValue);
        ASSERT_NE(diagnostic, nullptr);
        EXPECT_EQ(diagnostic->range.start.offset,
                  (prefix + "void check(bool condition) { " + body + " }")
                      .rfind("first"));
    }
    EXPECT_NE(FindDiagnostic(Analyze(prefix + R"(
        void check() { value := Box::init(); value = value; }
    )"),
                             DiagnosticCode::NonCopyableValue),
              nullptr);
    EXPECT_FALSE(Analyze(prefix + R"(
        void check(bool condition) {
            value := Box::init();
            if condition { moved := value; }
            value = Box::init();
            stdout(value.value);
        }
    )")
                     .diagnostics.HasErrors());
    EXPECT_NE(FindDiagnostic(Analyze(prefix + R"(
        void check() {
            left, right := (Box::init(), Box::init());
            (left, right) = (left, right);
        }
    )"),
                             DiagnosticCode::NonCopyableValue),
              nullptr);
}

TEST(SemanticStructureTests, WholeMovesRespectLoansAndRejectPartialMoves) {
    const std::string prefix = R"(
        structure Box { i32 value; };
        void Box::tear(&self) override {}
        void consume(Box value) {}
        void inspect(&const i32 value) { stdout(*value); }
    )";
    for (const std::string& body : {
             "box := Box::init(); reference := &box.value; moved := box; "
             "stdout(*reference);",
             "box := Box::init(); reference := &box.value; consume(box); "
             "stdout(*reference);",
             "box := Box::init(); reference := &box.value; defer "
             "inspect(reference); moved := box;",
         }) {
        SCOPED_TRACE(body);
        EXPECT_NE(
            FindDiagnostic(Analyze(prefix + "void check() { " + body + " }"),
                           DiagnosticCode::BorrowConflict),
            nullptr);
    }
    EXPECT_NE(FindDiagnostic(Analyze(prefix + R"(
        void check() { values := [Box::init()]; box := values[0]; }
    )"),
                             DiagnosticCode::NonCopyableValue),
              nullptr);
    EXPECT_FALSE(Analyze(prefix + R"(
        void check() {
            box := Box::init();
            reference := &box.value;
            stdout(*reference);
            moved := box;
        }
    )")
                     .diagnostics.HasErrors());
    EXPECT_NE(FindDiagnostic(Analyze(R"(
            structure Buffer { own *i32 data; usize length; };
            unsafe Buffer Buffer::init(own *i32 data, usize length) override {
                return build Buffer {
                    self.data = data;
                    self.length = length;
                };
            }
            void Buffer::tear(&self) override {
                unsafe { free(self->data); }
            }
            unsafe []i32 Buffer::view(&self) <- self {
                return self->data[:self->length];
            }
            unsafe void check() {
                data, err := new<[]i32>(1);
                buffer := Buffer::init(data, 1);
                values := buffer.view();
                moved := buffer;
                stdout(values[0]);
            }
        )"),
                             DiagnosticCode::BorrowConflict),
              nullptr);
}

TEST(SemanticStructureTests, MovesWholeTuplesArraysAndNestedValues) {
    auto result = Analyze(R"(
        structure Box { i32 value; };
        void Box::tear(&self) override {}
        structure Outer { Box child; };
        void Outer::tear(&self) override {}
        void check() {
            pair := (Box::init(), Box::init());
            left, right := pair;
            values := [left, right];
            movedValues := values;
            outer := Outer::init();
            movedOuter := outer;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
}

TEST(SemanticStructureTests, MoveOnlyArrayRepeatsConsumeAtMostOnce) {
    EXPECT_FALSE(Analyze(R"(
        structure Box {};
        void Box::tear(&self) override {}
        void check() {
            zero := Box::init();
            empty := [zero; 0];
            one := Box::init();
            singleton := [one; 1];
        }
    )")
                     .diagnostics.HasErrors());
    EXPECT_NE(FindDiagnostic(Analyze(R"(
        structure Box {};
        void Box::tear(&self) override {}
        void check() {
            box := Box::init();
            duplicated := [box; 2];
        }
    )"),
                             DiagnosticCode::NonCopyableValue),
              nullptr);
}

TEST(SemanticStructureTests,
     PreservesBorrowLoansAcrossFieldsAndMethodArguments) {
    const std::string prefix =
        "structure Box { i32 x; }; void Box::set(&self, &Box other) {} ";
    EXPECT_NE(FindDiagnostic(
                  Analyze(prefix +
                          "void f() { Box box = Box::init(); box.set(&box); }"),
                  DiagnosticCode::BorrowConflict),
              nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze(prefix + "void f() { Box box = Box::init(); r "
                                        ":= &box.x; box.x = 2; value := *r; }"),
                       DiagnosticCode::BorrowConflict),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze(prefix + "void f() { Box box = Box::init(); r "
                                        ":= &box; moved := r; r->x = 2; }"),
                       DiagnosticCode::MovedReference),
        nullptr);
    EXPECT_FALSE(
        Analyze(
            prefix +
            "void f() { Box a = Box::init(); Box b = Box::init(); a.set(&b); }")
            .diagnostics.HasErrors());
    EXPECT_NE(
        FindDiagnostic(Analyze(prefix + "void f() { &i32 r = none; { Box box = "
                                        "Box::init(); r = &box.x; } }"),
                       DiagnosticCode::IncompatibleType),
        nullptr);
}

TEST(SemanticStructureTests,
     RejectsRecursiveLayoutsAndUnfinishedConstructionEscapes) {
    for (const std::string& source :
         {"structure Box { Box value; };",
          "structure A { B b; }; structure B { A a; };",
          "structure Box { void value; };",
          "structure Box { &i32 reference; };",
          "structure Box {}; Box Box::init() override { return build Box { "
          "copy := self; }; }",
          "structure Box {}; void Box::f(&self) {} Box Box::init() override { "
          "return build Box { self.f(); }; }"}) {
        SCOPED_TRACE(source);
        EXPECT_NE(
            FindDiagnostic(Analyze(source), DiagnosticCode::InvalidStructure),
            nullptr);
    }
    EXPECT_FALSE(Analyze("structure Node { *Node next; i32 value; }; void f() "
                         "{ Node n = Node::init(); }")
                     .diagnostics.HasErrors());
}

TEST(SemanticStructureTests, ReportsPreciseAccessAndLifecycleRanges) {
    const std::string prefix = "structure Box { private i32 secret; }; ";
    const std::string source =
        prefix + "void f() { Box box = Box::init(); box.secret; }";
    Analysis result = Analyze(source);
    const Diagnostic* diagnostic =
        FindDiagnostic(result, DiagnosticCode::PrivateAccess);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->range.start.offset, source.rfind("secret"));
    EXPECT_EQ(diagnostic->range.Length(), 6);
    EXPECT_NE(
        FindDiagnostic(
            Analyze(prefix + "void f() { Box box = Box::init(); box.tear(); }"),
            DiagnosticCode::InvalidLifecycle),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(
            Analyze("structure Box { i32 x; }; Box copy(Box b) { return b; } "
                    "Box f() { return build Box { x := copy(self).x; }; }"),
            DiagnosticCode::InvalidStructure),
        nullptr);
}

TEST(SemanticStructureTests, ConstructionFieldBorrowsCannotEscapeBuild) {
    const std::string prefix = "structure Box { i32 x; }; ";
    EXPECT_NE(FindDiagnostic(
                  Analyze(prefix + "void f() { i32 n = 0; r := &n; Box b = "
                                   "build Box { r = &self.x; }; *r = 42; }"),
                  DiagnosticCode::ReferenceEscape),
              nullptr);
    EXPECT_FALSE(
        Analyze(prefix +
                "Box f() { return build Box { r := &self.x; *r = 42; }; }")
            .diagnostics.HasErrors());
}

TEST(SemanticStructureTests, ValidatesCompoundFieldTypesAndBuildControlFlow) {
    EXPECT_NE(
        FindDiagnostic(Analyze("structure Box { *[1]string unsupported; };"),
                       DiagnosticCode::InvalidArrayElement),
        nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("structure Box { *&i32 unsupported; };"),
                             DiagnosticCode::UnsupportedCompoundType),
              nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("structure Box {}; Box f() { return build "
                                     "Box { return Box::init(); }; }"),
                             DiagnosticCode::InvalidStructure),
              nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("structure Box {}; void f() { loop ;; { "
                                     "Box b = build Box { break; }; } }"),
                             DiagnosticCode::LoopControlOutsideLoop),
              nullptr);
}

TEST(SemanticStructureTests, AllowsParameterizedInitOverridesAndChecksCalls) {
    const std::string prefix =
        "structure Box { i32 value; }; "
        "Box Box::init(i32 value) override { return build Box { self.value = "
        "value; }; } ";
    EXPECT_FALSE(
        Analyze(prefix + "i32 f() { Box b = Box::init(42); return b.value; }")
            .diagnostics.HasErrors());
    for (const std::string& arguments : {"", "1, 2"}) {
        SCOPED_TRACE(arguments);
        EXPECT_NE(
            FindDiagnostic(Analyze(prefix + "void f() { Box b = Box::init(" +
                                   arguments + "); }"),
                           DiagnosticCode::WrongArgumentCount),
            nullptr);
    }
    EXPECT_NE(FindDiagnostic(
                  Analyze(prefix + "void f() { Box b = Box::init(true); }"),
                  DiagnosticCode::IncompatibleArgument),
              nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("structure Box {}; Box Box::init(i32 "
                                     "value) { return build Box {}; }"),
                             DiagnosticCode::InvalidLifecycle),
              nullptr);
    EXPECT_FALSE(
        Analyze("structure Box { i32 value; }; Box Box::init(&const i32 value) "
                "override { return build Box { self.value = *value; }; } "
                "void f() { i32 n = 42; Box b = Box::init(&n); }")
            .diagnostics.HasErrors());
    EXPECT_NE(
        FindDiagnostic(Analyze("structure Box {}; unsafe Box Box::init(i32 "
                               "value) override { return build Box {}; } "
                               "void f() { Box b = Box::init(42); }"),
                       DiagnosticCode::UnsafeOperation),
        nullptr);
}

TEST(SemanticStructureTests,
     RequiresExplicitFieldDefaultsForParameterizedInitializers) {
    const std::string prefix =
        "structure Box { i32 value; }; Box Box::init(i32 value) override { "
        "return build Box { self.value = value; }; } ";
    for (const std::string& field :
         {"Box box;", "[2]Box boxes;", "(Box, i32) pair;"}) {
        SCOPED_TRACE(field);
        EXPECT_NE(FindDiagnostic(
                      Analyze(prefix + "structure Parent { " + field + " };"),
                      DiagnosticCode::InvalidStructure),
                  nullptr);
    }
    EXPECT_FALSE(Analyze(prefix +
                         "structure Parent { Box box = Box::init(42); [2]Box "
                         "boxes = [Box::init(1), Box::init(2)]; "
                         "(Box, i32) pair = (Box::init(3), 4); *Box pointer; "
                         "}; void f() { Parent p = Parent::init(); }")
                     .diagnostics.HasErrors());
    EXPECT_NE(FindDiagnostic(
                  Analyze(prefix + "structure Parent { &Box reference; };"),
                  DiagnosticCode::InvalidStructure),
              nullptr);
}

TEST(SemanticAllocationTests, TypesNewFreeAndAllocationErrors) {
    Analysis result = Analyze(
        "unsafe void f() { (*u8 p, error err) = new<u8>(42); "
        "if err == AllocationError.OutOfMemory { return; } defer free(p); "
        "free(null); free(none); (fixed, failure) := new<const i32>(none); "
        "free(fixed); }");
    EXPECT_FALSE(result.diagnostics.HasErrors());
    for (const std::string& body :
         {"p, e := new<u8>(42);", "free(p);", "defer free(p);"}) {
        SCOPED_TRACE(body);
        EXPECT_NE(FindDiagnostic(Analyze("void f(*u8 p) { " + body + " }"),
                                 DiagnosticCode::UnsafeOperation),
                  nullptr);
    }
    EXPECT_NE(
        FindDiagnostic(
            Analyze("unsafe void f() { (*u8 p, error e) = new<u8>(true); }"),
            DiagnosticCode::IncompatibleArgument),
        nullptr);
    EXPECT_NE(FindDiagnostic(
                  Analyze("unsafe void f() { p := new<u8>(42); free(p); }"),
                  DiagnosticCode::InvalidAllocation),
              nullptr);
}

TEST(SemanticAllocationTests, RejectsUnsupportedStorageAndInvalidCalls) {
    for (const std::string& body :
         {"new(42);", "new<void>(none);", "new<stringv>(\"view\");",
          "new<&i32>(none);", "new<[]string>(1);", "free(42);",
          "*void p = null; free(p);"}) {
        SCOPED_TRACE(body);
        EXPECT_NE(FindDiagnostic(Analyze("unsafe void f() { " + body + " }"),
                                 DiagnosticCode::InvalidAllocation),
                  nullptr);
    }
    for (const std::string& body :
         {"new<u8>();", "new<u8>(1, 2);", "free();", "free(null, null);"}) {
        SCOPED_TRACE(body);
        EXPECT_NE(FindDiagnostic(Analyze("unsafe void f() { " + body + " }"),
                                 DiagnosticCode::WrongArgumentCount),
                  nullptr);
    }
    EXPECT_NE(FindDiagnostic(Analyze("unsafe void f() { defer new<u8>(1); }"),
                             DiagnosticCode::InvalidDefer),
              nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("void free() {}"),
                             DiagnosticCode::DuplicateDeclaration),
              nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("error AllocationError { Other }"),
                             DiagnosticCode::DuplicateDeclaration),
              nullptr);
    const std::string source = "unsafe void f() { new<stringv>(\"view\"); }";
    Analysis result = Analyze(source);
    const Diagnostic* diagnostic =
        FindDiagnostic(result, DiagnosticCode::InvalidAllocation);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->range.start.offset, source.find("stringv"));
    EXPECT_EQ(diagnostic->range.Length(), 7);
}

TEST(SemanticAllocationTests,
     PreservesConstructionAndConditionalInitializationRules) {
    const std::string prefix =
        "structure Box { i32 x; }; unsafe void Box::tear(&self) override {} ";
    EXPECT_FALSE(
        Analyze(
            prefix +
            "unsafe void f() { p, e := new<Box>(Box::init()); defer free(p); }")
            .diagnostics.HasErrors());
    EXPECT_NE(
        FindDiagnostic(
            Analyze(prefix +
                    "unsafe void f() { Box b = Box::init(); new<Box>(b); }"),
            DiagnosticCode::NonCopyableValue),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze(prefix + "unsafe void f() { new<Box>(none); }"),
                       DiagnosticCode::IncompatibleArgument),
        nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("unsafe i32 f() { i32 x; p, e := "
                                     "new<i32>(x = 42); free(p); return x; }"),
                             DiagnosticCode::UninitializedVariable),
              nullptr);
    EXPECT_FALSE(Analyze("unsafe i32 f() { i32 x = 0; p, e := new<i32>(x = "
                         "42); free(p); return x; }")
                     .diagnostics.HasErrors());
    EXPECT_NE(
        FindDiagnostic(
            Analyze("unsafe void f() { const i32 x = 42; new<i32>(x = 1); }"),
            DiagnosticCode::SymbolNotWritable),
        nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("unsafe void f() { p, e := new<const "
                                     "i32>(42); *p = 1; free(p); }"),
                             DiagnosticCode::SymbolNotWritable),
              nullptr);
    EXPECT_NE(
        FindDiagnostic(
            Analyze("unsafe void f() { i32 x = 1; i32 y = 2; r := &x; "
                    "q := &y; p, e := new<i32>(*(r = q)); free(p); *q = 42; }"),
            DiagnosticCode::MovedReference),
        nullptr);
    EXPECT_FALSE(Analyze("unsafe void f() { i32 x = 1; q := &x; "
                         "p, e := new<i32>(*q); *q = 42; free(p); }")
                     .diagnostics.HasErrors());
}

TEST(SemanticMemoryTests, DiagnosesLeaksWithAllocationAndLossLocations) {
    const std::string source =
        "unsafe void f() { p, e := new<u8>(42); p = null; }";
    const Analysis result = Analyze(source);
    const Diagnostic* leak = FindDiagnostic(result, DiagnosticCode::MemoryLeak);
    ASSERT_NE(leak, nullptr);
    EXPECT_EQ(leak->severity, DiagnosticSeverity::Error);
    EXPECT_EQ(leak->range.start.offset, source.find("p = null"));
    bool origin = false;
    for (const Diagnostic& diagnostic : result.diagnostics.Diagnostics())
        if (diagnostic.severity == DiagnosticSeverity::Note &&
            diagnostic.range.start.offset == source.find("new<u8>"))
            origin = true;
    EXPECT_TRUE(origin);
    for (const std::string& body :
         {"new<u8>(42);", "p, e := new<u8>(42);",
          "p, e := new<u8>(42); if e != none { return; } return;",
          "p, e := new<u8>(42); if flag { free(p); }",
          "{ p, e := new<u8>(42); }"}) {
        SCOPED_TRACE(body);
        EXPECT_NE(
            FindDiagnostic(Analyze("unsafe void f(bool flag) { " + body + " }"),
                           DiagnosticCode::MemoryLeak),
            nullptr);
    }
}

TEST(SemanticMemoryTests, CorrelatesFailureAndChecksNullDereferences) {
    for (const std::string& guard :
         {"e != none", "p == null", "e == AllocationError.OutOfMemory",
          "e != none or p == null"}) {
        SCOPED_TRACE(guard);
        EXPECT_FALSE(Analyze("unsafe void f() { p, e := new<u8>(42); if " +
                             guard + " { return; } defer free(p); *p = 1; }")
                         .diagnostics.HasErrors());
    }
    EXPECT_NE(FindDiagnostic(Analyze("unsafe void f() { p, e := new<u8>(42); "
                                     "defer free(p); *p = 1; }"),
                             DiagnosticCode::NullDereference),
              nullptr);
    EXPECT_FALSE(
        Analyze("unsafe void f() { p, e := new<u8>(42); defer free(p); "
                "if e == none { *p = 1; } }")
            .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("unsafe void f() { p, e := new<u8>(42); "
                         "if e == none and p != null { *p = 1; } free(p); }")
                     .diagnostics.HasErrors());
    EXPECT_FALSE(
        Analyze("unsafe void f() { p, e := new<u8>(42); "
                "ok := e == none and p != null; if ok { *p = 1; } free(p); }")
            .diagnostics.HasErrors());
}

TEST(SemanticMemoryTests, AliasesShareAllocationIdentity) {
    EXPECT_FALSE(Analyze("unsafe void f() { p, e := new<u8>(42); q := p; p = "
                         "null; free(q); }")
                     .diagnostics.HasErrors());
    EXPECT_NE(FindDiagnostic(Analyze("unsafe void f() { p, e := new<u8>(42); q "
                                     ":= p; free(p); free(q); }"),
                             DiagnosticCode::DoubleFree),
              nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("unsafe void f() { p, e := new<u8>(42); "
                                     "if e != none { return; } "
                                     "q := p; free(p); *q = 1; }"),
                             DiagnosticCode::UseAfterFree),
              nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze("unsafe void f(bool c) { p, e := new<u8>(42); "
                               "if c { free(p); } free(p); }"),
                       DiagnosticCode::DoubleFree),
        nullptr);
    EXPECT_FALSE(Analyze("unsafe void f(bool c) { p, e := new<u8>(42); "
                         "if c { free(p); } else { free(p); } }")
                     .diagnostics.HasErrors());
}

TEST(SemanticMemoryTests, DefersCaptureValuesAndRunOnAllExits) {
    EXPECT_FALSE(
        Analyze(
            "unsafe void f() { p, e := new<u8>(42); defer free(p); p = null; }")
            .diagnostics.HasErrors());
    EXPECT_NE(FindDiagnostic(Analyze("unsafe void f() { p, e := new<u8>(42); "
                                     "defer free(p); free(p); }"),
                             DiagnosticCode::DoubleFree),
              nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("unsafe void f() { p, e := new<u8>(42); "
                                     "defer free(p); defer free(p); }"),
                             DiagnosticCode::DoubleFree),
              nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("unsafe void f() { p, e := new<u8>(42); "
                                     "if e != none { return; } "
                                     "{ defer free(p); } *p = 1; }"),
                             DiagnosticCode::UseAfterFree),
              nullptr);
    for (const std::string& exit :
         {"return;", "break;", "proceed;", "jumpto done;"}) {
        SCOPED_TRACE(exit);
        EXPECT_FALSE(
            Analyze("unsafe void f(bool c) { while c { p, e := new<u8>(42); "
                    "defer free(p); " +
                    exit + " #done break; } }")
                .diagnostics.HasErrors());
    }
}

TEST(SemanticMemoryTests, UsesRealControlFlowAndFreshLoopInstances) {
    EXPECT_FALSE(Analyze("unsafe void f(bool c) { while c { p, e := "
                         "new<u8>(42); free(p); } }")
                     .diagnostics.HasErrors());
    EXPECT_NE(FindDiagnostic(Analyze("unsafe void f(bool c) { while c { p, e "
                                     ":= new<u8>(42); break; } }"),
                             DiagnosticCode::MemoryLeak),
              nullptr);
    EXPECT_NE(
        FindDiagnostic(
            Analyze(
                "unsafe void f(bool c) { loop ;; { #again p, e := new<u8>(42); "
                "defer free(p); if c { jumpto again; } break; } }"),
            DiagnosticCode::UnverifiedOwnership),
        nullptr);
    EXPECT_FALSE(
        Analyze(
            "unsafe void f(bool c) { loop ;; { #again { p, e := new<u8>(42); "
            "defer free(p); if c { jumpto again; } } break; } }")
            .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("unsafe void f() { loop ;; { jumpto done; "
                         "new<u8>(42); #done break; } }")
                     .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("unsafe void f() { if false { new<u8>(42); } }")
                     .diagnostics.HasErrors());
}

TEST(SemanticMemoryTests, FollowsConsumingBorrowingAndFactoryCalls) {
    const std::string prefix =
        "unsafe void release(*u8 p) { defer free(p); } "
        "unsafe void borrow(*u8 p) { *p = 1; } "
        "unsafe (*u8, error) make() { return new<u8>(42); } ";
    EXPECT_FALSE(
        Analyze(prefix +
                "unsafe void f() { p, e := make(); if e != none { return; } "
                "borrow(p); release(p); }")
            .diagnostics.HasErrors());
    EXPECT_NE(
        FindDiagnostic(
            Analyze(prefix +
                    "unsafe void f() { p, e := make(); release(p); free(p); }"),
            DiagnosticCode::DoubleFree),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(
            Analyze(
                prefix +
                "unsafe void f() { p, e := make(); if e != none { return; } "
                "release(p); borrow(p); }"),
            DiagnosticCode::UseAfterFree),
        nullptr);
    EXPECT_NE(FindDiagnostic(Analyze(prefix + "unsafe void f() { make(); }"),
                             DiagnosticCode::MemoryLeak),
              nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("unsafe *u8 f() { p, e := new<u8>(42); "
                                     "defer free(p); return p; }"),
                             DiagnosticCode::UseAfterFree),
              nullptr);
}

TEST(SemanticMemoryTests, RejectsForeignFreesAndUnverifiedStorage) {
    EXPECT_NE(
        FindDiagnostic(
            Analyze("unsafe void f() { u8 value = 1; free(&raw value); }"),
            DiagnosticCode::InvalidFree),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(
            Analyze("structure Box { u8 value; }; unsafe void f() { "
                    "p, e := new<Box>(Box::init()); if e != none { return; } "
                    "free(&raw p->value); free(p); }"),
            DiagnosticCode::InvalidFree),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze("structure Box { *u8 p; }; unsafe void f() { "
                               "Box box = Box::init(); "
                               "p, e := new<u8>(42); box.p = p; free(p); }"),
                       DiagnosticCode::UnverifiedOwnership),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze("unsafe void recurse(*u8 p) { recurse(p); } "
                               "unsafe void f() { p, e := new<u8>(42); "
                               "recurse(p); free(p); }"),
                       DiagnosticCode::UnverifiedOwnership),
        nullptr);
}

TEST(SemanticMemoryTests, PreservesTupleAndIndirectBindingAliases) {
    EXPECT_FALSE(Analyze("unsafe void f() { result := new<u8>(42); p, e := "
                         "result; free(p); }")
                     .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("unsafe void f() { p, e := new<u8>(42); q := p; r := "
                         "&p; *r = null; free(q); }")
                     .diagnostics.HasErrors());
    EXPECT_NE(
        FindDiagnostic(
            Analyze(
                "unsafe void f() { p, e := new<u8>(42); r := &p; *r = null; }"),
            DiagnosticCode::MemoryLeak),
        nullptr);
}

TEST(SemanticMemoryTests, FollowsConditionalConsumptionAndDeferredCallOrder) {
    EXPECT_NE(
        FindDiagnostic(
            Analyze("unsafe void release(*u8 p, bool c) { if c { free(p); } } "
                    "unsafe void f(bool c) { p, e := new<u8>(1); release(p, "
                    "c); free(p); }"),
            DiagnosticCode::DoubleFree),
        nullptr);
    const std::string prefix =
        "unsafe void read(*u8 p) { *p = 1; } unsafe void f() { "
        "p, e := new<u8>(1); if e != none { return; } ";
    EXPECT_FALSE(Analyze(prefix + "defer free(p); defer read(p); }")
                     .diagnostics.HasErrors());
    EXPECT_NE(
        FindDiagnostic(Analyze(prefix + "defer read(p); defer free(p); }"),
                       DiagnosticCode::UseAfterFree),
        nullptr);
    EXPECT_NE(FindDiagnostic(
                  Analyze("unsafe void clear(&*u8 p) { *p = null; } "
                          "unsafe void f() { p, e := new<u8>(1); clear(&p); }"),
                  DiagnosticCode::MemoryLeak),
              nullptr);
}

TEST(SemanticMemoryTests,
     DoesNotResurrectStaleAliasesWhenAllocationSiteRepeats) {
    EXPECT_NE(
        FindDiagnostic(
            Analyze("unsafe void f(bool c) { *u8 old = null; loop ;; { p, e := "
                    "new<u8>(1); "
                    "if e != none { return; } if old != null { *old = 3; } "
                    "old = p; free(p); if c { break; } } }"),
            DiagnosticCode::UseAfterFree),
        nullptr);
}

TEST(SemanticMemoryTests,
     TracksAggregateAliasesAndRejectsHiddenFieldOwnership) {
    EXPECT_NE(FindDiagnostic(Analyze("unsafe void f() { p, e := new<u8>(1); a "
                                     ":= [p, p]; free(a[0]); free(a[1]); }"),
                             DiagnosticCode::DoubleFree),
              nullptr);
    EXPECT_NE(
        FindDiagnostic(
            Analyze("*u8 make() { unsafe { p, e := new<u8>(1); return p; } } "
                    "structure Box { *u8 p = make(); }; void f() { Box b = "
                    "Box::init(); }"),
            DiagnosticCode::UnverifiedOwnership),
        nullptr);
    EXPECT_NE(FindDiagnostic(Analyze("unsafe void f() { p, e := new<u8>(1); q, "
                                     "err := new<*u8>(p); free(q); free(p); }"),
                             DiagnosticCode::UnverifiedOwnership),
              nullptr);
}

TEST(SemanticMemoryTests,
     PreservesCleanupAcrossRebindingShadowingAndUnsafeScopes) {
    EXPECT_FALSE(Analyze("unsafe void f() { p, e := new<u8>(1); defer free(p); "
                         "(p, e) = new<u8>(2); free(p); }")
                     .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("unsafe void f() { p, e := new<u8>(1); { p, e := "
                         "new<u8>(2); free(p); } free(p); }")
                     .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("void f() { *u8 p = null; unsafe { (p, error e) = "
                         "new<u8>(1); } unsafe { free(p); } }")
                     .diagnostics.HasErrors());
    EXPECT_NE(
        FindDiagnostic(
            Analyze("unsafe void f(bool c) { loop ;; { p, e := new<u8>(1); "
                    "if c { jumpto end; } defer free(p); #end break; } }"),
            DiagnosticCode::MemoryLeak),
        nullptr);
}

TEST(SemanticMemoryTests,
     ChecksNullableArgumentsAndRejectsUntrackedContainerAliases) {
    EXPECT_NE(
        FindDiagnostic(
            Analyze(
                "unsafe void write(*u8 p) { *p = 1; } "
                "unsafe void f() { p, e := new<u8>(1); write(p); free(p); }"),
            DiagnosticCode::NullDereference),
        nullptr);
    for (const std::string& operation :
         {"a[0] = null;", "s := a[0:2];", "r := &raw a[0];",
          "r := a.front();"}) {
        SCOPED_TRACE(operation);
        EXPECT_NE(
            FindDiagnostic(
                Analyze("unsafe void f() { p, e := new<u8>(1); a := [p, p]; " +
                        operation + " free(p); }"),
                DiagnosticCode::UnverifiedOwnership),
            nullptr);
    }
}

TEST(SemanticMemoryTests,
     InteriorAddressesDoNotTransferTheAllocationObligation) {
    EXPECT_NE(
        FindDiagnostic(Analyze("unsafe *u8 f() { p, e := new<[1]u8>([42]); if "
                               "e != none { return null; } "
                               "return &raw (*p)[0]; }"),
                       DiagnosticCode::MemoryLeak),
        nullptr);
    EXPECT_NE(
        FindDiagnostic(Analyze("unsafe void f() { p, e := new<[1]u8>([42]); if "
                               "e != none { return; } "
                               "q := &raw (*p)[0]; p = null; *q = 1; }"),
                       DiagnosticCode::MemoryLeak),
        nullptr);
}

TEST(SemanticMemoryTests, OwnershipContractsVerifyBodiesIndependently) {
    for (const std::string& source : {
             "unsafe void consume(own *i32 p) { free(p); }",
             "unsafe void consume(own *i32 p) { if p == null { return; } "
             "free(p); }",
             "unsafe void inspect(lend *const i32 p) { if p != null { x := *p; "
             "} }",
             "unsafe own *i32 pass(own *i32 p) { return p; }",
             "unsafe void consume(own *i32 p, bool again) { if again { "
             "consume(p, false); } else { free(p); } }",
             "unsafe void inspect(lend *i32 p, bool again) { if again { "
             "inspect(p, false); } }",
         }) {
        SCOPED_TRACE(source);
        const auto result = Analyze(source);
        for (const auto& diagnostic : result.diagnostics.Diagnostics())
            ADD_FAILURE() << diagnostic.message;
    }
    for (const std::string& source : {
             "unsafe void leak(own *i32 p) {}",
             "unsafe void leak(own *i32 p, bool flag) { if flag { free(p); } }",
             "unsafe void destroy(lend *i32 p) { free(p); }",
             "unsafe *i32 escape(lend *i32 p) { return p; }",
             "unsafe *i32 escape(own *i32 p) { return p; }",
             "unsafe own *i32 escape(lend *i32 p) { return p; }",
             "unsafe own *i32 bad() { i32 x = 1; return &raw x; }",
             "structure Box { *i32 p; }; unsafe Box bad(lend *i32 p) { return "
             "build Box { self.p = p; }; }",
         }) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
}

TEST(SemanticMemoryTests, OwnershipTransfersInvalidateAliasesAndRespectDefers) {
    const std::string prefix =
        "unsafe void consume(own *i32 p) { free(p); } unsafe void f() { p, e "
        ":= new<i32>(42); ";
    for (const std::string& body : {
             "q := p; consume(p); free(q);",
             "q := p; consume(p); x := *q;",
             "defer free(p); consume(p);",
             "defer consume(p); consume(p);",
         }) {
        SCOPED_TRACE(body);
        EXPECT_TRUE(Analyze(prefix + body + " }").diagnostics.HasErrors());
    }
    EXPECT_FALSE(Analyze(prefix + "consume(p); }").diagnostics.HasErrors());
    EXPECT_FALSE(
        Analyze(prefix + "defer consume(p); }").diagnostics.HasErrors());
    EXPECT_TRUE(Analyze("unsafe void both(own *i32 a, lend *i32 b) { free(a); "
                        "} unsafe void f() { p,e := new<i32>(1); both(p,p); }")
                    .diagnostics.HasErrors());
    EXPECT_TRUE(Analyze("unsafe void inspect(lend *i32 p) { x := *p; } unsafe "
                        "void f() { inspect(null); }")
                    .diagnostics.HasErrors());
}

TEST(SemanticMemoryTests,
     OwningReturnsPreserveFailurePathsAndTransferResponsibility) {
    const std::string prefix =
        "unsafe (own *i32, error) create() { return new<i32>(42); } ";
    EXPECT_FALSE(Analyze(prefix + "unsafe void f() { p,e := create(); if e != "
                                  "none { return; } free(p); }")
                     .diagnostics.HasErrors());
    EXPECT_TRUE(Analyze(prefix + "unsafe void f() { create(); }")
                    .diagnostics.HasErrors());
    EXPECT_TRUE(Analyze(prefix + "unsafe void f() { p,e := create(); }")
                    .diagnostics.HasErrors());
    EXPECT_FALSE(
        Analyze("unsafe own *i32 give(own *i32 p) { return p; } unsafe void "
                "f() { p,e := new<i32>(42); q := give(p); free(q); }")
            .diagnostics.HasErrors());
    EXPECT_TRUE(
        Analyze("unsafe own *i32 give(own *i32 p) { return p; } unsafe void "
                "f() { p,e := new<i32>(42); q := give(p); free(p); free(q); }")
            .diagnostics.HasErrors());
}

TEST(SemanticMemoryTests,
     OwningFieldsRequireVerifiedTearAndConstructionTransfer) {
    const std::string box =
        "structure Box { own *i32 p; }; unsafe Box Box::init(own *i32 p) "
        "override { return build Box { self.p = p; }; } ";
    const std::string tear =
        "unsafe void Box::tear(&self) override { free(self->p); } ";
    const auto good =
        Analyze(box + tear +
                "unsafe void f() { p,e := new<i32>(42); b := Box::init(p); }");
    for (const auto& diagnostic : good.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    EXPECT_TRUE(Analyze(box).diagnostics.HasErrors());
    EXPECT_TRUE(Analyze(box + "unsafe void Box::tear(&self) override {}")
                    .diagnostics.HasErrors());
    EXPECT_TRUE(Analyze(box + "unsafe void Box::tear(&self) override { "
                              "free(self->p); free(self->p); }")
                    .diagnostics.HasErrors());
    EXPECT_TRUE(Analyze(box + tear +
                        "unsafe void f() { p,e := new<i32>(42); b := "
                        "Box::init(p); free(p); }")
                    .diagnostics.HasErrors());
    EXPECT_TRUE(Analyze(box + tear +
                        "unsafe void f() { p,e := new<i32>(42); b := "
                        "Box::init(p); other := b; }")
                    .diagnostics.HasErrors());
    EXPECT_TRUE(
        Analyze(box + tear + "unsafe void Box::bad(&self) { free(self->p); }")
            .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze(box + "unsafe void Box::tear(&self) override { if "
                               "self->p != null { free(self->p); } }")
                     .diagnostics.HasErrors());
}

TEST(SemanticMemoryTests, ContractsNeverQualifyReferencesOrOrdinaryStorage) {
    for (const std::string& source : {
             "void f(own &i32 x) {}",
             "void f(lend &i32 x) {}",
             "void f(own i32 x) {}",
             "unsafe lend *i32 f() { return null; }",
             "structure Box { lend *i32 p; };",
             "void f() { own *i32 p = null; }",
             "void f(*own *i32 p) {}",
             "void f((own *i32, i32) p) {}",
             "structure Box { (own *i32, i32) p; };",
         }) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
}

TEST(SemanticMemoryTests, ContractSummariesRejectIndirectHandleMutation) {
    EXPECT_TRUE(
        Analyze("unsafe void clear(lend **i32 slot) { *slot = null; } unsafe "
                "void f() { p,e := new<i32>(42); clear(&raw p); free(p); }")
            .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze("unsafe own *i32 give(own *i32 p) { return p; } "
                         "unsafe void f() { give(null); }")
                     .diagnostics.HasErrors());
}

TEST(SemanticMemoryTests, ContractsRejectDuplicatedOwnersAndInteriorAliases) {
    EXPECT_TRUE(Analyze("unsafe (own *i32, own *i32) duplicate(own *i32 p) { "
                        "return (p, p); }")
                    .diagnostics.HasErrors());
    EXPECT_TRUE(Analyze("structure Item { i32 n; }; unsafe void consume(own "
                        "*Item p, lend *i32 inside) { free(p); } unsafe void "
                        "f() { p,e := new<Item>(Item::init()); if e != none { "
                        "return; } consume(p, &raw p->n); }")
                    .diagnostics.HasErrors());
}

TEST(SemanticMemoryTests, RuntimeArraysUseCountsAndExistingOwnershipContracts) {
    auto analysis = Analyze(
        "unsafe (own *i32, error) make(usize count) { return "
        "new<[]i32>(count); } "
        "unsafe i32 read(lend *const i32 p) { if p == null { return 0; } "
        "return p[0]; } "
        "unsafe void f(usize count) { p, e := make(count); if e != none { "
        "return; } "
        "defer free(p); p[0] = 42; read(p); }");
    EXPECT_FALSE(analysis.diagnostics.HasErrors());
}

TEST(SemanticMemoryTests,
     RuntimeArraysRejectUnsupportedElementsAndInvalidCounts) {
    for (const std::string source :
         {"unsafe void f(i32 n) { p, e := new<[]i32>(n); free(p); }",
          "unsafe void f() { p, e := new<[]i32>(-1); free(p); }",
          "unsafe void f() { p, e := new<[]string>(2); free(p); }",
          "unsafe void f() { p, e := new<[]*i32>(2); free(p); }",
          "structure Text { string value; }; Text Text::zero() override { "
          "return build Text {}; } unsafe void f() { p, e := "
          "new<[]Text>(2); free(p); }",
          "structure Owned { i32 value; }; void Owned::tear(&self) override "
          "{} Owned Owned::zero() override { return build Owned {}; } "
          "unsafe void f() { p, e := new<[]Owned>(2); free(p); }",
          "unsafe void f() { p, e := new<[]i32>(); }",
          "void f() { p, e := new<[]i32>(2); }"})
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors()) << source;
}

TEST(SemanticMemoryTests, RuntimeArrayIndexingRequiresLiveAllocationBases) {
    for (const std::string source :
         {"unsafe void f() { p, e := new<[]i32>(2); free(p); p[0] = 1; }",
          "unsafe void f() { i32 x = 0; p := &raw x; p[0] = 1; }",
          "unsafe void f() { *i32 p = null; p[0] = 1; }",
          "unsafe void f() { p, e := new<[]const i32>(2); defer free(p); p[0] "
          "= 1; }",
          "unsafe void f() { p, e := new<[]i32>(2); defer free(p); r := &p[0]; "
          "}",
          "unsafe void f(*void p) { p[0]; }",
          "unsafe void f() { p, e := new<const []i32>(2); defer free(p); p[0] "
          "= 1; }",
          "void f(*i32 p) { p[0] = 1; }"})
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors()) << source;
}

TEST(SemanticMemoryTests, RuntimeArraySlicesRequireTrackedOwnershipAndUnsafe) {
    const std::string buffer =
        "structure Buffer { own *i32 data; usize length; }; "
        "unsafe Buffer Buffer::init(own *i32 data, usize length) override { "
        "return build Buffer { self.data = data; self.length = length; }; } "
        "unsafe void Buffer::tear(&self) override { free(self->data); } ";
    auto valid = Analyze(buffer +
                         "unsafe []i32 Buffer::view(&self) <- self { return "
                         "self->data[:self->length]; }");
    for (const auto& diagnostic : valid.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;

    for (const std::string& source : {
             "unsafe void f(*i32 p) { values := p[:1]; values[0]; }",
             "unsafe void f(lend *i32 p) { values := p[:1]; values[0]; }",
             "void f(lend *i32 p) { values := p[:1]; values[0]; }",
             "unsafe void f() { i32 value = 0; p := &raw value; values := "
             "p[:1]; values[0]; }",
         }) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
}

TEST(SemanticMemoryTests, RuntimeArraySlicesProtectAllocationLifetime) {
    const std::string consume = "unsafe void consume(own *i32 p) { free(p); } ";
    for (const std::string& action : {"free(p);", "consume(p);"}) {
        SCOPED_TRACE(action);
        auto result =
            Analyze(consume +
                    "unsafe i32 f() { p, e := new<[]i32>(2); if e != none { "
                    "return 0; } values := p[:]; " +
                    action + " return values[0]; }");
        EXPECT_NE(FindDiagnostic(result, DiagnosticCode::BorrowConflict),
                  nullptr);
    }

    auto lastUse = Analyze(
        "unsafe i32 f() { p, e := new<[]i32>(2); if e != none { return 0; } "
        "values := p[:]; value := values[0]; free(p); return value; }");
    for (const auto& diagnostic : lastUse.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
}

TEST(SemanticMemoryTests, RuntimeArrayCountRunsBeforeAllocationFailure) {
    auto analysis = Analyze(
        "unsafe usize count(own *i32 p) { free(p); return 3; } "
        "unsafe void f() { old, e := new<i32>(1); "
        "p, failure := new<[]i32>(count(old)); free(p); }");
    EXPECT_FALSE(analysis.diagnostics.HasErrors());
}

TEST(SemanticMemoryTests, TakeAllowsExplicitFieldReplacementAndNullsTheSlot) {
    const std::string prefix =
        "structure Box { own *i32 p; }; "
        "unsafe void Box::tear(&self) override { free(self->p); } ";
    for (const std::string body :
         {"old := take(self->p); free(old); self->p = replacement;",
          "old := take(self->p); free(old); again := take(self->p); "
          "free(again); self->p = replacement;"}) {
        auto result =
            Analyze(prefix +
                    "unsafe void Box::replace(&self, own *i32 replacement) { " +
                    body + " }");
        for (const auto& diagnostic : result.diagnostics.Diagnostics())
            ADD_FAILURE() << diagnostic.message;
    }
    auto tear = Analyze(
        "structure Box { own *i32 p; }; unsafe void Box::tear(&self) override "
        "{ old := take(self->p); free(old); }");
    EXPECT_FALSE(tear.diagnostics.HasErrors());
}

TEST(SemanticMemoryTests, TakeRejectsLostOwnersAndAliasesAcrossReplacement) {
    const std::string prefix =
        "structure Box { own *i32 p; }; "
        "unsafe void Box::tear(&self) override { free(self->p); } ";
    for (const std::string body :
         {"self->p = replacement;", "take(self->p); self->p = replacement;",
          "alias := self->p; old := take(self->p); free(old); self->p = "
          "replacement; *alias = 1;",
          "old := take(self->p); self->p = old; free(old); free(replacement);",
          "defer use(self->p); old := take(self->p); free(old); self->p = "
          "replacement;",
          "defer take(self->p); free(replacement);"}) {
        auto result =
            Analyze(prefix +
                    "unsafe void use(lend *i32 p) {} "
                    "unsafe void Box::replace(&self, own *i32 replacement) { " +
                    body + " }");
        EXPECT_TRUE(result.diagnostics.HasErrors()) << body;
    }
    EXPECT_TRUE(
        Analyze(prefix +
                "unsafe void Box::bad(&const self) { free(take(self->p)); }")
            .diagnostics.HasErrors());
    EXPECT_TRUE(
        Analyze(prefix + "unsafe void bad(own *i32 p) { free(take(p)); }")
            .diagnostics.HasErrors());
}

TEST(SemanticMemoryTests, MutableOwnerCallsInvalidateFieldAliases) {
    const std::string prefix =
        "structure Box { own *i32 p; }; "
        "unsafe void Box::tear(&self) override { free(self->p); } "
        "unsafe void Box::reset(&self) { old := take(self->p); free(old); } ";
    EXPECT_TRUE(Analyze(prefix +
                        "unsafe void Box::bad(&self) { "
                        "alias := self->p; self->reset(); *alias = 1; }")
                    .diagnostics.HasErrors());
    EXPECT_TRUE(Analyze(prefix + "unsafe void use(lend *i32 p) {} "
                                 "unsafe void Box::bad(&self) { defer "
                                 "use(self->p); self->reset(); }")
                    .diagnostics.HasErrors());
}

TEST(SemanticMemoryTests, MutableOwnerCallsCannotReceiveTheirFieldAliases) {
    auto result = Analyze(
        "structure Box { own *i32 p; }; "
        "unsafe void Box::tear(&self) override { free(self->p); } "
        "unsafe void Box::change(&self, lend *i32 alias) { old := "
        "take(self->p); free(old); *alias = 1; } "
        "unsafe void Box::bad(&self) { self->change(self->p); }");
    EXPECT_TRUE(result.diagnostics.HasErrors());
}

TEST(SemanticGenericTests,
     ReusesConcreteFunctionsAndAllowsRecursionAndReferences) {
    auto result = Analyze(R"(
        T identity<T>(T value) { return value; }
        T recur<T>(T value, i32 count) {
            if count == 0 { return value; }
            return recur<T>(value, count - 1);
        }
        void change<T>(&T value) { *value = (42 as T); }
        T read<T>(&const T value) { return *value; }
        (A, B) pair<A, B>(A a, B b) { return (a, b); }
        i32 main() {
            value := identity<i32>(20);
            change<i32>(&value);
            a, b := pair<i32, f64>(recur<i32>(read<i32>(&value), 2), 1.0);
            return identity<i32>(a);
        }
    )");
    EXPECT_FALSE(result.diagnostics.HasErrors());
    EXPECT_EQ(result.unit->genericTemplates.size(), 5u);
    EXPECT_EQ(result.unit->Declarations().size(), 6u);
}

TEST(SemanticGenericTests, PreservesBorrowedResultContracts) {
    auto result = Analyze(R"(
        &const T identity_ref<T>(&const T value) <- value { return value; }
        i32 main() {
            i32 value = 42;
            result := identity_ref<i32>(&value);
            return *result;
        }
    )");
    EXPECT_FALSE(result.diagnostics.HasErrors());
}

TEST(SemanticGenericTests, StructuresMethodsNestedTypesAndFieldComparisons) {
    auto result = Analyze(R"(
        structure Box<T> { T value; };
        T Box<T>::get(&const self) { return self->value; }
        U Box<T>::convert<U>(&const self) { return self->value as U; }
        structure Nested<T> { Box<T> box; };
        i32 main() {
            Box<i32> a = Box<i32>::init();
            b := Box<f64>::init();
            nested := Nested<i32>::init();
            if a.value < 10 { return nested.box.get() + b.convert<i32>(); }
            return a.get();
        }
    )");
    for (const auto& d : result.diagnostics.Diagnostics())
        ADD_FAILURE() << d.message;
}

TEST(SemanticGenericTests,
     GenericMethodsOnlyInstantiateForTheirActualReceiver) {
    auto result = Analyze(R"(
        structure Box<T> { T value; };
        U Box<T>::convert<U>(&const self) { return self->value as U; }
        structure NonNumeric { i32 value; };
        Box<i32> make() { return Box<i32>::init(); }
        i32 main() {
            unused := Box<NonNumeric>::init();
            value := make();
            return value.convert<i32>() + (&value)->convert<i32>();
        }
    )");
    for (const auto& d : result.diagnostics.Diagnostics())
        ADD_FAILURE() << d.message;
}

TEST(SemanticGenericTests,
     RejectsDifferentArgumentsAndDeclarationsAsDifferentTypes) {
    for (const auto& source :
         {"structure Box<T> { T n; }; void main() { Box<i32> a = "
          "Box<f64>::init(); }",
          "structure Box<T> { T n; }; structure Other<T> { T n; }; void main() "
          "{ Box<i32> a = Other<i32>::init(); }"}) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
}

TEST(SemanticGenericTests,
     RequiresExplicitArgumentsAndRejectsInvalidTemplates) {
    for (const auto& source :
         {"T id<T>(T x) { return x; } void main() { id(1); }",
          "T id<T>(T x) { return x; } void main() { id<i32, f64>(1); }",
          "i32 id(i32 x) { return x; } void main() { id<i32>(1); }",
          "structure Box<T> { T value; }; void main() { Box value; }",
          "T id<T>(T T) { return T; }", "T id<T, T>(T x) { return x; }",
          "structure Box<T> { T value; }; void Box<U>::method(&self) {}",
          "structure Box<T> { T value; }; U Box<T>::method<T, U>(&self) { "
          "return none; }",
          "structure Box<T> { T value; }; void Box<T>::tear<U>(&self) override "
          "{}"}) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
}

TEST(SemanticGenericTests,
     ChecksOperationsAtInstantiationAndReportsCallOrigin) {
    auto result = Analyze(R"(
        T plus<T>(T value) { return value + value; }
        structure Box { i32 n; };
        void main() { plus<Box>(Box::init()); }
    )");
    EXPECT_TRUE(result.diagnostics.HasErrors());
    bool note = false;
    for (const auto& d : result.diagnostics.Diagnostics())
        note |= d.severity == DiagnosticSeverity::Note &&
                d.message.find("while instantiating") != std::string::npos;
    EXPECT_TRUE(note);
    EXPECT_FALSE(
        Analyze("T plus<T>(T value) { return value + value; } void main() {}")
            .diagnostics.HasErrors());
}

TEST(SemanticGenericTests, BoundsExpandingRecursion) {
    auto result =
        Analyze("void grow<T>() { grow<*T>(); } void main() { grow<i32>(); }");
    EXPECT_TRUE(result.diagnostics.HasErrors());
    bool budget = false;
    for (const auto& d : result.diagnostics.Diagnostics())
        budget |= d.message.find("budget") != std::string::npos;
    EXPECT_TRUE(budget);
}

TEST(SemanticGenericTests, PreservesReferenceStorageAndOwnershipRules) {
    EXPECT_TRUE(Analyze("structure Box<T> { T value; }; void main() { "
                        "Box<&const i32> b = Box<&const i32>::init(); }")
                    .diagnostics.HasErrors());
    EXPECT_TRUE(Analyze(R"(
        unsafe void consume<T>(own *T p) { free(p); }
        unsafe void main() {
            p, e := new<i32>(42);
            if e != none { return; }
            consume<i32>(p);
            free(p);
        }
    )")
                    .diagnostics.HasErrors());
    EXPECT_FALSE(Analyze(R"(
        unsafe void consume<T>(own *T p) { free(p); }
        unsafe void main() {
            p, e := new<i32>(42);
            if e != none { return; }
            consume<i32>(p);
        }
    )")
                     .diagnostics.HasErrors());
}

TEST(SemanticGenericTests, MethodsResolveHeapTupleArrayAndSliceReceivers) {
    auto result = Analyze(R"(
        structure Box<T> { T n; };
        U Box<T>::convert<U>(&const self) { return self->n as U; }
        (Box<T>, Box<T>) boxes<T>() { return (Box<T>::init(), Box<T>::init()); }
        unsafe i32 main() {
            a, b := boxes<i32>();
            array := [a, b];
            first := array[0].convert<i32>();
            slice := array[0:2];
            p, e := new<Box<i32>>(Box<i32>::init());
            if e != none { return 0; }
            defer free(p);
            return p->convert<i32>() + a.convert<i32>() + first + slice[0].convert<i32>();
        }
    )");
    for (const auto& d : result.diagnostics.Diagnostics())
        ADD_FAILURE() << d.message;
}

TEST(SemanticGenericTests, GenericLifecycleKeepsUnsafeTearRestrictions) {
    const std::string prefix = R"(
        structure Box<T> { T value; };
        unsafe void Box<T>::tear(&self) override {}
    )";
    EXPECT_TRUE(Analyze(prefix +
                        "unsafe void main() { a := Box<i32>::init(); b := a; }")
                    .diagnostics.HasErrors());
    EXPECT_TRUE(
        Analyze(prefix +
                "unsafe void main() { a := Box<i32>::init(); a.tear(); }")
            .diagnostics.HasErrors());
    EXPECT_FALSE(
        Analyze(prefix + "unsafe void main() { a := Box<i32>::init(); }")
            .diagnostics.HasErrors());
}

TEST(SemanticGenericTests, GenericSafeTearValuesMoveByValue) {
    auto result = Analyze(R"(
        structure Box<T> { T value; };
        void Box<T>::tear(&self) override {}
        Box<T> pass<T>(Box<T> value) { return value; }
        i32 main() {
            first := Box<i32>::init();
            second := pass<i32>(first);
            first = Box<i32>::init();
            return second.value + first.value;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
}

TEST(SemanticGenericTests, GenericMethodsResolveMixedTuplesAndBuildSelfFields) {
    auto result = Analyze(R"(
        structure Box<T> { T n; };
        U Box<T>::convert<U>(&const self) { return self->n as U; }
        structure Outer<T> { Box<T> box; i32 n; };
        Outer<T> Outer<T>::init() override {
            return build Outer<T> {
                self.box = Box<T>::init();
                self.n = self.box.convert<i32>();
            };
        }
        i32 main() {
            box, count := (Box<i32>::init(), 42);
            outer := Outer<i32>::init();
            return box.convert<i32>() + count + outer.n;
        }
    )");
    for (const auto& d : result.diagnostics.Diagnostics())
        ADD_FAILURE() << d.message;
}

TEST(SemanticGenericTests,
     SupportsRecursivePointerTypesAndRejectsMemberCollisions) {
    EXPECT_FALSE(Analyze(R"(
        structure Node<T> { T value; *Node<T> next; };
        void main() { node := Node<i32>::init(); }
    )")
                     .diagnostics.HasErrors());
    EXPECT_TRUE(Analyze(R"(
        structure Box { i32 value; };
        T Box::value<T>(&const self) { return none; }
        void main() {}
    )")
                    .diagnostics.HasErrors());
}

TEST(SemanticGenericTests, TypeArgumentsRespectLocalShadowing) {
    EXPECT_TRUE(Analyze(R"(
        structure Box { i32 n; };
        void use<T>() {}
        void main() { i32 Box = 0; use<Box>(); }
    )")
                    .diagnostics.HasErrors());
}

TEST(SemanticReferenceTests, AllowsDisjointStructureFieldBorrows) {
    auto result = Analyze(R"(
        structure Pair { i32 x; i32 y; };
        structure Nested { Pair pair; i32 other; };
        void set_both(&i32 x, &i32 y) { *x = 20; *y = 22; }
        i32 Pair::sum(&self) {
            x := &self->x;
            self->y = 22;
            *x = 20;
            return self->y + *x;
        }
        i32 check() {
            value := Nested::init();
            x := &value.pair.x;
            y := &value.pair.y;
            *x = 20;
            *y = 22;
            value.other = 1;
            set_both(x, y);
            return *x + *y + value.other;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
}

TEST(SemanticReferenceTests,
     FieldBorrowsRejectOverlappingAndWholeStructureAccess) {
    const std::string prefix = R"(
        structure Pair { i32 x; i32 y; };
        structure Outer { Pair pair; i32 other; };
        void touch(&Pair value) {}
        void inspect(&const Pair value) {}
        void use(&i32 a, &i32 b) {}
    )";
    for (const auto& body :
         {"r := &value.pair.x; value.pair.x = 2; return *r;",
          "r := &value.pair.x; copy := value.pair; return *r + copy.x;",
          "r := &value.pair.x; value.pair = Pair::init(); return *r;",
          "r := &value.pair.x; value = Outer::init(); return *r;",
          "r := &value.pair; value.pair.y = 2; return r->x;",
          "r := &value.pair.x; touch(&value.pair); return *r;",
          "r := &value.pair.x; inspect(&value.pair); return *r;",
          "use(&value.pair.x, &value.pair.x); return 0;",
          "parent := &value.pair; child := &parent->x; moved := parent; return "
          "*child;",
          "parent := &value.pair; child := &parent->x; parent = &value.pair; "
          "return *child;"}) {
        SCOPED_TRACE(body);
        auto result = Analyze(
            prefix + "i32 check() { value := Outer::init(); " + body + " }");
        EXPECT_NE(FindDiagnostic(result, DiagnosticCode::BorrowConflict),
                  nullptr);
    }
}

TEST(SemanticReferenceTests, FieldBorrowsTrackReborrowsJoinsAndLoops) {
    const std::string prefix = "structure Triple { i32 x; i32 y; i32 z; }; ";
    auto valid = Analyze(prefix + R"(
        i32 check(bool condition) {
            value := Triple::init();
            parent := &value;
            child := &parent->x;
            if condition { child = &parent->y; }
            while condition {
                parent->z = 42;
                *child = *child + 1;
                break;
            }
            parent->z = 42;
            return parent->z + *child;
        }
    )");
    for (const auto& diagnostic : valid.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    auto invalid = Analyze(prefix + R"(
        i32 check(bool condition) {
            value := Triple::init();
            child := &value.x;
            if condition { child = &value.y; }
            value.x = 42;
            return *child;
        }
    )");
    EXPECT_NE(FindDiagnostic(invalid, DiagnosticCode::BorrowConflict), nullptr);
}

TEST(SemanticReferenceTests, FieldBorrowsPreserveWholeArrayConflicts) {
    const std::string prefix =
        "structure Pair { i32 x; i32 y; }; structure Buffer { [2]Pair values; "
        "usize length; }; ";
    auto valid = Analyze(prefix + R"(
        i32 check() {
            buffer := Buffer::init();
            element := &buffer.values[0].x;
            buffer.length = 2;
            return *element + (buffer.length as i32);
        }
    )");
    EXPECT_FALSE(valid.diagnostics.HasErrors());
    for (const auto& access :
         {"buffer.values[1].x = 2;", "buffer.values[1].y = 2;",
          "buffer.values = [Pair::init(), Pair::init()];"}) {
        auto invalid =
            Analyze(prefix +
                    "i32 check() { buffer := Buffer::init(); element "
                    ":= &buffer.values[0].x; " +
                    access + " return *element; }");
        EXPECT_NE(FindDiagnostic(invalid, DiagnosticCode::BorrowConflict),
                  nullptr);
    }
}

TEST(SemanticReferenceTests, DeferredFieldBorrowsProtectOnlyTheirField) {
    const std::string prefix =
        "structure Pair { i32 x; i32 y; }; void touch(&i32 n) { *n = 42; } ";
    EXPECT_FALSE(Analyze(prefix + "void check() { value := Pair::init(); defer "
                                  "touch(&value.x); value.y = 22; }")
                     .diagnostics.HasErrors());
    auto invalid = Analyze(prefix +
                           "void check() { value := Pair::init(); defer "
                           "touch(&value.x); value.x = 22; }");
    EXPECT_NE(FindDiagnostic(invalid, DiagnosticCode::BorrowConflict), nullptr);
}

TEST(SemanticReferenceTests, DerefMemberSharesDereferenceLvalueAndBorrowRules) {
    auto valid = Analyze(R"(
        structure Pair { i32 x; i32 y; };
        i32 check() {
            pair := Pair::init();
            parent := &pair;
            x := &parent->deref().x;
            x->deref() = 20;
            x->deref()++;
            parent->y = 21;
            child := &x->deref();
            return child->deref() + parent->y;
        }
        T read<T>(&const T r) { return r->deref(); }
        i32 generic() { i32 x = 42; return read<i32>(&x); }
    )");
    for (const auto& diagnostic : valid.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    for (const auto& source :
         {"void check() { i32 x = 1; &const i32 r = &x; r->deref() = 2; }",
          "i32 check() { i32 x = 1; r := &x; child := &r->deref(); r->deref() "
          "= "
          "2; return child->deref(); }",
          "i32 check() { i32 x = 1; r := &x; moved := r; return r->deref(); }",
          "unsafe i32 check(*i32 p) { return p->deref(); }",
          "i32 check() { i32 x = 1; return x->deref(); }",
          "i32 check() { i32 x = 1; r := &x; return r->deref(1); }"}) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(Analyze(source).diagnostics.HasErrors());
    }
}
