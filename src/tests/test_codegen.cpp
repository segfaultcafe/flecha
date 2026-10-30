#include <gtest/gtest.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "codegen/llvm_code_generator.hpp"
#include "codegen/native_compiler.hpp"
#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "lowering/lowerer.hpp"
#include "semantic/semantic_analyzer.hpp"

using namespace flecha::codegen;
using namespace flecha::core;
using namespace flecha::diagnostics;
using namespace flecha::lowering;
using namespace flecha::semantic;

namespace {

void SetEnvironment(const std::string& name,
                    const std::optional<std::string>& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.value_or("").c_str());
#else
    if (value.has_value())
        setenv(name.c_str(), value->c_str(), 1);
    else
        unsetenv(name.c_str());
#endif
}

class ScopedEnvironment final {
   public:
    ScopedEnvironment(std::string name, std::optional<std::string> value)
        : _name(std::move(name)) {
        if (const char* previous = std::getenv(_name.c_str()))
            _previous = previous;
        SetEnvironment(_name, value);
    }

    ~ScopedEnvironment() { SetEnvironment(_name, _previous); }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

   private:
    std::string _name;
    std::optional<std::string> _previous;
};

struct GeneratedSource final {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit;
    SemanticModel semanticModel;
    std::unique_ptr<BoundProgram> boundProgram;
    std::unique_ptr<LlvmCodeGenerator> generator;
    std::unique_ptr<llvm::Module> module;
};

GeneratedSource Generate(const std::string& source) {
    GeneratedSource result;
    Tokenizer tokenizer(source, result.diagnostics);
    std::vector<Token> tokens = tokenizer.Tokenize();
    Parser parser(std::move(tokens), result.diagnostics);
    result.unit = parser.Parse();
    SemanticAnalyzer analyzer(result.diagnostics);
    result.semanticModel = analyzer.Analyze(*result.unit);
    Lowerer lowerer;
    result.boundProgram =
        lowerer.Lower(*result.unit, result.semanticModel, result.diagnostics);
    if (result.boundProgram != nullptr) {
        result.generator = std::make_unique<LlvmCodeGenerator>();
        result.module =
            result.generator->Generate(*result.boundProgram, "test_module");
    }
    return result;
}

std::string ModuleText(const llvm::Module& module) {
    std::string text;
    llvm::raw_string_ostream output(text);
    module.print(output, nullptr);
    output.flush();
    return text;
}

}  // namespace

TEST(LlvmCodeGeneratorTests, EmitsVerifiedFunctionsLocalsCallsAndReturns) {
    GeneratedSource result = Generate(
        "i32 add(i32 left, i32 right) { return left + right; } "
        "i32 main() { x := 20; y := 22; return add(x, y); }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("define i32 @add(i32 %left, i32 %right)"),
              std::string::npos);
    EXPECT_NE(ir.find("%x = alloca i32"), std::string::npos);
    EXPECT_NE(ir.find("call i32 @add"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, ForwardsHostArgumentsBeforeCallingFlechaMain) {
    GeneratedSource result = Generate("i32 main() { return 42; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());

    const llvm::Function* entry = result.module->getFunction("main");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->arg_size(), 2u);
    EXPECT_NE(result.module->getFunction("$flecha$main"), nullptr);

    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(
        ir.find("call void @flecha_runtime_v1_process_arguments_initialize"),
        std::string::npos);
    EXPECT_NE(ir.find("call i32 @\"$flecha$main\"()"), std::string::npos);
}

TEST(NativeCompilerTests, CapturesForwardedProcessArguments) {
    GeneratedSource result = Generate(R"(
        quiver extern unsafe usize
            flecha_runtime_v1_process_argument_count();
        quiver extern unsafe i32
            flecha_runtime_v1_process_argument_capture(
                *string output,
                usize index
            );

        i32 main() {
            unsafe {
                if flecha_runtime_v1_process_argument_count() != 3 {
                    return 1;
                }

                string first = none;
                string second = none;
                if flecha_runtime_v1_process_argument_capture(
                    &raw first,
                    1
                ) != 0 or flecha_runtime_v1_process_argument_capture(
                    &raw second,
                    2
                ) != 0 {
                    return 2;
                }
                if first != "alpha" or second != "two words" {
                    return 3;
                }

                string missing = none;
                if flecha_runtime_v1_process_argument_capture(
                    &raw missing,
                    3
                ) != 2 or missing.len() != 0 {
                    return 4;
                }
            }
            return 42;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() / "flecha_process_argument_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(
        *result.boundProgram, "process_argument_test", executablePath.string()))
        << compiler.Error();

    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText, "alpha",
                                         "two words"};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, InlinesModuleConstantExpressions) {
    GeneratedSource result = Generate(R"(
        const i32 BASE = 20;
        const i32 ANSWER = BASE * 2 + 2;
        i32 main() { return ANSWER; }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_module_constant_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(
        *result.boundProgram, "module_constant_test", executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(LlvmCodeGeneratorTests, EmitsUniformCallableValuesAndIndirectCalls) {
    GeneratedSource result = Generate(R"(
        func Mapper(i32) => (i32);
        i32 twice(i32 value) { return value * 2; }
        i32 apply(Mapper callback, i32 value) { return callback(value); }
        i32 main() { Mapper callback = twice; return apply(callback, 21); }
    )");

    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("{ ptr, ptr, ptr }"), std::string::npos);
    EXPECT_NE(ir.find("call i32 %function.invoke(ptr %function.environment"),
              std::string::npos);
    EXPECT_NE(ir.find("twice.$callable"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsGenericCallableSpecializationsWithSameAbi) {
    GeneratedSource result = Generate(R"(
        func Mapper<T>(T) => (T);
        i32 twice(i32 value) { return value * 2; }
        i32 apply(Mapper<i32> callback, i32 value) {
            return callback(value);
        }
        i32 main() {
            Mapper<i32> callback = twice;
            return apply(callback, 21);
        }
    )");

    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("{ ptr, ptr, ptr }"), std::string::npos);
    EXPECT_NE(ir.find("call i32 %function.invoke(ptr %function.environment"),
              std::string::npos);
    EXPECT_NE(ir.find("twice.$callable"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsHeapBackedCopyCaptureEnvironments) {
    GeneratedSource result = Generate(R"(
        func Mapper(i32) => (i32);
        i32 main() {
            i32 factor = 2;
            Mapper callback = [copy factor](value) => {
                return value * factor;
            };
            return callback(21);
        }
    )");

    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("flecha.closure.env."), std::string::npos);
    EXPECT_NE(ir.find("flecha_runtime_v1_alloc"), std::string::npos);
    EXPECT_NE(ir.find("$closure.destroy"), std::string::npos);
    EXPECT_NE(ir.find("flecha_runtime_v1_free"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsOwnedRawResourceCaptureCleanup) {
    GeneratedSource result = Generate(R"(
        unsafe func Reader() => (i32);
        unsafe i32 main() {
            pointer, failure := new<i32>(42);
            if failure != none { return 1; }
            Reader reader = unsafe [own pointer]() => { return *pointer; };
            return reader();
        }
    )");

    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("owned.capture"), std::string::npos);
    EXPECT_NE(ir.find("$closure.destroy"), std::string::npos);
    EXPECT_NE(ir.find("free.release"), std::string::npos);
    EXPECT_NE(ir.find("flecha_runtime_v1_free"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsForwardAndRecursiveCalls) {
    GeneratedSource result = Generate(
        "i32 main() { return Add(Factorial(3), 36); } "
        "i32 Add(i32 left, i32 right) { return left + right; } "
        "i32 Factorial(i32 value) { if value <= 1 { return 1; } "
        "return value * Factorial(value - 1); }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("call i32 @Add"), std::string::npos);
    EXPECT_NE(ir.find("call i32 @Factorial"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsEnumDiscriminantsDefaultsAndMethods) {
    GeneratedSource result = Generate(
        "enum Status: u8 { default Idle = 3, Running = 7, Done, } "
        "bool Status::is_running(&const self) { "
        "return *self == Status.Running; } "
        "Status Status::done() { return Status.Done; } "
        "i32 main() { Status status = none; [2]Status values = none; "
        "if status != none { return 2; } status = Status.Running; "
        "if status.is_running() and "
        "values[0] == Status.Idle and Status::done() == Status.Done { "
        "return 0; } return 1; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("define i1 @\"Status::is_running\"(ptr"),
              std::string::npos);
    EXPECT_NE(ir.find("define i8 @\"Status::done\"()"), std::string::npos);
    EXPECT_NE(ir.find("store i8 3"), std::string::npos);
    EXPECT_NE(ir.find("store i8 7"), std::string::npos);
    EXPECT_NE(ir.find("ret i8 8"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsVerifiedEnumMatchSwitches) {
    GeneratedSource result = Generate(R"(
        enum TokenKind: u8 { Identifier = 2, Number = 7, End = 9, }
        i32 classify(TokenKind kind) {
            return match kind {
                TokenKind.Identifier => 10,
                TokenKind.Number => 20,
                TokenKind.End => 42,
            };
        }
        i32 main() { return classify(TokenKind.End); }
    )");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("switch i8"), std::string::npos);
    EXPECT_NE(ir.find("i8 2, label %match.arm"), std::string::npos);
    EXPECT_NE(ir.find("i8 7, label %match.arm"), std::string::npos);
    EXPECT_NE(ir.find("i8 9, label %match.arm"), std::string::npos);
    EXPECT_NE(ir.find("match.invalid"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EvaluatesMatchInputsOnceAndUsesWildcardDefault) {
    GeneratedSource result = Generate(R"(
        enum TokenKind: u8 { Identifier, End, }
        TokenKind next() { return TokenKind.End; }
        i32 main() {
            return match next() {
                TokenKind.Identifier => 1,
                _ => 42,
            };
        }
    )");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    const std::size_t call = ir.find("call i8 @next()");
    ASSERT_NE(call, std::string::npos);
    EXPECT_EQ(ir.find("call i8 @next()", call + 1), std::string::npos);
    EXPECT_EQ(ir.find("match.invalid"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, DefaultConstructsRuntimeEnumArrays) {
    GeneratedSource result = Generate(
        "enum Status: u8 { default Idle = 3, Running = 7, } "
        "unsafe i32 main() { values, failure := new<[]Status>(2); "
        "if failure != none { return 1; } i32 answer = 0; "
        "if values[0] != Status.Idle or values[1] != Status.Idle { "
        "answer = 2; } free(values); return answer; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("allocation.zero.condition"), std::string::npos);
    EXPECT_NE(ir.find("store i8 3"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, CarriesRuntimeArrayLengthInTheOwningValue) {
    GeneratedSource result = Generate(
        "unsafe i32 main() { values, failure := new<[]i32>(4); "
        "if failure != none { return 1; } owner := values; defer free(owner); "
        "owner[3] = 42; return owner[3]; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("allocation.owner"), std::string::npos);
    EXPECT_NE(ir.find("moved.value"), std::string::npos);
    EXPECT_NE(ir.find("free.data"), std::string::npos);
    EXPECT_EQ(ir.find("flecha_runtime_v1_allocation_count"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, AutomaticallyDestroysSafeOwningSlices) {
    GeneratedSource result = Generate(R"(
        own []i32 make() {
            values, failure := new<own []i32>(2);
            return values;
        }
        i32 main() {
            values := make();
            values[0] = 20;
            values[1] = 22;
            return values[0] + values[1];
        }
    )");

    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("allocation.owner"), std::string::npos);
    EXPECT_NE(ir.find("call void @flecha_runtime_v1_free"), std::string::npos);
    EXPECT_EQ(ir.find("flecha_runtime_v1_allocation_count"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, InitializesAndDestroysOwningStringSlices) {
    GeneratedSource result = Generate(R"(
        own []string make() {
            values, failure := new<own []string>(3);
            if failure != none { return none; }
            values[0] = "first";
            values[1] = "second";
            return values;
        }
        i32 main() {
            values := make();
            moved := values;
            moved[0] = "replaced";
            if moved[0] != "replaced" or moved[1] != "second" or
                moved[2].len() != 0 {
                return 1;
            }
            return 42;
        }
    )");

    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("elements.check"), std::string::npos);
    EXPECT_NE(ir.find("elements.body"), std::string::npos);
    EXPECT_NE(ir.find("call void @flecha_runtime_v1_string_destroy"),
              std::string::npos);
}

TEST(NativeCompilerTests, RunsOwningStringSlicesAcrossMovesAndFailures) {
    GeneratedSource result = Generate(R"(
        own []string make() {
            values, failure := new<own []string>(3);
            if failure != none { return none; }
            values[0] = "first";
            values[1] = "second";
            return values;
        }
        i32 main() {
            empty, emptyError := new<own []string>(0);
            if emptyError != none or empty.len() != 0 { return 1; }

            impossible, allocationError :=
                new<own []string>(usize.max_value);
            if allocationError == none { return 2; }

            values := make();
            moved := values;
            moved[0] = "replaced";
            moved[2] = moved[1];
            if moved[0] != "replaced" or moved[1] != "second" or
                moved[2] != "second" {
                return 3;
            }

            unsafe {
                raw, rawError := new<[]string>(2);
                if rawError != none { return 4; }
                raw[0] = "explicitly freed";
                raw[1] = raw[0];
                free(raw);
            }
            return 42;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_owning_string_slice_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "owning_string_slice_test",
                                         executablePath.string()))
        << compiler.Error();

    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsManagedSlicesStoredAsStructureBorrows) {
    GeneratedSource result = Generate(R"(
        structure Buffer { own []i32 values; };
        Buffer Buffer::init(own []i32 values) override {
            return build Buffer { self.values = values; };
        }
        structure Reader { []const i32 values; };
        Reader Reader::init([]const i32 values) <- values override {
            return build Reader { self.values = values; };
        }
        i32 main() {
            values, failure := new<own []i32>(2);
            if failure != none { return 1; }
            values[1] = 42;
            buffer := Buffer::init(values);
            reader := Reader::init(buffer.values[:]);
            return reader.values[1];
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_managed_slice_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_managed_slice_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsGenericAndMovableDefaultArguments) {
    GeneratedSource result = Generate(R"(
        T add<T: integer>(T value, T amount = 2) {
            return value + amount;
        }
        i32 score(string value = "john") {
            return add<i32>(40);
        }
        i32 main() { return score(); }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_default_arguments_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_default_arguments_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsFieldsAndMethodsWithTheSameName) {
    GeneratedSource result = Generate(R"(
        structure Cursor { usize position; };
        usize Cursor::position(&const self) { return self->position; }
        usize Cursor::throughReference(&const self) {
            return self->position();
        }
        i32 main() {
            cursor := Cursor::init();
            cursor.position = 14;
            direct := cursor.position();
            reference := &cursor;
            return (direct + reference->position() +
                reference->throughReference()).cast<i32>;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_field_method_name_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_field_method_name_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(LlvmCodeGeneratorTests, ExtractsCheckedSliceDataPointers) {
    GeneratedSource result = Generate(R"(
        unsafe i32 first([]const i32 values) { return *values.data(); }
        unsafe i32 main() {
            values := [42, 7];
            slice := values[:];
            *i32 data = slice.data();
            *data = first(slice);
            return values[0];
        }
    )");

    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("extractvalue { ptr,"), std::string::npos);
    EXPECT_NE(ir.find("slice.data"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, TransfersFreshStringConcatenationResults) {
    GeneratedSource result =
        Generate("void f() { string text = \"\"; text = text + \"x\"; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("flecha_runtime_v1_string_concat"), std::string::npos);
    EXPECT_EQ(ir.find("flecha_runtime_v1_string_clone"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsExternCallsWithoutFunctionBodies) {
    GeneratedSource result = Generate(
        "extern unsafe i32 native_add(i32 left, i32 right); "
        "i32 add(i32 left, i32 right) { unsafe { return "
        "native_add(left, right); } }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("declare i32 @native_add(i32, i32)"), std::string::npos);
    EXPECT_EQ(ir.find("define i32 @native_add"), std::string::npos);
    EXPECT_NE(ir.find("call i32 @native_add"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, UsesTargetCAbiIntegerExtensionAttributes) {
    GeneratedSource result = Generate(
        "extern i8 native_i8(i8 value); "
        "extern u16 native_u16(u16 value); "
        "i32 use(i8 signed_value, u16 unsigned_value) { "
        "native_i8(signed_value); native_u16(unsigned_value); return 0; }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    LlvmCodeGenerator x86Generator;
    x86Generator.ConfigureTarget("x86_64-unknown-linux-gnu",
                                 llvm::DataLayout("e-p:64:64"));
    std::unique_ptr<llvm::Module> x86Module =
        x86Generator.Generate(*result.boundProgram, "extern_x86_abi");
    ASSERT_NE(x86Module, nullptr) << x86Generator.Error();
    const std::string x86Ir = ModuleText(*x86Module);
    EXPECT_NE(x86Ir.find("declare signext i8 @native_i8(i8 signext)"),
              std::string::npos);
    EXPECT_NE(x86Ir.find("declare zeroext i16 @native_u16(i16 zeroext)"),
              std::string::npos);
    EXPECT_NE(x86Ir.find("call signext i8 @native_i8(i8 signext"),
              std::string::npos);
    EXPECT_NE(x86Ir.find("call zeroext i16 @native_u16(i16 zeroext"),
              std::string::npos);

    LlvmCodeGenerator aarch64Generator;
    aarch64Generator.ConfigureTarget("aarch64-unknown-linux-gnu",
                                     llvm::DataLayout("e-p:64:64"));
    std::unique_ptr<llvm::Module> aarch64Module =
        aarch64Generator.Generate(*result.boundProgram, "extern_aarch64_abi");
    ASSERT_NE(aarch64Module, nullptr) << aarch64Generator.Error();
    const std::string aarch64Ir = ModuleText(*aarch64Module);
    EXPECT_NE(aarch64Ir.find("declare i8 @native_i8(i8)"), std::string::npos);
    EXPECT_NE(aarch64Ir.find("declare i16 @native_u16(i16)"),
              std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsAssignmentsAndConditionalControlFlow) {
    GeneratedSource result = Generate(
        "i32 main() { value := 1; if value < 2 { value = value + 41; } "
        "else { value = 0; } return value; }");

    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("br i1"), std::string::npos);
    EXPECT_NE(ir.find("if.then"), std::string::npos);
    EXPECT_NE(ir.find("if.else"), std::string::npos);
    EXPECT_NE(ir.find("store i32"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsWhileAndConditionalAlsoControlFlow) {
    GeneratedSource result = Generate(
        "i32 main() { value := 0; counter := 0; while counter < 4 { "
        "value = value + 10; counter = counter + 1; } "
        "also counter % 2 == 0 { value = value + 1; } return value; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("while.condition"), std::string::npos);
    EXPECT_NE(ir.find("while.body"), std::string::npos);
    EXPECT_NE(ir.find("while.end"), std::string::npos);
    EXPECT_NE(ir.find("if.then"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsLoopControlAndCrossNestedJumps) {
    GeneratedSource result = Generate(
        "i32 main() { value := 0; warmup := 0; while warmup < 1 { "
        "warmup = warmup + 1; proceed; } while true { if value == 0 { "
        "jumpto inner_position; } #outer_position break; while true { "
        "#inner_position value = 42; jumpto outer_position; } } "
        "return value; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("position.outer_position"), std::string::npos);
    EXPECT_NE(ir.find("position.inner_position"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsChainedNumericConversions) {
    GeneratedSource result = Generate(
        "i32 main() { base := 42; wide := base.cast<i64>; "
        "narrow := wide.cast<u8>; "
        "decimal := narrow.cast<f64>; result := (decimal as f32) as i32; "
        "return result; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("sext i32"), std::string::npos);
    EXPECT_NE(ir.find("trunc i64"), std::string::npos);
    EXPECT_NE(ir.find("uitofp i8"), std::string::npos);
    EXPECT_NE(ir.find("fptrunc double"), std::string::npos);
    EXPECT_NE(ir.find("fptosi float"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests,
     EmitsByteCharacterCastsAsRepresentationPreserving) {
    GeneratedSource result = Generate(
        "i32 main() { u8 byte = 65; character := byte.cast<char>; "
        "roundTrip := character.cast<u8>; return roundTrip as i32; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    EXPECT_TRUE(result.generator->Error().empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("store i8 65"), std::string::npos);
    EXPECT_EQ(ir.find("unsupported LLVM cast"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsShortCircuitBooleanLogic) {
    GeneratedSource result =
        Generate("bool both(bool left, bool right) { return left and right; }");

    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("logic.right"), std::string::npos);
    EXPECT_NE(ir.find("phi i1"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsStringViewsAndContentEquality) {
    GeneratedSource result = Generate(
        "stringv Identity(stringv value) <- value { return value; } "
        "bool Equal(stringv left, stringv right) { return left == right; } "
        "i32 main() { value := Identity(\"Flecha\"); "
        "if Equal(value, \"Flecha\") and value != \"Arrow\" { return 42; } "
        "return 0; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("@memcmp"), std::string::npos);
    EXPECT_NE(ir.find("stringv.content"), std::string::npos);
    EXPECT_NE(ir.find("{ ptr, i64 }"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsBorrowedByteSlicesAsStringViews) {
    GeneratedSource result = Generate(
        "stringv view([]const u8 bytes) <-bytes { "
        "return bytes.cast<stringv>; } "
        "i32 main() { [3]u8 bytes = ['f', 'l', 'e']; "
        "text := view(bytes[:]); if text[1] == 'l' { return 42; } "
        "return 0; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("slice.stringv"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsCheckedStringViewOperations) {
    GeneratedSource result = Generate(
        "i32 main() { text := \"flecha\"; first := text.at(0); "
        "last := text[5]; part := text.substr(1, 3); length := text.len(); "
        "return 0; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("@flecha_runtime_v1_string_view_at"), std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_string_view_check_substr"),
              std::string::npos);
    EXPECT_NE(ir.find("getelementptr inbounds i8"), std::string::npos);
    EXPECT_NE(ir.find("stringv.len"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsOwnedStringConversionsAndCleanup) {
    GeneratedSource result = Generate(
        "i32 main() { string greeting = \"hi mom\".string(); "
        "string copy = greeting; string answer = 42.string(); "
        "stringv view = greeting.view(); "
        "stdoutln(greeting); return 42; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("@flecha_runtime_v1_string_from_view"),
              std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_string_from_i32"), std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_string_destroy"), std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_stdout_string_view"),
              std::string::npos);
}

TEST(LlvmCodeGeneratorTests, CoercesCharactersAndExternBytePointers) {
    GeneratedSource result = Generate(
        "extern unsafe void native_write(*const u8 data, usize length); "
        "void write(stringv text) { unsafe { native_write(text.data(), "
        "text.len().cast<usize>); } } "
        "u8 first() { [2]u8 bytes = ['h', 'i']; return bytes[0]; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("call void @native_write(ptr"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsLengthAwareStandardOutputAndError) {
    GeneratedSource result = Generate(
        "i32 main() { stdout(); stdout(\"Flecha\", -42, true); "
        "stdoutln(); stdoutln(\"Language\"); stdout(1 as i8); "
        "stdout(2 as i16); stdout(3 as i64); stdout(4 as isize); "
        "stdout(5 as u8); stdout(6 as u16); stdout(7 as u32); "
        "stdout(8 as u64); stdout(9 as usize); stdout(1 as f32); "
        "stdout(1.5); stdout(true); stdout('x'); stderr(); "
        "stderr(\"Flecha\", -42, true); stderrln(); "
        "stderrln(\"Language\"); stderr(1 as i8); stderr(2 as i16); "
        "stderr(3 as i64); stderr(4 as isize); stderr(5 as u8); "
        "stderr(6 as u16); stderr(7 as u32); stderr(8 as u64); "
        "stderr(9 as usize); stderr(1 as f32); stderr(1.5); "
        "stderr(true); stderr('x'); return 42; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    const std::string suffixes[] = {"string_view", "i8",    "i16",   "i32",
                                    "i64",         "isize", "u8",    "u16",
                                    "u32",         "u64",   "usize", "f32",
                                    "f64",         "bool",  "char",  "newline"};
    for (const std::string& suffix : suffixes) {
        EXPECT_NE(ir.find("@flecha_runtime_v1_stdout_" + suffix),
                  std::string::npos)
            << suffix;
        EXPECT_NE(ir.find("@flecha_runtime_v1_stderr_" + suffix),
                  std::string::npos)
            << suffix;
    }
    EXPECT_EQ(ir.find("@write"), std::string::npos);
    EXPECT_EQ(ir.find("@snprintf"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsFormattedStandardOutputAndError) {
    GeneratedSource result = Generate(
        "void output_byte(u8 value) { stdoutf(\"{}\", value); } "
        "void deferred(i32 value) { defer stderrf(\"value: {}\", value); } "
        "i32 main() { stdoutf(\"expected: {}, text: {}{}, ok: {}/{}, "
        "braces: {{}}\", 42, \"flecha\", '!', true, 1 as u8); return 0; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("@flecha_runtime_v1_stdout_i32"), std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_stdout_string_view"),
              std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_stdout_char"), std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_stdout_bool"), std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_stdout_u8"), std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_stderr_i32"), std::string::npos);
    EXPECT_NE(ir.find("braces: {}"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsFallibleFormattedStringConstruction) {
    GeneratedSource result = Generate(R"(
        (string, error) sprintf(stringv format, values...) {
            return __sprintf(format, values...);
        }
        i32 main() {
            text, err := sprintf(
                "{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{{}}",
                1 as i8, 2 as i16, 3, 4 as i64, 5 as isize,
                6 as u8, 7 as u16, 8 as u32, 9 as u64, 10 as usize,
                1.25 as f32, 1.5, true, 'x', "text"
            );
            if err != none { return 1; }
            stdout(text);
            return 0;
        }
    )");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    for (const std::string& suffix :
         {"i8", "i16", "i32", "i64", "isize", "u8", "u16", "u32", "u64",
          "usize", "f32", "f64", "bool", "char", "string_view"}) {
        EXPECT_NE(ir.find("@flecha_runtime_v1_format_buffer_append_" + suffix),
                  std::string::npos)
            << suffix;
    }
    EXPECT_NE(ir.find("@flecha_runtime_v1_format_buffer_finish"),
              std::string::npos);
    EXPECT_NE(ir.find(".flecha.error.AllocationError"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsTypedLineInputAndPreservesErrorFamilies) {
    GeneratedSource result = Generate(R"(
        i32 main() {
            i8 a; i16 b; i32 c; i64 d; isize e;
            u8 f; u16 g; u32 h; u64 i; usize j;
            f32 k; f64 l; bool m; char n; string text;
            error err = stdin("number: ", &c);
            if err != none { return 1; }
            stdin(&a); stdin(&b); stdin(&d); stdin(&e);
            stdin(&f); stdin(&g); stdin(&h); stdin(&i); stdin(&j);
            stdin(&k); stdin(&l); stdin(&m); stdin(&n);
            stdinln("text", &text);
            return c;
        }
    )");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("@flecha_runtime_v1_standard_output_flush"),
              std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_standard_input_read_line"),
              std::string::npos);
    for (const std::string& suffix :
         {"i8", "i16", "i32", "i64", "isize", "u8", "u16", "u32", "u64",
          "usize", "f32", "f64", "bool", "char"}) {
        EXPECT_NE(ir.find("@flecha_runtime_v1_input_parse_" + suffix),
                  std::string::npos)
            << suffix;
    }
    EXPECT_NE(ir.find(".flecha.error.InputError"), std::string::npos);
    EXPECT_NE(ir.find(".flecha.error.AllocationError"), std::string::npos);
}

TEST(NativeCompilerTests, RunsVariadicAndFormattedStandardOutput) {
    GeneratedSource result = Generate(R"(
        void print(values...) { stdout(values...); }
        void println(values...) { stdoutln(values...); }
        void printf(stringv format, values...) {
            stdoutf(format, values...);
        }
        void eprint(values...) { stderr(values...); }
        void eprintln(values...) { stderrln(values...); }
        void eprintf(stringv format, values...) {
            stderrf(format, values...);
        }
        (string, error) sprintf(stringv format, values...) {
            return __sprintf(format, values...);
        }
        i32 main() {
            print("answer: ", 42);
            println('!');
            printf("expected: {}, text: {}{}, ok: {}/{}, braces: {{}}",
                   -7, "flecha", '.', true, 0 as u8);
            eprint("error: ");
            eprintln(7);
            eprintf("detail: {} {{}}", false);
            text, err := sprintf("|string: {} {} {{}}", -9, "ok");
            if err != none { return 2; }
            print(text);
            return 0;
        }
    )");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path temporaryDirectory =
        std::filesystem::temp_directory_path();
    const std::filesystem::path executablePath =
        temporaryDirectory / "flecha_formatted_stdout_test";
    const std::filesystem::path outputPath =
        temporaryDirectory / "flecha_formatted_stdout_test.txt";
    const std::filesystem::path errorPath =
        temporaryDirectory / "flecha_formatted_stderr_test.txt";
    std::filesystem::remove(executablePath);
    std::filesystem::remove(outputPath);
    std::filesystem::remove(errorPath);

    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(
        *result.boundProgram, "formatted_stdout_test", executablePath.string()))
        << compiler.Error();

    const std::string executablePathText = executablePath.string();
    const std::string outputPathText = outputPath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    const std::string errorPathText = errorPath.string();
    const std::array<std::optional<llvm::StringRef>, 3> redirects = {
        std::nullopt, llvm::StringRef(outputPathText),
        llvm::StringRef(errorPathText)};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments,
                                        std::nullopt, redirects),
              0);

    std::ifstream output(outputPath, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(output)),
                           std::istreambuf_iterator<char>());
    EXPECT_EQ(text,
              "answer: 42!\n"
              "expected: -7, text: flecha., ok: true/0, braces: {}"
              "|string: -9 ok {}");

    std::ifstream errorOutput(errorPath, std::ios::binary);
    const std::string errorText((std::istreambuf_iterator<char>(errorOutput)),
                                std::istreambuf_iterator<char>());
    EXPECT_EQ(errorText, "error: 7\ndetail: false {}");

    std::filesystem::remove(executablePath);
    std::filesystem::remove(outputPath);
    std::filesystem::remove(errorPath);
}

TEST(NativeCompilerTests, ReadsAndConvertsTypedStandardInputLines) {
    GeneratedSource result = Generate(R"(
        i32 main() {
            i32 number;
            if stdin("number: ", &number) != none or number != 42 {
                return 1;
            }
            bool flag;
            char letter;
            string text = none;
            f64 decimal;
            if stdin(&flag) != none or stdin(&letter) != none or
                stdinln("text", &text) != none or stdin(&decimal) != none {
                return 2;
            }
            i32 preserved = 99;
            if stdin(&preserved) != InputError.InvalidSyntax or
                preserved != 99 {
                return 3;
            }
            if stdin(&preserved) != InputError.EndOfFile or preserved != 99 {
                return 4;
            }
            stdout(number, "|", flag, "|", letter, "|", text, "|",
                   decimal, "|", preserved);
            return 0;
        }
    )");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path temporaryDirectory =
        std::filesystem::temp_directory_path();
    const std::filesystem::path executablePath =
        temporaryDirectory / "flecha_standard_input_test";
    const std::filesystem::path inputPath =
        temporaryDirectory / "flecha_standard_input_test.txt";
    const std::filesystem::path outputPath =
        temporaryDirectory / "flecha_standard_input_output.txt";
    const std::filesystem::path errorPath =
        temporaryDirectory / "flecha_standard_input_error.txt";
    std::filesystem::remove(executablePath);
    std::filesystem::remove(inputPath);
    std::filesystem::remove(outputPath);
    std::filesystem::remove(errorPath);

    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(
        *result.boundProgram, "standard_input_test", executablePath.string()))
        << compiler.Error();

    {
        std::ofstream input(inputPath, std::ios::binary);
        input << "42\ntrue\nx\nhello world\r\n1.25\nnope\n";
    }
    const std::string executablePathText = executablePath.string();
    const std::string inputPathText = inputPath.string();
    const std::string outputPathText = outputPath.string();
    const std::string errorPathText = errorPath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    const std::array<std::optional<llvm::StringRef>, 3> redirects = {
        llvm::StringRef(inputPathText), llvm::StringRef(outputPathText),
        llvm::StringRef(errorPathText)};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments,
                                        std::nullopt, redirects),
              0);

    std::ifstream output(outputPath, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(output)),
                           std::istreambuf_iterator<char>());
    EXPECT_EQ(text, "number: text\n42|true|x|hello world|1.25|99");
    std::ifstream errors(errorPath, std::ios::binary);
    const std::string errorText((std::istreambuf_iterator<char>(errors)),
                                std::istreambuf_iterator<char>());
    EXPECT_TRUE(errorText.empty());

    std::filesystem::remove(executablePath);
    std::filesystem::remove(inputPath);
    std::filesystem::remove(outputPath);
    std::filesystem::remove(errorPath);
}

TEST(LlvmCodeGeneratorTests, UsesTargetPointerWidthForSizeTypes) {
    GeneratedSource result =
        Generate("isize identity(isize value) { return value; }");
    ASSERT_NE(result.boundProgram, nullptr);

    LlvmCodeGenerator generator;
    generator.ConfigureTarget("i386-unknown-linux-gnu",
                              llvm::DataLayout("e-p:32:32"));
    std::unique_ptr<llvm::Module> module =
        generator.Generate(*result.boundProgram, "pointer_width_test");
    ASSERT_NE(module, nullptr) << generator.Error();
    const std::string ir = ModuleText(*module);
    EXPECT_NE(ir.find("define i32 @identity(i32 %value)"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, UsesTargetPointerWidthForSizeCasts) {
    GeneratedSource result =
        Generate("usize Convert(i64 value) { return value.cast<usize>; }");
    ASSERT_NE(result.boundProgram, nullptr);

    LlvmCodeGenerator generator;
    generator.ConfigureTarget("i386-unknown-linux-gnu",
                              llvm::DataLayout("e-p:32:32"));
    std::unique_ptr<llvm::Module> module =
        generator.Generate(*result.boundProgram, "size_cast_test");
    ASSERT_NE(module, nullptr) << generator.Error();
    const std::string ir = ModuleText(*module);
    EXPECT_NE(ir.find("trunc i64"), std::string::npos);
    EXPECT_NE(ir.find("to i32"), std::string::npos);
}

TEST(NativeCompilerTests, EmitsObjectAndLinksExecutableForHost) {
    GeneratedSource result = Generate(
        "i32 add(i32 left, i32 right) { return left + right; } "
        "i32 main() { return add(20, 22); }");
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path temporaryDirectory =
        std::filesystem::temp_directory_path();
    const std::filesystem::path executablePath =
        temporaryDirectory / "flecha_native_codegen_test";
    const std::filesystem::path objectPath =
        temporaryDirectory / "flecha_native_codegen_test.o";
    std::filesystem::remove(executablePath);
    std::filesystem::remove(objectPath);

    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    EXPECT_FALSE(compiler.TargetTriple().empty());
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram, "native_test",
                                         executablePath.string()))
        << compiler.Error();
    EXPECT_FALSE(std::filesystem::exists(objectPath));
    EXPECT_TRUE(std::filesystem::is_regular_file(executablePath));

    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    const int exitCode =
        llvm::sys::ExecuteAndWait(executablePathText, arguments);
    EXPECT_EQ(exitCode, 42);

    std::filesystem::remove(executablePath);
    std::filesystem::remove(objectPath);
}

TEST(NativeCompilerTests, RunsForwardAndRecursiveCalls) {
    GeneratedSource result = Generate(
        "i32 main() { return Add(Factorial(3), 36); } "
        "i32 Add(i32 left, i32 right) { return left + right; } "
        "i32 Factorial(i32 value) { if value <= 1 { return 1; } "
        "return value * Factorial(value - 1); }");
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path temporaryDirectory =
        std::filesystem::temp_directory_path();
    const std::filesystem::path executablePath =
        temporaryDirectory / "flecha_native_functions_test";
    const std::filesystem::path objectPath =
        temporaryDirectory / "flecha_native_functions_test.o";
    std::filesystem::remove(executablePath);
    std::filesystem::remove(objectPath);

    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(
        *result.boundProgram, "native_functions_test", executablePath.string()))
        << compiler.Error();

    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);

    std::filesystem::remove(executablePath);
    std::filesystem::remove(objectPath);
}

TEST(NativeCompilerTests, RunsCallableParametersReturnsAndAggregateResults) {
    GeneratedSource result = Generate(R"(
        func Mapper(i32) => (i32);
        func PairFactory(i32) => (Pair);
        func Split(i32) => (i32, i32);
        structure Pair { i32 left; i32 right; };
        structure Handler { Mapper callback; };

        i32 twice(i32 value) { return value * 2; }
        Mapper choose() { return twice; }
        i32 apply(Mapper callback, i32 value) { return callback(value); }
        Handler make_handler() {
            return build Handler { self.callback = twice; };
        }

        Pair make_pair(i32 left) {
            return build Pair {
                self.left = left;
                self.right = 22;
            };
        }
        Pair invoke_factory(PairFactory factory, i32 value) {
            return factory(value);
        }
        (i32, i32) split(i32 left) { return (left, 22); }

        i32 main() {
            callback := choose();
            handler := make_handler();
            PairFactory factory = make_pair;
            Split splitter = split;
            pair := invoke_factory(factory, 20);
            left, right := splitter(20);
            return apply(callback, 21) + pair.left + pair.right + left +
                   right + (handler.callback)(1) - 86;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_callable_values_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_callable_values_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsOnceCallablesAndConsumesOwnedCaptures) {
    GeneratedSource result = Generate(R"(
        once func Action() => (i32);
        unsafe once func UnsafeAction() => (i32);

        structure Token { *i32 tears; i32 value; };
        unsafe Token Token::init(*i32 tears, i32 value) override {
            return build Token {
                self.tears = tears;
                self.value = value;
            };
        }
        void Token::tear(&self) override {
            unsafe { *self->tears = *self->tears + 1; }
        }
        i32 consume(Token token) { return token.value; }
        i32 answer() { return 22; }

        unsafe i32 main() {
            i32 tears = 0;
            {
                Token token = Token::init(&raw tears, 20);
                Action action = once [own token]() => (i32) {
                    return consume(token);
                };
                if action() != 20 { return 1; }
                if tears != 1 { return 2; }
            }
            if tears != 1 { return 3; }

            {
                Token token = Token::init(&raw tears, 1);
                Action never = once [own token]() => (i32) {
                    return consume(token);
                };
            }
            pointer, failure := new<i32>(20);
            if failure != none { return 4; }
            UnsafeAction release = unsafe once [own pointer]() => (i32) {
                i32 value = *pointer;
                free(pointer);
                return value;
            };
            Action direct = answer;
            return release() + direct() + tears - 2;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_once_callable_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_once_callable_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsGenericCallablesWithCapturingClosures) {
    GeneratedSource result = Generate(R"(
        func Mapper<T>(T) => (T);
        T apply<T>(Mapper<T> callback, T value) {
            return callback(value);
        }
        i32 main() {
            i32 offset = 20;
            Mapper<i32> callback = [copy offset](value) => {
                return value + offset;
            };
            return apply<i32>(callback, 22);
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_generic_callable_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_generic_callable_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, FoldsTypeSetMembershipAtSpecialization) {
    GeneratedSource result = Generate(R"(
        structure UserId { i32 value; };
        typeset identifier { integer, UserId, };
        bool is_numeric<T>() { return T.isof<numeric>; }
        bool is_identifier<T>() { return T.isof<identifier>; }
        bool is_i32<T>() { return T.isof<i32>; }
        bool same<T, U>() { return T.isof<U>; }
        i32 main() {
            if !is_numeric<i32>() { return 1; }
            if is_numeric<bool>() { return 2; }
            if !is_identifier<UserId>() { return 3; }
            if is_identifier<f64>() { return 4; }
            if !3.isof<i32> { return 5; }
            if !3.isof<numeric> { return 6; }
            if 3.isof<f64> { return 7; }
            if !is_i32<i32>() { return 8; }
            if is_i32<f64>() { return 9; }
            if !UserId::init().isof<UserId> { return 10; }
            if !UserId::init().isof<identifier> { return 11; }
            if !same<i32, i32>() { return 12; }
            if same<i32, i64>() { return 13; }
            return 42;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() / "flecha_native_type_set_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(
        *result.boundProgram, "native_type_set_test", executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, ReportsSignedAndUnsignedIntegerOverflow) {
    GeneratedSource result = Generate(R"(
        (T, bool) add<T: integer>(T left, T right) {
            result, overflowed := left.overflow_add(right);
            return (result, overflowed);
        }

        i32 main() {
            signed_sum, signed_add_overflow := add<i8>(127, 1);
            if signed_sum != ((-127 - 1) as i8) or !signed_add_overflow {
                return 1;
            }

            signed_difference, signed_sub_overflow :=
                ((-127 - 1) as i8).overflow_sub(1);
            if signed_difference != (127 as i8) or !signed_sub_overflow {
                return 2;
            }

            signed_product, signed_mul_overflow :=
                (64 as i8).overflow_mul(2);
            if signed_product != ((-127 - 1) as i8) or
                !signed_mul_overflow {
                return 3;
            }

            unsigned_sum, unsigned_add_overflow :=
                (255 as u8).overflow_add(1);
            if unsigned_sum != (0 as u8) or !unsigned_add_overflow {
                return 4;
            }

            unsigned_difference, unsigned_sub_overflow :=
                (0 as u8).overflow_sub(1);
            if unsigned_difference != (255 as u8) or
                !unsigned_sub_overflow {
                return 5;
            }

            unsigned_product, unsigned_mul_overflow :=
                (16 as u8).overflow_mul(16);
            if unsigned_product != (0 as u8) or !unsigned_mul_overflow {
                return 6;
            }

            exact, exact_overflow := (20 as i32).overflow_add(22);
            if exact != 42 or exact_overflow { return 7; }
            return 42;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);
    ASSERT_NE(result.module, nullptr);
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("llvm.sadd.with.overflow.i8"), std::string::npos);
    EXPECT_NE(ir.find("llvm.usub.with.overflow.i8"), std::string::npos);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_overflowing_arithmetic_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_overflowing_arithmetic_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsDefinedIntegerBitwiseAndShiftOperations) {
    GeneratedSource result = Generate(R"(
        u8 shift_left(u8 value, usize count) { return value << count; }
        u8 shift_right(u8 value, usize count) { return value >> count; }
        i8 signed_shift_right(i8 value, usize count) {
            return value >> count;
        }
        T generic_bits<T: integer>(T left, T right) {
            return ~(left & right) | (left ^ right);
        }

        i32 main() {
            if ((12 as u8) & (10 as u8)) != (8 as u8) { return 1; }
            if ((12 as u8) | (10 as u8)) != (14 as u8) { return 2; }
            if ((12 as u8) ^ (10 as u8)) != (6 as u8) { return 3; }
            if ~(0 as u8) != (255 as u8) { return 4; }
            if shift_left(1, 7) != (128 as u8) { return 5; }
            if shift_right(128, 7) != (1 as u8) { return 6; }
            if signed_shift_right(-2, 1) != (-1 as i8) { return 7; }
            if shift_left(1, 8) != (0 as u8) { return 8; }
            if shift_right(255, 8) != (0 as u8) { return 9; }
            if signed_shift_right(-1, 8) != (-1 as i8) { return 10; }
            if shift_left(1, usize.max_value) != (0 as u8) { return 11; }
            if signed_shift_right(1, 8) != (0 as i8) { return 12; }
            if generic_bits(12 as u8, 10 as u8) != (247 as u8) { return 13; }
            return 42;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);
    ASSERT_NE(result.module, nullptr);
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find(" shl "), std::string::npos);
    EXPECT_NE(ir.find(" lshr "), std::string::npos);
    EXPECT_NE(ir.find(" ashr "), std::string::npos);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_bitwise_shift_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_bitwise_shift_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, WrapsOrdinaryIntegerArithmeticAtTypeWidth) {
    GeneratedSource result = Generate(R"(
        i32 main() {
            i8 signed_max = 127;
            i8 signed_min = -127 - 1;
            i8 signed_one = 1;
            i8 signed_sixty_four = 64;
            if signed_max + signed_one != signed_min { return 1; }
            if signed_min - signed_one != signed_max { return 2; }
            if signed_sixty_four * (2 as i8) != signed_min { return 3; }

            u8 unsigned_max = 255;
            u8 unsigned_zero = 0;
            u8 unsigned_one = 1;
            u8 unsigned_sixteen = 16;
            if unsigned_max + unsigned_one != unsigned_zero { return 4; }
            if unsigned_zero - unsigned_one != unsigned_max { return 5; }
            if unsigned_sixteen * unsigned_sixteen != unsigned_zero {
                return 6;
            }
            return 42;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_wrapping_arithmetic_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_wrapping_arithmetic_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, EmitsIntegerTypeLimits) {
    GeneratedSource result = Generate(R"(
        (T, T) bounds<T: integer>() {
            return (T.min_value, T.max_value);
        }

        i32 main() {
            i8_low, i8_high := bounds<i8>();
            if i8_low != ((-127 - 1) as i8) or i8_high != (127 as i8) {
                return 1;
            }
            if u8.min_value != (0 as u8) or
                u8.max_value != (255 as u8) {
                return 2;
            }
            if i16.min_value - (1 as i16) != i16.max_value {
                return 3;
            }
            if u64.max_value + (1 as u64) != u64.min_value {
                return 4;
            }
            if isize.min_value - (1 as isize) != isize.max_value {
                return 5;
            }
            if usize.max_value + (1 as usize) != usize.min_value {
                return 6;
            }
            return 42;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_integer_limits_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_integer_limits_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, EmitsIeeeFloatingPointConstantsAndComparisons) {
    GeneratedSource result = Generate(R"(
        bool different(f64 left, f64 right) { return left != right; }

        (T, T) special<T: floating_point>() {
            return (T.infinity, T.nan);
        }

        i32 main() {
            f64 nan = f64.nan;
            if nan == nan { return 1; }
            if !different(nan, nan) { return 2; }
            if nan < 0.0 or nan <= 0.0 or nan > 0.0 or nan >= 0.0 {
                return 3;
            }
            if !(nan !== nan) { return 4; }

            if f32.infinity <= f32.max_value { return 5; }
            if f32.negative_infinity >= f32.min_value { return 6; }
            if f32.negative_infinity != -f32.infinity { return 7; }
            if f64.min_value != -f64.max_value { return 8; }
            if f32.min_positive <= (0.0 as f32) { return 9; }

            f32 one = 1.0 as f32;
            if one + f32.epsilon == one { return 10; }
            if one + f32.epsilon / (2.0 as f32) != one { return 11; }
            if (-0.0) != 0.0 or !((-0.0) === 0.0) { return 12; }

            f64 one64 = 1.0;
            if one64 + f64.epsilon == one64 { return 13; }
            if one64 + f64.epsilon / 2.0 != one64 { return 14; }
            if f64.min_positive <= 0.0 { return 15; }
            if f32.max_value + f32.max_value != f32.infinity { return 16; }

            infinity, genericNan := special<f32>();
            if infinity != f32.infinity or !(genericNan != genericNan) {
                return 17;
            }
            return 42;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);
    ASSERT_NE(result.module, nullptr);
    EXPECT_NE(ModuleText(*result.module).find("fcmp une"), std::string::npos);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_ieee_constants_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_ieee_constants_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsStructuralHashIntrinsic) {
    GeneratedSource result = Generate(R"(
        structure HashPair {
            [2]i32 values;
            u8 category;
        };

        HashPair HashPair::zero() override {
            return build HashPair {};
        }

        HashPair HashPair::init([2]i32 values, u8 category) override {
            return build HashPair {
                self.values = values;
                self.category = category;
            };
        }

        i32 main() {
            i32 first = 42;
            i32 same = 42;
            i32 different = 43;
            if __hash<i32>(&first) != __hash<i32>(&same) { return 1; }
            if __hash<i32>(&first) == __hash<i32>(&different) { return 2; }

            f64 positive = 0.0;
            f64 negative = -0.0;
            if __hash<f64>(&positive) != __hash<f64>(&negative) { return 3; }

            string text = "struc" + "tural";
            if __hash<stringv>(text.view()) != __hash<stringv>("structural") {
                return 4;
            }

            left := HashPair::init([7, 11], 3);
            right := HashPair::init([7, 11], 3);
            other := HashPair::init([7, 12], 3);
            if __hash<HashPair>(&left) != __hash<HashPair>(&right) { return 5; }
            if __hash<HashPair>(&left) == __hash<HashPair>(&other) { return 6; }
            return 42;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_structural_hash_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_structural_hash_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsCapturelessAnonymousFunctions) {
    GeneratedSource result = Generate(R"(
        func Mapper(i32) => (i32);
        func Split(i32) => (i32, i32);
        i32 apply(Mapper callback, i32 value) { return callback(value); }
        Mapper make() { return (value) => { return value + 1; }; }

        i32 main() {
            explicit := [](i32 value) => (i32) {
                return value * 2;
            };
            Mapper inferred = (value) => { return value + 2; };
            Mapper assigned = [](i32 value) => (i32) { return value; };
            assigned = (value) => { return value + 3; };
            Split split = (i32 value) => (i32, i32) {
                return (value, value + 1);
            };
            left, right := split(3);
            return apply(explicit, 10) + apply(inferred, 5) +
                   apply([](value) => { return value; }, 6) + make()(1) +
                   left + right;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_anonymous_functions_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_anonymous_functions_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsCopyCapturesAfterTheirCreatorReturns) {
    GeneratedSource result = Generate(R"(
        func Mapper(i32) => (i32);

        Mapper make(i32 factor) {
            string label = "original";
            Mapper callback = [copy factor, copy label](value) => {
                if label == "original" { return value * factor; }
                return 0;
            };
            factor = 99;
            label = "changed";
            return callback;
        }

        i32 main() {
            Mapper callback = make(1);
            callback = make(2);
            return callback(21);
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_copy_capture_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_copy_capture_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsNestedCopyCaptures) {
    GeneratedSource result = Generate(R"(
        func Mapper(i32) => (i32);
        func Factory(i32) => (Mapper);

        i32 main() {
            i32 base = 1;
            Factory outer = [copy base](delta) => {
                return [copy base, copy delta](value) => {
                    return base + delta + value;
                };
            };
            base = 100;
            Mapper inner = outer(20);
            return inner(21);
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_nested_capture_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_nested_capture_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsCheckedReferenceCaptures) {
    GeneratedSource result = Generate(R"(
        func Counter() => (i32);
        i32 main() {
            i32 value = 40;
            Counter increment = [&value]() => {
                value = value + 1;
                return value;
            };
            increment();
            return increment();
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_reference_capture_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_reference_capture_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsReturnedCheckedReferenceCaptures) {
    GeneratedSource result = Generate(R"(
        func Reader() => (i32);
        Reader bind(&const i32 source) <- source {
            return [&const source]() => { return *source; };
        }
        i32 main() {
            i32 value = 42;
            Reader callback = bind(&value);
            return callback();
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_returned_reference_capture_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(
        *result.boundProgram, "native_returned_reference_capture_test",
        executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsUnsafeRawPointerCopyCaptures) {
    GeneratedSource result = Generate(R"(
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
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_raw_capture_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_raw_capture_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsLentAndOwnedRawResourceCaptures) {
    GeneratedSource result = Generate(R"(
        unsafe func Reader() => (i32);

        structure Probe { *i32 tears; i32 value; };
        unsafe Probe Probe::init(*i32 tears, i32 value) override {
            return build Probe {
                self.tears = tears;
                self.value = value;
            };
        }
        void Probe::tear(&self) override {
            unsafe { *self->tears = *self->tears + 1; }
        }

        unsafe i32 main() {
            borrowed, borrowFailure := new<i32>(20);
            if borrowFailure != none { return 1; }
            i32 borrowedValue = 0;
            {
                Reader reader = unsafe [lend borrowed]() => {
                    return *borrowed;
                };
                borrowedValue = reader();
            }
            free(borrowed);

            i32 tears = 0;
            probe, ownFailure := new<Probe>(Probe::init(&raw tears, 22));
            if ownFailure != none { return 2; }
            {
                Reader owner = unsafe [own probe]() => {
                    return probe->value;
                };
                if owner() != 22 { return 3; }
                if owner() != 22 { return 4; }
                if tears != 0 { return 5; }
            }
            if tears != 1 { return 6; }
            return borrowedValue + 22;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_raw_resource_capture_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_raw_resource_capture_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, NestsOwnedClosuresContainersAndNoncopyableState) {
    GeneratedSource result = Generate(R"(
        func Reader() => (i32);

        structure State { *i32 tears; i32 value; };
        unsafe State State::init(*i32 tears, i32 value) override {
            return build State {
                self.tears = tears;
                self.value = value;
            };
        }
        void State::tear(&self) override {
            unsafe { *self->tears = *self->tears + 1; }
        }
        i32 State::read(&const self) { return self->value; }

        unsafe i32 main() {
            i32 tears = 0;
            i32 result = 0;
            {
                State state = State::init(&raw tears, 20);
                Reader stateful = [own state]() => { return state.read(); };
                i32 offset = 2;
                Reader captured = [copy offset]() => { return offset; };
                [2]Reader callbacks = [stateful, captured];
                Reader nested = [own callbacks]() => {
                    return (callbacks[0])() + (callbacks[1])();
                };
                if nested() != 22 { return 1; }
                if nested() != 22 { return 2; }
                if tears != 0 { return 3; }
                result = nested() + 20;
            }
            if tears != 1 { return 4; }
            return result;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_generalized_own_capture_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_generalized_own_capture_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, ReturnsOwnedRawResourceCaptures) {
    GeneratedSource result = Generate(R"(
        unsafe func Reader() => (i32);

        unsafe Reader bind(own *i32 pointer) {
            return unsafe [own pointer]() => { return *pointer; };
        }

        unsafe i32 main() {
            pointer, pointerFailure := new<i32>(20);
            if pointerFailure != none { return 1; }
            Reader returned = bind(pointer);

            buffer, bufferFailure := new<[]i32>(2);
            if bufferFailure != none { return 2; }
            buffer[0] = 22;
            Reader dynamic = unsafe [own buffer]() => { return buffer[0]; };
            return returned() + dynamic();
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_returned_raw_resource_capture_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(
        *result.boundProgram, "native_returned_raw_resource_capture_test",
        executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsUnsafeCallableOwnershipContracts) {
    GeneratedSource result = Generate(R"(
        unsafe func Reader(lend *const i32 source) => (i32);
        unsafe func Consumer(own *i32 source) => ();
        unsafe i32 read(lend *const i32 source) { return *source; }
        unsafe void consume(own *i32 source) { free(source); }
        i32 main() {
            unsafe {
                pointer, failure := new<i32>(42);
                if failure != none { return 1; }
                Reader reader = read;
                Consumer consumer = consume;
                result := reader(pointer);
                consumer(pointer);
                return result;
            }
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_callable_contracts_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_callable_contracts_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, PreservesBorrowContractsAcrossIndirectCalls) {
    GeneratedSource result = Generate(R"(
        func Borrower(&i32 source) => (&i32) <- source;
        &i32 borrow(&i32 source) <- source { return source; }
        i32 main() {
            value := 42;
            Borrower callback = borrow;
            reference := callback(&value);
            *reference = *reference + 1;
            return value - 1;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_callable_borrow_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_callable_borrow_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsBorrowedReferenceAndSliceResults) {
    GeneratedSource result = Generate(R"(
        structure Pair { i32 left; i32 right; };
        (&i32, &i32) fields(&Pair pair) <- pair {
            return (&pair->left, &pair->right);
        }
        []const i32 view([]const i32 values) <- values { return values; }
        i32 main() {
            Pair pair = none;
            left, right := fields(&pair);
            *left = 20;
            *right = 22;
            values := [20, 22];
            readonly := view(values[:]);
            return pair.left + pair.right + readonly[0] - 20;
        }
    )");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_borrowed_results_test";
    std::filesystem::remove(executablePath);

    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_borrowed_results_test",
                                         executablePath.string()))
        << compiler.Error();

    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);

    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsMixedShortDestructuringDeclarations) {
    GeneratedSource result = Generate(R"(
        (i32, i32) pair(i32 value, i32 status) {
            return (value, status);
        }
        i32 main() {
            first, err := pair(20, 1);
            second, err := pair(22, 0);
            return first + second + err;
        }
    )");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_short_destructure_test";
    std::filesystem::remove(executablePath);

    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_short_destructure_test",
                                         executablePath.string()))
        << compiler.Error();

    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);

    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsOwnerBackedCheckedSlices) {
    GeneratedSource result = Generate(R"(
        structure Buffer { own *i32 data; usize length; };
        unsafe Buffer Buffer::init(own *i32 data, usize length) override {
            return build Buffer {
                self.data = data;
                self.length = length;
            };
        }
        unsafe void Buffer::tear(&self) override { free(self->data); }
        unsafe []i32 Buffer::view(&self) <- self {
            return self->data[:self->length];
        }
        i32 main() {
            unsafe {
                empty := Buffer::init(null, 0);
                emptyValues := empty.view();
                if emptyValues.len() != 0 { return 2; }
                emptyValues.data();
                data, err := new<[]i32>(2);
                if err != none { return 1; }
                buffer := Buffer::init(data, 2);
                values := buffer.view();
                *i32 raw = values.data();
                *raw = 42;
                return values[0];
            }
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_owner_backed_slice_test";
    std::filesystem::remove(executablePath);

    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_owner_backed_slice_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsWholeValueMovesWithExactlyOnceCleanup) {
    GeneratedSource result = Generate(R"(
        structure Box { own *i32 data; };
        unsafe Box Box::init(own *i32 data) override {
            return build Box { self.data = data; };
        }
        void Box::tear(&self) override {
            unsafe { free(self->data); }
        }
        unsafe i32 Box::read(&const self) { return *self->data; }
        Box relay(Box value) { return value; }
        (Box, string, i32) tag(Box value) {
            return (value, "moved", 20);
        }
        i32 main() {
            unsafe {
                firstData, err := new<i32>(20);
                if err != none { return 1; }
                secondData, err := new<i32>(22);
                if err != none { free(firstData); return 2; }
                discardedData, err := new<i32>(1);
                if err != none { free(firstData); free(secondData); return 4; }
                discarded := Box::init(discardedData);
                discarded;
                first := Box::init(firstData);
                second := Box::init(secondData);
                first = second;
                tagged, label, extra := tag(first);
                if label != "moved" { return 3; }
                result := relay(relay(tagged));
                return result.read() + extra;
            }
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_whole_value_moves_test";
    std::filesystem::remove(executablePath);

    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_whole_value_moves_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsRecursiveSafeAggregateOwnership) {
    GeneratedSource result = Generate(R"(
        structure Box { own *i32 data; };
        Box Box::zero() override { return build Box {}; }
        unsafe Box Box::init(own *i32 data) override {
            return build Box { self.data = data; };
        }
        void Box::tear(&self) override { unsafe { free(self->data); } }
        unsafe i32 Box::read(&const self) { return *self->data; }
        unsafe (Box, error) make_recursive(u32 depth) {
            if depth == 0 {
                data, err := new<i32>(42);
                if err != none { return (none, err); }
                return (Box::init(data), none);
            }
            return make_recursive(depth - 1);
        }
        i32 main() {
            unsafe {
                value, err := make_recursive(3);
                if err != none { return 1; }
                return value.read();
            }
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_recursive_safe_aggregate_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_recursive_safe_aggregate_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsChainedOwningReceiverMethods) {
    GeneratedSource result = Generate(R"(
        structure Box { *i32 tears; i32 value; };
        unsafe Box Box::init(*i32 tears) override {
            return build Box { self.tears = tears; };
        }
        void Box::tear(&self) override {
            unsafe { (*self->tears)++; }
        }
        Box Box::withValue(self, i32 value) {
            self.value = value;
            return self;
        }
        i32 Box::read(&const self) { return self->value; }
        i32 main() {
            tears := 0;
            answer := 0;
            unsafe {
                box := Box::init(&raw tears)
                    .withValue(20)
                    .withValue(42);
                answer = box.read();
            }
            if tears != 1 { return 1; }
            return answer;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_owning_receiver_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_owning_receiver_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsMoveOnlyMatchResults) {
    GeneratedSource result = Generate(R"(
        enum Pick { Left, Right, }
        structure Box { i32 value; };
        void Box::tear(&self) override {}
        Box make(i32 value) {
            box := Box::init();
            box.value = value;
            return box;
        }
        Box choose(Pick pick, Box left, Box right) {
            return match pick {
                Pick.Left => left,
                Pick.Right => right,
            };
        }
        i32 main() {
            result := choose(Pick.Right, make(20), make(22));
            return result.value + 20;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_move_match_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_move_match_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, DeferredMoveOnlyArgumentsTransferCleanupOnce) {
    GeneratedSource result = Generate(R"(
        structure Box { own *i32 data; *i32 tears; };
        unsafe Box Box::init(own *i32 data, *i32 tears) override {
            return build Box {
                self.data = data;
                self.tears = tears;
            };
        }
        void Box::tear(&self) override {
            unsafe {
                (*self->tears)++;
                free(self->data);
            }
        }
        void consume(Box value) {}

        structure Snapshot { i32 value; };
        void observe(Snapshot value, &i32 output) {
            *output = value.value;
        }
        void observe_text(string value, &i32 output) {
            if value == "before" { *output = 20; }
        }

        i32 main() {
            unsafe {
                tears := 0;
                observed := 0;
                textObserved := 0;
                {
                    snapshot := Snapshot::init();
                    snapshot.value = 20;
                    defer observe(snapshot, &observed);
                    snapshot.value = 1;

                    string word = "before";
                    defer observe_text(word, &textObserved);
                    word = "after";

                    data, err := new<i32>(42);
                    if err != none { return 1; }
                    box := Box::init(data, &raw tears);
                    defer consume(box);
                }
                if tears != 1 { return 2; }
                return observed + textObserved + 2;
            }
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_deferred_move_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_deferred_move_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsDeferredIndirectAndMemberCalls) {
    GeneratedSource result = Generate(R"(
        func Action(i32) => ();
        unsafe func Consumer(own *i32) => ();
        unsafe void consume(own *i32 value) { free(value); }
        structure Writer { *i32 output; };
        unsafe Writer Writer::init(*i32 output) override {
            return build Writer { self.output = output; };
        }
        void Writer::flush(&self) {
            unsafe { *self->output = 20; }
        }
        structure Token { *i32 tears; };
        unsafe Token Token::init(*i32 tears) override {
            return build Token { self.tears = tears; };
        }
        void Token::tear(&self) override {
            unsafe { (*self->tears)++; }
        }
        void schedule(Action callback, Writer writer) {
            defer callback(40);
            defer writer.flush();
            return;
        }

        i32 main() {
            indirect := 0;
            member := 0;
            tears := 0;
            unsafe {
                {
                    add := 2;
                    token := Token::init(&raw tears);
                    Action callback = [copy add, &indirect, own token](value) => {
                        indirect = value + add;
                    };
                    writer := Writer::init(&raw member);
                    schedule(callback, writer);

                    pointer, failure := new<i32>(42);
                    if failure != none { return 2; }
                    Consumer consumer = consume;
                    defer consumer(pointer);
                }
            }
            if tears != 1 { return 1; }
            return indirect + member - 20;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_deferred_callable_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_deferred_callable_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, ZeroLengthMoveOnlyRepeatsDestroyTheirInitializer) {
    GeneratedSource result = Generate(R"(
        structure Box { *i32 tears; };
        unsafe Box Box::init(*i32 tears) override {
            return build Box { self.tears = tears; };
        }
        void Box::tear(&self) override {
            unsafe { (*self->tears)++; }
        }
        i32 main() {
            unsafe {
                tears := 0;
                zero := Box::init(&raw tears);
                empty := [zero; 0];
                if tears != 1 { return 1; }
                {
                    one := Box::init(&raw tears);
                    values := [one; 1];
                }
                if tears != 2 { return 2; }
                return 42;
            }
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_zero_length_move_repeat_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_zero_length_move_repeat_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsEnumMatchesInMemberFunctions) {
    GeneratedSource result = Generate(R"(
        enum TokenKind: u8 { Identifier = 2, Number = 7, End = 9, }
        i32 TokenKind::score(&const self) {
            return match *self {
                TokenKind.Identifier => 10,
                TokenKind.Number => 20,
                TokenKind.End => 42,
            };
        }
        i32 main() {
            TokenKind kind = TokenKind.End;
            return kind.score();
        }
    )");
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() / "flecha_native_match_test";
    std::filesystem::remove(executablePath);

    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram, "native_match",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);

    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, ObjectEmissionDoesNotRequireRuntimeLibrary) {
    GeneratedSource result = Generate("i32 main() { stdoutln(42); return 0; }");
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path objectPath =
        std::filesystem::temp_directory_path() /
        "flecha_object_without_runtime_test.o";
    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() / "flecha_missing_runtime_test";
    std::filesystem::remove(objectPath);
    std::filesystem::remove(executablePath);

    NativeCompiler compiler("/missing/flecha/runtime/library.a");
    ASSERT_TRUE(compiler.EmitObject(*result.boundProgram, "object_only_test",
                                    objectPath.string()))
        << compiler.Error();
    EXPECT_TRUE(std::filesystem::is_regular_file(objectPath));
    EXPECT_FALSE(
        compiler.LinkExecutable(objectPath.string(), executablePath.string()));
    ASSERT_TRUE(compiler.LastDiagnostic().has_value());
    EXPECT_EQ(compiler.LastDiagnostic()->code,
              DiagnosticCode::RuntimeUnavailable);

    std::filesystem::remove(objectPath);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, ReportsLinkerFailuresAsStructuredDiagnostics) {
    const std::filesystem::path temporaryDirectory =
        std::filesystem::temp_directory_path();
    const std::filesystem::path missingObject =
        temporaryDirectory / "flecha_intentionally_missing_object.o";
    const std::filesystem::path executablePath =
        temporaryDirectory / "flecha_failed_link_test";
    std::filesystem::remove(missingObject);
    std::filesystem::remove(executablePath);

    NativeCompiler compiler;
    EXPECT_FALSE(compiler.LinkExecutable(missingObject.string(),
                                         executablePath.string()));
    ASSERT_TRUE(compiler.LastDiagnostic().has_value());
    EXPECT_EQ(compiler.LastDiagnostic()->code, DiagnosticCode::LinkerFailed);
    EXPECT_NE(compiler.Error().find("system linker '"), std::string::npos);
    EXPECT_NE(compiler.Error().find("failed for target '"), std::string::npos);
    EXPECT_NE(compiler.Error().find("linker output:"), std::string::npos);
    EXPECT_NE(compiler.Error().find(missingObject.filename().string()),
              std::string::npos);
    EXPECT_NE(compiler.Error().find("FLECHA_LINKER"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(executablePath));
}

TEST(NativeCompilerTests, ReportsAttemptedDriversWhenNoLinkerIsOnPath) {
    const std::filesystem::path emptyPath =
        std::filesystem::temp_directory_path() /
        "flecha_linker_search_path_that_does_not_exist";
    ScopedEnvironment linkerOverride("FLECHA_LINKER", std::nullopt);
    ScopedEnvironment path("PATH", emptyPath.string());

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "flecha_missing_linker_test";
    NativeCompiler compiler;
    EXPECT_FALSE(compiler.LinkExecutable("missing-object.o", output.string()));
    ASSERT_TRUE(compiler.LastDiagnostic().has_value());
    EXPECT_EQ(compiler.LastDiagnostic()->code, DiagnosticCode::LinkerFailed);
    EXPECT_NE(compiler.Error().find("unable to find a supported system C++ "
                                    "linker driver for target '"),
              std::string::npos);
    EXPECT_NE(compiler.Error().find("tried:"), std::string::npos);
    EXPECT_NE(compiler.Error().find("FLECHA_LINKER"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(NativeCompilerTests, HonorsConfiguredLinkerWithoutFallback) {
    constexpr const char* CONFIGURED_LINKER =
        "flecha-linker-that-does-not-exist";
    ScopedEnvironment linkerOverride("FLECHA_LINKER", CONFIGURED_LINKER);

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "flecha_linker_override_test";
    NativeCompiler compiler;
    EXPECT_FALSE(compiler.LinkExecutable("missing-object.o", output.string()));
    ASSERT_TRUE(compiler.LastDiagnostic().has_value());
    EXPECT_EQ(compiler.LastDiagnostic()->code, DiagnosticCode::LinkerFailed);
    EXPECT_NE(compiler.Error().find(std::string("tried: ") + CONFIGURED_LINKER +
                                    ". "),
              std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(LlvmCodeGeneratorTests, EmitsDefaultedScalarAndStringDeclarations) {
    GeneratedSource result = Generate(
        "i32 main(bool c) { i32 value; string text; "
        "if c { return 0; } else { value = 42; text = \"ready\"; } "
        "stdoutln(text); return value; }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("%value = alloca i32"), std::string::npos);
    EXPECT_NE(ir.find("zeroinitializer, ptr %text"), std::string::npos);
    EXPECT_NE(ir.find("store i32 0, ptr %value"), std::string::npos);
}

TEST(NativeCompilerTests, RunsDefaultedStringAssignmentAndCleanup) {
    GeneratedSource result = Generate(R"(
        structure Text { string value; };

        i32 exercise(bool early) {
            string value;
            (string, string) pair;
            Text composite;
            if value != "" { return 1; }
            if composite.value != "" { return 6; }
            if early { return 42; }

            value = "owned";
            if value != "owned" { return 2; }
            composite.value = "nested";
            if composite.value != "nested" { return 8; }
            value = none;
            if value != "" { return 3; }
            return 42;
        }

        i32 main() {
            if exercise(false) != 42 { return 4; }
            return exercise(true);
        }
    )");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() / "flecha_defaulted_string_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(
        *result.boundProgram, "defaulted_string_test", executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(LlvmCodeGeneratorTests, CleansLaterStringSlotsAtEarlierBackwardJumpExits) {
    GeneratedSource result = Generate(
        "i32 main() { count := 0; while count < 2 { #again "
        "if count == 1 { return 42; } count = count + 1; "
        "string late = \"owned\"; jumpto again; } return 0; }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const llvm::Function* function = result.module->getFunction("$flecha$main");
    ASSERT_NE(function, nullptr);
    bool foundReturn = false;
    for (const llvm::BasicBlock& block : *function) {
        const auto* terminator =
            llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
        if (terminator == nullptr) continue;
        const auto* value =
            llvm::dyn_cast<llvm::ConstantInt>(terminator->getReturnValue());
        if (value == nullptr || value->getSExtValue() != 42) continue;
        foundReturn = true;
        bool cleansLate = false;
        for (const llvm::Instruction& instruction : block) {
            const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
            if (store != nullptr &&
                store->getPointerOperand()->getName() == "late") {
                cleansLate = llvm::isa<llvm::ConstantAggregateZero>(
                    store->getValueOperand());
            }
        }
        EXPECT_TRUE(cleansLate);
    }
    EXPECT_TRUE(foundReturn);
}

TEST(LlvmCodeGeneratorTests, EmitsRuntimeInitializedConstValuesAndParameters) {
    GeneratedSource result = Generate(
        "const i32 add(const i32 input) { return input + 2; } "
        "i32 main() { const i32 value = add(40); copy := value; "
        "copy = copy + 1; const i64 wide = value.cast<const i64>; "
        "const bool same = value == 42; const i32 zero = none; "
        "const string text = \"hi\"; const stringv view = text.view(); "
        "stdoutln(view); return copy - 1 + zero; }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("define i32 @add(i32 %input)"), std::string::npos);
    EXPECT_NE(ir.find("call i32 @add(i32 40)"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsTypedZeroValuesForConstInitializers) {
    GeneratedSource result = Generate(
        "const i32 zero() { return none; } "
        "i32 identity(const i32 value) { return value; } "
        "i32 main() { const bool b = none; const char c = none; "
        "const f64 f = none; const string s = none; const stringv v = none; "
        "const bool defaultBool; const char defaultChar; "
        "const f64 defaultFloat; const string defaultString; "
        "const stringv defaultView = none; i32 defaultInteger; "
        "i32 mutableZero = 1; mutableZero = none; "
        "const i32 result = identity((none)) + zero(); "
        "return result + defaultInteger; }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
}

TEST(LlvmCodeGeneratorTests,
     EmitsRawAddressesIndirectAccessAndPointerEquality) {
    GeneratedSource result = Generate(
        "unsafe i32 read(*const i32 p) { return *p; } "
        "i32 main() { i32 value = 40; *i32 p = null; "
        "unsafe { p = &raw value; (*p) = *p + 2; } "
        "if p != none and null != p { unsafe { return read(p); } } return 0; "
        "}");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("define i32 @read(ptr %p)"), std::string::npos);
    EXPECT_NE(ir.find("icmp ne ptr"), std::string::npos);
    EXPECT_NE(ir.find("load i32, ptr"), std::string::npos);
}

TEST(CodeGeneratorTests, EmitsCheckedReferencesAsAddressesAndReborrowingCalls) {
    GeneratedSource result = Generate(
        "void bump(&u8 r) { *r = *r + 1; }"
        "u8 read(&const u8 r) { return *r; }"
        "u8 main() { u8 x = 3; r := &x; *r = *r * 2; bump(r); return read(r); "
        "}");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("define void @bump(ptr %r)"), std::string::npos);
    EXPECT_NE(ir.find("define i8 @read(ptr %r)"), std::string::npos);
    EXPECT_NE(ir.find("call void @bump(ptr"), std::string::npos);
    EXPECT_NE(ir.find("mul i8"), std::string::npos);
}

TEST(CodeGeneratorTests, EmitsAggregateValuesAndBoundsCheckedElementAddresses) {
    GeneratedSource result = Generate(
        "[2]u8 make() { return [1, 2]; }"
        "[2]u8 copy([2]u8 value) { return value; }"
        "u8 access(usize i) { a := copy(make()); a[i] = 4; return a[i]; }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("define [2 x i8] @make()"), std::string::npos);
    EXPECT_NE(ir.find("define [2 x i8] @copy([2 x i8] %value)"),
              std::string::npos);
    EXPECT_NE(ir.find("icmp ult i64"), std::string::npos);
    EXPECT_NE(ir.find("call void @llvm.trap()"), std::string::npos);
    EXPECT_NE(ir.find("getelementptr inbounds [2 x i8]"), std::string::npos);
}

TEST(CodeGeneratorTests, EmitsRepeatLoopWithoutDuplicatingInitializerCalls) {
    GeneratedSource result = Generate(
        "u8 item() { return 7; }"
        "u8 f(usize i) { a := [item(); 4096]; return a[i]; }"
        "usize g() { a := [item(); 0]; return a.len(); }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    const std::size_t first = ir.find("call i8 @item()");
    const std::size_t second = ir.find("call i8 @item()", first + 1);
    ASSERT_NE(first, std::string::npos);
    ASSERT_NE(second, std::string::npos);
    EXPECT_EQ(ir.find("call i8 @item()", second + 1), std::string::npos);
    EXPECT_NE(ir.find("array.repeat.index = phi i64"), std::string::npos);
    EXPECT_LT(ir.size(), 10000);
}

TEST(CodeGeneratorTests, EmitsLoopUpdateTargetsAndSingleEvaluationIncrements) {
    GeneratedSource result = Generate(
        "usize index() { return 0; } "
        "[2]i32 values() { return [1, 2]; } "
        "i32 f() { a := [10, 20]; old := a[index()]++; "
        "loop i := 0; i < 2; i++ { proceed; } "
        "for (i, v) := range values() { proceed; } return old; }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("br label %loop.update"), std::string::npos);
    const std::size_t indexCall = ir.find("call i64 @index()");
    ASSERT_NE(indexCall, std::string::npos);
    EXPECT_EQ(ir.find("call i64 @index()", indexCall + 1), std::string::npos);
    const std::size_t sourceCall = ir.find("call [2 x i32] @values()");
    ASSERT_NE(sourceCall, std::string::npos);
    EXPECT_EQ(ir.find("call [2 x i32] @values()", sourceCall + 1),
              std::string::npos);
    EXPECT_NE(ir.find("increment.old = load i32"), std::string::npos);
}

TEST(CodeGeneratorTests, EmitsSingleEvaluationCompoundUpdates) {
    GeneratedSource result = Generate(
        "usize index() { return 0; } "
        "i32 f() { a := [16]; old := a[index()]--; a[index()] += 2; "
        "a[index()] -= 1; a[index()] *= 3; a[index()] /= 2; "
        "a[index()] ^= 4; return old + a[0]; }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    std::size_t calls = 0;
    std::size_t position = 0;
    while ((position = ir.find("call i64 @index()", position)) !=
           std::string::npos) {
        ++calls;
        ++position;
    }
    EXPECT_EQ(calls, 6);
    EXPECT_NE(ir.find("decrement.old = load i32"), std::string::npos);
    EXPECT_NE(ir.find("compound.add = add i32"), std::string::npos);
    EXPECT_NE(ir.find("compound.sub = sub i32"), std::string::npos);
    EXPECT_NE(ir.find("compound.mul = mul i32"), std::string::npos);
    EXPECT_NE(ir.find("compound.div = sdiv i32"), std::string::npos);
    EXPECT_NE(ir.find("compound.xor = xor i32"), std::string::npos);
}

TEST(CodeGeneratorTests, EmitsSliceDescriptorsChecksAndReborrowedCalls) {
    GeneratedSource result = Generate(
        "usize size([]const u8 s) { return s.len(); }"
        "void change([]u8 s) { s.front()++; s.back() = 5; }"
        "usize f(usize start, usize end) { [4]u8 a = [1, 2, 3, 4]; "
        "s := a[start:end]; change(s); return size(s); }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("{ ptr, i64 }"), std::string::npos);
    EXPECT_NE(ir.find("icmp ule i64"), std::string::npos);
    EXPECT_NE(ir.find("icmp ult i64"), std::string::npos);
    EXPECT_NE(ir.find("call void @llvm.trap()"), std::string::npos);
}

TEST(CodeGeneratorTests, EmitsDynamicDeferredRecordsAndScopeStackRestoration) {
    GeneratedSource result = Generate(
        "void f() { i := 0; while i < 3 { #again defer stdoutln(i); i++; "
        "if i < 3 { jumpto again; } } }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("defer.record = alloca"), std::string::npos);
    EXPECT_NE(ir.find("@llvm.stacksave"), std::string::npos);
    EXPECT_NE(ir.find("@llvm.stackrestore"), std::string::npos);
    EXPECT_NE(ir.find("switch i32"), std::string::npos);
    EXPECT_EQ(ir.find("@malloc"), std::string::npos);
}

TEST(CodeGeneratorTests, VerifiesDeferAcrossEarlyReturnsAndScopeEntryJumps) {
    GeneratedSource result = Generate(
        "void f(bool b) { while true { jumpto inside; { defer stdoutln(1); "
        "#inside defer stdoutln(2); if b { return; } break; } } }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
}

TEST(CodeGeneratorTests, VerifiesNestedTupleReturnsArgumentsAndDestructuring) {
    auto result = Generate(R"(
        (i32, (bool, u8)) pair(i32 n) { return (n, (true, 7)); }
        (i32, (bool, u8)) pass((i32, (bool, u8)) p) { return p; }
        i32 main() { t := pass(pair(42)); (n, inner) := t; (ok, byte) := inner; return n; }
    )");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr);
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    EXPECT_NE(
        ModuleText(*result.module).find("define { i32, { i1, i8 } } @pair"),
        std::string::npos);
}

TEST(CodeGeneratorTests, VerifiesReferenceAndSliceResultContracts) {
    auto result = Generate(R"(
        &i32 identity(&i32 value) <- value { return value; }
        structure Pair { i32 left; i32 right; };
        (&i32, &i32) fields(&Pair pair) <- pair {
            return (&pair->left, &pair->right);
        }
        []const i32 view([]const i32 values) <- values { return values; }
        i32 main() {
            Pair pair = none;
            left, right := fields(&pair);
            *left = 21;
            *right = 21;
            values := [20, 22];
            slice := values[:];
            readonly := view(slice);
            return pair.left + pair.right + readonly[0] + readonly[1];
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
}

TEST(CodeGeneratorTests, VerifiesOwnedTupleCleanupAndDeferredCaptures) {
    auto result = Generate(R"(
        (string, i32) pair() { return ("value", 42); }
        void use((string, i32) p) { (s, n) := p; }
        i32 main() {
            i32 count = 0;
            loop ;; {
                #again
                if count == 1 { return 42; }
                t := pair();
                defer use(t);
                defer pair();
                t = pair();
                (string s, _) = t;
                (s, _) = pair();
                count++;
                jumpto again;
            }
        }
    )");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr);
    EXPECT_TRUE(VerifyModule(*result.module).empty());
}

TEST(CodeGeneratorTests,
     VerifiesErrorFamilyIdentityWideningAndTuplePropagation) {
    GeneratedSource result = Generate(
        "error Fault { Bad } error Other { Bad } "
        "(i32, Fault) named(bool bad) { if bad { return (0, Fault.Bad); } "
        "return (42, none); } "
        "(i32, error) generic(bool bad) { return named(bad); } "
        "bool equal(error a, error b) { return a == b; } "
        "bool compare() { return equal(Fault.Bad, Other.Bad); }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("@.flecha.error.Fault = private constant i8 0"),
              std::string::npos);
    EXPECT_NE(ir.find("@.flecha.error.Other = private constant i8 0"),
              std::string::npos);
    EXPECT_NE(ir.find("select i1"), std::string::npos);
    EXPECT_NE(ir.find("define { i32, { ptr, i32 } } @generic"),
              std::string::npos);
}

TEST(LlvmCodeGeneratorTests, ConstructsStructureResultsInCallerStorage) {
    GeneratedSource result = Generate(
        "structure Box { i32 x = 42; }; "
        "unsafe void Box::tear(&self) override {} "
        "unsafe Box factory() { return Box::init(); } "
        "unsafe (Box, i32) pair() { return (factory(), 42); } "
        "i32 main() { unsafe { Box box = factory(); defer pair(); return "
        "box.x; } }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("define void @factory(ptr"), std::string::npos);
    EXPECT_NE(ir.find("define void @pair(ptr"), std::string::npos);
    EXPECT_NE(ir.find("call void @factory(ptr"), std::string::npos);
    EXPECT_NE(ir.find("call void @pair(ptr"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsConditionalAllocationAndTypedFree) {
    GeneratedSource result = Generate(
        "structure Box { i32 x; }; unsafe void Box::tear(&self) override {} "
        "unsafe void release(*Box p) { return free(p); } "
        "i32 main() { unsafe { p, e := new<Box>(Box::init()); "
        "if e != none { return 1; } defer free(p); return p->x; } }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("@flecha_runtime_v1_alloc"), std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_free"), std::string::npos);
    EXPECT_NE(ir.find("allocation.failure"), std::string::npos);
    EXPECT_NE(ir.find("free.release"), std::string::npos);
}
